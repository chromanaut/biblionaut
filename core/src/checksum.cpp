#include "core/checksum.h"

#include <array>

namespace bn::core {

namespace {

// What each byte does to a checksum: the usual table of 256, worked out
// when this is compiled, so that a byte costs one lookup. It is 1 KB of
// flash.
//
// Two other ways were timed on an X4, each by itself and in the reading of
// 34 books. A table of 16, looked up twice a byte, is 64 bytes and seven
// tenths as fast as this. Four tables of 256, which take four bytes at
// once, are 4 KB and nearly twice as fast as this by themselves; but with
// them the books took a tenth longer to read, all of it in the code that
// runs between one checksum and the next.
constexpr std::array<std::uint32_t, 256> MakeTable() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t byte = 0; byte < table.size(); ++byte) {
    std::uint32_t crc = byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
    table[byte] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

}  // namespace

std::uint32_t Crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
  crc = ~crc;
  for (const std::uint8_t byte : data) {
    crc = kTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return ~crc;
}

}  // namespace bn::core
