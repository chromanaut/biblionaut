#include <cstdlib>
#include <string>

#include "desktop/sim_display.h"
#include "hw/platform.h"

namespace bn::hw {

core::Display& SystemDisplay() noexcept {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
  const char* prefix = std::getenv("BN_DISPLAY_OUT");
  static SimDisplay instance(SystemClock(), prefix != nullptr ? prefix : "");
  return instance;
}

}  // namespace bn::hw
