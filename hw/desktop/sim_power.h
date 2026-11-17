#pragma once

#include "core/power.h"

// The desktop's stand-in for a device that sleeps.

namespace bn::hw {

// The desktop cannot be put to sleep, so this notes that the device is
// asleep, and whoever runs the application's loop does the rest: the
// desktop application ends the run, and begins a new one, as after a deep
// sleep, when Power is pressed (app/desktop/main.cpp).
class SimPower final : public core::Power {
 public:
  bool CanSleep() noexcept override { return true; }
  void Sleep() noexcept override { asleep_ = true; }

  bool Asleep() const noexcept { return asleep_; }
  // Woken, for the new run that follows.
  void Wake() noexcept { asleep_ = false; }

 private:
  bool asleep_ = false;
};

// The desktop's, as what it is: the one SystemPower() hands out.
SimPower& SystemSimPower() noexcept;

}  // namespace bn::hw
