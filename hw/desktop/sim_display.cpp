#include "desktop/sim_display.h"

#include <algorithm>
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

// A PNG of grays, `depth` bits a pixel (0 black), from rows of `rowBytes`
// bytes each, not compressed.
std::vector<std::uint8_t> EncodeGrayPng(std::uint32_t width, std::uint32_t height, std::uint8_t depth,
                                        std::size_t rowBytes, std::span<const std::uint8_t> rows) {
  // The image data PNG wants: each row with a "no filter" byte in front.
  std::vector<std::uint8_t> raw;
  raw.reserve(height * (rowBytes + 1));
  for (std::size_t y = 0; y < height; ++y) {
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
  PutBe32(header, width);
  PutBe32(header, height);
  header.push_back(depth);  // Bits per pixel.
  header.push_back(0);      // Greyscale, 0 black.
  header.push_back(0);      // Compression method, filter method, no interlace.
  header.push_back(0);
  header.push_back(0);
  PutChunk(png, "IHDR", header);
  PutChunk(png, "IDAT", zlib);
  PutChunk(png, "IEND", {});
  return png;
}

}  // namespace

std::vector<std::uint8_t> EncodePng(core::DisplaySize size, std::span<const std::uint8_t> rows) {
  // A plane's rows are already a 1-bit PNG's: 0 is black, 1 is white.
  return EncodeGrayPng(size.width, size.height, 1, size.RowBytes(), rows);
}

std::vector<std::uint8_t> EncodeFourLevelPng(core::DisplaySize size, std::span<const std::uint8_t> levels) {
  const std::size_t rowBytes = ((static_cast<std::size_t>(size.width) * 2) + 7) / 8;
  std::vector<std::uint8_t> rows(rowBytes * size.height, 0);
  for (std::size_t y = 0; y < size.height; ++y) {
    for (std::size_t x = 0; x < size.width; ++x) {
      const unsigned shown = levels[(y * size.width) + x];
      const unsigned level = shown > 3 ? 3U : shown;
      std::uint8_t& byte = rows[(y * rowBytes) + (x / 4)];
      byte = static_cast<std::uint8_t>(byte | (level << (6U - (2U * (x % 4)))));
    }
  }
  return EncodeGrayPng(size.width, size.height, 2, rowBytes, rows);
}

SimDisplay::SimDisplay(core::Clock& clock, const DeviceProfile& device, std::string outputPrefix, std::uint32_t speed)
    : clock_(clock),
      device_(device),
      outputPrefix_(std::move(outputPrefix)),
      speed_(speed < 1 ? 1 : speed),
      memory_{std::vector<std::uint8_t>(device.panel.RowBytes() * device.panel.height, 0xFF),
              std::vector<std::uint8_t>(device.panel.RowBytes() * device.panel.height, 0xFF)},
      shown_(memory_[0]),
      shownBefore_(memory_[0]),
      shownLevels_(static_cast<std::size_t>(device.panel.width) * device.panel.height,
                   static_cast<std::uint8_t>(device.levels - 1)),
      shownLevelsBefore_(shownLevels_) {}

std::vector<std::uint8_t>& SimDisplay::Memory(core::DisplayPlane plane) noexcept {
  return memory_[plane == core::DisplayPlane::kNew ? newIs_ : 1 - newIs_];
}

const std::vector<std::uint8_t>& SimDisplay::Memory(core::DisplayPlane plane) const noexcept {
  return memory_[plane == core::DisplayPlane::kNew ? newIs_ : 1 - newIs_];
}

std::span<const std::uint8_t> SimDisplay::Plane(core::DisplayPlane plane) const noexcept { return Memory(plane); }

bool SimDisplay::Start() noexcept {
  // Resetting a panel stops a refresh in progress. (A real one would be
  // left part-way between the two pictures; here it counts as finished.)
  busy_ = false;
  // Setting a panel up clears both planes and puts them in their first
  // places; what it shows does not change.
  for (std::vector<std::uint8_t>& memory : memory_) {
    std::fill(memory.begin(), memory.end(), std::uint8_t{0xFF});
  }
  newIs_ = 0;
  started_ = true;
  powered_ = false;
  return true;
}

