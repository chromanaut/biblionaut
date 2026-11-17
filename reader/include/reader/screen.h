#pragma once

#include "core/input.h"

namespace bn::reader {

// Something given the panel and the buttons in place of the test pattern,
// such as the drawing viewer (reader/drawing_viewer.h). The application
// reaches it only through this, so that a build which asks for none has
// none of its code in it.
class Screen {
 public:
  virtual ~Screen() = default;

  // Sets the panel up and shows what it shows first. Storage is ready, if
  // there is any, by the time this is called.
  virtual void Start() noexcept = 0;
  // Takes a button event.
  virtual void OnInput(const core::InputEvent& event) noexcept = 0;
  // Moves on when the panel and the clock allow. Call often; it never waits.
  virtual void Tick() noexcept = 0;
  // Puts the panel to sleep, for the device to sleep, once whatever it is
  // doing allows.
  virtual void Sleep() noexcept = 0;
  // Whether the device can sleep now: the panel is asleep, or there is
  // nothing more to be done with it.
  virtual bool Resting() const noexcept = 0;

 protected:
  Screen() = default;
  Screen(const Screen&) = default;
  Screen& operator=(const Screen&) = default;
  Screen(Screen&&) = default;
  Screen& operator=(Screen&&) = default;
};

}  // namespace bn::reader
