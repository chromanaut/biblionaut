#include "draw/picture_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/checksum.h"

namespace bn::draw {

namespace {

// What each file begins with, and the form of what follows.
constexpr std::array<std::uint8_t, 4> kMagic = {'B', 'N', 'P', 'C'};
constexpr std::uint16_t kVersion = 3;
// Where things are in the header: what is checksummed, and its checksum,
// which is the last of what it says.
constexpr std::size_t kHeaderChecked = 40;
constexpr std::size_t kHeaderSaid = kHeaderChecked + 4;
// The first bytes of a header say what the picture is, and nothing else:
// they choose its set.
constexpr std::size_t kKeyBytes = 32;

void Put16(std::span<std::uint8_t> to, std::size_t at, std::uint32_t value) noexcept {
  to[at] = static_cast<std::uint8_t>(value);
  to[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void Put32(std::span<std::uint8_t> to, std::size_t at, std::uint32_t value) noexcept {
  Put16(to, at, value & 0xFFFFU);
  Put16(to, at + 2, value >> 16U);
}

std::uint32_t Get16(std::span<const std::uint8_t> from, std::size_t at) noexcept {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}

std::uint32_t Get32(std::span<const std::uint8_t> from, std::size_t at) noexcept {
  return Get16(from, at) | (Get16(from, at + 2) << 16U);
}

// What the header for `key` says, with `crc` the checksum of its rows and
// `order` when it was kept. The rest of the header's sector is 0.
//
//   0  "BNPC"            12  turn, then 3 bytes of 0
//   4  version (3)       16  source (8 bytes)
//   6  levels            24  source's size
//   8  width             28  source's modified
//  10  height            32  checksum of the rows
//                        36  order
//                        40  checksum of bytes 0 to 39
std::array<std::uint8_t, kHeaderSaid> Header(const PictureKey& key, std::uint32_t crc, std::uint32_t order) noexcept {
  std::array<std::uint8_t, kHeaderSaid> header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  Put16(header, 4, kVersion);
  Put16(header, 6, key.levels);
  Put16(header, 8, key.width);
  Put16(header, 10, key.height);
  header[12] = key.turn;
  Put32(header, 16, static_cast<std::uint32_t>(key.source));
  Put32(header, 20, static_cast<std::uint32_t>(key.source >> 32U));
  Put32(header, 24, key.sourceSize);
  Put32(header, 28, key.sourceModified);
  Put32(header, 32, crc);
  Put32(header, 36, order);
  Put32(header, kHeaderChecked, core::Crc32(std::span(header).first(kHeaderChecked)));
  return header;
}

// A key the cache can keep: some pixels, levels it has bits for, and a
// file of fewer than 4 GB.
bool Keepable(const PictureKey& key) noexcept {
  return key.width > 0 && key.height > 0 && PictureCache::BitsFor(key.levels) != 0 &&
         (std::uint64_t{PictureCache::RowBytesFor(key)} * key.height) + PictureCache::kHeaderBytes <= UINT32_MAX;
}

// Whether `a` was kept after `b`: the count may have gone round.
bool Later(std::uint32_t a, std::uint32_t b) noexcept { return a != b && a - b < 0x80000000U; }

// How bad a status found in a set is, to say the worst of them.
int Badness(PictureCacheStatus status) noexcept {
  switch (status) {
    case PictureCacheStatus::kStoreFailed:
      return 3;
    case PictureCacheStatus::kDamaged:
      return 2;
    case PictureCacheStatus::kOther:
      return 1;
    default:
      return 0;
  }
}

}  // namespace

const char* PictureCacheStatusName(PictureCacheStatus status) noexcept {
  switch (status) {
    case PictureCacheStatus::kOk:
      return "ok";
    case PictureCacheStatus::kMissing:
      return "missing";
    case PictureCacheStatus::kOther:
      return "other";
    case PictureCacheStatus::kDamaged:
      return "damaged";
    case PictureCacheStatus::kStoreFailed:
      return "store failed";
    case PictureCacheStatus::kMisused:
      return "misused";
  }
  return "unknown";
}

std::uint64_t PictureKey::PathHash(std::string_view path) noexcept {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const char c : path) {
    hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ULL;
  }
  return hash;
}

PictureCache::PictureCache(core::Storage& storage, std::string_view folder) noexcept
    : storage_(storage), folder_(folder) {}

void PictureCache::GatherIn(std::span<std::uint8_t> buffer) noexcept {
  Close();
  buffer_ = buffer.first(buffer.size() - (buffer.size() % kSectorBytes));
  gathered_ = 0;
  given_ = 0;
}

std::size_t PictureCache::HomeOf(const PictureKey& key) noexcept {
  // The whole key decides the home, so that one picture at two sizes, say,
  // need not have the same one.
  const std::array<std::uint8_t, kHeaderSaid> header = Header(key, 0, 0);
  return core::Crc32(std::span(header).first(kKeyBytes)) % kSlots;
}

std::string_view PictureCache::SlotPath(std::size_t home, std::size_t place) noexcept {
  const std::size_t slot = (home + place) % kSlots;
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::copy(folder_.begin(), folder_.end(), path_.begin());
  std::size_t at = folder_.size();
  path_[at++] = '/';
  path_[at++] = kDigits[slot / 16];
  path_[at++] = kDigits[slot % 16];
  return {path_.data(), at};
}

void PictureCache::Close() noexcept {
  if (file_ != core::kNoFile) {
    storage_.Close(file_);
    file_ = core::kNoFile;
  }
  mode_ = Mode::kNone;
}

PictureCache::Held PictureCache::Look(std::string_view path) noexcept {
  Held held;
  const core::StorageStatus opened = storage_.Open(path, core::OpenMode::kRead, file_);
  if (opened != core::StorageStatus::kOk) {
    Close();
    held.status =
        opened == core::StorageStatus::kNotFound ? PictureCacheStatus::kMissing : PictureCacheStatus::kStoreFailed;
    return held;
  }
  // The header's whole sector, into the buffer, which holds nothing now.
  const std::span<std::uint8_t> header = buffer_.first(kHeaderBytes);
  std::size_t read = 0;
  if (storage_.ReadAt(file_, 0, header, read) != core::StorageStatus::kOk ||
      storage_.Size(file_, held.size) != core::StorageStatus::kOk) {
    Close();
    held.status = PictureCacheStatus::kStoreFailed;
    return held;
  }
  const std::span<const std::uint8_t> got(header);
  if (read != kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), got.begin()) || Get16(got, 4) != kVersion ||
      Get32(got, kHeaderChecked) != core::Crc32(got.first(kHeaderChecked))) {
    Close();
    held.status = PictureCacheStatus::kDamaged;
    return held;
  }
  held.status = PictureCacheStatus::kOk;
  held.key = {
      .source = std::uint64_t{Get32(got, 16)} | (std::uint64_t{Get32(got, 20)} << 32U),
      .sourceSize = Get32(got, 24),
      .sourceModified = Get32(got, 28),
      .width = static_cast<std::uint16_t>(Get16(got, 8)),
      .height = static_cast<std::uint16_t>(Get16(got, 10)),
      .levels = static_cast<std::uint16_t>(Get16(got, 6)),
      .turn = got[12],
  };
  held.rowsCrc = Get32(got, 32);
  held.order = Get32(got, 36);
  if (!seen_ || Later(held.order, latest_)) {
    latest_ = held.order;
    seen_ = true;
  }
  return held;
}

