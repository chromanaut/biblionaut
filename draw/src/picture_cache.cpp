#include "draw/picture_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/checksum.h"

namespace bn::draw {

namespace {

// What each file begins with, and the form of what follows.
constexpr std::array<std::uint8_t, 4> kMagic = {'B', 'N', 'P', 'C'};
constexpr std::uint16_t kVersion = 1;
// Where things are in the header: what is checksummed, and its checksum.
constexpr std::size_t kHeaderChecked = 36;

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

// The header for `key`, with `crc` the checksum of its rows.
//
//   0  "BNPC"            12  turn, then 3 bytes of 0
//   4  version (1)       16  source (8 bytes)
//   6  levels            24  source's size
//   8  width             28  source's modified
//  10  height            32  checksum of the rows
//                        36  checksum of bytes 0 to 35, then 0 to the end
std::array<std::uint8_t, PictureCache::kHeaderBytes> Header(const PictureKey& key, std::uint32_t crc) noexcept {
  std::array<std::uint8_t, PictureCache::kHeaderBytes> header{};
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
  Put32(header, kHeaderChecked, core::Crc32(std::span(header).first(kHeaderChecked)));
  return header;
}

// A key the cache can keep: some pixels, levels it has bits for, and a
// file of fewer than 4 GB.
bool Keepable(const PictureKey& key) noexcept {
  return key.width > 0 && key.height > 0 && PictureCache::BitsFor(key.levels) != 0 &&
         (std::uint64_t{PictureCache::RowBytesFor(key)} * key.height) + PictureCache::kHeaderBytes <= UINT32_MAX;
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

std::string_view PictureCache::SlotPath(const PictureKey& key) noexcept {
  // The whole key decides the slot, so that one picture at two sizes, say,
  // need not take each other's place.
  const std::array<std::uint8_t, kHeaderBytes> header = Header(key, 0);
  const std::uint32_t slot = core::Crc32(std::span(header).first(32)) % kSlots;
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

PictureCacheStatus PictureCache::BeginWrite(const PictureKey& key) noexcept {
  Close();
  if (!Keepable(key) || folder_.size() > kLongestFolder) {
    return PictureCacheStatus::kMisused;
  }
  const core::StorageStatus made = storage_.MakeDirectory(folder_);
  if (made != core::StorageStatus::kOk && made != core::StorageStatus::kExists) {
    return PictureCacheStatus::kStoreFailed;
  }
  // The header's room, empty until the rows are all there.
  const std::array<std::uint8_t, kHeaderBytes> empty{};
  if (storage_.Open(SlotPath(key), core::OpenMode::kCreate, file_) != core::StorageStatus::kOk ||
      storage_.WriteAt(file_, 0, empty) != core::StorageStatus::kOk) {
    Close();
    return PictureCacheStatus::kStoreFailed;
  }
  mode_ = Mode::kWriting;
  key_ = key;
  rows_ = 0;
  crc_ = 0;
  return PictureCacheStatus::kOk;
}

PictureCacheStatus PictureCache::WriteRows(std::span<const std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = RowBytesFor(key_);
  if (mode_ != Mode::kWriting || rowBytes == 0 || rows.size() % rowBytes != 0 ||
      rows.size() / rowBytes > key_.height - rows_) {
    return PictureCacheStatus::kMisused;
  }
  const auto offset = static_cast<std::uint32_t>(kHeaderBytes + (std::size_t{rows_} * rowBytes));
  if (storage_.WriteAt(file_, offset, rows) != core::StorageStatus::kOk) {
    Close();
    return PictureCacheStatus::kStoreFailed;
  }
  crc_ = core::Crc32(rows, crc_);
  rows_ += static_cast<std::uint32_t>(rows.size() / rowBytes);
  return PictureCacheStatus::kOk;
}

PictureCacheStatus PictureCache::FinishWrite() noexcept {
  if (mode_ != Mode::kWriting || rows_ != key_.height) {
    return PictureCacheStatus::kMisused;
  }
  const bool written = storage_.WriteAt(file_, 0, Header(key_, crc_)) == core::StorageStatus::kOk;
  Close();
  return written ? PictureCacheStatus::kOk : PictureCacheStatus::kStoreFailed;
}

PictureCacheStatus PictureCache::BeginRead(const PictureKey& key) noexcept {
  Close();
  if (!Keepable(key) || folder_.size() > kLongestFolder) {
    return PictureCacheStatus::kMisused;
  }
  const core::StorageStatus opened = storage_.Open(SlotPath(key), core::OpenMode::kRead, file_);
  if (opened != core::StorageStatus::kOk) {
    Close();
    return opened == core::StorageStatus::kNotFound ? PictureCacheStatus::kMissing : PictureCacheStatus::kStoreFailed;
  }
  std::array<std::uint8_t, kHeaderBytes> header{};
  std::size_t read = 0;
  std::uint32_t size = 0;
  if (storage_.ReadAt(file_, 0, header, read) != core::StorageStatus::kOk ||
      storage_.Size(file_, size) != core::StorageStatus::kOk) {
    Close();
    return PictureCacheStatus::kStoreFailed;
  }
  const std::span<const std::uint8_t> got(header);
  if (read != kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), got.begin()) || Get16(got, 4) != kVersion ||
      Get32(got, kHeaderChecked) != core::Crc32(got.first(kHeaderChecked))) {
    Close();
    return PictureCacheStatus::kDamaged;
  }
  const PictureKey kept = {
      .source = std::uint64_t{Get32(got, 16)} | (std::uint64_t{Get32(got, 20)} << 32U),
      .sourceSize = Get32(got, 24),
      .sourceModified = Get32(got, 28),
      .width = static_cast<std::uint16_t>(Get16(got, 8)),
      .height = static_cast<std::uint16_t>(Get16(got, 10)),
      .levels = static_cast<std::uint16_t>(Get16(got, 6)),
      .turn = got[12],
  };
  if (!(kept == key)) {
    Close();
    return PictureCacheStatus::kOther;
  }
  if (size != kHeaderBytes + (RowBytesFor(key) * key.height)) {
    Close();
    return PictureCacheStatus::kDamaged;
  }
  mode_ = Mode::kReading;
  key_ = key;
  rows_ = 0;
  crc_ = 0;
  kept_ = Get32(got, 32);
  return PictureCacheStatus::kOk;
}

PictureCacheStatus PictureCache::ReadRows(std::span<std::uint8_t> rows) noexcept {
  const std::size_t rowBytes = RowBytesFor(key_);
  if (mode_ != Mode::kReading || rowBytes == 0 || rows.size() % rowBytes != 0 ||
      rows.size() / rowBytes > key_.height - rows_) {
    return PictureCacheStatus::kMisused;
  }
  const auto offset = static_cast<std::uint32_t>(kHeaderBytes + (std::size_t{rows_} * rowBytes));
  std::size_t read = 0;
  if (storage_.ReadAt(file_, offset, rows, read) != core::StorageStatus::kOk) {
    Close();
    return PictureCacheStatus::kStoreFailed;
  }
  if (read != rows.size()) {
    Close();
    return PictureCacheStatus::kDamaged;
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
