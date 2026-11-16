#include "core/start.h"

namespace bn::core {

const char* StartCauseName(StartCause cause) noexcept {
  switch (cause) {
    case StartCause::kUnknown:
      return "unknown";
    case StartCause::kPowerOn:
      return "power on";
    case StartCause::kResetPin:
      return "the reset pin";
    case StartCause::kHost:
      return "a host on USB";
    case StartCause::kSoftware:
      return "the firmware";
    case StartCause::kCrash:
      return "a crash";
    case StartCause::kWatchdog:
      return "a watchdog";
    case StartCause::kBrownout:
      return "a brownout";
    case StartCause::kDeepSleep:
      return "waking from deep sleep";
  }
  return "unknown";
}

}  // namespace bn::core
