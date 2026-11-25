#include "core/checksum.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

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

// The checksum worked out a bit at a time, as its definition has it: slow,
// and too plain to have a mistake of its own.
std::uint32_t BitAtATime(std::span<const std::uint8_t> data, std::uint32_t crc = 0) {
  crc = ~crc;
  for (const std::uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return ~crc;
}

TEST(Crc32, IsTheSameWhereverTheBytesBeginAndHoweverManyThereAre) {
  // Every length from none to a good many, begun at each of eight places
  // in turn, from nothing and carried on from a checksum already begun:
  // however the bytes are taken, one at a time or several, nothing of
  // where they lie or how many there are may show.
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same input every run, so that a failure can be found again.
  std::mt19937 random(1988);
  alignas(8) std::array<std::uint8_t, 96> bytes{};
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  for (std::size_t start = 0; start < 8; ++start) {
    for (std::size_t length = 0; start + length <= bytes.size(); ++length) {
      const std::span<const std::uint8_t> piece = std::span(bytes).subspan(start, length);
      ASSERT_EQ(bn::core::Crc32(piece), BitAtATime(piece)) << length << " bytes from " << start;
      ASSERT_EQ(bn::core::Crc32(piece, 0x12345678U), BitAtATime(piece, 0x12345678U))
          << length << " bytes from " << start;
    }
  }
}

TEST(Crc32, OfALongStretchIsWhatItIsInPiecesOfAnySize) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same input every run, so that a failure can be found again.
  std::mt19937 random(1989);
  std::vector<std::uint8_t> bytes(100003);
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  const std::uint32_t whole = bn::core::Crc32(bytes);
  EXPECT_EQ(whole, BitAtATime(bytes));
  for (const std::size_t piece : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{7},
                                  std::size_t{64}, std::size_t{1000}, std::size_t{4096}}) {
    std::uint32_t crc = 0;
    for (std::size_t at = 0; at < bytes.size(); at += piece) {
      crc = bn::core::Crc32(std::span(bytes).subspan(at, std::min(piece, bytes.size() - at)), crc);
    }
    EXPECT_EQ(crc, whole) << "in pieces of " << piece;
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
