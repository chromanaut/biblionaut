#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "hw/platform.h"

namespace bn::hw {

namespace {

// How many tasks get their own number in a trace. Later ones share
// core::kTraceTaskOther.
constexpr std::size_t kMaxNumberedTasks = 12;

class EspTraceSync final : public core::TraceSync {
 public:
  // A critical section, in the form that is also valid inside an interrupt
  // handler, so interrupts can record trace instants.
  void Lock() noexcept override { portENTER_CRITICAL_SAFE(&lock_); }
  void Unlock() noexcept override { portEXIT_CRITICAL_SAFE(&lock_); }

  // Tasks are numbered in the order they first record something.
  std::uint8_t CurrentTask() noexcept override {
    if (xPortInIsrContext() != 0) {
      return core::kTraceTaskInterrupt;
    }
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();

    // Entries are written once and the count only grows, so the table can be
    // searched without the lock; only adding to it needs one.
    const std::size_t known = count_;
    for (std::size_t i = 0; i < known; ++i) {
      if (tasks_[i] == task) {
        return static_cast<std::uint8_t>(i);
      }
    }

    std::uint8_t number = core::kTraceTaskOther;
    portENTER_CRITICAL(&lock_);
    if (count_ < kMaxNumberedTasks) {
      tasks_[count_] = task;
      number = static_cast<std::uint8_t>(count_);
      count_ = count_ + 1;
    }
    portEXIT_CRITICAL(&lock_);
    return number;
  }

 private:
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  std::array<TaskHandle_t, kMaxNumberedTasks> tasks_{};
  volatile std::size_t count_ = 0;
};

}  // namespace

core::TraceSync& SystemTraceSync() noexcept {
  static EspTraceSync instance;
  return instance;
}

}  // namespace bn::hw
