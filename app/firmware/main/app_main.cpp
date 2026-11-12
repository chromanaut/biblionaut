#include <esp_rom_sys.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/crash.h"
#include "core/heap.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

// How often the application's periodic work runs.
constexpr TickType_t kTickPeriod = pdMS_TO_TICKS(20);

// Plain console output goes through the ROM printf: it is unbuffered and does
// not touch the C library's buffered stdio. This toolchain's picolibc has an
// off-by-one in its buffered put path (fputc and friends) that can write one
// byte past the stream buffer and corrupt the heap.
//
// It is only used before the diagnostic stream starts and for fatal
// conditions: ROM output bypasses the serial driver, so printing while frames
// are being sent could land in the middle of one.
void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  if (!failure.fatal) {
    esp_rom_printf("heap allocation failed: %u bytes, alignment %u\n", static_cast<unsigned>(failure.size),
                   static_cast<unsigned>(failure.alignment));
    return;
  }
  // Nothing can be returned to the caller, so the firmware stops here. Doing
  // it with this message, instead of leaving it to the abort() that follows,
  // puts the size that could not be had into the crash report.
  static char message[64];
  std::snprintf(message, sizeof(message), "out of memory: %u bytes, alignment %u", static_cast<unsigned>(failure.size),
                static_cast<unsigned>(failure.alignment));
  esp_system_abort(message);
}

// Says on the console that an earlier run crashed. The report itself goes
// out on the diagnostic stream once the application starts.
void AnnounceSavedCrash() noexcept {
  const bn::core::CrashReport* report = bn::hw::SystemCrashStore().Saved();
  if (report == nullptr) {
    return;
  }
  // No addresses here: they would be read as belonging to this firmware,
  // and the crash may have been in an earlier one.
  esp_rom_printf("Crash report saved %u restart(s) ago: %s, in task %s. bn-trace shows all of it.\n",
                 static_cast<unsigned>(report->bootsSince), report->reason.data(), report->task.data());
}

#ifdef BN_CRASH_TEST

// A firmware built to crash on purpose, for checking crash reports. The kind
// of crash is chosen when configuring the build: BN_CRASH_TEST is its number
// in this list.
enum class CrashTest { kFault = 0, kAbort = 1, kOutOfMemory = 2, kStackOverflow = 3 };

constexpr std::uint64_t kCrashTestDelayUs = 10'000'000;

// Volatile, so the compiler cannot see through any of this and remove it.
volatile std::uintptr_t crashTestAddress = 0x10;
volatile std::size_t crashTestSize = 0x7000'0000;
volatile bool crashTestRecurse = true;

std::uint32_t RecurseForever(std::uint32_t depth) noexcept {
  volatile std::uint8_t frame[64];
  frame[0] = static_cast<std::uint8_t>(depth);
  if (crashTestRecurse) {
    return RecurseForever(depth + 1) + frame[0];
  }
  return frame[0];
}

void CrashOnPurpose() noexcept {
  switch (static_cast<CrashTest>(BN_CRASH_TEST)) {
    case CrashTest::kFault: {
      // Read through a pointer that points nowhere.
      const volatile std::uint32_t* nowhere = reinterpret_cast<const volatile std::uint32_t*>(crashTestAddress);
      crashTestAddress = *nowhere;
      break;
    }
    case CrashTest::kAbort:
      std::abort();
    case CrashTest::kOutOfMemory: {
      // Plain `new` cannot report failure, so it ends in OnHeapFailure().
      const char* block = new char[crashTestSize];
      crashTestAddress = reinterpret_cast<std::uintptr_t>(block);
      break;
    }
    case CrashTest::kStackOverflow:
      crashTestAddress = RecurseForever(0);
      break;
  }
}

// True if this start is the restart after a crash. The test does not crash
// again then, so the report can be read and the firmware cannot loop.
bool StartedByACrash() noexcept {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

#endif  // BN_CRASH_TEST

}  // namespace

extern "C" void app_main() {
  bn::core::SetHeapFailureHandler(OnHeapFailure);
  esp_rom_printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());
  AnnounceSavedCrash();

#ifdef BN_CRASH_TEST
  const bool crashTestArmed = !StartedByACrash();
  esp_rom_printf(crashTestArmed ? "Crash test build: this run will crash (%s) in ten seconds.\n"
                                : "Crash test build (%s): this run follows a crash, so it will not crash again.\n",
                 BN_CRASH_TEST_NAME);
#endif

  static bn::reader::App app(bn::reader::DefaultServices());
  app.Start();

  while (true) {
    vTaskDelay(kTickPeriod);
    app.Tick();
#ifdef BN_CRASH_TEST
    if (crashTestArmed && app.UptimeUs() >= kCrashTestDelayUs) {
      CrashOnPurpose();
    }
#endif
  }
}
