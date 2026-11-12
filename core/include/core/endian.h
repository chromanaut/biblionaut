#pragma once

#include <cstdint>
#include <span>

// Little-endian integers in byte buffers, read and written a byte at a time.
// Every stored or transmitted format uses these, so no code depends on the
// processor's byte order or on a buffer being aligned.

namespace bn::core {

constexpr void StoreLe16(std::span<std::uint8_t, 2> out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8U);
}

constexpr void StoreLe32(std::span<std::uint8_t, 4> out, std::uint32_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8U);
  out[2] = static_cast<std::uint8_t>(value >> 16U);
  out[3] = static_cast<std::uint8_t>(value >> 24U);
}

constexpr std::uint16_t LoadLe16(std::span<const std::uint8_t, 2> in) noexcept {
  return static_cast<std::uint16_t>(static_cast<unsigned>(in[0]) | (static_cast<unsigned>(in[1]) << 8U));
}

constexpr std::uint32_t LoadLe32(std::span<const std::uint8_t, 4> in) noexcept {
  return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8U) |
         (static_cast<std::uint32_t>(in[2]) << 16U) | (static_cast<std::uint32_t>(in[3]) << 24U);
}

}  // namespace bn::core
