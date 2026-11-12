#include "core/checksum.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/endian.h"

namespace {

std::span<const std::uint8_t> Bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST(Crc32, MatchesTheStandardCheckValue) { EXPECT_EQ(bn::core::Crc32(Bytes("123456789")), 0xCBF43926u); }

TEST(Crc32, OfNothingIsZero) { EXPECT_EQ(bn::core::Crc32({}), 0u); }

TEST(Crc32, MatchesKnownValues) {
  EXPECT_EQ(bn::core::Crc32(Bytes("a")), 0xE8B7BE43u);
  EXPECT_EQ(bn::core::Crc32(Bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339u);
  const std::array<std::uint8_t, 4> zeros{};
  EXPECT_EQ(bn::core::Crc32(zeros), 0x2144DF1Cu);
}

TEST(Crc32, CanBeComputedInPieces) {
  const std::string_view text = "The quick brown fox jumps over the lazy dog";
  for (std::size_t split = 0; split <= text.size(); ++split) {
    const std::uint32_t first = bn::core::Crc32(Bytes(text.substr(0, split)));
    EXPECT_EQ(bn::core::Crc32(Bytes(text.substr(split)), first), 0x414FA339u) << "split at " << split;
  }
}

TEST(Endian, StoresLeastSignificantByteFirst) {
  std::array<std::uint8_t, 2> two{};
  bn::core::StoreLe16(two, 0xA1B2);
  EXPECT_EQ(two, (std::array<std::uint8_t, 2>{0xB2, 0xA1}));

  std::array<std::uint8_t, 4> four{};
  bn::core::StoreLe32(four, 0xA1B2C3D4);
  EXPECT_EQ(four, (std::array<std::uint8_t, 4>{0xD4, 0xC3, 0xB2, 0xA1}));
}

TEST(Endian, LoadsWhatItStored) {
  const std::array<std::uint8_t, 2> two{0xFE, 0xFF};
  EXPECT_EQ(bn::core::LoadLe16(two), 0xFFFEu);

  const std::array<std::uint8_t, 4> four{0x78, 0x56, 0x34, 0xF2};
  EXPECT_EQ(bn::core::LoadLe32(four), 0xF2345678u);
}

TEST(Endian, WorksAtCompileTime) {
  constexpr std::array<std::uint8_t, 4> kBytes{0x01, 0x02, 0x03, 0x04};
  static_assert(bn::core::LoadLe32(kBytes) == 0x04030201u);
  static_assert(bn::core::LoadLe16(std::span(kBytes).first<2>()) == 0x0201u);
}

}  // namespace
