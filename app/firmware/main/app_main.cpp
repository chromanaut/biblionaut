#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "core/heap.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

// Console output goes through the ROM printf: it is unbuffered and does not
// touch the C library's buffered stdio. This toolchain's picolibc has an
// off-by-one in its buffered put path (fputc and friends) that can write one
// byte past the stream buffer and corrupt the heap.
void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  esp_rom_printf("heap allocation failed: %u bytes, alignment %u%s\n", static_cast<unsigned>(failure.size),
                 static_cast<unsigned>(failure.alignment), failure.fatal ? " (fatal)" : "");
}

}  // namespace

extern "C" void app_main() {
  bn::core::SetHeapFailureHandler(OnHeapFailure);

  static bn::reader::App app(bn::reader::DefaultServices());
  app.Start();

  esp_rom_printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_rom_printf("uptime %u s\n", static_cast<unsigned>(app.UptimeUs() / 1000000));
  }
}
