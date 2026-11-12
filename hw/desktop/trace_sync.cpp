#include <atomic>
#include <cstdint>
#include <mutex>

#include "hw/platform.h"

namespace bn::hw {

namespace {

class DesktopTraceSync final : public core::TraceSync {
 public:
  void Lock() noexcept override { mutex_.lock(); }
  void Unlock() noexcept override { mutex_.unlock(); }

  // Threads are numbered in the order they first record something.
  std::uint8_t CurrentTask() noexcept override {
    thread_local const std::uint8_t kThreadNumber = NextNumber();
    return kThreadNumber;
  }

 private:
  std::uint8_t NextNumber() noexcept {
    const std::uint32_t next = threadCount_.fetch_add(1, std::memory_order_relaxed);
    return next < core::kTraceTaskOther ? static_cast<std::uint8_t>(next) : core::kTraceTaskOther;
  }

  std::mutex mutex_;
  std::atomic<std::uint32_t> threadCount_{0};
};

}  // namespace

core::TraceSync& SystemTraceSync() noexcept {
  static DesktopTraceSync instance;
  return instance;
}

}  // namespace bn::hw
