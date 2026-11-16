#include "desktop/sim_power.h"
#include "hw/platform.h"

namespace bn::hw {

SimPower& SystemSimPower() noexcept {
  static SimPower instance;
  return instance;
}

core::Power& SystemPower() noexcept { return SystemSimPower(); }

}  // namespace bn::hw
