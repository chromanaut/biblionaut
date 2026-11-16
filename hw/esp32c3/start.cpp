#include <esp_system.h>

#include "hw/platform.h"

namespace bn::hw {

// The chip keeps why it last reset, and ESP-IDF adds what its own code
// knows: that the reset was its panic handler's, or a watchdog's.
core::StartCause WhyStarted() noexcept {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return core::StartCause::kPowerOn;
    case ESP_RST_EXT:
      return core::StartCause::kResetPin;
    case ESP_RST_USB:
    case ESP_RST_JTAG:
      return core::StartCause::kHost;
    case ESP_RST_SW:
      return core::StartCause::kSoftware;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
      return core::StartCause::kCrash;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return core::StartCause::kWatchdog;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
      return core::StartCause::kBrownout;
    case ESP_RST_DEEPSLEEP:
      return core::StartCause::kDeepSleep;
    default:
      return core::StartCause::kUnknown;
  }
}

}  // namespace bn::hw
