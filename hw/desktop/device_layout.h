#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/display.h"
#include "core/input.h"
#include "desktop/device_profile.h"
#include "desktop/panel_view.h"

// A device as a window on the desktop draws it: the panel, and the buttons
// around it where its DeviceProfile says they are.

namespace bn::hw {

// A rectangle in the window, in its units: x to the right, y down.
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool Contains(float px, float py) const noexcept {
    return px >= static_cast<float>(x) && px < static_cast<float>(x + width) && py >= static_cast<float>(y) &&
           py < static_cast<float>(y + height);
  }
};

struct ButtonPlace {
  core::Button button = core::Button::kBack;
  const char* label = "";
  Rect rect;
};

// Where things are. The device is laid out held the way a book is read on
// it: the panel turned by the profile's ReadingTurn, with a strip along
// each edge that has buttons, and the buttons in it where the profile puts
// them, a button at several spots at each of them. Turned to the panel's own
// way, all of it is turned back by as much.
// An X4, held for reading, has four buttons in a row under the panel and
// three down its right edge; turned to the panel's own way, a quarter-turn
// anticlockwise, the row is down the right edge, Back at the bottom, and
// the three are along the top, Power on the left.
//
// The layout is in units of the panel's pixels, which is what the window
// draws everything in. A strip is deep enough for a label of seven 8 by 8
// letters across it, so a button has room for its label whichever way the
// layout is turned, if it is long enough for its label along the edge.
class DeviceLayout {
 public:
  // `device` must stay alive as long as this does.
  DeviceLayout(const DeviceProfile& device, ViewTurn turn) noexcept;

  // The size of all of it, the device's edges included.
  core::DisplaySize Size() const noexcept { return size_; }
  // Where the panel's picture goes, which is PanelView::Size() for the same
  // turn.
  Rect Panel() const noexcept { return panel_; }
  // Every button the device has, once each.
  // Every place a button is, in the profile's order. A button at several
  // spots is here once for each.
  std::span<const ButtonPlace> Places() const noexcept { return std::span(places_).first(count_); }
  // Whether the device has `button`.
  bool Has(core::Button button) const noexcept;
  // The button at a point, if there is one there.
  std::optional<core::Button> ButtonAt(float x, float y) const noexcept;

 private:
  core::DisplaySize size_{};
  Rect panel_;
  std::array<ButtonPlace, kMaxButtonSpots> places_{};
  std::size_t count_ = 0;
};

}  // namespace bn::hw
