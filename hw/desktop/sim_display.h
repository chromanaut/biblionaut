#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/display.h"
#include "core/trace.h"

// The desktop's stand-in for the e-ink panel.

namespace bn::hw {

// A picture of `size`, rows as core::Display::WriteRows() takes them, as the
// bytes of a PNG file: 1 bit per pixel, not compressed.
std::vector<std::uint8_t> EncodePng(core::DisplaySize size, std::span<const std::uint8_t> rows);

// Behaves like the X4's panel as far as the code driving it can tell: it
// keeps the two planes, refuses work while a refresh is "running" (for about
// as long as a real one takes), and works out what the panel would show
// afterwards. In particular a fast refresh changes only the pixels where the
// planes differ, so code that lets the old-image plane fall out of step
// with the panel gets the same stale pixels here that it would on a device.
class SimDisplay final : public core::Display {
 public:
  // How long each operation keeps the panel busy, as measured on an X4.
  static constexpr std::uint64_t kFullRefreshUs = 3'579'000;
  static constexpr std::uint64_t kHalfRefreshUs = 1'571'000;
  static constexpr std::uint64_t kFastRefreshUs = 418'000;
  static constexpr std::uint64_t kPowerOffUs = 139'000;

  // If `outputPrefix` is not empty, the picture after each refresh is saved
  // as <outputPrefix>-001.png, -002.png, and so on. `speed` divides the
  // times above, for tests that cannot wait as long as a real panel takes.
  SimDisplay(core::Clock& clock, std::string outputPrefix, std::uint32_t speed = 1);

  core::DisplaySize Size() const noexcept override { return kSize; }
  [[nodiscard]] bool Start() noexcept override;
  [[nodiscard]] bool WriteRows(core::DisplayPlane plane, std::uint16_t firstRow,
                               std::span<const std::uint8_t> rows) noexcept override;
  [[nodiscard]] bool StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept override;
  bool Busy() noexcept override;
  [[nodiscard]] bool PowerOff() noexcept override;
  [[nodiscard]] bool Sleep() noexcept override;

  // What the panel shows (once the refresh in progress, if any, is over).
  std::span<const std::uint8_t> Shown() const noexcept { return shown_; }
  std::span<const std::uint8_t> Plane(core::DisplayPlane plane) const noexcept;
  int RefreshCount() const noexcept { return refreshCount_; }
  bool Started() const noexcept { return started_; }
  bool Powered() const noexcept { return powered_; }

 private:
  static constexpr core::DisplaySize kSize{.width = 800, .height = 480};

  void SavePicture();
  void BeginBusy(std::uint64_t durationUs, core::SpanId span, std::uint32_t argument) noexcept;

  core::Clock& clock_;
  std::string outputPrefix_;
  std::uint32_t speed_;
  std::vector<std::uint8_t> newPlane_;
  std::vector<std::uint8_t> oldPlane_;
  std::vector<std::uint8_t> shown_;
  bool started_ = false;
  bool powered_ = false;
  bool busy_ = false;
  std::uint64_t busyUntilUs_ = 0;
  std::uint32_t busySinceTrace_ = 0;  // Trace time the refresh in progress began.
  core::SpanId busySpan_{};
  std::uint32_t busySpanArg_ = 0;
  int refreshCount_ = 0;
};

}  // namespace bn::hw
