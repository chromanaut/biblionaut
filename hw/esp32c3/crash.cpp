#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_memory_utils.h>
#include <esp_private/panic_internal.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <riscv/rvruntime-frames.h>
#include <soc/soc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/checksum.h"
#include "core/trace.h"
#include "hw/platform.h"

// Crash reports on the ESP32-C3.
//
// Every fatal error on this chip (a processor fault, abort(), a failed
// assertion, a watchdog, a stack overflow) ends in ESP-IDF's panic handler,
// which prints what it knows on the console and restarts the chip. The
// firmware is linked so that calls to that handler come here first (the
// linker's --wrap option, set where this component is registered). This code
// copies what a report needs into RTC memory, which a restart leaves alone,
// and then lets ESP-IDF's handler carry on as usual.

// The handler being wrapped and its replacement. The linker's --wrap option
// fixes these names, underscores and all.
// NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" void __real_esp_panic_handler(panic_info_t* info);
extern "C" void __wrap_esp_panic_handler(panic_info_t* info);
// NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)

namespace bn::hw {

namespace {

// Marks a complete report. The number changes whenever core::CrashReport's
// layout does, so a report left by other firmware is never misread.
constexpr std::uint32_t kMagic = 0x31434E42;  // "BNC1"

// Registers of ESP-IDF's exception frame that go into a report: everything
// up to and including mtval, in the frame's own order.
constexpr std::size_t kFrameRegisters = 36;
static_assert(kFrameRegisters <= core::kCrashMaxRegisters);
static_assert(offsetof(RvExcFrame, mepc) == 0);
static_assert(offsetof(RvExcFrame, mtval) == (kFrameRegisters - 1) * sizeof(std::uint32_t));

struct SavedCrash {
  std::uint32_t magic;
  std::uint32_t size;      // sizeof(core::CrashReport), as a second check on the layout.
  std::uint32_t checksum;  // Of the report's bytes.
  core::CrashReport report;
};
// Nothing may run to initialise it: that would wipe what the last run left.
static_assert(std::is_trivially_default_constructible_v<SavedCrash>);

// RTC memory keeps its contents through a restart and through deep sleep,
// and loses them only with power. After power-on it holds noise, which the
// magic number and checksum reject.
RTC_NOINIT_ATTR SavedCrash saved;

// The firmware's name for itself, from SetVersion().
const char* firmwareVersion = nullptr;

std::uint32_t ReportChecksum() noexcept {
  return core::Crc32({reinterpret_cast<const std::uint8_t*>(&saved.report), sizeof(saved.report)});
}

// Fills in the report. Runs inside the panic handler: interrupts are off,
// the heap and any lock may be in a broken state, and whatever it calls must
// not need either.
void SaveCrash(const panic_info_t& info) noexcept {
  saved.magic = 0;  // Not a report again until it is complete.
  core::CrashReport& report = saved.report;
  std::memset(&report, 0, sizeof(report));

  report.arch = core::CrashArch::kEsp32C3;
  report.timeUs = static_cast<std::uint64_t>(esp_timer_get_time());

  // abort() and failed assertions arrive as a deliberate fault with the real
  // cause in a side variable; ESP-IDF's handler makes the same substitution.
  const char* reason = g_panic_abort ? g_panic_abort_details : info.reason;
  if (reason == nullptr) {
    reason = g_panic_abort ? "abort() was called" : "unknown";
  }
  core::SetCrashText(report.reason, reason);

  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    if (const char* name = pcTaskGetName(nullptr)) {
      core::SetCrashText(report.task, name);
    }
  }
  if (firmwareVersion != nullptr) {
    core::SetCrashText(report.version, firmwareVersion);
  }
  core::SetCrashText(report.buildId, esp_app_get_elf_sha256_str());

  if (const auto* frame = static_cast<const RvExcFrame*>(info.frame)) {
    std::memcpy(report.registers.data(), frame, kFrameRegisters * sizeof(std::uint32_t));
    report.registerCount = kFrameRegisters;

    // The stack, upwards from the stack pointer: return addresses and local
    // variables of the functions that were running. The stack pointer itself
    // may be what went wrong, so it is only followed if it points into RAM,
    // and never past the end of RAM.
    const auto sp = static_cast<std::uint32_t>(frame->sp);
    // The stack pointer is a number in the saved registers; reading the
    // stack means treating it as an address.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const void* const stack = reinterpret_cast<const void*>(sp);
    if (esp_ptr_in_dram(stack) && (sp % 4) == 0) {
      const std::uint32_t available = SOC_DRAM_HIGH - sp;
      const std::uint32_t size = available < core::kCrashMaxStackBytes ? available : core::kCrashMaxStackBytes;
      std::memcpy(report.stack.data(), stack, size);
      report.stackAddress = sp;
      report.stackSize = static_cast<std::uint16_t>(size);
    }
  }

  if (const core::Tracer* tracer = core::ActiveTracer()) {
    report.traceCount = static_cast<std::uint8_t>(tracer->CopyLatestUnlocked(report.trace));
  }

  saved.size = sizeof(core::CrashReport);
  saved.checksum = ReportChecksum();
  saved.magic = kMagic;
}

class RtcCrashStore final : public core::CrashStore {
 public:
  RtcCrashStore() noexcept {
    if (saved.magic != kMagic || saved.size != sizeof(core::CrashReport) || saved.checksum != ReportChecksum()) {
      return;
    }
    // One more start since the crash.
    saved.report.bootsSince = saved.report.bootsSince + 1;
    saved.checksum = ReportChecksum();
    hasReport_ = true;
  }

  const core::CrashReport* Saved() noexcept override { return hasReport_ ? &saved.report : nullptr; }

  void SetVersion(const char* version) noexcept override { firmwareVersion = version; }

 private:
  bool hasReport_ = false;
};

}  // namespace

core::CrashStore& SystemCrashStore() noexcept {
  static RtcCrashStore instance;
  return instance;
}

}  // namespace bn::hw

extern "C" void __wrap_esp_panic_handler(panic_info_t* info) {
  // Only the first entry saves a report. If saving faults, or something
  // later in the handler does, the handler is entered again; that must
  // neither loop nor replace the report of the original crash.
  static bool entered = false;
  if (!entered && info != nullptr) {
    entered = true;
    bn::hw::SaveCrash(*info);
  }
  __real_esp_panic_handler(info);
}
