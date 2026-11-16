#pragma once

#include <memory>

#include "core/clock.h"
#include "core/input.h"
#include "desktop/sim_display.h"

// A window on the desktop: the simulated panel as it would look at each
// moment, in a drawing of the device with its buttons.

namespace bn::hw {

// Only a build configured with BN_DESKTOP_WINDOW has one, made with SDL.
//
// The buttons are drawn where they are on an X4 (DeviceLayout), and are
// pressed by clicking them, or by keys:
//
//   Left, Right, Up, Down            the buttons of those names
//   Page Up, Page Down               Up and Down, the two on the side
//   Return                           Confirm
//   Escape, Backspace                Back
//   P                                Power
//   R                                turns the device between the way a
//                                    book is read and the panel's own way
//
// A button is drawn lit while the application has been told it is down,
// however it was pressed, BN_INPUT's script included. A refresh is shown as
// it goes (RefreshStage), and named in the window's title while it lasts.
class PanelWindow {
 public:
  // The keys, as above, in a line or two for the person at the keyboard.
  static const char* KeysHelp() noexcept;

  // Opens a window onto `display`, with buttons from the keyboard and from
  // `script` (the desktop's BN_INPUT) alongside them. Null, with why on the
  // standard error stream, if no window can be opened, or this build has
  // none.
  static std::unique_ptr<PanelWindow> Open(core::Clock& clock, SimDisplay& display, core::Input& script);

  PanelWindow() = default;
  virtual ~PanelWindow() = default;
  PanelWindow(const PanelWindow&) = delete;
  PanelWindow& operator=(const PanelWindow&) = delete;

  // The buttons, for the application.
  virtual core::Input& Buttons() noexcept = 0;

  // Takes what has happened in the window, and shows the panel anew if it
  // looks different. Call it every turn of the application's loop, from the
  // thread that opened the window. False once the window has been closed.
  virtual bool Pump() = 0;
};

}  // namespace bn::hw
