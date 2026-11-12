#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): posix_memalign is POSIX, not part of <cstdlib>.

#include <atomic>
#include <cstdlib>
#include <new>

#include "core/heap.h"

// This file is the general heap's only gateway, so it is the one place that
// calls malloc and free directly.
// NOLINTBEGIN(cppcoreguidelines-no-malloc)

namespace {

std::atomic<bn::core::HeapFailureHandler> failureHandler{nullptr};
std::atomic<std::uint32_t> failureCount{0};

constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);

void* Allocate(std::size_t size, std::size_t alignment) noexcept {
  if (size == 0) {
    size = 1;  // operator new must return a unique non-null pointer.
  }
  if (alignment <= kDefaultAlignment) {
    return std::malloc(size);
  }
  // posix_memalign rather than std::aligned_alloc: the device's C++ library
  // does not expose the latter. The result is released with free().
  void* p = nullptr;
  return ::posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}

void* AllocateOrReport(std::size_t size, std::size_t alignment, bool fatal) noexcept {
  void* p = Allocate(size, alignment);
  if (p) {
    return p;
  }
  failureCount.fetch_add(1, std::memory_order_relaxed);
  if (const auto handler = failureHandler.load(std::memory_order_relaxed)) {
    handler(bn::core::HeapFailure{size, alignment, fatal});
  }
  if (fatal) {
    std::abort();
  }
  return nullptr;
}

}  // namespace

namespace bn::core {

HeapFailureHandler SetHeapFailureHandler(HeapFailureHandler handler) noexcept {
  return failureHandler.exchange(handler, std::memory_order_relaxed);
}

std::uint32_t HeapFailureCount() noexcept { return failureCount.load(std::memory_order_relaxed); }

}  // namespace bn::core

// Every replaceable form is defined here so allocation and release always
// pair up (malloc family with free), whichever forms the caller uses.

void* operator new(std::size_t size) { return AllocateOrReport(size, kDefaultAlignment, true); }
void* operator new[](std::size_t size) { return AllocateOrReport(size, kDefaultAlignment, true); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateOrReport(size, static_cast<std::size_t>(alignment), true);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateOrReport(size, static_cast<std::size_t>(alignment), true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateOrReport(size, kDefaultAlignment, false);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateOrReport(size, kDefaultAlignment, false);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return AllocateOrReport(size, static_cast<std::size_t>(alignment), false);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return AllocateOrReport(size, static_cast<std::size_t>(alignment), false);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

// NOLINTEND(cppcoreguidelines-no-malloc)
