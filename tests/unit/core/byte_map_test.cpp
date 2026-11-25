#include "core/byte_map.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace {

using bn::core::MemoryByteMap;

std::vector<std::uint8_t> Counting(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
  return bytes;
}

TEST(MemoryByteMap, GivesTheBytesAskedForWhereTheyLie) {
  const std::vector<std::uint8_t> bytes = Counting(200);
  MemoryByteMap map(bytes);
  EXPECT_EQ(map.Size(), 200U);
  const std::span<const std::uint8_t> stretch = map.Map(50, 20);
  ASSERT_EQ(stretch.size(), 20U);
  EXPECT_EQ(stretch.data(), bytes.data() + 50);
  EXPECT_EQ(stretch[0], 50);
  EXPECT_EQ(stretch[19], 69);
  EXPECT_EQ(map.Stretches(), 1U);
  EXPECT_EQ(map.MappedBytes(), 20U);
  // The last byte, and all of it at once.
  EXPECT_EQ(map.Map(199, 1).size(), 1U);
  EXPECT_EQ(map.Map(0, 200).size(), 200U);
  EXPECT_EQ(map.Stretches(), 3U);
  EXPECT_EQ(map.MappedBytes(), 221U);
}

TEST(MemoryByteMap, GivesNothingThatIsNotAllThere) {
  const std::vector<std::uint8_t> bytes = Counting(200);
  MemoryByteMap map(bytes);
  EXPECT_TRUE(map.Map(0, 0).empty());
  EXPECT_TRUE(map.Map(200, 1).empty());
  EXPECT_TRUE(map.Map(199, 2).empty());
  EXPECT_TRUE(map.Map(201, 0).empty());
  EXPECT_TRUE(map.Map(1, SIZE_MAX).empty());
  EXPECT_TRUE(map.Map(SIZE_MAX, 1).empty());
  EXPECT_EQ(map.Stretches(), 0U);
  EXPECT_EQ(map.Maps(), 0U);

  MemoryByteMap none;
  EXPECT_EQ(none.Size(), 0U);
  EXPECT_TRUE(none.Map(0, 1).empty());
}

TEST(MemoryByteMap, TakesRoomInWholePagesAndHasOnlySoMany) {
  const std::vector<std::uint8_t> bytes = Counting(1000);
  // Pages of 100 bytes, four of them.
  MemoryByteMap map(bytes, 100, 4);
  // Two bytes either side of a page's edge take both pages.
  const std::span<const std::uint8_t> across = map.Map(199, 2);
  ASSERT_EQ(across.size(), 2U);
  EXPECT_EQ(map.MappedBytes(), 200U);
  // A page of its own, and then one that the first stretch already
  // touches, which is taken again.
  const std::span<const std::uint8_t> whole = map.Map(500, 100);
  ASSERT_EQ(whole.size(), 100U);
  const std::span<const std::uint8_t> shared = map.Map(210, 5);
  ASSERT_EQ(shared.size(), 5U);
  EXPECT_EQ(map.MappedBytes(), 400U);
  // No room for another, however small; and none was taken by the asking.
  EXPECT_TRUE(map.Map(900, 1).empty());
  EXPECT_EQ(map.Stretches(), 3U);
  EXPECT_EQ(map.Maps(), 3U);

  // Given up, a stretch's pages are there to be had again.
  map.Unmap(across);
  EXPECT_EQ(map.MappedBytes(), 200U);
  EXPECT_TRUE(map.Map(0, 201).empty());
  EXPECT_EQ(map.Map(0, 200).size(), 200U);
  EXPECT_EQ(map.MappedBytes(), 400U);
  EXPECT_EQ(map.Maps(), 4U);
  // The most that was ever mapped stays what it was when less is.
  map.Unmap(whole);
  map.Unmap(shared);
  EXPECT_EQ(map.MappedBytes(), 200U);
  EXPECT_EQ(map.MostMappedBytes(), 400U);
  // And when more is mapped again that is still less than it was.
  ASSERT_EQ(map.Map(900, 1).size(), 1U);
  EXPECT_EQ(map.MappedBytes(), 300U);
  EXPECT_EQ(map.MostMappedBytes(), 400U);
  EXPECT_EQ(map.Maps(), 5U);
}

TEST(MemoryByteMap, PassesOverWhatItDidNotGive) {
  const std::vector<std::uint8_t> bytes = Counting(100);
  MemoryByteMap map(bytes);
  const std::span<const std::uint8_t> stretch = map.Map(10, 30);
  ASSERT_EQ(stretch.size(), 30U);
  map.Unmap({});
  map.Unmap(stretch.first(29));        // The same place, another length.
  map.Unmap(stretch.subspan(1));       // Inside it.
  map.Unmap({bytes.data() + 60, 30});  // Never mapped.
  EXPECT_EQ(map.Stretches(), 1U);
  EXPECT_EQ(map.MappedBytes(), 30U);
  map.Unmap(stretch);
  EXPECT_EQ(map.Stretches(), 0U);
  EXPECT_EQ(map.MappedBytes(), 0U);
  // And a second time is nothing.
  map.Unmap(stretch);
  EXPECT_EQ(map.Stretches(), 0U);
  EXPECT_EQ(map.MappedBytes(), 0U);
}

TEST(MemoryByteMap, HasOnlySoManyStretchesAtOnce) {
  const std::vector<std::uint8_t> bytes = Counting(100);
  MemoryByteMap map(bytes);
  std::vector<std::span<const std::uint8_t>> stretches;
  stretches.reserve(MemoryByteMap::kMostStretches);
  for (std::size_t i = 0; i < MemoryByteMap::kMostStretches; ++i) {
    stretches.push_back(map.Map(i, 1));
    ASSERT_EQ(stretches.back().size(), 1U) << i;
  }
  EXPECT_TRUE(map.Map(50, 1).empty());
  // The same byte mapped twice is two stretches: giving one up leaves the
  // other, and makes room for one more.
  map.Unmap(stretches[3]);
  const std::span<const std::uint8_t> again = map.Map(4, 1);
  ASSERT_EQ(again.size(), 1U);
  EXPECT_EQ(again.data(), stretches[4].data());
  map.Unmap(again);
  EXPECT_EQ(map.Stretches(), MemoryByteMap::kMostStretches - 1);
  map.Unmap(stretches[4]);
  EXPECT_EQ(map.Stretches(), MemoryByteMap::kMostStretches - 2);
}

}  // namespace
