#include "core/arena.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using bn::core::Arena;
using bn::core::StaticArena;

bool IsAligned(const void* p, std::size_t alignment) {
  return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0;
}

TEST(Arena, StartsEmpty) {
  const StaticArena<64> arena;
  EXPECT_EQ(arena.Capacity(), 64u);
  EXPECT_EQ(arena.Used(), 0u);
  EXPECT_EQ(arena.Remaining(), 64u);
  EXPECT_EQ(arena.HighWater(), 0u);
  EXPECT_EQ(arena.ResetCount(), 0u);
  EXPECT_EQ(arena.FailureCount(), 0u);
}

TEST(Arena, AllocationsAreDistinctAndInOrder) {
  StaticArena<64> arena;
  auto* a = static_cast<std::byte*>(arena.Allocate(8, 1));
  auto* b = static_cast<std::byte*>(arena.Allocate(8, 1));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b, a + 8);
  EXPECT_EQ(arena.Used(), 16u);
}

TEST(Arena, RespectsAlignment) {
  StaticArena<256> arena;
  ASSERT_NE(arena.Allocate(1, 1), nullptr);
  for (const std::size_t alignment : {2u, 4u, 8u, 16u, 32u}) {
    void* p = arena.Allocate(1, alignment);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(IsAligned(p, alignment)) << "alignment " << alignment;
  }
}

TEST(Arena, AlignsTheAddressWhenStorageIsMisaligned) {
  alignas(16) std::byte storage[64];
  Arena arena(storage + 1, 63);
  void* p = arena.Allocate(4, 8);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(IsAligned(p, 8));
  EXPECT_EQ(arena.Used(), 7u + 4u);  // 7 bytes of padding to reach storage + 8.
}

TEST(Arena, CanBeFilledExactly) {
  StaticArena<32> arena;
  EXPECT_NE(arena.Allocate(32, 1), nullptr);
  EXPECT_EQ(arena.Remaining(), 0u);
  EXPECT_EQ(arena.FailureCount(), 0u);
}

TEST(Arena, RefusesWhatDoesNotFitAndStaysUsable) {
  StaticArena<32> arena;
  ASSERT_NE(arena.Allocate(24, 1), nullptr);

  EXPECT_EQ(arena.Allocate(9, 1), nullptr);
  EXPECT_EQ(arena.Used(), 24u);
  EXPECT_EQ(arena.FailureCount(), 1u);

  EXPECT_NE(arena.Allocate(8, 1), nullptr);
  EXPECT_EQ(arena.Used(), 32u);
}

TEST(Arena, RefusesWhenPaddingAloneDoesNotFit) {
  StaticArena<16> arena;
  ASSERT_NE(arena.Allocate(15, 1), nullptr);
  EXPECT_EQ(arena.Allocate(1, 8), nullptr);
  EXPECT_EQ(arena.Used(), 15u);
}

TEST(Arena, RefusesHugeSizesWithoutOverflowing) {
  StaticArena<32> arena;
  ASSERT_NE(arena.Allocate(1, 1), nullptr);
  EXPECT_EQ(arena.Allocate(SIZE_MAX, 1), nullptr);
  EXPECT_EQ(arena.Allocate(SIZE_MAX - 4, 8), nullptr);
  EXPECT_EQ(arena.Used(), 1u);
}

TEST(Arena, ZeroSizedAllocationSucceedsAndUsesNothing) {
  StaticArena<16> arena;
  EXPECT_NE(arena.Allocate(0, 1), nullptr);
  EXPECT_EQ(arena.Used(), 0u);
}

TEST(Arena, ResetReusesTheStorageAndKeepsTheHighWaterMark) {
  StaticArena<64> arena;
  void* first = arena.Allocate(40, 1);
  ASSERT_NE(first, nullptr);
  arena.Reset();

  EXPECT_EQ(arena.Used(), 0u);
  EXPECT_EQ(arena.ResetCount(), 1u);
  EXPECT_EQ(arena.HighWater(), 40u);

  EXPECT_EQ(arena.Allocate(8, 1), first);
  EXPECT_EQ(arena.HighWater(), 40u);
}

TEST(Arena, CreateConstructsInPlace) {
  struct Point {
    int x;
    int y;
  };
  StaticArena<64> arena;
  Point* p = arena.Create<Point>(Point{3, 4});
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->x, 3);
  EXPECT_EQ(p->y, 4);
  EXPECT_TRUE(IsAligned(p, alignof(Point)));
}

TEST(Arena, CreateReturnsNullWhenFull) {
  StaticArena<4> arena;
  EXPECT_EQ(arena.Create<std::uint64_t>(1u), nullptr);
  EXPECT_EQ(arena.FailureCount(), 1u);
}

TEST(Arena, AllocateArray) {
  StaticArena<64> arena;
  ASSERT_NE(arena.Allocate(1, 1), nullptr);
  const std::span<std::uint32_t> values = arena.AllocateArray<std::uint32_t>(4);
  ASSERT_EQ(values.size(), 4u);
  EXPECT_TRUE(IsAligned(values.data(), alignof(std::uint32_t)));
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<std::uint32_t>(i);
  }
  EXPECT_EQ(values[3], 3u);
}

TEST(Arena, AllocateArrayRefusesTooManyAndOverflowingCounts) {
  StaticArena<16> arena;
  EXPECT_TRUE(arena.AllocateArray<std::uint32_t>(5).empty());
  EXPECT_TRUE(arena.AllocateArray<std::uint32_t>(SIZE_MAX / 2).empty());
  EXPECT_EQ(arena.FailureCount(), 2u);
  EXPECT_EQ(arena.Used(), 0u);
}

TEST(Arena, NullStorageHasNoCapacity) {
  Arena arena(nullptr, 128);
  EXPECT_EQ(arena.Capacity(), 0u);
  EXPECT_EQ(arena.Allocate(1, 1), nullptr);
}

}  // namespace