PictureCacheStatus PictureCache::BeginWrite(const PictureKey& key) noexcept {
  Close();
  if (!Keepable(key) || folder_.size() > kLongestFolder || buffer_.empty()) {
    return PictureCacheStatus::kMisused;
  }
  const core::StorageStatus made = storage_.MakeDirectory(folder_);
  if (made != core::StorageStatus::kOk && made != core::StorageStatus::kExists) {
    return PictureCacheStatus::kStoreFailed;
  }
  // Which place: the one this picture was kept in, else the first that
  // holds nothing whole, else the one kept longest ago. Nothing is kept
  // past a place with no file, so the looking stops there.
  const std::size_t home = HomeOf(key);
  std::size_t own = kPlaces;
  std::size_t empty = kPlaces;
  std::size_t oldest = kPlaces;
  std::uint32_t oldestOrder = 0;
  for (std::size_t place = 0; place < kPlaces && own == kPlaces; ++place) {
    const Held held = Look(SlotPath(home, place));
    Close();
    if (held.status != PictureCacheStatus::kOk) {
      empty = std::min(empty, place);
      if (held.status == PictureCacheStatus::kMissing) {
        break;
      }
    } else if (held.key == key) {
      own = place;
    } else if (oldest == kPlaces || Later(oldestOrder, held.order)) {
      oldest = place;
      oldestOrder = held.order;
    }
  }
  const std::size_t chosen = own != kPlaces ? own : empty != kPlaces ? empty : oldest;
  // Kept after everything it has seen.
  order_ = seen_ ? latest_ + 1 : 0;
  // The header's sector, empty until the rows are all there.
  const std::span<std::uint8_t> nothing = buffer_.first(kHeaderBytes);
  std::fill(nothing.begin(), nothing.end(), std::uint8_t{0});
  if (storage_.Open(SlotPath(home, chosen), core::OpenMode::kCreate, file_) != core::StorageStatus::kOk ||
      storage_.WriteAt(file_, 0, nothing) != core::StorageStatus::kOk) {
    Close();
    return PictureCacheStatus::kStoreFailed;
  }
  latest_ = order_;
  seen_ = true;
  mode_ = Mode::kWriting;
  key_ = key;
  rows_ = 0;
  crc_ = 0;
  gathered_ = 0;
  position_ = static_cast<std::uint32_t>(kHeaderBytes);
  return PictureCacheStatus::kOk;
}

bool PictureCache::Flush() noexcept {
  if (gathered_ == 0) {
    return true;
  }
  if (storage_.WriteAt(file_, position_, buffer_.first(gathered_)) != core::StorageStatus::kOk) {
    return false;
  }
  position_ += static_cast<std::uint32_t>(gathered_);
  gathered_ = 0;
  return true;
}