bool SimDisplay::WriteRows(core::DisplayPlane plane, std::uint16_t firstRow,
                           std::span<const std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = device_.panel.RowBytes();
  if (!started_ || Busy() || rows.empty() || rows.size() % rowBytes != 0 ||
      firstRow + (rows.size() / rowBytes) > device_.panel.height) {
    return false;
  }
  BN_TRACE_SCOPE_ARG(kSpanPanelWrite, static_cast<std::uint32_t>(rows.size()));
  std::memcpy(Memory(plane).data() + (firstRow * rowBytes), rows.data(), rows.size());
  return true;
}

bool SimDisplay::WriteWindow(core::DisplayPlane plane, core::DisplayWindow window,
                             std::span<const std::uint8_t> data) noexcept {
  if (!started_ || Busy() || !window.Fits(device_.panel, data.size())) {
    return false;
  }
  BN_TRACE_SCOPE_ARG(kSpanPanelWindow, static_cast<std::uint32_t>(data.size()));
  const std::size_t rowBytes = device_.panel.RowBytes();
  std::vector<std::uint8_t>& memory = Memory(plane);
  for (std::size_t row = 0; row < window.rows; ++row) {
    std::memcpy(memory.data() + ((window.firstRow + row) * rowBytes) + window.firstByte,
                data.data() + (row * window.bytes), window.bytes);
  }
  return true;
}

bool SimDisplay::ReadRows(core::DisplayPlane plane, std::uint16_t firstRow, std::span<std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = device_.panel.RowBytes();
  if (!started_ || Busy() || rows.empty() || rows.size() % rowBytes != 0 ||
      firstRow + (rows.size() / rowBytes) > device_.panel.height) {
    return false;
  }
  BN_TRACE_SCOPE_ARG(kSpanPanelRead, static_cast<std::uint32_t>(rows.size()));
  std::memcpy(rows.data(), Memory(plane).data() + (firstRow * rowBytes), rows.size());
  return true;
}

bool SimDisplay::StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept {
  if (!started_ || Busy()) {
    return false;
  }
  shownBefore_ = shown_;
  shownLevelsBefore_ = shownLevels_;
  const std::vector<std::uint8_t>& newPlane = Memory(core::DisplayPlane::kNew);
  const std::vector<std::uint8_t>& oldPlane = Memory(core::DisplayPlane::kOld);
  std::uint64_t durationUs = device_.times.fullRefreshUs;
  core::SpanId span = kSpanRefreshFull;
  if (mode == core::RefreshMode::kFast) {
    // Where both planes say white (a set bit) the panel is left as it is,
    // right or wrong, a gray among it; everywhere else it goes to what the
    // new one says.
    std::vector<std::uint8_t> driven(shown_.size());
    for (std::size_t i = 0; i < shown_.size(); ++i) {
      const unsigned leftAlone = static_cast<unsigned>(newPlane[i]) & oldPlane[i];
      shown_[i] = static_cast<std::uint8_t>((shown_[i] & leftAlone) | (newPlane[i] & ~leftAlone));
      driven[i] = static_cast<std::uint8_t>(~leftAlone);
    }
    ShowLevels(newPlane, driven);
    // And the planes change places: what was just shown is now the old
    // image. (The moment within the refresh cannot matter: nothing can be
    // written or read until it is over.)
    newIs_ = 1 - newIs_;
    durationUs = device_.times.fastRefreshUs;
    span = kSpanRefreshFast;
  } else {
    shown_ = newPlane;
    ShowLevels(newPlane, std::vector<std::uint8_t>(shown_.size(), 0xFF));
    if (mode == core::RefreshMode::kHalf) {
      durationUs = device_.times.halfRefreshUs;
      span = kSpanRefreshHalf;
    }
  }
  Finish(durationUs, span, after, Refresh{.mode = mode});
  return true;
}

