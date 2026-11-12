#include "core/crash.h"

#include <algorithm>
#include <cstring>

#include "core/checksum.h"
#include "core/endian.h"

namespace bn::core {

namespace {

constexpr std::size_t kChecksumSize = 4;

// Receives the encoding a byte at a time and keeps only the part that falls
// in a window: bytes before `offset` are counted and dropped, and so are
// bytes after the window is full. Everything is checksummed either way, so
// the checksum at the end is right whichever window is being produced.
class WindowWriter {
 public:
  WindowWriter(std::size_t offset, std::span<std::uint8_t> out) noexcept : skip_(offset), out_(out) {}

  void Put(std::uint8_t byte) noexcept {
    crc_ = Crc32({&byte, 1}, crc_);
    PutUnchecked(byte);
  }

  void Put(std::span<const std::uint8_t> bytes) noexcept {
    for (const std::uint8_t byte : bytes) {
      Put(byte);
    }
  }

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
    for (const char c : text) {
      Put(static_cast<std::uint8_t>(c));
    }
  }

  // Ends the encoding with the checksum of what came before.
  void PutChecksum() noexcept {
    std::array<std::uint8_t, kChecksumSize> bytes{};
    StoreLe32(bytes, crc_);
    for (const std::uint8_t byte : bytes) {
      PutUnchecked(byte);
    }
  }

  // Bytes kept in the window.
  std::size_t Written() const noexcept { return written_; }
  // Bytes of encoding produced, kept or not.
  std::size_t Total() const noexcept { return total_; }

 private:
  void PutUnchecked(std::uint8_t byte) noexcept {
    ++total_;
    if (skip_ > 0) {
      --skip_;
    } else if (written_ < out_.size()) {
      out_[written_++] = byte;
    }
  }

  std::size_t skip_;
  std::span<std::uint8_t> out_;
  std::size_t written_ = 0;
  std::size_t total_ = 0;
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
  WindowWriter writer(0, {});
  Encode(report, writer);
  return writer.Total();
}

std::size_t EncodeCrashReport(const CrashReport& report, std::size_t offset, std::span<std::uint8_t> out) noexcept {
  WindowWriter writer(offset, out);
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
