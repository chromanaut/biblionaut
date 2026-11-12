#pragma once

#include <cstddef>
#include <cstdint>

namespace bn::core {

// The general heap is for boot and a short allow-list of call sites; steady
// state work uses arenas. These hooks make every failed heap allocation
// visible.
//
// Linking bn_core_runtime replaces the global operator new and delete:
//
// - `new (std::nothrow)` returns nullptr when memory runs out. With
//   exceptions disabled the toolchain's own version aborts instead, because
//   it is written as a try/catch around the throwing form.
// - Plain `new` cannot report failure without exceptions, so it reports the
//   failure through the handler and then aborts.

struct HeapFailure {
  std::size_t size;
  std::size_t alignment;
  bool fatal;  // true: plain `new`, the program aborts when the handler returns.
};

using HeapFailureHandler = void (*)(const HeapFailure&) noexcept;

// Installs the handler called on every failed operator new. Returns the
// previous one. The handler must not allocate.
HeapFailureHandler SetHeapFailureHandler(HeapFailureHandler handler) noexcept;

// Failed operator new calls since boot, fatal or not.
std::uint32_t HeapFailureCount() noexcept;

}  // namespace bn::core
