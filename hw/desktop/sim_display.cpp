#include "desktop/sim_display.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/checksum.h"
#include "core/trace.h"
#include "hw/trace_ids.h"

namespace bn::hw {

namespace {

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24U));
  out.push_back(static_cast<std::uint8_t>(value >> 16U));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

// One PNG chunk: length, four-letter type, data, and a checksum of the type
// and data.
void PutChunk(std::vector<std::uint8_t>& out, const char* type, std::span<const std::uint8_t> data) {
  PutBe32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t checksummedFrom = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBe32(out, core::Crc32(std::span(out).subspan(checksummedFrom)));
}

std::uint32_t Adler32(std::span<const std::uint8_t> data) {
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : data) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16U) | a;
}

}  // namespace

std::vector<std::uint8_t> EncodePng(core::DisplaySize size, std::span<const std::uint8_t> rows) {
  const std::size_t rowBytes = size.RowBytes();

  // The image data PNG wants: each row with a "no filter" byte in front.
  std::vector<std::uint8_t> raw;
  raw.reserve(size.height * (rowBytes + 1));
  for (std::size_t y = 0; y < size.height; ++y) {
    raw.push_back(0);
    const std::span<const std::uint8_t> row = rows.subspan(y * rowBytes, rowBytes);
    raw.insert(raw.end(), row.begin(), row.end());
  }

  // That, wrapped as a zlib stream of stored (uncompressed) blocks.
  std::vector<std::uint8_t> zlib = {0x78, 0x01};
  constexpr std::size_t kMaxStored = 65535;
  std::size_t at = 0;
  do {
    const std::size_t length = std::min(raw.size() - at, kMaxStored);
    const bool last = at + length == raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(static_cast<std::uint8_t>(length));
    zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
    zlib.push_back(static_cast<std::uint8_t>(~length));
    zlib.push_back(static_cast<std::uint8_t>(~length >> 8U));
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                raw.begin() + static_cast<std::ptrdiff_t>(at + length));
    at += length;
  } while (at < raw.size());
  PutBe32(zlib, Adler32(raw));

  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> header;
  PutBe32(header, size.width);
  PutBe32(header, size.height);
  header.push_back(1);  // Bits per pixel.
  header.push_back(0);  // Greyscale: 0 is black, 1 is white, as in a plane.
  header.push_back(0);  // Compression method, filter method, no interlace.
  header.push_back(0);
  header.push_back(0);
  PutChunk(png, "IHDR", header);
  PutChunk(png, "IDAT", zlib);
  PutChunk(png, "IEND", {});
  return png;
}

SimDisplay::SimDisplay(core::Clock& clock, std::string outputPrefix, std::uint32_t speed)
    : clock_(clock),
      outputPrefix_(std::move(outputPrefix)),
      speed_(speed < 1 ? 1 : speed),
      newPlane_(kSize.RowBytes() * kSize.height, 0xFF),
      oldPlane_(newPlane_),
      shown_(newPlane_) {}

std::span<const std::uint8_t> SimDisplay::Plane(core::DisplayPlane plane) const noexcept {
  return plane == core::DisplayPlane::kNew ? newPlane_ : oldPlane_;
}

bool SimDisplay::Start() noexcept {
  // Resetting a panel stops a refresh in progress. (A real one would be
  // left part-way between the two pictures; here it counts as finished.)
  busy_ = false;
  // Setting a panel up clears both planes; what it shows does not change.
  std::memset(newPlane_.data(), 0xFF, newPlane_.size());
  std::memset(oldPlane_.data(), 0xFF, oldPlane_.size());
  started_ = true;
  powered_ = false;
  return true;
}

bool SimDisplay::WriteRows(core::DisplayPlane plane, std::uint16_t firstRow,
                           std::span<const std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = kSize.RowBytes();
  if (!started_ || Busy() || rows.empty() || rows.size() % rowBytes != 0 ||
      firstRow + (rows.size() / rowBytes) > kSize.height) {
    return false;
  }
  BN_TRACE_SCOPE_ARG(kSpanPanelWrite, static_cast<std::uint32_t>(rows.size()));
  std::vector<std::uint8_t>& target = plane == core::DisplayPlane::kNew ? newPlane_ : oldPlane_;
  std::memcpy(target.data() + (firstRow * rowBytes), rows.data(), rows.size());
  return true;
}

bool SimDisplay::StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept {
  if (!started_ || Busy()) {
    return false;
  }
  std::uint64_t durationUs = kFullRefreshUs;
  core::SpanId span = kSpanRefreshFull;
  if (mode == core::RefreshMode::kFast) {
    // Only the pixels that differ between the planes are driven; the rest
    // stay as they are on the panel, right or wrong.
    for (std::size_t i = 0; i < shown_.size(); ++i) {
      const unsigned changed = static_cast<unsigned>(newPlane_[i]) ^ oldPlane_[i];
      shown_[i] = static_cast<std::uint8_t>((shown_[i] & ~changed) | (newPlane_[i] & changed));
    }
    durationUs = kFastRefreshUs;
    span = kSpanRefreshFast;
  } else {
    shown_ = newPlane_;
    if (mode == core::RefreshMode::kHalf) {
      durationUs = kHalfRefreshUs;
      span = kSpanRefreshHalf;
    }
  }
  const bool powerOff = after == core::AfterRefresh::kPowerOff;
  powered_ = !powerOff;
  BeginBusy(durationUs + (powerOff ? kPowerOffUs : 0), span, powerOff ? 1 : 0);
  ++refreshCount_;
  SavePicture();
  return true;
}

void SimDisplay::BeginBusy(std::uint64_t durationUs, core::SpanId span, std::uint32_t argument) noexcept {
  busySpan_ = span;
  busySpanArg_ = argument;
  busySinceTrace_ = BN_TRACE_NOW();
  busyUntilUs_ = clock_.NowUs() + (durationUs / speed_);
  busy_ = true;
}

bool SimDisplay::Busy() noexcept {
  if (busy_ && clock_.NowUs() >= busyUntilUs_) {
    busy_ = false;
    BN_TRACE_TOP_SPAN_SINCE(busySpan_, busySinceTrace_, busySpanArg_);
  }
  return busy_;
}

bool SimDisplay::PowerOff() noexcept {
  if (!started_ || Busy()) {
    return false;
  }
  if (powered_) {
    powered_ = false;
    BeginBusy(kPowerOffUs, kSpanPanelPowerOff, 0);
  }
  return true;
}

bool SimDisplay::Sleep() noexcept {
  if (Busy()) {
    return false;
  }
  started_ = false;
  powered_ = false;
  return true;
}

void SimDisplay::SavePicture() {
  if (outputPrefix_.empty()) {
    return;
  }
  std::array<char, 16> suffix{};
  std::snprintf(suffix.data(), suffix.size(), "-%03d.png", refreshCount_);
  const std::string path = outputPrefix_ + suffix.data();
  const std::vector<std::uint8_t> png = EncodePng(kSize, shown_);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr || std::fwrite(png.data(), 1, png.size(), file) != png.size()) {
    std::fprintf(stderr, "warning: cannot write the panel picture to '%s'\n", path.c_str());
  }
  if (file != nullptr) {
    std::fclose(file);
  }
}

}  // namespace bn::hw
