#pragma once

#include <cstddef>
#include <cstdint>

// Why the device started: what ended the run before this one, as far as
// its hardware says. It is told at the head of the diagnostic stream, so
// that a run which was cut short by something other than its owner (the
// supply falling too low, a watchdog) is not taken for a fresh start.

namespace bn::core {

// The numbers are sent in the stream: add at the end, and never renumber.
enum class StartCause : std::uint8_t {
  kUnknown = 0,  // The hardware does not say, or says something there is no name for here.
  kPowerOn,      // Power came on.
  kResetPin,     // The reset pin was pulled: on a device with a reset button, the button.
  kHost,         // A computer on the USB port asked for it, as it does when firmware is flashed.
  kSoftware,     // The firmware restarted itself.
  kCrash,        // A fault or an abort ended the run before. core/crash.h has what is kept of it.
  kWatchdog,     // A watchdog found the run before not turning, or stuck.
  kBrownout,     // The supply fell too low to go on.
  kDeepSleep,    // It woke from deep sleep.
};
inline constexpr std::size_t kStartCauses = 9;

// "unknown", "power on", "the reset pin", ... ; "unknown" for a number
// that is none of them.
const char* StartCauseName(StartCause cause) noexcept;

}  // namespace bn::core
