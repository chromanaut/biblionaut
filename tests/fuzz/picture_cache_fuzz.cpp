// The fuzz target for the cache of pictures (draw/picture_cache.h), reading
// a file that the card holds.
//
// What it is given:
//
//   byte 0   bit 0: the header's own checksum is mended, and the picture
//                   looked for is the one the header says it is, so that
//                   the rows are read; otherwise one picture is looked for
//                   whatever the header says
//            bit 1: the checksum of the rows is mended as well
//            bits 2 to 4: how many rows are read at a call, less one
//   the rest the file, put where every picture would be looked for
//
// What has to hold, whatever the bytes are:
//
//   - reading ends in a status, and a call gives rows only when it says ok;
//   - another picture can be kept among those files, and read back the
//     same;
//   - no more rows are given than the picture has, and no file is left
//     open once reading has ended;
//   - when every row was given and the last call said ok, they are the
//     file's own rows, the header says it is the picture looked for, and
//     the rows' checksum is the header's;
//   - those rows, kept again as that picture, are read back the same.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/checksum.h"
#include "draw/picture_cache.h"
#include "fuzz_support.h"
#include "storage_fakes.h"

namespace {

using bn::draw::PictureCache;
using bn::draw::PictureCacheStatus;
using bn::draw::PictureKey;
using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view kFolder = "/.p";
constexpr std::size_t kHeaderChecked = 40;

std::uint32_t Get16(const Bytes& from, std::size_t at) {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}

std::uint32_t Get32(const Bytes& from, std::size_t at) { return Get16(from, at) | (Get16(from, at + 2) << 16U); }

void Put32(Bytes& to, std::size_t at, std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    to[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
  }
}

// What the header in `file` says the picture is.
PictureKey KeyIn(const Bytes& file) {
  return {.source = std::uint64_t{Get32(file, 16)} | (std::uint64_t{Get32(file, 20)} << 32U),
          .sourceSize = Get32(file, 24),
          .sourceModified = Get32(file, 28),
          .width = static_cast<std::uint16_t>(Get16(file, 8)),
          .height = static_cast<std::uint16_t>(Get16(file, 10)),
          .levels = static_cast<std::uint16_t>(Get16(file, 6)),
          .turn = file[12]};
}

// Storage with `file` in every slot.
void PutEverywhere(bn::test::FakeStorage& storage, const Bytes& file) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  for (std::size_t slot = 0; slot < PictureCache::kSlots; ++slot) {
    storage.Put(std::string(kFolder) + "/" + kDigits[slot / 16] + kDigits[slot % 16], file);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  Bytes file(rest.begin(), rest.end());

  PictureKey key = {.source = PictureKey::PathHash("/drawings/a.svg"),
                    .sourceSize = 100,
                    .sourceModified = 1,
                    .width = 20,
                    .height = 10,
                    .levels = 2,
                    .turn = 0};
  const bool mended = (choices & 1U) != 0 && file.size() >= PictureCache::kHeaderBytes;
  if (mended) {
    key = KeyIn(file);
    const std::size_t rowsAt = PictureCache::kHeaderBytes;
    const std::size_t rowBytes = PictureCache::RowBytesFor(key);
    if ((choices & 2U) != 0 && PictureCache::BitsFor(key.levels) != 0) {
      const std::size_t rowsEnd = std::min(file.size(), rowsAt + (rowBytes * key.height));
      Put32(file, 32, bn::core::Crc32(std::span(file).subspan(rowsAt, rowsEnd - rowsAt)));
    }
    Put32(file, kHeaderChecked, bn::core::Crc32(std::span(file).first(kHeaderChecked)));
  }
  const std::size_t rowsAtATime = 1 + ((choices >> 2U) & 7U);

  bn::test::FakeStorage storage;
  BN_FUZZ_CHECK(storage.Start() == bn::core::StorageStatus::kOk, "the storage did not start");
  PutEverywhere(storage, file);
  PictureCache cache(storage, kFolder);
  PictureCacheStatus status = cache.BeginRead(key);
  const std::size_t rowBytes = PictureCache::RowBytesFor(key);
  Bytes rows;
  while (status == PictureCacheStatus::kOk && cache.Rows() < key.height) {
    const std::size_t count = std::min<std::size_t>(rowsAtATime, key.height - cache.Rows());
    Bytes piece(count * rowBytes);
    const std::uint32_t before = cache.Rows();
    status = cache.ReadRows(piece);
    if (status == PictureCacheStatus::kOk) {
      BN_FUZZ_CHECK(cache.Rows() == before + count, "a call that said ok did not give the rows asked for");
      rows.insert(rows.end(), piece.begin(), piece.end());
    }
    BN_FUZZ_CHECK(cache.Rows() <= key.height, "more rows were given than the picture has");
  }
  BN_FUZZ_CHECK(status == PictureCacheStatus::kOk || status == PictureCacheStatus::kOther ||
                    status == PictureCacheStatus::kDamaged || status == PictureCacheStatus::kMisused,
                "reading ended with a status it should not have");
  BN_FUZZ_CHECK(storage.OpenCount() == 0, "a file was left open once reading had ended");

  // Whatever the files hold, another picture can be kept among them, in
  // place of one of them, and read back.
  {
    PictureKey another = key;
    another.source = PictureKey::PathHash("/drawings/another.svg");
    another.width = 9;
    another.height = 3;
    another.levels = 4;
    Bytes kept(PictureCache::RowBytesFor(another) * another.height);
    for (std::size_t at = 0; at < kept.size(); ++at) {
      kept[at] = static_cast<std::uint8_t>(at * 29);
    }
    Bytes back(kept.size());
    PictureCache beside(storage, kFolder);
    BN_FUZZ_CHECK(beside.BeginWrite(another) == PictureCacheStatus::kOk &&
                      beside.WriteRows(kept) == PictureCacheStatus::kOk &&
                      beside.FinishWrite() == PictureCacheStatus::kOk,
                  "a picture could not be kept among the files there");
    BN_FUZZ_CHECK(beside.BeginRead(another) == PictureCacheStatus::kOk &&
                      beside.ReadRows(back) == PictureCacheStatus::kOk && back == kept,
                  "a picture kept among the files there was not read back the same");
    BN_FUZZ_CHECK(storage.OpenCount() == 0, "a file was left open once keeping had ended");
  }
  if (status != PictureCacheStatus::kOk) {
    return 0;
  }

  // Read through: it is the file's own rows, of the picture looked for.
  BN_FUZZ_CHECK(file.size() == PictureCache::kHeaderBytes + rows.size(), "the rows given are not the file's");
  BN_FUZZ_CHECK(std::equal(rows.begin(), rows.end(), file.begin() + PictureCache::kHeaderBytes),
                "the rows given are not the file's");
  BN_FUZZ_CHECK(KeyIn(file) == key, "a picture was given back that the header says is another");
  BN_FUZZ_CHECK(Get32(file, 32) == bn::core::Crc32(rows), "rows were given whose checksum is not the header's");

  // Kept again, they come back the same.
  bn::test::FakeStorage again;
  BN_FUZZ_CHECK(again.Start() == bn::core::StorageStatus::kOk, "the storage did not start");
  PictureCache other(again, kFolder);
  BN_FUZZ_CHECK(other.BeginWrite(key) == PictureCacheStatus::kOk && other.WriteRows(rows) == PictureCacheStatus::kOk &&
                    other.FinishWrite() == PictureCacheStatus::kOk,
                "a picture read back could not be kept again");
  Bytes back(rows.size());
  BN_FUZZ_CHECK(other.BeginRead(key) == PictureCacheStatus::kOk && other.ReadRows(back) == PictureCacheStatus::kOk &&
                    back == rows,
                "a picture kept again was not read back the same");
  return 0;
}
