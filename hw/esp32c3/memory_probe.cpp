#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hw/platform.h"

namespace bn::hw {

namespace {

class EspMemoryProbe final : public core::MemoryProbe {
 public:
  // The heap the C and C++ allocators draw from.
  std::optional<core::HeapStats> Heap() noexcept override {
    return core::HeapStats{
        .freeBytes = static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
        .largestFreeBlock = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
        .lowestFreeBytes = static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)),
    };
  }

  // FreeRTOS records the deepest each task's stack has ever reached; on this
  // port the figure is in bytes.
  std::optional<std::uint32_t> StackHeadroom() noexcept override {
    return static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  }
};

}  // namespace

core::MemoryProbe& SystemMemoryProbe() noexcept {
  static EspMemoryProbe instance;
  return instance;
}

}  // namespace bn::hw
