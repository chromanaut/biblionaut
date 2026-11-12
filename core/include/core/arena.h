#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace bn::core {

// Bump allocator over storage the caller owns. Allocation is a pointer
// increment; nothing is freed individually and no destructors run, so only
// trivially destructible objects may live in an arena. Reset() discards
// everything at once.
//
// Each stage of work (import, layout, image decode) runs inside one arena of
// a fixed size, so peak memory is a design-time number. The counters exist so
// that number can be checked: HighWater() says how close a stage came to its
// budget and FailureCount() says whether it ever went over.
//
// Not thread safe: an arena belongs to exactly one task.
class Arena {
 public:
  Arena(void* storage, std::size_t capacity) noexcept;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() = default;

  // Returns `size` bytes aligned to `alignment` (a power of two), or nullptr
  // if they do not fit. A failed call leaves the arena unchanged apart from
  // FailureCount().
  [[nodiscard]] void* Allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept;

  // Constructs a T in the arena. Returns nullptr if it does not fit.
  template <typename T, typename... Args>
  [[nodiscard]] T* Create(Args&&... args) noexcept {
    static_assert(std::is_trivially_destructible_v<T>, "arenas never run destructors");
    void* p = Allocate(sizeof(T), alignof(T));
    return p ? ::new (p) T(std::forward<Args>(args)...) : nullptr;
  }

  // Allocates `count` default-initialised T (so trivial types are left
  // uninitialised). Returns an empty span if they do not fit.
  template <typename T>
  [[nodiscard]] std::span<T> AllocateArray(std::size_t count) noexcept {
    static_assert(std::is_trivially_destructible_v<T>, "arenas never run destructors");
    if (count > SIZE_MAX / sizeof(T)) {
      NoteFailure();
      return {};
    }
    void* p = Allocate(count * sizeof(T), alignof(T));
    if (!p) {
      return {};
    }
    T* first = static_cast<T*>(p);
    for (std::size_t i = 0; i < count; ++i) {
      ::new (static_cast<void*>(first + i)) T;
    }
    return {first, count};
  }

  // Discards every allocation. Pointers into the arena become invalid.
  void Reset() noexcept;

  std::size_t Capacity() const noexcept { return capacity_; }
  std::size_t Used() const noexcept { return used_; }
  std::size_t Remaining() const noexcept { return capacity_ - used_; }
  // Largest Used() ever reached, across resets.
  std::size_t HighWater() const noexcept { return highWater_; }
  std::uint32_t ResetCount() const noexcept { return resetCount_; }
  // Allocations refused for lack of space.
  std::uint32_t FailureCount() const noexcept { return failureCount_; }

 private:
  void NoteFailure() noexcept { ++failureCount_; }

  std::byte* base_;
  std::size_t capacity_;
  std::size_t used_ = 0;
  std::size_t highWater_ = 0;
  std::uint32_t resetCount_ = 0;
  std::uint32_t failureCount_ = 0;
};

// An arena that carries its own storage, for static or member placement.
template <std::size_t Bytes>
class StaticArena final : public Arena {
 public:
  StaticArena() noexcept : Arena(storage_, Bytes) {}

 private:
  alignas(std::max_align_t) std::byte storage_[Bytes];
};

}  // namespace bn::core
