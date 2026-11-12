#include "core/checksum.h"

#include <array>

namespace bn::core {

namespace {

// Half-byte table: 64 bytes of flash and two lookups per byte, against 1 KB
// for the usual whole-byte table.
constexpr std::array<std::uint32_t, 16> kNibbleTable = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

}  // namespace

std::uint32_t Crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
  crc = ~crc;
  for (const std::uint8_t byte : data) {
    crc ^= byte;
    crc = kNibbleTable[crc & 0x0FU] ^ (crc >> 4U);
    crc = kNibbleTable[crc & 0x0FU] ^ (crc >> 4U);
  }
  return ~crc;
}

}  // namespace bn::core
