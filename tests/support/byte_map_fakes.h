#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/byte_map.h"

namespace bn::test {

// A ByteMap that gives each stretch as a copy, in memory of exactly the
// stretch's size, which it frees when the stretch is given up: so the
// sanitizer sees a byte read outside a stretch, though it would be inside
// the bytes mapped from, and one read after its stretch was given up. It
// can be given only so many bytes of room, which is counted in the
// stretches' own bytes.
class CopyingByteMap final : public core::ByteMap {
 public:
  explicit CopyingByteMap(std::span<const std::uint8_t> bytes, std::size_t room = SIZE_MAX)
      : bytes_(bytes), room_(room) {}

  std::size_t Size() const noexcept override { return bytes_.size(); }

  [[nodiscard]] std::span<const std::uint8_t> Map(std::size_t at, std::size_t length) noexcept override {
    ++asked;
    if (length == 0 || at > bytes_.size() || length > bytes_.size() - at || length > room_ - mapped_) {
      return {};
    }
    stretches_.push_back({std::make_unique<std::uint8_t[]>(length), length});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at), length, stretches_.back().copy.get());
    mapped_ += length;
    ++maps;
    return {stretches_.back().copy.get(), length};
  }

  void Unmap(std::span<const std::uint8_t> stretch) noexcept override {
    for (std::size_t i = 0; i < stretches_.size(); ++i) {
      if (stretches_[i].copy.get() == stretch.data() && stretches_[i].length == stretch.size()) {
        mapped_ -= stretches_[i].length;
        stretches_.erase(stretches_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  std::size_t MappedBytes() const noexcept override { return mapped_; }

  std::size_t Stretches() const noexcept { return stretches_.size(); }

  std::size_t asked = 0;  // Calls of Map().
  std::size_t maps = 0;   // Those that gave a stretch.

 private:
  struct Stretch {
    std::unique_ptr<std::uint8_t[]> copy;
    std::size_t length = 0;
  };

  std::span<const std::uint8_t> bytes_;
  std::size_t room_;
  std::size_t mapped_ = 0;
  std::vector<Stretch> stretches_;
};

}  // namespace bn::test
