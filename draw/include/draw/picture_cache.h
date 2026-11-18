#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/storage.h"

namespace bn::draw {

// What a picture kept in the cache is of, and as what: a drawing or a
// picture file, made a size and the panel's levels. Two are the same only
// if every part is.
struct PictureKey {
  std::uint64_t source = 0;          // The path of the file it was made from (PathHash()),
  std::uint32_t sourceSize = 0;      // its size,
  std::uint32_t sourceModified = 0;  // and when it was last written (core::FileInfo::modified).
  std::uint16_t width = 0;           // Pixels across, as kept,
  std::uint16_t height = 0;          // and rows.
  std::uint16_t levels = 0;          // The panel's levels: from 2 to 256.
  // However the caller says it lies on the panel: with a panel held
  // turned, a picture kept as it lies on the panel is turned with it.
  std::uint8_t turn = 0;

  bool operator==(const PictureKey& other) const noexcept = default;

  // A path, as a number to put in `source`: FNV-1a, 64 bits.
  static std::uint64_t PathHash(std::string_view path) noexcept;
};

// How something asked of the cache went.
enum class PictureCacheStatus : std::uint8_t {
  kOk,
  kMissing,      // Nothing is kept where that picture would be.
  kOther,        // Something else is kept there: another picture, or this one before its file changed.
  kDamaged,      // What is kept there is not whole, or not what was written.
  kStoreFailed,  // Storage did not do what it was asked.
  // Asked for what cannot be: a key with no pixels or levels out of range,
  // rows that are not whole or are more than the picture has, rows from no
  // picture begun, or a folder too long.
  kMisused,
};

// The name of a status, for diagnostics: "ok", "missing", "other",
// "damaged", "store failed", "misused".
const char* PictureCacheStatusName(PictureCacheStatus status) noexcept;

// Pictures made once, kept on storage, to be shown again without making
// them again: a drawing drawn, or a picture file read and scaled, at the
// size shown and in the panel's levels.
//
// It is a folder of kSlots files, each holding one picture. Which file a
// picture goes in is fixed by its key, so the cache never holds more than
// kSlots of them, and a picture put where another was takes its place.
// Each file is a header of kHeaderBytes, which says what the picture is
// and carries a checksum of itself and of the rows, and then the rows,
// RowBytesFor() each: a pixel is its level, 0 for black, packed into
// BitsFor() bits, the leftmost pixel in the top bits of the first byte.
//
// A picture is written row after row, and the header last, so a file left
// half written by a press, or by the power going, has no header and is
// found damaged. The file is not synced: what the card had not written
// when the power went is found damaged too, at the latest when the last
// row is read and the checksum is not what it says. A caller that finds a
// picture damaged makes it again.
//
// What is read is not trusted: whatever a file holds, reading it ends in a
// status, and gives no row that is not whole. One file is open at a time,
// between BeginWrite() or BeginRead() and Close(), FinishWrite() or the
// reading of the last row.
class PictureCache {
 public:
  static constexpr std::size_t kSlots = 64;
  static constexpr std::size_t kHeaderBytes = 64;
  // The longest folder it can keep pictures in: given a longer one, it
  // refuses everything.
  static constexpr std::size_t kLongestFolder = 64;

  // The bits a pixel takes in `levels` levels: 1, 2, 4 or 8. 0 for levels
  // it cannot keep.
  static constexpr unsigned BitsFor(std::uint32_t levels) noexcept {
    return levels < 2 ? 0 : levels <= 2 ? 1 : levels <= 4 ? 2 : levels <= 16 ? 4 : levels <= 256 ? 8 : 0;
  }
  // The bytes of each row of a picture with that key.
  static constexpr std::size_t RowBytesFor(const PictureKey& key) noexcept {
    return ((std::size_t{key.width} * BitsFor(key.levels)) + 7) / 8;
  }

  // Keeps the pictures in `folder` on `storage`, which it makes when it
  // first writes one. `folder` has to outlive the cache.
  PictureCache(core::Storage& storage, std::string_view folder) noexcept;
  ~PictureCache() { Close(); }
  PictureCache(const PictureCache&) = delete;
  PictureCache& operator=(const PictureCache&) = delete;

  // Begins keeping the picture of `key`, in place of whatever was kept
  // where it goes.
  [[nodiscard]] PictureCacheStatus BeginWrite(const PictureKey& key) noexcept;
  // Its next rows, whole: a multiple of RowBytesFor() bytes.
  [[nodiscard]] PictureCacheStatus WriteRows(std::span<const std::uint8_t> rows) noexcept;
  // Once every row is written: the header, and the file closed.
  [[nodiscard]] PictureCacheStatus FinishWrite() noexcept;

  // Finds the picture of `key`, and begins reading it. kOk only if what is
  // there says it is that picture, and is as large as it should be.
  [[nodiscard]] PictureCacheStatus BeginRead(const PictureKey& key) noexcept;
  // Its next rows, whole, filling `rows`: a multiple of RowBytesFor()
  // bytes. kDamaged if they are not all there, or, with the last row, if
  // the rows are not what was written.
  [[nodiscard]] PictureCacheStatus ReadRows(std::span<std::uint8_t> rows) noexcept;

  // The rows written or read so far.
  std::uint32_t Rows() const noexcept { return rows_; }

  // Ends whatever was begun. A picture not finished is left without its
  // header: found damaged if it is looked for.
  void Close() noexcept;

 private:
  enum class Mode : std::uint8_t { kNone, kWriting, kReading };

  // Where the picture of `key` goes: the folder, a '/' and two hex digits.
  std::string_view SlotPath(const PictureKey& key) noexcept;

  core::Storage& storage_;
  std::string_view folder_;
  std::array<char, kLongestFolder + 4> path_{};
  core::FileHandle file_ = core::kNoFile;
  Mode mode_ = Mode::kNone;
  PictureKey key_;
  std::uint32_t rows_ = 0;
  std::uint32_t crc_ = 0;   // Of the rows so far,
  std::uint32_t kept_ = 0;  // and as the header says, when reading.
};

}  // namespace bn::draw
