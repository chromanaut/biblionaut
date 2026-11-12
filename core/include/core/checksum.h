#pragma once

#include <cstdint>
#include <span>

namespace bn::core {

// CRC-32 as used by zip, zlib and PNG (polynomial 0xEDB88320, reflected).
//
// Pass the result of one call as `crc` to the next to checksum data that
// arrives in pieces; start with 0.
std::uint32_t Crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept;

}  // namespace bn::core
