// The fuzz target for the heap inside a block (core/region_heap.h), driven
// directly: the XML target drives it as expat does, which is only ever
// correctly.
//
// What it is given:
//
//   byte 0   bits 0 to 2: how big the block is
//            bits 3 to 5: how far off a multiple of eight it starts
//   then     requests, three bytes each: what to do, and a number of two
//            bytes (low byte first) that says how big, or which piece
//
//   what to do, by the first byte's remainder on division by eight:
//     0, 1  ask for a piece
//     2     give a piece back
//     3     resize a piece
//     4     give back something that is not a piece in use: a piece given
//           back before, a place inside a piece, or somewhere else
//     5     resize such a thing
//     6     take everything back at once (only if the byte is 6 itself, so
//           that it is rare)
//     7     ask for a piece of no bytes
//
// What has to hold, whatever the requests are:
//
//   - a piece that is given out lies inside the block, at a multiple of
//     eight, and shares no byte with any other piece in use;
//   - a piece holds what was put in it until it is given back, and after a
//     resize as much of it as the shorter length;
//   - nothing outside the block is written;
//   - a mistake is counted, is refused, and changes nothing;
//   - the heap's records agree with one another after every request, and
//     its counts are the counts of what was asked;
//   - a request is refused only for want of room, and is counted;
//   - what it says is the largest piece it could give, it can give, and
//     nothing larger.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "core/region_heap.h"
#include "fuzz_support.h"

namespace {

using bn::core::RegionHeap;

constexpr std::array<std::size_t, 8> kBlockBytes = {4096, 1024, 256, 64, 24576, 40, 16, 0};
// Memory on either side of the block that the heap has no business in.
constexpr std::size_t kMargin = 64;
constexpr std::uint8_t kMarginFill = 0xC5;
// The most pieces held at once: a request for another gives one back
// instead.
constexpr std::size_t kMostPieces = 48;

struct Piece {
  std::uint8_t* at;
  std::size_t size;
  std::uint8_t fill;
};

bool Holds(const std::uint8_t* at, std::size_t size, std::uint8_t fill) {
  return std::all_of(at, at + size, [fill](std::uint8_t byte) { return byte == fill; });
}

class Run {
 public:
  Run(std::span<std::uint8_t> arena, std::span<std::uint8_t> block) : arena_(arena), block_(block), heap_(block) {}

  void Ask(std::size_t size, std::uint8_t fill) {
    if (pieces_.size() == kMostPieces) {
      GiveBack(size);
      return;
    }
    const std::uint32_t refusedBefore = heap_.FailureCount();
    const std::size_t largest = heap_.LargestFree();
    auto* at = static_cast<std::uint8_t*>(heap_.Allocate(size));
    if (at == nullptr) {
      BN_FUZZ_CHECK(heap_.FailureCount() == refusedBefore + 1, "a request that was refused was not counted");
      BN_FUZZ_CHECK(size > largest || largest == 0, "a request was refused that there was room for");
      return;
    }
    BN_FUZZ_CHECK(heap_.FailureCount() == refusedBefore, "a request that was met was counted as refused");
    BN_FUZZ_CHECK(size <= largest, "a piece was given that is larger than the largest there was room for");
    CheckPlace(at, size, nullptr);
    std::memset(at, fill, size);
    pieces_.push_back({at, size, fill});
  }

  void GiveBack(std::size_t which) {
    if (pieces_.empty()) {
      return;
    }
    which %= pieces_.size();
    const Piece piece = pieces_[which];
    BN_FUZZ_CHECK(Holds(piece.at, piece.size, piece.fill), "a piece no longer holds what was put in it");
    heap_.Free(piece.at);
    givenBack_.push_back(piece.at);
    pieces_[which] = pieces_.back();
    pieces_.pop_back();
  }