PictureCacheStatus PictureCache::WriteRows(std::span<const std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = RowBytesFor(key_);
  if (mode_ != Mode::kWriting || rowBytes == 0 || rows.size() % rowBytes != 0 ||
      rows.size() / rowBytes > key_.height - rows_) {
    return PictureCacheStatus::kMisused;
  }
  crc_ = core::Crc32(rows, crc_);
  rows_ += static_cast<std::uint32_t>(rows.size() / rowBytes);
  // Into the buffer, which goes to storage each time it is full: whole
  // sectors, begun where a sector begins.
  while (!rows.empty()) {
    const std::size_t count = std::min(rows.size(), buffer_.size() - gathered_);
    std::memcpy(buffer_.data() + gathered_, rows.data(), count);
    gathered_ += count;
    rows = rows.subspan(count);
    if (gathered_ == buffer_.size() && !Flush()) {
      Close();
      return PictureCacheStatus::kStoreFailed;
    }
  }
  return PictureCacheStatus::kOk;
}

PictureCacheStatus PictureCache::FinishWrite() noexcept {
  if (mode_ != Mode::kWriting || rows_ != key_.height) {
    return PictureCacheStatus::kMisused;
  }
  // What is left of the rows, which is the one write that ends part way
  // through a sector; and then the header's sector.
  bool written = Flush();
  if (written) {
    const std::array<std::uint8_t, kHeaderSaid> said = Header(key_, crc_, order_);
    const std::span<std::uint8_t> header = buffer_.first(kHeaderBytes);
    std::fill(header.begin(), header.end(), std::uint8_t{0});
    std::copy(said.begin(), said.end(), header.begin());
    written = storage_.WriteAt(file_, 0, header) == core::StorageStatus::kOk;
  }
  Close();
  return written ? PictureCacheStatus::kOk : PictureCacheStatus::kStoreFailed;
}

PictureCacheStatus PictureCache::BeginRead(const PictureKey& key) noexcept {
  Close();
  if (!Keepable(key) || folder_.size() > kLongestFolder || buffer_.empty()) {
    return PictureCacheStatus::kMisused;
  }
  const std::size_t home = HomeOf(key);
  PictureCacheStatus worst = PictureCacheStatus::kMissing;
  for (std::size_t place = 0; place < kPlaces; ++place) {
    const Held held = Look(SlotPath(home, place));
    PictureCacheStatus found = held.status;
    if (found == PictureCacheStatus::kMissing) {
      break;  // Nothing is kept past a place with no file.
    }
    if (found == PictureCacheStatus::kOk) {
      if (!(held.key == key)) {
        found = PictureCacheStatus::kOther;
      } else if (held.size != kHeaderBytes + (RowBytesFor(key) * key.height)) {
        // This picture, but not all of it there: no other file holds it.
        Close();
        return PictureCacheStatus::kDamaged;
      } else {
        mode_ = Mode::kReading;
        key_ = key;
        rows_ = 0;
        crc_ = 0;
        kept_ = held.rowsCrc;
        gathered_ = 0;
        given_ = 0;
        position_ = static_cast<std::uint32_t>(kHeaderBytes);
        return PictureCacheStatus::kOk;
      }
      Close();
    }
    if (Badness(found) > Badness(worst)) {
      worst = found;
    }
  }
  return worst;
}

PictureCacheStatus PictureCache::ReadRows(std::span<std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = RowBytesFor(key_);
  if (mode_ != Mode::kReading || rowBytes == 0 || rows.size() % rowBytes != 0 ||
      rows.size() / rowBytes > key_.height - rows_) {
    return PictureCacheStatus::kMisused;
  }
  // Out of the buffer, which is filled from storage each time it is
  // empty: as many whole sectors as it holds, or what is left of the file.
  const auto end = static_cast<std::uint32_t>(kHeaderBytes + (rowBytes * key_.height));
  for (std::span<std::uint8_t> left = rows; !left.empty();) {
    if (given_ == gathered_) {
      const std::size_t wanted = std::min<std::size_t>(buffer_.size(), end - position_);
      std::size_t read = 0;
      if (storage_.ReadAt(file_, position_, buffer_.first(wanted), read) != core::StorageStatus::kOk) {
        Close();
        return PictureCacheStatus::kStoreFailed;
      }
      if (wanted == 0 || read != wanted) {
        Close();
        return PictureCacheStatus::kDamaged;
      }
      position_ += static_cast<std::uint32_t>(wanted);
      gathered_ = wanted;
      given_ = 0;
    }
    const std::size_t count = std::min(left.size(), gathered_ - given_);
    std::memcpy(left.data(), buffer_.data() + given_, count);
    given_ += count;
    left = left.subspan(count);
  }
  crc_ = core::Crc32(rows, crc_);
  rows_ += static_cast<std::uint32_t>(rows.size() / rowBytes);
  if (rows_ == key_.height) {
    const bool whole = crc_ == kept_;
    Close();
    return whole ? PictureCacheStatus::kOk : PictureCacheStatus::kDamaged;
  }
  return PictureCacheStatus::kOk;
}

}  // namespace bn::draw
