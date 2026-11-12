#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::core {

// A forward-only stream of bytes: a file, a zip entry being inflated, a block
// of memory. The portable parts read through this and never see where the
// bytes come from.
class ByteSource {
 public:
  static constexpr std::ptrdiff_t kError = -1;

  // Reads up to dst.size() bytes into dst. Returns how many were read, which
  // may be fewer than asked for. Returns 0 only at the end of the data (or
  // when dst is empty) and kError if the source failed.
  [[nodiscard]] virtual std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept = 0;

 protected:
  ~ByteSource() = default;  // Never destroyed through the interface.
};

// A ByteSource over memory the caller keeps alive.
class MemoryByteSource final : public ByteSource {
 public:
  explicit MemoryByteSource(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

  std::size_t Remaining() const noexcept { return data_.size() - position_; }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t position_ = 0;
};

}  // namespace bn::core
