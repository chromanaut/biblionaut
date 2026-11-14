#include "core/region_heap.h"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(BN_REGION_HEAP_GUARDS)
#include <sanitizer/asan_interface.h>
#endif

namespace bn::core {

namespace {

constexpr std::uint32_t kHeaderBytes = 8;
// The smallest piece: a header and one unit of room.
constexpr std::uint32_t kSmallestPiece = kHeaderBytes + RegionHeap::kAlignment;
constexpr std::uint32_t kInUse = 1;
constexpr std::uint32_t kNowhere = std::numeric_limits<std::uint32_t>::max();

// Marks bytes as not to be touched, or as free to use again. Only a build
// with AddressSanitizer does anything here.
void Guard([[maybe_unused]] const std::uint8_t* from, [[maybe_unused]] std::size_t count) noexcept {
#if defined(BN_REGION_HEAP_GUARDS)
  ASAN_POISON_MEMORY_REGION(from, count);
#endif
}

void Unguard([[maybe_unused]] const std::uint8_t* from, [[maybe_unused]] std::size_t count) noexcept {
#if defined(BN_REGION_HEAP_GUARDS)
  ASAN_UNPOISON_MEMORY_REGION(from, count);
#endif
}

// Whether the bytes at `at` could be a piece's header. In a guarded build
// they could not if any of them is marked as not to be touched: a header
// never is. Asked before each header is read on the way along the pieces,
// in case something has written over one and the way leads astray.
bool MayBeHeader([[maybe_unused]] const std::uint8_t* at) noexcept {
#if defined(BN_REGION_HEAP_GUARDS)
  return __asan_region_is_poisoned(const_cast<std::uint8_t*>(at), kHeaderBytes) == nullptr;  // NOLINT(*-const-cast)
#else
  return true;
#endif
}

constexpr std::uint32_t SizeOf(std::uint32_t field) noexcept { return field & ~kInUse; }
constexpr bool IsInUse(std::uint32_t field) noexcept { return (field & kInUse) != 0; }

}  // namespace

RegionHeap::RegionHeap(std::span<std::uint8_t> memory) noexcept {
  // Start at a multiple of the alignment, and end at one.
  const auto address = reinterpret_cast<std::uintptr_t>(memory.data());  // NOLINT(*-reinterpret-cast): to align it.
  const std::size_t skipped = static_cast<std::size_t>((kAlignment - (address % kAlignment)) % kAlignment);
  if (memory.data() == nullptr || memory.size() < skipped + kSmallestPiece) {
    return;
  }
  const std::size_t usable = std::min<std::size_t>(memory.size() - skipped, std::numeric_limits<std::int32_t>::max());
  base_ = memory.data() + skipped;
  capacity_ = static_cast<std::uint32_t>(usable - (usable % kAlignment));
  Reset();
}

RegionHeap::~RegionHeap() {
  if (base_ != nullptr) {
    Unguard(base_, capacity_);
  }
}

void RegionHeap::Reset() noexcept {
  used_ = 0;
  highWaterSinceReset_ = 0;
  livePieces_ = 0;
  if (base_ == nullptr) {
    return;
  }
  Unguard(base_, kHeaderBytes);
  Write(0, Header{.size = capacity_, .previous = 0});
  Guard(base_ + kHeaderBytes, capacity_ - kHeaderBytes);
}

RegionHeap::Header RegionHeap::Read(std::uint32_t offset) const noexcept {
  Header header;
  std::memcpy(&header, base_ + offset, sizeof(header));
  return header;
}

void RegionHeap::Write(std::uint32_t offset, Header header) noexcept {
  static_assert(sizeof(Header) == kHeaderBytes);
  std::memcpy(base_ + offset, &header, sizeof(header));
}

void RegionHeap::NoteUsed(std::size_t used) noexcept {
  used_ = used;
  highWater_ = std::max(highWater_, used_);
  highWaterSinceReset_ = std::max(highWaterSinceReset_, used_);
}

void RegionHeap::TellNext(std::uint32_t offset) noexcept {
  const std::uint32_t size = SizeOf(Read(offset).size);
  if (offset + size < capacity_) {
    Header next = Read(offset + size);
    next.previous = size;
    Write(offset + size, next);
  }
}

void RegionHeap::Take(std::uint32_t offset, std::uint32_t needed) noexcept {
  Header header = Read(offset);
  const std::uint32_t size = SizeOf(header.size);
  if (size - needed >= kSmallestPiece) {
    // What is over becomes a free piece of its own.
    const std::uint32_t rest = offset + needed;
    Unguard(base_ + rest, kHeaderBytes);
    Write(rest, Header{.size = size - needed, .previous = needed});
    TellNext(rest);
    header.size = needed | kInUse;
  } else {
    header.size = size | kInUse;
  }
  Write(offset, header);
}

void* RegionHeap::Allocate(std::size_t size) noexcept {
  // The piece: a header, the bytes asked for rounded up, and in a guarded
  // build the bytes after them that nothing may touch.
  if (size > capacity_) {
    ++failureCount_;
    return nullptr;
  }
  const std::size_t rounded = std::max<std::size_t>(kAlignment, (size + kAlignment - 1) / kAlignment * kAlignment);
  const auto needed = static_cast<std::uint32_t>(rounded + kOverhead);
  for (std::uint32_t offset = 0; offset < capacity_;) {
    const Header header = Read(offset);
    const std::uint32_t pieceSize = SizeOf(header.size);
    if (!IsInUse(header.size) && pieceSize >= needed) {
      Take(offset, needed);
      NoteUsed(used_ + SizeOf(Read(offset).size));
      ++livePieces_;
      Unguard(base_ + offset + kHeaderBytes, size);
      return base_ + offset + kHeaderBytes;
    }
    offset += pieceSize;
  }
  ++failureCount_;
  return nullptr;
}

std::uint32_t RegionHeap::OffsetOf(const void* piece) const noexcept {
  if (base_ == nullptr) {
    return kNowhere;
  }
  // How far into the region the piece's header would be. The pointer may
  // be to anything at all, so the two are compared as numbers; and one
  // from before the first piece comes out, by the way the subtraction
  // wraps round, as further in than the region is long, so that a single
  // comparison turns away what is before the region and what is after.
  const auto address = reinterpret_cast<std::uintptr_t>(piece);               // NOLINT(*-reinterpret-cast)
  const auto first = reinterpret_cast<std::uintptr_t>(base_) + kHeaderBytes;  // NOLINT(*-reinterpret-cast)
  const std::uintptr_t distance = address - first;
  if (distance >= capacity_ - kHeaderBytes) {
    return kNowhere;
  }
  const auto offset = static_cast<std::uint32_t>(distance);
  if (offset % kAlignment != 0) {
    return kNowhere;
  }
  // Whether a piece starts there cannot be told from the bytes there. A
  // piece that joined the free space before it has left its header behind
  // inside that space, still saying "in use"; so has every piece from
  // before a reset; and whoever holds a piece may write in it what looks
  // like a header. What can be trusted is the headers as they follow one
  // another from the start of the region, so those are gone along until
  // they arrive at the place or pass it.
  std::uint32_t at = 0;
  while (at < offset) {
    if (!MayBeHeader(base_ + at)) {
      return kNowhere;  // Not a heap any more; IsConsistent() says so.
    }
    const std::uint32_t size = SizeOf(Read(at).size);
    if (size < kSmallestPiece || size > capacity_ - at) {
      return kNowhere;
    }
    at += size;
  }
  if (at != offset || !MayBeHeader(base_ + offset)) {
    return kNowhere;
  }
  const Header header = Read(offset);
  const std::uint32_t size = SizeOf(header.size);
  if (!IsInUse(header.size) || size < kSmallestPiece || size % kAlignment != 0 || size > capacity_ - offset) {
    return kNowhere;
  }
  return offset;
}

void RegionHeap::Free(void* piece) noexcept {
  if (piece == nullptr) {
    return;
  }
  std::uint32_t offset = OffsetOf(piece);
  if (offset == kNowhere) {
    ++misuseCount_;
    return;
  }
  Header header = Read(offset);
  std::uint32_t size = SizeOf(header.size);
  used_ -= size;
  --livePieces_;
  // Join the free space after it, and then the free space before it.
  if (offset + size < capacity_) {
    const Header next = Read(offset + size);
    if (!IsInUse(next.size)) {
      size += SizeOf(next.size);
    }
  }
  if (header.previous != 0) {
    const Header before = Read(offset - header.previous);
    if (!IsInUse(before.size)) {
      offset -= header.previous;
      size += SizeOf(before.size);
      header.previous = before.previous;
    }
  }
  header.size = size;
  Write(offset, header);
  TellNext(offset);
  Guard(base_ + offset + kHeaderBytes, size - kHeaderBytes);
}

void* RegionHeap::Resize(void* piece, std::size_t size) noexcept {
  if (piece == nullptr) {
    return Allocate(size);
  }
  const std::uint32_t offset = OffsetOf(piece);
  if (offset == kNowhere) {
    ++misuseCount_;
    return nullptr;
  }
  if (size > capacity_) {
    ++failureCount_;
    return nullptr;
  }
  const std::size_t rounded = std::max<std::size_t>(kAlignment, (size + kAlignment - 1) / kAlignment * kAlignment);
  const auto needed = static_cast<std::uint32_t>(rounded + kOverhead);
  Header header = Read(offset);
  const std::uint32_t current = SizeOf(header.size);
  auto* bytes = static_cast<std::uint8_t*>(piece);

  // Room to grow where it is, if the space after it is free.
  std::uint32_t available = current;
  if (offset + current < capacity_) {
    const Header next = Read(offset + current);
    if (!IsInUse(next.size)) {
      available += SizeOf(next.size);
    }
  }
  if (needed <= available) {
    // It stays. Take in the free space after it, if there is any, and
    // give back whatever is then over.
    header.size = available;
    Write(offset, header);
    TellNext(offset);
    Take(offset, needed);
    const std::uint32_t now = SizeOf(Read(offset).size);
    NoteUsed(used_ - current + now);
    if (now < available) {
      Guard(base_ + offset + now + kHeaderBytes, available - now - kHeaderBytes);
    }
    Guard(bytes, now - kHeaderBytes);
    Unguard(bytes, size);
    return piece;
  }

  // It has to move.
  void* fresh = Allocate(size);
  if (fresh == nullptr) {
    return nullptr;
  }
  const std::size_t held = current - kHeaderBytes;
  Unguard(bytes, held);
  std::memcpy(fresh, piece, std::min(held, size));
  Free(piece);
  return fresh;
}

std::size_t RegionHeap::LargestFree() const noexcept {
  std::uint32_t largest = 0;
  for (std::uint32_t offset = 0; offset < capacity_;) {
    if (!MayBeHeader(base_ + offset)) {
      break;  // Not a heap any more; IsConsistent() says so.
    }
    const Header header = Read(offset);
    const std::uint32_t size = SizeOf(header.size);
    if (size < kSmallestPiece || size > capacity_ - offset) {
      break;
    }
    if (!IsInUse(header.size)) {
      largest = std::max(largest, size);
    }
    offset += size;
  }
  return largest > kOverhead ? largest - kOverhead : 0;
}

bool RegionHeap::IsConsistent() const noexcept {
  if (base_ == nullptr) {
    return used_ == 0 && livePieces_ == 0;
  }
  std::size_t used = 0;
  std::uint32_t live = 0;
  std::uint32_t previous = 0;
  bool previousFree = false;
  std::uint32_t offset = 0;
  while (offset < capacity_) {
    if (!MayBeHeader(base_ + offset)) {
      return false;
    }
    const Header header = Read(offset);
    const std::uint32_t size = SizeOf(header.size);
    if (size < kSmallestPiece || size % kAlignment != 0 || size > capacity_ - offset || header.previous != previous) {
      return false;
    }
    if (IsInUse(header.size)) {
      used += size;
      ++live;
    } else if (previousFree) {
      return false;  // Two free pieces side by side should have been one.
    }
    previousFree = !IsInUse(header.size);
    previous = size;
    offset += size;
  }
  return offset == capacity_ && used == used_ && live == livePieces_;
}

}  // namespace bn::core
