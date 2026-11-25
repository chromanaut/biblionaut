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
//            bits 5 and 6: how much memory the cache gathers rows in: one
//                   sector, two and a part of one, three, or eight and a
//                   few bytes
//   the rest the file, put where every picture would be looked for
//
// What has to hold, whatever the bytes are:
//
//   - reading ends in a status, and a call gives rows only when it says ok;
//   - another picture can be kept among those files, and read back the
//     same: one of more rows than the memory holds, as many as the file
//     is long, given and taken the same few rows at a call; a page's width
//     of it, or for a file of an odd length rows of 13 bytes, which end at
//     every place in a sector there is;
//   - a cache given no memory to gather in keeps and reads nothing;
//   - keeping it and reading it back, storage is written and read from
//     where a sector begins, in whole sectors but for the last of the
//     rows; what the header leaves of its sector is nothing; and a
//     picture given up part way through is not found whole;
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
  constexpr std::array<std::size_t, 4> kMemory = {512, 1124, 1536, 4103};
  Bytes memory(kMemory[(choices >> 5U) & 3U]);

  bn::test::FakeStorage storage;
  BN_FUZZ_CHECK(storage.Start() == bn::core::StorageStatus::kOk, "the storage did not start");
  PutEverywhere(storage, file);
  PictureCache cache(storage, kFolder);
  cache.GatherIn(memory);
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
  // place of one of them, and read back: a page's width of it, and as many
  // rows as the file is long, which is more than the memory holds as often
  // as not.
  {
    const std::size_t sector = PictureCache::kSectorBytes;
    PictureKey another = key;
    another.source = PictureKey::PathHash("/drawings/another.svg");
    if (file.size() % 2 == 0) {
      another.width = 480;
      another.height = static_cast<std::uint16_t>(3 + (file.size() % 61));
      another.levels = 4;
    } else {
      another.width = 100;
      another.height = static_cast<std::uint16_t>(60 + ((file.size() % 61) * 9));
      another.levels = 2;
    }
    const std::size_t anotherRow = PictureCache::RowBytesFor(another);
    Bytes kept(anotherRow * another.height);
    for (std::size_t at = 0; at < kept.size(); ++at) {
      kept[at] = static_cast<std::uint8_t>((at * 29) + (at >> 8U));
    }
    PictureCache bare(storage, kFolder);
    BN_FUZZ_CHECK(bare.BeginWrite(another) == PictureCacheStatus::kMisused &&
                      bare.BeginRead(another) == PictureCacheStatus::kMisused,
                  "a cache with no memory to gather in did not refuse");
    PictureCache beside(storage, kFolder);
    beside.GatherIn(memory);

    // Given up part way through, it is not there to be read.
    BN_FUZZ_CHECK(beside.BeginWrite(another) == PictureCacheStatus::kOk &&
                      beside.WriteRows(std::span(kept).first(2 * anotherRow)) == PictureCacheStatus::kOk,
                  "a picture could not be begun among the files there");
    beside.Close();
    BN_FUZZ_CHECK(beside.BeginRead(another) != PictureCacheStatus::kOk, "a picture given up part way was found whole");
    beside.Close();

    storage.written.clear();
    BN_FUZZ_CHECK(beside.BeginWrite(another) == PictureCacheStatus::kOk,
                  "a picture could not be kept among the files there");
    for (std::size_t at = 0; at < kept.size(); at += rowsAtATime * anotherRow) {
      const std::size_t count = std::min(rowsAtATime * anotherRow, kept.size() - at);
      BN_FUZZ_CHECK(beside.WriteRows(std::span(kept).subspan(at, count)) == PictureCacheStatus::kOk,
                    "a picture's rows could not be kept among the files there");
    }
    BN_FUZZ_CHECK(beside.FinishWrite() == PictureCacheStatus::kOk, "a picture could not be finished among the files");
    // The header's sector, the rows, the header's sector again: each begun
    // where a sector begins, and whole sectors but for the last of the rows.
    BN_FUZZ_CHECK(storage.written.size() >= 3, "a picture was kept in fewer writes than it can be");
    for (std::size_t call = 0; call < storage.written.size(); ++call) {
      BN_FUZZ_CHECK(storage.written[call].offset % sector == 0, "a write did not begin where a sector begins");
      BN_FUZZ_CHECK(call + 2 == storage.written.size() || storage.written[call].size % sector == 0,
                    "a write that is not the last of the rows was not whole sectors");
    }
    std::size_t found = 0;
    for (const auto& [path, entry] : storage.entries) {
      if (entry.data.size() == sector + kept.size() && Get32(entry.data, 16) == (another.source & 0xFFFFFFFFU) &&
          Get32(entry.data, 20) == (another.source >> 32U) && Get16(entry.data, 8) == another.width) {
        ++found;
        BN_FUZZ_CHECK(std::all_of(entry.data.begin() + 44, entry.data.begin() + static_cast<std::ptrdiff_t>(sector),
                                  [](std::uint8_t byte) { return byte == 0; }),
                      "what the header leaves of its sector is not nothing");
      }
    }
    BN_FUZZ_CHECK(found >= 1, "the picture kept is in no file");

    storage.reads.clear();
    Bytes back;
    PictureCacheStatus reading = beside.BeginRead(another);
    while (reading == PictureCacheStatus::kOk && beside.Rows() < another.height) {
      Bytes piece(std::min<std::size_t>(rowsAtATime, another.height - beside.Rows()) * anotherRow);
      reading = beside.ReadRows(piece);
      back.insert(back.end(), piece.begin(), piece.end());
    }
    BN_FUZZ_CHECK(reading == PictureCacheStatus::kOk && back == kept,
                  "a picture kept among the files there was not read back the same");
    for (const auto& call : storage.reads) {
      BN_FUZZ_CHECK(call.offset % sector == 0, "a read did not begin where a sector begins");
    }
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
  other.GatherIn(memory);
  BN_FUZZ_CHECK(other.BeginWrite(key) == PictureCacheStatus::kOk && other.WriteRows(rows) == PictureCacheStatus::kOk &&
                    other.FinishWrite() == PictureCacheStatus::kOk,
                "a picture read back could not be kept again");
  Bytes back(rows.size());
  BN_FUZZ_CHECK(other.BeginRead(key) == PictureCacheStatus::kOk && other.ReadRows(back) == PictureCacheStatus::kOk &&
                    back == rows,
                "a picture kept again was not read back the same");
  return 0;
}
