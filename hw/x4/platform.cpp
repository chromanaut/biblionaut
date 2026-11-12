#include "hw/platform.h"

#include <esp_timer.h>

namespace bn::hw {

namespace {

class EspClock final : public core::Clock {
 public:
  std::uint64_t NowUs() noexcept override { return static_cast<std::uint64_t>(esp_timer_get_time()); }
};

}  // namespace

core::Clock& SystemClock() noexcept {
  static EspClock instance;
  return instance;
}

const char* PortName() noexcept { return "x4"; }

}  // namespace bn::hw
