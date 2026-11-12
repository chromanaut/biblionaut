#pragma once

#include <cstdint>

namespace bn::core {

// Monotonic time since an arbitrary start, in microseconds. Implemented by
// each hardware port; the portable parts only ever see this interface.
class Clock {
 public:
  virtual std::uint64_t NowUs() noexcept = 0;

 protected:
  ~Clock() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
