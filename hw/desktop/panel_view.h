#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/display.h"
#include "desktop/sim_display.h"

// What a window on the desktop shows of the simulated panel.

namespace bn::hw {

// Which way up the panel is shown.
enum class ViewTurn : std::uint8_t {
  // As a book is read on an X4, which is the panel turned a quarter-turn
  // clockwise from its own orientation: its rows run down the window, the
  // first of them on the right.
  kAsRead = 0,
  // In the panel's own orientation, as the pictures BN_DISPLAY_OUT saves are.
  kAsPanel = 1,
};

// How far a refresh has got, as the window shows it. A real panel drives
// its pixels through a sequence of its own, which is not copied here. This
// is only enough of one to show that a full or a half refresh flashes and
// takes its time, and when the new picture is there.
enum class RefreshStage : std::uint8_t {
  kBefore = 0,    // The picture from before: through a fast refresh, until it is over.
  kInverted = 1,  // The picture from before, black for white: the first third of a full refresh.
  kBlack = 2,     // All black: the second third of a full refresh, the first half of a half refresh.
  kWhite = 3,     // All white: the rest of either.
  kAfter = 4,     // What the panel shows once the refresh is over.
};

// The stage `refresh` is at, at `nowUs`.
RefreshStage StageAt(const SimDisplay::Refresh& refresh, std::uint64_t nowUs) noexcept;

// The picture a window shows: what the panel looks like at a moment,
// turned, a 32-bit pixel each.
class PanelView {
 public:
  // A white pixel, and a black one: 0xAARRGGBB, opaque.
  static constexpr std::uint32_t kWhite = 0xFFFFFFFF;
  static constexpr std::uint32_t kBlack = 0xFF000000;

  explicit PanelView(core::DisplaySize panel, ViewTurn turn = ViewTurn::kAsRead);

  // The picture's width and height.
  core::DisplaySize Size() const noexcept;
  ViewTurn Turn() const noexcept { return turn_; }
  // Takes effect at the next Update().
  void SetTurn(ViewTurn turn) noexcept { turn_ = turn; }

  // Brings the picture up to date with what `display` looks like at `nowUs`.
  // True if the picture changed, or the way it is turned did.
  bool Update(const SimDisplay& display, std::uint64_t nowUs);

  // Row after row of Size(), top to bottom; kWhite and kBlack.
  std::span<const std::uint32_t> Pixels() const noexcept { return pixels_; }
  // The stage of the last refresh that the picture shows.
  RefreshStage Stage() const noexcept { return stage_; }

 private:
  void Paint(std::span<const std::uint8_t> rows, bool inverted);
  void Fill(std::uint32_t pixel);

  core::DisplaySize panel_;
  ViewTurn turn_;
  std::vector<std::uint32_t> pixels_;
  // What the picture was last made from, so that Update() can tell whether
  // anything has changed since. Nothing has been made before the first.
  bool painted_ = false;
  int refreshCount_ = 0;
  RefreshStage stage_ = RefreshStage::kAfter;
  ViewTurn paintedTurn_ = ViewTurn::kAsRead;
};

}  // namespace bn::hw
