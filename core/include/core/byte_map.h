#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::core {

// Bytes that are read where they lie, a stretch of them at a time. On a
// device they are a partition of its flash, and a stretch is read by being
// mapped into the processor's address space, which has room for only so
// much at once: so whoever reads asks for the stretch that is wanted, reads
// it through the span it is given, and gives it up when done with it.
class ByteMap {
 public:
  // How many bytes there are.
  virtual std::size_t Size() const noexcept = 0;

  // The `length` bytes from `at`, which can be read until the span is given
  // to Unmap(). Empty if `length` is 0, if the bytes are not all there, or
  // if there is no room for them: room is taken in whole pages, so two
  // stretches that share a page each take it.
  [[nodiscard]] virtual std::span<const std::uint8_t> Map(std::size_t at, std::size_t length) noexcept = 0;

  // Gives up a stretch Map() gave. A span that is not one is passed over.
  virtual void Unmap(std::span<const std::uint8_t> stretch) noexcept = 0;

  // How much room the stretches now mapped take, in bytes.
  virtual std::size_t MappedBytes() const noexcept = 0;

 protected:
  ~ByteMap() = default;  // Never destroyed through the interface.
};

// A ByteMap over memory the caller keeps where it is. It can be told to
// count room as a device does, in pages, and to have only so many of them,
// so that what a device would refuse is refused on the desktop too.
class MemoryByteMap final : public ByteMap {
 public:
  // The most stretches mapped at once.
  static constexpr std::size_t kMostStretches = 32;

  MemoryByteMap() noexcept = default;
  // All of `bytes` can be mapped at once, and a stretch takes the room of
  // its own bytes.
  explicit MemoryByteMap(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
  // Room is counted in pages of `pageBytes`, which is not 0, and there are
  // `pages` of them.
  MemoryByteMap(std::span<const std::uint8_t> bytes, std::size_t pageBytes, std::size_t pages) noexcept
      : bytes_(bytes), pageBytes_(pageBytes == 0 ? 1 : pageBytes), pages_(pages) {}

  std::size_t Size() const noexcept override { return bytes_.size(); }
  [[nodiscard]] std::span<const std::uint8_t> Map(std::size_t at, std::size_t length) noexcept override;
  void Unmap(std::span<const std::uint8_t> stretch) noexcept override;
  std::size_t MappedBytes() const noexcept override { return mappedPages_ * pageBytes_; }

  // How many stretches are mapped now, how many have been mapped in all,
  // and the most room that was ever taken at once, in bytes.
  std::size_t Stretches() const noexcept { return stretches_; }
  std::size_t Maps() const noexcept { return maps_; }
  std::size_t MostMappedBytes() const noexcept { return mostMappedPages_ * pageBytes_; }

 private:
  struct Stretch {
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;
    std::size_t pages = 0;
  };

  std::span<const std::uint8_t> bytes_;
  std::size_t pageBytes_ = 1;
  std::size_t pages_ = SIZE_MAX;
  std::array<Stretch, kMostStretches> mapped_{};
  std::size_t stretches_ = 0;
  std::size_t mappedPages_ = 0;
  std::size_t mostMappedPages_ = 0;
  std::size_t maps_ = 0;
};

}  // namespace bn::core
