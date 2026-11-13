#include "core/region_heap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

using bn::core::RegionHeap;

constexpr std::size_t kOverhead = RegionHeap::kOverhead;

// What a piece of `size` bytes takes from the region.
constexpr std::size_t Cost(std::size_t size) { return std::max<std::size_t>(8, (size + 7) / 8 * 8) + kOverhead; }

// A heap over 4 KB of its own.
struct HeapFixture {
  alignas(8) std::array<std::uint8_t, 4096> memory{};
  RegionHeap heap{memory};

  std::uint8_t* Get(std::size_t size, std::uint8_t fill) {
    auto* piece = static_cast<std::uint8_t*>(heap.Allocate(size));
    EXPECT_NE(piece, nullptr) << size;
    if (piece != nullptr) {
      std::memset(piece, fill, size);
    }
    return piece;
  }
};

bool Holds(const std::uint8_t* piece, std::size_t size, std::uint8_t fill) {
  return std::all_of(piece, piece + size, [fill](std::uint8_t byte) { return byte == fill; });
}

TEST(RegionHeap, StartsEmptyWithAllOfItsMemoryToGive) {
  const HeapFixture fixture;
  EXPECT_EQ(fixture.heap.Capacity(), 4096u);
  EXPECT_EQ(fixture.heap.Used(), 0u);
  EXPECT_EQ(fixture.heap.LivePieces(), 0u);
  EXPECT_EQ(fixture.heap.LargestFree(), 4096u - kOverhead);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, GivesOutPiecesThatAreAlignedAndDoNotOverlap) {
  HeapFixture fixture;
  std::vector<std::pair<std::uint8_t*, std::size_t>> pieces;
  std::size_t used = 0;
  for (const std::size_t size : {1u, 7u, 8u, 9u, 100u, 0u, 333u, 64u}) {
    std::uint8_t* piece = fixture.Get(size, static_cast<std::uint8_t>(pieces.size() + 1));
    ASSERT_NE(piece, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(piece) % RegionHeap::kAlignment, 0u) << size;
    EXPECT_GE(piece, fixture.memory.data());
    EXPECT_LE(piece + size, fixture.memory.data() + fixture.memory.size());
    pieces.emplace_back(piece, size);
    used += Cost(size);
    EXPECT_EQ(fixture.heap.Used(), used) << size;
  }
  // Each still holds what was put in it: none was written over by another.
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    EXPECT_TRUE(Holds(pieces[i].first, pieces[i].second, static_cast<std::uint8_t>(i + 1))) << i;
  }
  EXPECT_EQ(fixture.heap.LivePieces(), pieces.size());
  EXPECT_EQ(fixture.heap.HighWater(), used);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, RefusesWhatDoesNotFitAndCountsIt) {
  HeapFixture fixture;
  EXPECT_EQ(fixture.heap.Allocate(4096), nullptr) << "there is the overhead to fit as well";
  EXPECT_EQ(fixture.heap.Allocate(SIZE_MAX), nullptr);
  EXPECT_EQ(fixture.heap.Allocate(SIZE_MAX - 3), nullptr);
  EXPECT_EQ(fixture.heap.FailureCount(), 3u);
  EXPECT_EQ(fixture.heap.Used(), 0u);

  // All of it in one piece, and then there is none left.
  void* all = fixture.heap.Allocate(4096 - kOverhead);
  ASSERT_NE(all, nullptr);
  EXPECT_EQ(fixture.heap.Used(), 4096u);
  EXPECT_EQ(fixture.heap.LargestFree(), 0u);
  EXPECT_EQ(fixture.heap.Allocate(1), nullptr);
  EXPECT_EQ(fixture.heap.Allocate(0), nullptr);
  EXPECT_EQ(fixture.heap.FailureCount(), 5u);
  fixture.heap.Free(all);
  EXPECT_NE(fixture.heap.Allocate(1), nullptr);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, HandlesTheSmallestPieceAtTheVeryEndOfItsMemory) {
  HeapFixture fixture;
  // Everything but room for one piece of the smallest size, and then that.
  void* most = fixture.heap.Allocate(4096 - Cost(1) - kOverhead);
  auto* last = static_cast<std::uint8_t*>(fixture.heap.Allocate(1));
  ASSERT_NE(most, nullptr);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(fixture.heap.Used(), 4096u);
  EXPECT_GT(last + 8, fixture.memory.data() + 4096 - kOverhead) << "it is the last thing in the region";
  // It can be made as big as its room allows, and given back.
  EXPECT_EQ(fixture.heap.Resize(last, 8), last);
  EXPECT_EQ(fixture.heap.Resize(last, 9), nullptr);
  fixture.heap.Free(last);
  EXPECT_EQ(fixture.heap.MisuseCount(), 0u);
  EXPECT_EQ(fixture.heap.Used(), 4096u - Cost(1));
  EXPECT_EQ(fixture.heap.LivePieces(), 1u);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, JoinsAPieceGivenBackToTheFreeSpaceOnEitherSide) {
  // Three pieces and then the rest, given back in every order: whichever
  // way round, the whole region is one free piece again at the end.
  std::array<int, 4> order = {0, 1, 2, 3};
  do {
    HeapFixture fixture;
    std::array<void*, 4> pieces{};
    pieces[0] = fixture.heap.Allocate(500);
    pieces[1] = fixture.heap.Allocate(700);
    pieces[2] = fixture.heap.Allocate(900);
    pieces[3] = fixture.heap.Allocate(fixture.heap.LargestFree());
    ASSERT_NE(pieces[3], nullptr);
    EXPECT_EQ(fixture.heap.Used(), 4096u);
    for (const int which : order) {
      fixture.heap.Free(pieces.at(static_cast<std::size_t>(which)));
      EXPECT_TRUE(fixture.heap.IsConsistent());
    }
    EXPECT_EQ(fixture.heap.Used(), 0u);
    EXPECT_EQ(fixture.heap.LivePieces(), 0u);
    EXPECT_EQ(fixture.heap.LargestFree(), 4096u - kOverhead) << order[0] << order[1] << order[2] << order[3];
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST(RegionHeap, UsesTheFirstFreeSpaceThatIsBigEnough) {
  HeapFixture fixture;
  void* first = fixture.heap.Allocate(200);
  void* between = fixture.heap.Allocate(50);
  void* second = fixture.heap.Allocate(400);
  void* last = fixture.heap.Allocate(50);
  ASSERT_NE(last, nullptr);
  fixture.heap.Free(first);
  fixture.heap.Free(second);
  // 300 bytes do not fit where the 200 were, and go where the 400 were.
  EXPECT_EQ(fixture.heap.Allocate(300), second);
  // 100 fit in the first gap, and what is left of it can still be had.
  EXPECT_EQ(fixture.heap.Allocate(100), first);
  void* inTheGap = fixture.heap.Allocate(200 - Cost(100));
  EXPECT_GT(inTheGap, first);
  EXPECT_LT(inTheGap, between);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, ShrinksAPieceWhereItIsAndGivesBackTheRest) {
  HeapFixture fixture;
  std::uint8_t* piece = fixture.Get(1000, 0xAB);
  void* after = fixture.heap.Allocate(100);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(fixture.heap.Resize(piece, 200), piece);
  EXPECT_TRUE(Holds(piece, 200, 0xAB));
  EXPECT_EQ(fixture.heap.Used(), Cost(200) + Cost(100));
  // What was given back is there to be had again, between the two.
  void* inBetween = fixture.heap.Allocate(1000 - Cost(200));
  EXPECT_GT(inBetween, static_cast<void*>(piece));
  EXPECT_LT(inBetween, after);

  // A change too small to make a piece of leaves the piece as it is.
  EXPECT_EQ(fixture.heap.Resize(piece, 196), piece);
  EXPECT_EQ(fixture.heap.Resize(piece, 200), piece);
  EXPECT_EQ(fixture.heap.Resize(piece, 0), piece);
  EXPECT_EQ(fixture.heap.Used(), Cost(0) + Cost(100) + Cost(1000 - Cost(200)));
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, GrowsAPieceWhereItIsWhenTheSpaceAfterItIsFree) {
  HeapFixture fixture;
  std::uint8_t* piece = fixture.Get(100, 0x11);
  // Nothing after it but free space.
  EXPECT_EQ(fixture.heap.Resize(piece, 2000), piece);
  EXPECT_TRUE(Holds(piece, 100, 0x11));
  std::memset(piece, 0x22, 2000);
  EXPECT_EQ(fixture.heap.Used(), Cost(2000));

  // A free piece after it, with a piece in use beyond that: it can grow
  // into the gap, exactly to its end, and no further.
  void* gap = fixture.heap.Allocate(300);
  std::uint8_t* beyond = fixture.Get(100, 0x33);
  fixture.heap.Free(gap);
  EXPECT_EQ(fixture.heap.Resize(piece, 2000 + Cost(300)), piece);
  EXPECT_TRUE(Holds(piece, 2000, 0x22));
  EXPECT_TRUE(Holds(beyond, 100, 0x33));
  EXPECT_EQ(fixture.heap.Used(), Cost(2000 + Cost(300)) + Cost(100));
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, MovesAPieceThatCannotGrowWhereItIs) {
  HeapFixture fixture;
  std::uint8_t* piece = fixture.Get(100, 0x44);
  std::uint8_t* after = fixture.Get(100, 0x55);
  auto* moved = static_cast<std::uint8_t*>(fixture.heap.Resize(piece, 500));
  ASSERT_NE(moved, nullptr);
  EXPECT_NE(moved, piece);
  EXPECT_TRUE(Holds(moved, 100, 0x44));
  EXPECT_TRUE(Holds(after, 100, 0x55));
  EXPECT_EQ(fixture.heap.Used(), Cost(500) + Cost(100));
  EXPECT_EQ(fixture.heap.HighWater(), Cost(500) + 2 * Cost(100)) << "while it moved, both were held";
  // Where it was is free again.
  EXPECT_EQ(fixture.heap.Allocate(100), piece);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, LeavesAPieceAsItWasWhenItCannotBeResized) {
  HeapFixture fixture;
  std::uint8_t* piece = fixture.Get(1000, 0x66);
  std::uint8_t* after = fixture.Get(100, 0x77);
  EXPECT_EQ(fixture.heap.Resize(piece, 4000), nullptr);
  EXPECT_EQ(fixture.heap.Resize(piece, SIZE_MAX), nullptr);
  EXPECT_EQ(fixture.heap.FailureCount(), 2u);
  EXPECT_TRUE(Holds(piece, 1000, 0x66));
  EXPECT_TRUE(Holds(after, 100, 0x77));
  EXPECT_EQ(fixture.heap.Used(), Cost(1000) + Cost(100));
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, ResizingNothingIsAllocating) {
  HeapFixture fixture;
  void* piece = fixture.heap.Resize(nullptr, 64);
  ASSERT_NE(piece, nullptr);
  EXPECT_EQ(fixture.heap.Used(), Cost(64));
  EXPECT_EQ(fixture.heap.LivePieces(), 1u);
}

TEST(RegionHeap, IgnoresWhatIsNotAPieceInUseAndCountsIt) {
  HeapFixture fixture;
  std::uint8_t* piece = fixture.Get(64, 0x01);
  std::uint8_t* other = fixture.Get(64, 0x02);
  fixture.heap.Free(nullptr);
  EXPECT_EQ(fixture.heap.MisuseCount(), 0u);

  int elsewhere = 0;
  fixture.heap.Free(&elsewhere);
  fixture.heap.Free(fixture.memory.data());         // The region's first byte, which is a header's.
  fixture.heap.Free(piece + 3);                     // Not where a piece starts,
  fixture.heap.Free(piece + 16);                    // and neither is this, though it is aligned.
  fixture.heap.Free(fixture.memory.data() + 4096);  // One past the end.
  EXPECT_EQ(fixture.heap.Resize(&elsewhere, 10), nullptr);
  EXPECT_EQ(fixture.heap.MisuseCount(), 6u);
  EXPECT_EQ(fixture.heap.Used(), 2 * Cost(64));

  // Given back twice: the second time it is no longer a piece in use.
  fixture.heap.Free(piece);
  fixture.heap.Free(piece);
  EXPECT_EQ(fixture.heap.MisuseCount(), 7u);
  EXPECT_EQ(fixture.heap.Used(), Cost(64));
  EXPECT_EQ(fixture.heap.LivePieces(), 1u);
  EXPECT_TRUE(Holds(other, 64, 0x02));
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, IsNotFooledByBytesThatLookLikeAPieceOutsideItOrOffTheAlignment) {
  // What a piece in use of 16 bytes has in front of it.
  const std::array<std::uint32_t, 2> looksInUse = {16U | 1U, 0U};

  // Past the end of the heap's memory, in memory that is the test's own.
  alignas(8) std::array<std::uint8_t, 4096 + 32> memory{};
  RegionHeap heap{std::span(memory).first(4096)};
  std::memcpy(memory.data() + 4104, looksInUse.data(), sizeof(looksInUse));
  heap.Free(memory.data() + 4112);
  EXPECT_EQ(heap.MisuseCount(), 1u);
  EXPECT_EQ(heap.Resize(memory.data() + 4112, 8), nullptr);
  EXPECT_EQ(heap.MisuseCount(), 2u);
  EXPECT_EQ(heap.Used(), 0u);
  EXPECT_EQ(heap.LivePieces(), 0u);
  EXPECT_TRUE(heap.IsConsistent());

  // Inside a piece, where no piece can start.
  auto* piece = static_cast<std::uint8_t*>(heap.Allocate(64));
  ASSERT_NE(piece, nullptr);
  std::memcpy(piece + 4, looksInUse.data(), sizeof(looksInUse));
  heap.Free(piece + 12);
  EXPECT_EQ(heap.MisuseCount(), 3u);
  EXPECT_EQ(heap.Used(), Cost(64));
  EXPECT_EQ(heap.LivePieces(), 1u);
  EXPECT_TRUE(heap.IsConsistent());
}

TEST(RegionHeap, ResetTakesEverythingBackAndKeepsTheHighWater) {
  HeapFixture fixture;
  ASSERT_NE(fixture.heap.Allocate(1000), nullptr);
  ASSERT_NE(fixture.heap.Allocate(2000), nullptr);
  const std::size_t most = fixture.heap.Used();
  EXPECT_EQ(fixture.heap.HighWaterSinceReset(), most);
  fixture.heap.Reset();
  EXPECT_EQ(fixture.heap.Used(), 0u);
  EXPECT_EQ(fixture.heap.LivePieces(), 0u);
  EXPECT_EQ(fixture.heap.LargestFree(), 4096u - kOverhead);
  EXPECT_EQ(fixture.heap.HighWater(), most);
  EXPECT_EQ(fixture.heap.HighWaterSinceReset(), 0u);
  void* small = fixture.heap.Allocate(100);
  EXPECT_EQ(fixture.heap.HighWaterSinceReset(), Cost(100));
  EXPECT_EQ(fixture.heap.HighWater(), most);
  fixture.heap.Free(small);
  EXPECT_EQ(fixture.heap.HighWaterSinceReset(), Cost(100)) << "it is the most, not what is used now";
  EXPECT_NE(fixture.heap.Allocate(4096 - kOverhead), nullptr);
  EXPECT_TRUE(fixture.heap.IsConsistent());
}

TEST(RegionHeap, MakesDoWithMemoryThatIsTooSmallOrStartsAnywhere) {
  // Nothing, or too little to hold one piece: a heap that refuses.
  RegionHeap none{std::span<std::uint8_t>()};
  EXPECT_EQ(none.Capacity(), 0u);
  EXPECT_EQ(none.Allocate(1), nullptr);
  EXPECT_EQ(none.LargestFree(), 0u);
  none.Free(nullptr);
  none.Reset();
  EXPECT_TRUE(none.IsConsistent());

  alignas(8) std::array<std::uint8_t, 64> small{};
  RegionHeap tooSmall{std::span(small).first(15)};
  EXPECT_EQ(tooSmall.Capacity(), 0u);
  EXPECT_EQ(tooSmall.Allocate(1), nullptr);

  // Memory that starts off the alignment, and is an odd length: the
  // pieces are aligned all the same, and stay inside it.
  alignas(8) std::array<std::uint8_t, 256> odd{};
  for (std::size_t start = 1; start < 8; ++start) {
    const std::span<std::uint8_t> memory = std::span(odd).subspan(start, 200 + start);
    RegionHeap heap{memory};
    EXPECT_LE(heap.Capacity(), memory.size());
    EXPECT_GE(heap.Capacity() + 15, memory.size());
    auto* piece = static_cast<std::uint8_t*>(heap.Allocate(heap.LargestFree()));
    ASSERT_NE(piece, nullptr) << start;
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(piece) % 8, 0u);
    EXPECT_GE(piece, memory.data());
    EXPECT_LE(piece + (heap.Capacity() - kOverhead), memory.data() + memory.size());
    EXPECT_TRUE(heap.IsConsistent());
  }
}

TEST(RegionHeap, NoticesWhenItsRecordsHaveBeenWrittenOver) {
  HeapFixture fixture;
  ASSERT_NE(fixture.heap.Allocate(100), nullptr);
  ASSERT_NE(fixture.heap.Allocate(100), nullptr);
  ASSERT_TRUE(fixture.heap.IsConsistent());
  // The second piece's record of its own size, which begins where the
  // first piece ends.
  fixture.memory.at(Cost(100)) ^= 0x10U;
  EXPECT_FALSE(fixture.heap.IsConsistent());
  fixture.memory.at(Cost(100)) ^= 0x10U;
  EXPECT_TRUE(fixture.heap.IsConsistent());
  // And its record of the size of the piece before it.
  fixture.memory.at(Cost(100) + 4) ^= 0x08U;
  EXPECT_FALSE(fixture.heap.IsConsistent());
}

// Puts what a piece has in front of it at `offset` in `memory`: its size,
// whether it is in use, and the size of the piece before it.
void WriteHeader(std::span<std::uint8_t> memory, std::size_t offset, std::size_t size, bool inUse,
                 std::size_t previous) {
  const std::array<std::uint32_t, 2> header = {static_cast<std::uint32_t>(size) | (inUse ? 1U : 0U),
                                               static_cast<std::uint32_t>(previous)};
  std::memcpy(memory.data() + offset, header.data(), sizeof(header));
}

TEST(RegionHeap, NoticesRecordsThatAgreeWithEachOtherButNotWithItsCounts) {
  // Three pieces in use, one after another from the start of the region.
  // The damage below leaves every piece's record in step with its
  // neighbours', so only the heap's own counts can show it.
  HeapFixture fixture;
  ASSERT_NE(fixture.heap.Allocate(100), nullptr);
  void* second = fixture.heap.Allocate(100);
  ASSERT_NE(fixture.heap.Allocate(100), nullptr);
  ASSERT_TRUE(fixture.heap.IsConsistent());
  const std::size_t cost = Cost(100);

  // The first two made into one piece in use: the same bytes in use, one
  // piece fewer.
  WriteHeader(fixture.memory, 0, 2 * cost, true, 0);
  WriteHeader(fixture.memory, 2 * cost, cost, true, 2 * cost);
  EXPECT_FALSE(fixture.heap.IsConsistent());
  WriteHeader(fixture.memory, 0, cost, true, 0);
  WriteHeader(fixture.memory, 2 * cost, cost, true, cost);
  ASSERT_TRUE(fixture.heap.IsConsistent());

  // The second given back, and then the first made to cover it: as many
  // pieces in use as the heap thinks, and more bytes.
  fixture.heap.Free(second);
  ASSERT_TRUE(fixture.heap.IsConsistent());
  WriteHeader(fixture.memory, 0, 2 * cost, true, 0);
  WriteHeader(fixture.memory, 2 * cost, cost, true, 2 * cost);
  EXPECT_FALSE(fixture.heap.IsConsistent());
}

// Thousands of requests of every kind in no particular order, checked
// against a list kept on the side: every piece holds what was last put in
// it until it is given back, and the heap's records stay in order.
TEST(RegionHeap, SurvivesALongRunOfRequestsOfEveryKind) {
  alignas(8) static std::array<std::uint8_t, 24'576> memory{};
  RegionHeap heap{memory};
  struct Piece {
    std::uint8_t* at;
    std::size_t size;
    std::uint8_t fill;
  };
  std::vector<Piece> pieces;
  std::mt19937 rng(20261113);  // NOLINT(bugprone-random-generator-seed): the same run every time.
  const auto below = [&rng](std::size_t limit) -> std::size_t { return rng() % limit; };
  const auto someSize = [&below]() -> std::size_t {
    switch (below(10)) {
      case 0:
        return below(4000);  // A buffer.
      case 1:
        return 0;
      default:
        return below(120);  // The small things a parser keeps.
    }
  };
  std::size_t refused = 0;
  for (int step = 0; step < 30'000; ++step) {
    const std::size_t choice = below(10);
    if (pieces.empty() || choice < 4) {
      const std::size_t size = someSize();
      auto* at = static_cast<std::uint8_t*>(heap.Allocate(size));
      if (at == nullptr) {
        ++refused;
      } else {
        const auto fill = static_cast<std::uint8_t>(step);
        std::memset(at, fill, size);
        pieces.push_back({at, size, fill});
      }
    } else if (choice < 7) {
      const std::size_t which = below(pieces.size());
      ASSERT_TRUE(Holds(pieces[which].at, pieces[which].size, pieces[which].fill)) << step;
      heap.Free(pieces[which].at);
      pieces[which] = pieces.back();
      pieces.pop_back();
    } else {
      Piece& piece = pieces[below(pieces.size())];
      const std::size_t size = someSize();
      auto* at = static_cast<std::uint8_t*>(heap.Resize(piece.at, size));
      if (at == nullptr) {
        ++refused;
        ASSERT_TRUE(Holds(piece.at, piece.size, piece.fill)) << step;
      } else {
        ASSERT_TRUE(Holds(at, std::min(size, piece.size), piece.fill)) << step;
        const auto fill = static_cast<std::uint8_t>(step);
        std::memset(at, fill, size);
        piece = {at, size, fill};
      }
    }
    if (step % 16 == 0) {
      ASSERT_TRUE(heap.IsConsistent()) << step;
      ASSERT_EQ(heap.LivePieces(), pieces.size()) << step;
      ASSERT_LE(heap.Used(), heap.Capacity());
    }
  }
  EXPECT_GT(refused, 10u) << "the run should have filled the heap now and then";
  EXPECT_EQ(heap.FailureCount(), refused);
  EXPECT_EQ(heap.MisuseCount(), 0u);
  for (const Piece& piece : pieces) {
    ASSERT_TRUE(Holds(piece.at, piece.size, piece.fill));
    heap.Free(piece.at);
  }
  EXPECT_EQ(heap.Used(), 0u);
  EXPECT_EQ(heap.LargestFree(), heap.Capacity() - kOverhead);
  EXPECT_TRUE(heap.IsConsistent());
}

#if defined(BN_REGION_HEAP_GUARDS)
// In a build with AddressSanitizer, what has not been given out may not be
// touched, so that a library which runs off the end of its memory is
// caught here as it would be on the general heap.
using RegionHeapDeathTest = testing::Test;

TEST_F(RegionHeapDeathTest, CatchesAWritePastTheEndOfAPiece) {
  HeapFixture fixture;
  auto* piece = static_cast<volatile std::uint8_t*>(fixture.heap.Allocate(20));
  ASSERT_NE(piece, nullptr);
  piece[19] = 1;  // The last byte asked for.
  EXPECT_DEATH(piece[20] = 1, "use-after-poison");
  EXPECT_DEATH(piece[31] = 1, "use-after-poison");
}

TEST_F(RegionHeapDeathTest, CatchesAPieceUsedAfterItWasGivenBackOrShrunk) {
  HeapFixture fixture;
  auto* piece = static_cast<volatile std::uint8_t*>(fixture.heap.Allocate(100));
  auto* shrunk = static_cast<volatile std::uint8_t*>(fixture.heap.Allocate(100));
  ASSERT_NE(shrunk, nullptr);
  fixture.heap.Free(const_cast<std::uint8_t*>(piece));
  EXPECT_DEATH(piece[0] = 1, "use-after-poison");
  ASSERT_EQ(fixture.heap.Resize(const_cast<std::uint8_t*>(shrunk), 10), shrunk);
  shrunk[9] = 1;
  EXPECT_DEATH(shrunk[10] = 1, "use-after-poison");
  // Grown again where it is, the bytes are its own once more.
  ASSERT_EQ(fixture.heap.Resize(const_cast<std::uint8_t*>(shrunk), 60), shrunk);
  shrunk[59] = 1;
  EXPECT_DEATH(shrunk[60] = 1, "use-after-poison");
}

TEST(RegionHeap, LeavesItsMemoryFreeToUseWhenItIsDestroyed) {
  alignas(8) std::array<std::uint8_t, 1024> memory{};
  {
    RegionHeap heap{memory};
    ASSERT_NE(heap.Allocate(100), nullptr);
  }
  std::memset(memory.data(), 0x5A, memory.size());
  EXPECT_EQ(memory[1000], 0x5A);
}
#endif

}  // namespace
