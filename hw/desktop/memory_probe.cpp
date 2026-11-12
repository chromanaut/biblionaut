#include "hw/platform.h"

namespace bn::hw {

namespace {

// A desktop process has no fixed heap or small task stacks to run out of,
// so there is nothing meaningful to report.
class DesktopMemoryProbe final : public core::MemoryProbe {
 public:
  std::optional<core::HeapStats> Heap() noexcept override { return std::nullopt; }
  std::optional<std::uint32_t> StackHeadroom() noexcept override { return std::nullopt; }
};

}  // namespace

core::MemoryProbe& SystemMemoryProbe() noexcept {
  static DesktopMemoryProbe instance;
  return instance;
}

}  // namespace bn::hw
