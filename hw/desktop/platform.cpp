#include "hw/platform.h"

#include <chrono>

namespace bn::hw {

namespace {

class DesktopClock final : public core::Clock {
 public:
  std::uint64_t NowUs() noexcept override {
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace

core::Clock& SystemClock() noexcept {
  static DesktopClock instance;
  return instance;
}

const char* PortName() noexcept { return "desktop"; }

}  // namespace bn::hw
