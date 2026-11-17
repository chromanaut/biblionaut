#pragma once

// Putting the device to sleep.
//
// Asleep, a device draws as little as it can, and its panel keeps the last
// picture it was given. Its Power button wakes it, and waking is a new
// start: nothing is kept of what was in memory but what a crash store keeps
// (core/crash.h), and the start is told as StartCause::kDeepSleep.
//
// What it takes for a device to sleep, which of its pins can wake it, and
// what has to be switched off first, are its own: a port that has not
// worked them out says that its device cannot sleep, and it stays awake.

namespace bn::core {

// Implemented by each hardware port. Not for use from two tasks at once.
class Power {
 public:
  // Whether the device can be put to sleep, to be woken by its Power button.
  virtual bool CanSleep() noexcept = 0;

  // Puts the device to sleep. The caller makes it ready first: the panel at
  // rest, its voltages off and its controller asleep (Display::Sleep()), and
  // whatever is to leave the device sent. A device that sleeps does not come
  // back from this. Where it does come back (on the desktop), the device is
  // asleep from then on and its run is over: the caller is to do nothing
  // more. Does nothing if CanSleep() says no.
  virtual void Sleep() noexcept = 0;

 protected:
  ~Power() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
