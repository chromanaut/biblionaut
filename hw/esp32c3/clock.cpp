#include <esp_timer.h>

#include "hw/platform.h"

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

}  // namespace bn::hw
