#include "core/byte_map.h"

#include <algorithm>

namespace bn::core {

std::span<const std::uint8_t> MemoryByteMap::Map(std::size_t at, std::size_t length) noexcept {
  if (length == 0 || at > bytes_.size() || length > bytes_.size() - at || stretches_ == kMostStretches) {
    return {};
  }
  // The pages the stretch touches, from the one its first byte is in.
  const std::size_t pages = ((at % pageBytes_) + length + pageBytes_ - 1) / pageBytes_;
  if (pages > pages_ - mappedPages_) {
    return {};
  }
  const std::span<const std::uint8_t> stretch = bytes_.subspan(at, length);
  for (Stretch& free : mapped_) {
    if (free.data == nullptr) {
      free = Stretch{.data = stretch.data(), .length = length, .pages = pages};
      break;
    }
  }
  ++stretches_;
  ++maps_;
  mappedPages_ += pages;
  mostMappedPages_ = std::max(mostMappedPages_, mappedPages_);
  return stretch;
}

void MemoryByteMap::Unmap(std::span<const std::uint8_t> stretch) noexcept {
  if (stretch.empty()) {
    return;
  }
  for (Stretch& mapped : mapped_) {
    if (mapped.data == stretch.data() && mapped.length == stretch.size()) {
      mappedPages_ -= mapped.pages;
      --stretches_;
      mapped = Stretch{};
      return;
    }
  }
}

}  // namespace bn::core