  void Resize(std::size_t which, std::size_t size, std::uint8_t fill) {
    if (pieces_.empty()) {
      return;
    }
    Piece& piece = pieces_[which % pieces_.size()];
    const std::uint32_t refusedBefore = heap_.FailureCount();
    auto* at = static_cast<std::uint8_t*>(heap_.Resize(piece.at, size));
    if (at == nullptr) {
      BN_FUZZ_CHECK(heap_.FailureCount() == refusedBefore + 1, "a resize that was refused was not counted");
      BN_FUZZ_CHECK(size > piece.size, "a piece could not be made smaller");
      BN_FUZZ_CHECK(Holds(piece.at, piece.size, piece.fill),
                    "a piece that could not be resized was not left as it was");
      return;
    }
    CheckPlace(at, size, &piece);
    BN_FUZZ_CHECK(Holds(at, std::min(size, piece.size), piece.fill), "a resized piece does not hold what it held");
    if (at != piece.at) {
      givenBack_.push_back(piece.at);
    }
    std::memset(at, fill, size);
    piece = {at, size, fill};
  }

  // Gives back, or resizes, something that is not a piece in use.
  void Mistake(std::size_t number, bool resize) {
    std::uint8_t* at = NotAPiece(number);
    if (at == nullptr || std::any_of(pieces_.begin(), pieces_.end(), [at](const Piece& p) { return p.at == at; })) {
      return;  // Nothing to get wrong with; or where it was is a piece again, which makes it no mistake.
    }
    const std::uint32_t mistakesBefore = heap_.MisuseCount();
    const std::size_t usedBefore = heap_.Used();
    if (resize) {
      BN_FUZZ_CHECK(heap_.Resize(at, number & 0xFFU) == nullptr, "what is not a piece in use was resized");
    } else {
      heap_.Free(at);
    }
    BN_FUZZ_CHECK(heap_.MisuseCount() == mistakesBefore + 1, "a mistake was not counted");
    BN_FUZZ_CHECK(heap_.Used() == usedBefore, "a mistake changed how much is in use");
    CheckEveryPiece();
  }

  void TakeAllBack() {
    CheckEveryPiece();
    heap_.Reset();
    for (const Piece& piece : pieces_) {
      givenBack_.push_back(piece.at);
    }
    pieces_.clear();
    BN_FUZZ_CHECK(heap_.Used() == 0, "a heap with everything taken back has something in use");
  }

  // What has to be so after every request.
  void Check() {
    BN_FUZZ_CHECK(heap_.IsConsistent(), "the heap's records do not agree with one another");
    BN_FUZZ_CHECK(heap_.LivePieces() == pieces_.size(), "the heap's count of pieces is not the count of pieces");
    std::size_t held = 0;
    for (const Piece& piece : pieces_) {
      held += piece.size;
    }
    BN_FUZZ_CHECK(heap_.Used() >= held && heap_.Used() <= heap_.Capacity() && heap_.HighWater() >= heap_.Used() &&
                      heap_.HighWaterSinceReset() <= heap_.HighWater(),
                  "the heap's count of what is in use cannot be right");
    BN_FUZZ_CHECK(heap_.Capacity() <= block_.size(), "the heap has more to give than its block holds");
    const std::uint8_t* blockEnd = block_.data() + block_.size();
    const std::uint8_t* arenaEnd = arena_.data() + arena_.size();
    BN_FUZZ_CHECK(Holds(arena_.data(), static_cast<std::size_t>(block_.data() - arena_.data()), kMarginFill) &&
                      Holds(blockEnd, static_cast<std::size_t>(arenaEnd - blockEnd), kMarginFill),
                  "something outside the heap's block was written");
  }

  // At the end: the largest piece it says it could give, it gives, and
  // none larger; and with everything given back it is whole again.
  void Finish() {
    const std::size_t largest = heap_.LargestFree();
    BN_FUZZ_CHECK(heap_.Allocate(largest + 1) == nullptr, "a piece larger than the largest was given");
    if (largest > 0) {
      auto* at = static_cast<std::uint8_t*>(heap_.Allocate(largest));
      BN_FUZZ_CHECK(at != nullptr, "the largest piece there is said to be room for was refused");
      CheckPlace(at, largest, nullptr);
      std::memset(at, 0x5A, largest);
      pieces_.push_back({at, largest, 0x5A});
    }
    Check();
    while (!pieces_.empty()) {
      GiveBack(0);
    }
    Check();
    BN_FUZZ_CHECK(heap_.Used() == 0, "with every piece given back, something is still in use");
    if (heap_.Capacity() != 0) {
      BN_FUZZ_CHECK(heap_.LargestFree() == heap_.Capacity() - RegionHeap::kOverhead,
                    "with every piece given back, the heap is not one free piece");
    }
  }

