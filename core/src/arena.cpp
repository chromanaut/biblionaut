#include "core/arena.h"

#include <bit>
#include <cassert>

namespace bn::core {

Arena::Arena(void* storage, std::size_t capacity) noexcept
    : base_(static_cast<std::byte*>(storage)), capacity_(storage ? capacity : 0) {}

void* Arena::Allocate(std::size_t size, std::size_t alignment) noexcept {
  assert(std::has_single_bit(alignment));

  // Align the real address, not the offset: the storage itself may sit at any
  // alignment.
  const auto address = reinterpret_cast<std::uintptr_t>(base_) + used_;
  const std::size_t padding = static_cast<std::size_t>((alignment - (address & (alignment - 1))) & (alignment - 1));

  const std::size_t available = capacity_ - used_;
  if (padding > available || size > available - padding) {
    NoteFailure();
    return nullptr;
  }

  std::byte* result = base_ + used_ + padding;
  used_ += padding + size;
  if (used_ > highWater_) {
    highWater_ = used_;
  }
  return result;
}

void Arena::Reset() noexcept {
  used_ = 0;
  ++resetCount_;
}

}  // namespace bn::core
