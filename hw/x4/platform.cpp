#include "hw/platform.h"

namespace bn::hw {

namespace {

// The X4 stays awake. The chip can sleep, but which of the device's pins
// can wake it, and what of it has to be switched off first for it to draw
// little, have not been found out yet.
class AwakePower final : public core::Power {
 public:
  bool CanSleep() noexcept override { return false; }
  void Sleep() noexcept override {}
};

}  // namespace

core::Power& SystemPower() noexcept {
  static AwakePower instance;
  return instance;
}

const char* PortName() noexcept { return "x4"; }

}  // namespace bn::hw
