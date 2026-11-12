#include "core/crash.h"

#include <algorithm>
#include <cstring>

#include "core/checksum.h"
#include "core/endian.h"

namespace bn::core {

namespace {

constexpr std::size_t kChecksumSize = 4;
constexpr std::size_t kWireTraceRecordSize = 16;

// Receives the encoding in order and keeps only the part that falls in a
// window: bytes before `offset` are counted and dropped, and so are bytes
// after the window is full.
//
// The checksum that ends the encoding covers everything before it, so it
// costs a pass over the whole report. A window that stops short of those
// last bytes has no use for it; `checksum` says whether to work it out.
class WindowWriter {
 public:
  WindowWriter(std::size_t offset, std::span<std::uint8_t> out, bool checksum) noexcept
      : skip_(offset), out_(out), checksum_(checksum) {}

  void Put(std::span<const std::uint8_t> bytes) noexcept {
    if (checksum_) {
      crc_ = Crc32(bytes, crc_);
    }
    Keep(bytes);
  }

  void Put(std::uint8_t byte) noexcept { Put(std::span<const std::uint8_t>(&byte, 1)); }

  void PutU16(std::uint16_t value) noexcept {
    std::array<std::uint8_t, 2> bytes{};
    StoreLe16(bytes, value);
    Put(bytes);
  }

  void PutU32(std::uint32_t value) noexcept {
    std::array<std::uint8_t, 4> bytes{};
    StoreLe32(bytes, value);
    Put(bytes);
  }

