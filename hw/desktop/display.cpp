#include <cstdio>
#include <cstdlib>
#include <string>

#include "desktop/device_profile.h"
#include "desktop/sim_display.h"
#include "hw/platform.h"

namespace bn::hw {

namespace {

// The device BN_DEVICE names, or the X4.
const DeviceProfile& ChosenDevice() noexcept {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
  const char* name = std::getenv("BN_DEVICE");
  if (name == nullptr) {
    return X4();
  }
  if (const DeviceProfile* device = FindDevice(name)) {
    return *device;
  }
  std::fprintf(stderr, "warning: BN_DEVICE '%s' is not a device the desktop knows, so it simulates the %s\n", name,
               X4().name);
  return X4();
}

}  // namespace

SimDisplay& SystemSimDisplay() noexcept {
  // NOLINTBEGIN(concurrency-mt-unsafe): read once, before any other thread exists.
  const char* prefix = std::getenv("BN_DISPLAY_OUT");
  const char* speedText = std::getenv("BN_DISPLAY_SPEED");
  // NOLINTEND(concurrency-mt-unsafe)
  // How many times faster than a real panel the simulated one works.
  const long speed = speedText != nullptr ? std::strtol(speedText, nullptr, 10) : 1;
  static SimDisplay instance(SystemClock(), ChosenDevice(), prefix != nullptr ? prefix : "",
                             speed >= 1 ? static_cast<std::uint32_t>(speed) : 1);
  return instance;
}

core::Display& SystemDisplay() noexcept { return SystemSimDisplay(); }

}  // namespace bn::hw
