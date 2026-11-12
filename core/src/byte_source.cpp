#include "core/byte_source.h"

#include <algorithm>
#include <cstring>

namespace bn::core {

std::ptrdiff_t MemoryByteSource::Read(std::span<std::uint8_t> dst) noexcept {
  const std::size_t count = std::min(dst.size(), Remaining());
  if (count > 0) {
    std::memcpy(dst.data(), data_.data() + position_, count);
    position_ += count;
  }
  return static_cast<std::ptrdiff_t>(count);
}

}  // namespace bn::core
