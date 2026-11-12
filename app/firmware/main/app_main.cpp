#include <esp_rom_sys.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "core/crash.h"
#include "core/heap.h"
#include "crash_test.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

// How often the application's periodic work runs, which is also how long a
// button press can wait before it is acted on. (The buttons themselves are
// read, and presses timed, by the hardware layer on its own schedule.)
constexpr TickType_t kTickPeriod = pdMS_TO_TICKS(10);

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

}  // namespace

extern "C" void app_main() {
  bn::core::SetHeapFailureHandler(OnHeapFailure);
  esp_rom_printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());
  AnnounceSavedCrash();
  bn::firmware::AnnounceCrashTest();

  static bn::reader::App app(bn::reader::DefaultServices());
#ifdef BN_PLANE_CHECK
  esp_rom_printf("Plane check build: the panel's planes are read back whenever it comes to rest.\n");
  app.CheckPanelPlanes();
#endif
  app.Start();

  while (true) {
    vTaskDelay(kTickPeriod);
    app.Tick();
    bn::firmware::RunCrashTest(app);
  }
}