bool SimDisplay::StartGrayRefresh(core::AfterRefresh after) noexcept {
  if (device_.levels != 4 || !started_ || Busy()) {
    return false;
  }
  shownBefore_ = shown_;
  shownLevelsBefore_ = shownLevels_;
  const std::vector<std::uint8_t>& newPlane = Memory(core::DisplayPlane::kNew);
  const std::vector<std::uint8_t>& oldPlane = Memory(core::DisplayPlane::kOld);
  const std::size_t width = device_.panel.width;
  const std::size_t rowBytes = device_.panel.RowBytes();
  for (std::size_t y = 0; y < device_.panel.height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const std::size_t byte = (y * rowBytes) + (x / 8);
      const unsigned bit = 0x80U >> (x % 8);
      if ((oldPlane[byte] & bit) == 0) {
        continue;
      }
      // Towards white: two levels for a light gray, one for a dark.
      std::uint8_t& level = shownLevels_[(y * width) + x];
      const unsigned moved = level + ((newPlane[byte] & bit) != 0 ? 1U : 2U);
      level = static_cast<std::uint8_t>(moved > 3 ? 3U : moved);
    }
  }
  Finish(device_.times.grayRefreshUs, kSpanRefreshGray, after, Refresh{.gray = true});
  return true;
}

void SimDisplay::ShowLevels(std::span<const std::uint8_t> rows, std::span<const std::uint8_t> driven) noexcept {
  const std::size_t width = device_.panel.width;
  const std::size_t rowBytes = device_.panel.RowBytes();
  const auto white = static_cast<std::uint8_t>(device_.levels - 1);
  for (std::size_t y = 0; y < device_.panel.height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const std::size_t byte = (y * rowBytes) + (x / 8);
      const unsigned bit = 0x80U >> (x % 8);
      if ((driven[byte] & bit) != 0) {
        shownLevels_[(y * width) + x] = (rows[byte] & bit) != 0 ? white : std::uint8_t{0};
      }
    }
  }
}

void SimDisplay::Finish(std::uint64_t durationUs, core::SpanId span, core::AfterRefresh after, Refresh refresh) {
  const bool powerOff = after == core::AfterRefresh::kPowerOff;
  if (powerOff) {
    // By the rule measured for a switch-off made by itself. One made as
    // part of a refresh has not been read back on a device.
    newIs_ = 0;
  }
  powered_ = !powerOff;
  const std::uint64_t now = clock_.NowUs();
  refresh.startUs = now;
  refresh.doneUs = now + (durationUs / speed_);
  lastRefresh_ = refresh;
  BeginBusy(durationUs + (powerOff ? device_.times.powerOffUs : 0), span, powerOff ? 1 : 0);
  ++refreshCount_;
  SavePicture();
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
    newIs_ = 0;  // The planes go back to their first places.
    BeginBusy(device_.times.powerOffUs, kSpanPanelPowerOff, 0);
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
  // In black and white while that is all it shows, so a picture of it is
  // the same as before the panel had grays.
  // (A plain loop: a build with sanitizers takes seconds over an algorithm
  // called through a predicate for each of the panel's pixels.)
  const auto white = static_cast<std::uint8_t>(device_.levels - 1);
  bool grays = false;
  for (std::size_t at = 0; at < shownLevels_.size() && !grays; ++at) {
    grays = shownLevels_[at] != 0 && shownLevels_[at] != white;
  }
  const std::vector<std::uint8_t> png =
      grays ? EncodeFourLevelPng(device_.panel, shownLevels_) : EncodePng(device_.panel, shown_);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr || std::fwrite(png.data(), 1, png.size(), file) != png.size()) {
    std::fprintf(stderr, "warning: cannot write the panel picture to '%s'\n", path.c_str());
  }
  if (file != nullptr) {
    std::fclose(file);
  }
}

}  // namespace bn::hw
