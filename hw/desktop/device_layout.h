#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "core/display.h"
#include "core/input.h"
#include "desktop/panel_view.h"

// The X4 as a window on the desktop draws it: the panel, and the buttons
// around it on the edges they are on.

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
  core::Button button;
  const char* label;
  Rect rect;
};

// Where things are. Held the way a book is read, an X4 has four buttons in a
// row under the panel (Back, Confirm, Left, Right, from the left) and three
// down its right edge (Power, then Up, then Down). Turned to the panel's own
// way, a quarter-turn anticlockwise, the row is down the right edge, Back at
// the bottom, and the three are along the top, Power on the left.
//
// The layout is in units of the panel's pixels, which is what the window
// draws everything in. A button has room for its label in 8 by 8 letters
// whichever way the layout is turned.
class DeviceLayout {
 public:
  // `panel`: the panel's size in its own orientation.
  DeviceLayout(core::DisplaySize panel, ViewTurn turn) noexcept;

  // The size of all of it, the device's edges included.
  core::DisplaySize Size() const noexcept { return size_; }
  // Where the panel's picture goes, which is PanelView::Size() for the same
  // turn.
  Rect Panel() const noexcept { return panel_; }
  // Every button, once each.
  std::span<const ButtonPlace> Buttons() const noexcept { return buttons_; }
  // The button at a point, if there is one there.
  std::optional<core::Button> ButtonAt(float x, float y) const noexcept;

 private:
  core::DisplaySize size_{};
  Rect panel_;
  std::array<ButtonPlace, core::kButtonCount> buttons_{};
};

}  // namespace bn::hw
