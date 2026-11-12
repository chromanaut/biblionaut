#include "crash_test.h"

#include <esp_rom_sys.h>
#include <esp_system.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Only compiled into a crash-test build, which defines BN_CRASH_TEST as the
// number of the chosen kind of crash and BN_CRASH_TEST_NAME as its name.

namespace bn::firmware {

namespace {

// In the order the build's list of kinds is in.
enum class CrashTest : std::uint8_t { kFault = 0, kAbort = 1, kOutOfMemory = 2, kStackOverflow = 3 };

constexpr std::uint64_t kDelayUs = 10'000'000;

// Volatile, so the compiler cannot see through any of this and remove it.
volatile std::uintptr_t testAddress = 0x10;
volatile std::size_t testSize = 0x7000'0000;
volatile bool keepRecursing = true;

// NOLINTNEXTLINE(misc-no-recursion): recursing until the stack runs out is what it is for.
std::uint32_t RecurseForever(std::uint32_t depth) noexcept {
  volatile std::uint8_t frame[64];
  frame[0] = static_cast<std::uint8_t>(depth);
  if (keepRecursing) {
    return RecurseForever(depth + 1) + frame[0];
  }
  return frame[0];
}

void CrashOnPurpose() noexcept {
  switch (static_cast<CrashTest>(BN_CRASH_TEST)) {
    case CrashTest::kFault: {
      // Read through a pointer that points nowhere.
      // NOLINTNEXTLINE(performance-no-int-to-ptr): an address nothing lives at is the point.
      const volatile std::uint32_t* nowhere = reinterpret_cast<const volatile std::uint32_t*>(testAddress);
      testAddress = *nowhere;
      break;
    }
    case CrashTest::kAbort:
      std::abort();
    case CrashTest::kOutOfMemory: {
      // Plain `new` cannot report failure, so it ends in the firmware's
      // heap-failure handler.
      const char* block = new char[testSize];
      testAddress = reinterpret_cast<std::uintptr_t>(block);
      break;
    }
    case CrashTest::kStackOverflow:
      testAddress = RecurseForever(0);
      break;
  }
}

// True if this start is the restart after a crash. Asked of the chip, not of
// the saved crash report, so the test cannot loop even if saving the report
// is the thing that is broken.
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

// Whether this run will crash; decided once, at start.
bool armed = false;

}  // namespace

void AnnounceCrashTest() noexcept {
  armed = !StartedByACrash();
  esp_rom_printf(armed ? "Crash test build: this run will crash (%s) in ten seconds.\n"
                       : "Crash test build (%s): this run follows a crash, so it will not crash again.\n",
                 BN_CRASH_TEST_NAME);
}

void RunCrashTest(const reader::App& app) noexcept {
  if (armed && app.UptimeUs() >= kDelayUs) {
    CrashOnPurpose();
  }
}

}  // namespace bn::firmware
