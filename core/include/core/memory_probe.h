#pragma once

#include <cstdint>
#include <optional>

namespace bn::core {

// The state of the general heap.
struct HeapStats {
  std::uint32_t freeBytes;         // Free right now, in total.
  std::uint32_t largestFreeBlock;  // The biggest single allocation that would succeed right now.
  std::uint32_t lowestFreeBytes;   // The least that has ever been free since boot.
};

// How much memory the system has left. Implemented by each hardware port; a
// port that cannot tell (the desktop) answers with nothing.
class MemoryProbe {
 public:
  virtual std::optional<HeapStats> Heap() noexcept = 0;

  // Bytes of the calling task's stack that have never been used: how close
  // the task has come to overflowing it.
  virtual std::optional<std::uint32_t> StackHeadroom() noexcept = 0;

 protected:
  ~MemoryProbe() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