 private:
  // A piece just given out, of `size` bytes at `at`: inside the block, at
  // a multiple of eight, and clear of every other piece (`self` is the
  // piece it was before, for a resize).
  void CheckPlace(const std::uint8_t* at, std::size_t size, const Piece* self) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): to see how it is aligned.
    BN_FUZZ_CHECK(reinterpret_cast<std::uintptr_t>(at) % RegionHeap::kAlignment == 0, "a piece is not aligned");
    BN_FUZZ_CHECK(at >= block_.data() && size <= block_.size() &&
                      static_cast<std::size_t>(at - block_.data()) <= block_.size() - size,
                  "a piece is not inside the heap's block");
    for (const Piece& other : pieces_) {
      if (&other != self) {
        BN_FUZZ_CHECK(at + size <= other.at || other.at + other.size <= at, "two pieces in use share a byte");
      }
    }
  }

  void CheckEveryPiece() {
    for (const Piece& piece : pieces_) {
      BN_FUZZ_CHECK(Holds(piece.at, piece.size, piece.fill), "a piece no longer holds what was put in it");
    }
  }

  // Somewhere that is not the start of a piece in use, chosen by `number`.
  std::uint8_t* NotAPiece(std::size_t number) {
    const std::size_t kind = number % 4;
    number /= 4;
    if (kind == 0 && !givenBack_.empty()) {
      return givenBack_[number % givenBack_.size()];
    }
    if (kind == 1 && !pieces_.empty()) {
      // Inside a piece: any byte of it after the first.
      const Piece& piece = pieces_[number % pieces_.size()];
      return piece.size > 1 ? piece.at + 1 + ((number / pieces_.size()) % (piece.size - 1)) : nullptr;
    }
    if (kind == 2) {
      return arena_.data() + (number % kMargin);  // Before the block.
    }
    return arena_.data() + arena_.size() - 1 - (number % kMargin);  // After it.
  }

  std::span<std::uint8_t> arena_;
  std::span<std::uint8_t> block_;
  RegionHeap heap_;
  std::vector<Piece> pieces_;
  std::vector<std::uint8_t*> givenBack_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::size_t blockBytes = kBlockBytes[choices & 7U];
  const std::size_t offset = (choices >> 3U) & 7U;

  // The block, with a margin on either side.
  alignas(8) static std::array<std::uint8_t, kMargin + 8 + 24576 + kMargin> arena;
  std::fill(arena.begin(), arena.end(), kMarginFill);
  const std::span<std::uint8_t> used = std::span(arena).first(kMargin + offset + blockBytes + kMargin);
  Run run(used, used.subspan(kMargin + offset, blockBytes));
  run.Check();

  std::uint8_t fill = 0;
  for (std::span<const std::uint8_t> request = input.Take(3); request.size() == 3; request = input.Take(3)) {
    const std::size_t number = request[1] | (std::size_t{request[2]} << 8U);
    // Mostly the small things a parser keeps, now and then a buffer.
    const std::size_t bytes = (number & 0x8000U) != 0 ? number & 0x7FFFU : number & 0xFFU;
    ++fill;
    switch (request[0] % 8) {
      case 0:
      case 1:
        run.Ask(bytes, fill);
        break;
      case 2:
        run.GiveBack(number);
        break;
      case 3:
        run.Resize(number >> 8U, (number & 0x80U) != 0 ? (number & 0x7FU) * 64 : number & 0x7FU, fill);
        break;
      case 4:
        run.Mistake(number, false);
        break;
      case 5:
        run.Mistake(number, true);
        break;
      case 6:
        if (request[0] == 6) {
          run.TakeAllBack();
        }
        break;
      default:
        run.Ask(0, fill);
        break;
    }
    run.Check();
  }
  run.Finish();
  return 0;
}
