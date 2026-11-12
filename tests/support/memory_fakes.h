#pragma once

#include <cstdint>
#include <optional>

#include "core/memory_probe.h"

namespace bn::test {

// Reports whatever the test sets; nothing by default, like the desktop port.
class FakeMemoryProbe final : public core::MemoryProbe {
 public:
  std::optional<core::HeapStats> Heap() noexcept override {
    ++heapReads;
    return heap;
  }
  std::optional<std::uint32_t> StackHeadroom() noexcept override { return stackHeadroom; }

  std::optional<core::HeapStats> heap;
  std::optional<std::uint32_t> stackHeadroom;
  int heapReads = 0;
};

}  // namespace bn::test