  // A length byte, then the characters.
  void PutText(std::span<const char> field) noexcept {
    const std::string_view text = CrashText(field);
    Put(static_cast<std::uint8_t>(text.size()));
    Put({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
  }

  // Ends the encoding with the checksum of what came before.
  void PutChecksum() noexcept {
    std::array<std::uint8_t, kChecksumSize> bytes{};
    StoreLe32(bytes, crc_);
    Keep(bytes);
  }

  // Bytes kept in the window.
  std::size_t Written() const noexcept { return written_; }

 private:
  void Keep(std::span<const std::uint8_t> bytes) noexcept {
    const std::size_t skipped = std::min(skip_, bytes.size());
    skip_ -= skipped;
    bytes = bytes.subspan(skipped);
    const std::size_t kept = std::min(bytes.size(), out_.size() - written_);
    if (kept > 0) {
      std::memcpy(out_.data() + written_, bytes.data(), kept);
      written_ += kept;
    }
  }

  std::size_t skip_;
  std::span<std::uint8_t> out_;
  bool checksum_;
  std::size_t written_ = 0;
  std::uint32_t crc_ = 0;
};

// The counts of a report, limited to what its arrays hold: a report read
// from memory that survived a restart is not trusted to be sane.
struct Counts {
  std::size_t registers;
  std::size_t stack;
  std::size_t trace;
};

Counts CountsOf(const CrashReport& report) noexcept {
  return Counts{
      .registers = std::min<std::size_t>(report.registerCount, kCrashMaxRegisters),
      .stack = std::min<std::size_t>(report.stackSize, kCrashMaxStackBytes),
      .trace = std::min<std::size_t>(report.traceCount, kCrashMaxTraceRecords),
  };
}

// Takes values off the front of a byte span, noting if it ever runs short.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) noexcept : rest_(bytes) {}

  std::uint8_t U8() noexcept {
    const std::span<const std::uint8_t> bytes = Take(1);
    return bytes.empty() ? std::uint8_t{0} : bytes[0];
  }

  std::uint16_t U16() noexcept {
    const std::span<const std::uint8_t> bytes = Take(2);
    return bytes.empty() ? std::uint16_t{0} : LoadLe16(bytes.first<2>());
  }

  std::uint32_t U32() noexcept {
    const std::span<const std::uint8_t> bytes = Take(4);
    return bytes.empty() ? 0 : LoadLe32(bytes.first<4>());
  }

  // The next `count` bytes, or nothing (and the reader fails) if fewer are
  // left.
  std::span<const std::uint8_t> Take(std::size_t count) noexcept {
    if (count > rest_.size()) {
      failed_ = true;
      rest_ = {};
      return {};
    }
    const std::span<const std::uint8_t> bytes = rest_.first(count);
    rest_ = rest_.subspan(count);
    return bytes;
  }

  // A length byte and that many characters, into a text field. Fails if the
  // text would not fit the field.
  void Text(std::span<char> field) noexcept {
    const std::size_t length = U8();
    const std::span<const std::uint8_t> bytes = Take(length);
    if (length >= field.size()) {
      failed_ = true;
      return;
    }
    SetCrashText(field, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
  }

  bool Failed() const noexcept { return failed_; }
  std::size_t Remaining() const noexcept { return rest_.size(); }

 private:
  std::span<const std::uint8_t> rest_;
  bool failed_ = false;
};

// The whole encoding, in order, into `writer`.
void Encode(const CrashReport& report, WindowWriter& writer) noexcept {
  const Counts counts = CountsOf(report);

  writer.Put(kCrashFormat);
  writer.Put(static_cast<std::uint8_t>(report.arch));
  writer.Put(static_cast<std::uint8_t>(counts.registers));
  writer.Put(static_cast<std::uint8_t>(counts.trace));
  writer.PutU16(static_cast<std::uint16_t>(counts.stack));
  writer.PutU32(report.bootsSince);
  writer.PutU32(static_cast<std::uint32_t>(report.timeUs));
  writer.PutU32(static_cast<std::uint32_t>(report.timeUs >> 32U));
  writer.PutU32(report.stackAddress);

  writer.PutText(report.reason);
  writer.PutText(report.task);
  writer.PutText(report.version);
  writer.PutText(report.buildId);

  for (const std::uint32_t value : std::span(report.registers).first(counts.registers)) {
    writer.PutU32(value);
  }
  writer.Put(std::span(report.stack).first(counts.stack));
  for (const TraceRecord& record : std::span(report.trace).first(counts.trace)) {
    writer.PutU32(record.startUs);
    writer.PutU32(record.durationUs);
    writer.PutU32(record.arg);
    writer.PutU16(static_cast<std::uint16_t>(record.id));
    writer.Put(record.task);
    writer.Put(static_cast<std::uint8_t>(record.kind));
  }
  writer.PutChecksum();
}

}  // namespace

void SetCrashText(std::span<char> field, std::string_view text) noexcept {
  if (field.empty()) {
    return;
  }
  const std::size_t length = std::min(text.size(), field.size() - 1);
  for (std::size_t i = 0; i < length; ++i) {
    field[i] = text[i];
  }
  // Clearing the rest as well keeps a stored report free of stale bytes.
  for (std::size_t i = length; i < field.size(); ++i) {
    field[i] = '\0';
  }
}

std::string_view CrashText(std::span<const char> field) noexcept {
  std::size_t length = 0;
  while (length < field.size() && field[length] != '\0') {
    ++length;
  }
  // A field with no terminator is damaged; its last place is not text.
  if (length == field.size() && length > 0) {
    --length;
  }
  return {field.data(), length};
}

std::size_t EncodedCrashReportSize(const CrashReport& report) noexcept {
  const Counts counts = CountsOf(report);
  constexpr std::size_t kFixedFields = 22;  // Everything before the text fields.
  constexpr std::size_t kTextFields = 4;    // Each has a length byte.
  return kFixedFields + kTextFields + CrashText(report.reason).size() + CrashText(report.task).size() +
         CrashText(report.version).size() + CrashText(report.buildId).size() +
         (counts.registers * sizeof(std::uint32_t)) + counts.stack + (counts.trace * kWireTraceRecordSize) +
         kChecksumSize;
}

std::size_t EncodeCrashReport(const CrashReport& report, std::size_t offset, std::span<std::uint8_t> out) noexcept {
  const std::size_t total = EncodedCrashReportSize(report);
  if (offset >= total || out.empty()) {
    return 0;
  }
  // Only the piece that holds the end of the report needs the checksum.
  const std::size_t end = offset + std::min(out.size(), total - offset);
  WindowWriter writer(offset, out, end > total - kChecksumSize);
  Encode(report, writer);
  return writer.Written();
}

bool DecodeCrashReport(std::span<const std::uint8_t> bytes, CrashReport& report) noexcept {
  if (bytes.size() < kChecksumSize) {
    return false;
  }
  const std::span<const std::uint8_t> body = bytes.first(bytes.size() - kChecksumSize);
  if (Crc32(body) != LoadLe32(bytes.last<kChecksumSize>())) {
    return false;
  }

  Reader reader(body);
  if (reader.U8() != kCrashFormat) {
    return false;
  }
  report = CrashReport{};
  report.arch = static_cast<CrashArch>(reader.U8());
  report.registerCount = reader.U8();
  report.traceCount = reader.U8();
  report.stackSize = reader.U16();
  report.bootsSince = reader.U32();
  const std::uint64_t timeLow = reader.U32();
  const std::uint64_t timeHigh = reader.U32();
  report.timeUs = timeLow | (timeHigh << 32U);
  report.stackAddress = reader.U32();
  if (report.registerCount > kCrashMaxRegisters || report.stackSize > kCrashMaxStackBytes ||
      report.traceCount > kCrashMaxTraceRecords) {
    return false;
  }

  reader.Text(report.reason);
  reader.Text(report.task);
  reader.Text(report.version);
  reader.Text(report.buildId);

  for (std::size_t i = 0; i < report.registerCount; ++i) {
    report.registers[i] = reader.U32();
  }
  const std::span<const std::uint8_t> stack = reader.Take(report.stackSize);
  if (!stack.empty()) {
    std::memcpy(report.stack.data(), stack.data(), stack.size());
  }
  for (std::size_t i = 0; i < report.traceCount; ++i) {
    TraceRecord& record = report.trace[i];
    record.startUs = reader.U32();
    record.durationUs = reader.U32();
    record.arg = reader.U32();
    record.id = static_cast<SpanId>(reader.U16());
    record.task = reader.U8();
    record.kind = static_cast<TraceKind>(reader.U8());
  }
  return !reader.Failed() && reader.Remaining() == 0;
}

}  // namespace bn::core
