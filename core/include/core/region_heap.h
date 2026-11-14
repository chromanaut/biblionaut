#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Set in a build with AddressSanitizer, where the heap marks what it has
// not given out (see below).
#if defined(__SANITIZE_ADDRESS__)
#define BN_REGION_HEAP_GUARDS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BN_REGION_HEAP_GUARDS 1
#endif
#endif

namespace bn::core {

// A heap inside one block of memory that the caller provides. Pieces of it
// are given out, given back and resized, as malloc, free and realloc do,
// and it never touches the general heap.
//
// It is for code that must have those three calls, which is to say
// libraries written in C: the library is handed this in place of the
// general heap, and then what it can take is a number chosen beforehand,
// and whatever it does to the order of its memory stays in here. When the
// region is full a request fails, as malloc may, and the library deals
// with that as it would with the real thing.
//
// A piece costs kOverhead bytes beyond its own size rounded up to
// kAlignment. Finding room is a walk over the pieces, first fit: quick for
// the tens or hundreds of pieces a parser keeps, and not meant for many
// more. A piece given back joins any free space beside it at once.
//
// Giving a piece back, or resizing one, is a walk over the pieces as well,
// up to the one named: that is how the heap knows it has been handed a
// piece. Nothing in the bytes at a place can say so, since a piece that
// joined free space has left its old record inside it, and whoever holds a
// piece may write anything there. So a library's mistake with a pointer
// (given back twice, or kept across a reset, or pointing into the middle
// of something) is turned away and counted, in every build, and cannot
// make the heap give out memory it does not have.
//
// The counters are there so that the chosen size can be checked:
// HighWater() says how close its user came to the limit, and
// FailureCount() whether it was ever refused.
//
// In a build with AddressSanitizer, everything in the region that has not
// been given out is marked as not to be touched, and each piece is
// followed by eight such bytes, so a library that runs past the end of
// what it asked for is caught there as it would be on the general heap.
//
// Not thread safe: a region heap belongs to one task.
class RegionHeap {
 public:
  // Every piece starts at a multiple of this. It is enough for anything
  // made of integers of up to 64 bits and pointers.
  static constexpr std::size_t kAlignment = 8;

  // What a piece takes from the region beyond its size rounded up.
#if defined(BN_REGION_HEAP_GUARDS)
  static constexpr std::size_t kOverhead = 16;
#else
  static constexpr std::size_t kOverhead = 8;
#endif

  // `memory` must stay alive, and be left alone, for as long as the heap
  // is. Less than a few dozen bytes of it makes a heap that refuses
  // everything.
  explicit RegionHeap(std::span<std::uint8_t> memory) noexcept;

  RegionHeap(const RegionHeap&) = delete;
  RegionHeap& operator=(const RegionHeap&) = delete;
  ~RegionHeap();

  // Returns `size` bytes, or nullptr if there is nowhere they fit. Asking
  // for none gives a piece all the same, the smallest there is.
  [[nodiscard]] void* Allocate(std::size_t size) noexcept;

  // Gives a piece back. Nothing happens for nullptr, or for anything that
  // is not a piece in use (which is counted: MisuseCount()). One mistake
  // cannot be told from none: a piece given back, whose place has since
  // been given out again, is a piece in use.
  void Free(void* piece) noexcept;

  // Makes a piece `size` bytes long, keeping what it holds up to the
  // shorter of the two lengths, and returns where it now is: the same
  // place if it could stay. Returns nullptr, leaving the piece as it was,
  // if there is no room. Given nullptr, it is Allocate().
  [[nodiscard]] void* Resize(void* piece, std::size_t size) noexcept;

  // Takes every piece back at once. Pointers into the heap become invalid.
  void Reset() noexcept;

  // Bytes of the region that can be given out, overhead included.
  std::size_t Capacity() const noexcept { return capacity_; }
  // Bytes taken by the pieces in use, their overhead included.
  std::size_t Used() const noexcept { return used_; }
  // The most Used() has ever been, across resets; and the most it has
  // been since the last reset.
  std::size_t HighWater() const noexcept { return highWater_; }
  std::size_t HighWaterSinceReset() const noexcept { return highWaterSinceReset_; }
  // The largest size Allocate() could be asked for now and not refuse.
  std::size_t LargestFree() const noexcept;
  std::uint32_t LivePieces() const noexcept { return livePieces_; }
  // Requests refused for want of room.
  std::uint32_t FailureCount() const noexcept { return failureCount_; }
  // Calls of Free() or Resize() with something that was not a piece in use.
  std::uint32_t MisuseCount() const noexcept { return misuseCount_; }

  // Whether the heap's own records agree with one another. They always
  // should; a false here means something wrote where it had no business
  // to. For tests, and for assertions around code that is not trusted.
  bool IsConsistent() const noexcept;

 private:
  // What is kept in front of every piece, free or not.
  struct Header {
    std::uint32_t size = 0;      // Of the piece with this header; the lowest bit is set if it is in use.
    std::uint32_t previous = 0;  // Size of the piece before this one; 0 for the first.
  };

  Header Read(std::uint32_t offset) const noexcept;
  void Write(std::uint32_t offset, Header header) noexcept;
  // The offset of the header of the piece that starts at `piece`, or
  // kNowhere if no piece in use starts there. Found by going along the
  // pieces from the first.
  std::uint32_t OffsetOf(const void* piece) const noexcept;
  // Leaves the piece at `offset` exactly `needed` bytes, in use, making a
  // free piece of what is over if that is big enough to be one. The piece
  // must have at least `needed` bytes.
  void Take(std::uint32_t offset, std::uint32_t needed) noexcept;
  // Tells the piece after the one at `offset` how big the one before it is.
  void TellNext(std::uint32_t offset) noexcept;
  void NoteUsed(std::size_t used) noexcept;

  std::uint8_t* base_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::size_t used_ = 0;
  std::size_t highWater_ = 0;
  std::size_t highWaterSinceReset_ = 0;
  std::uint32_t livePieces_ = 0;
  std::uint32_t failureCount_ = 0;
  std::uint32_t misuseCount_ = 0;
};

}  // namespace bn::core
