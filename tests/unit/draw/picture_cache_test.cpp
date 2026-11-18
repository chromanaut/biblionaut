#include "draw/picture_cache.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "storage_fakes.h"

namespace {

using bn::draw::PictureCache;
using bn::draw::PictureCacheStatus;
using bn::draw::PictureKey;
using bn::test::FakeStorage;
using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view kFolder = "/.pictures";

PictureKey Key() {
  return {.source = PictureKey::PathHash("/drawings/a.svg"),
          .sourceSize = 1234,
          .sourceModified = 56,
          .width = 13,
          .height = 5,
          .levels = 2,
          .turn = 1};
}

// Rows for `key`, each byte something of its own.
Bytes RowsFor(const PictureKey& key, std::uint8_t seed = 0) {
  Bytes rows(PictureCache::RowBytesFor(key) * key.height);
  for (std::size_t at = 0; at < rows.size(); ++at) {
    rows[at] = static_cast<std::uint8_t>((at * 37) + seed);
  }
  return rows;
}

// Keeps `rows` as the picture of `key`, a row at a time.
void Keep(PictureCache& cache, const PictureKey& key, const Bytes& rows) {
  ASSERT_EQ(cache.BeginWrite(key), PictureCacheStatus::kOk);
  const std::size_t rowBytes = PictureCache::RowBytesFor(key);
  for (std::size_t at = 0; at < rows.size(); at += rowBytes) {
    ASSERT_EQ(cache.WriteRows(std::span(rows).subspan(at, rowBytes)), PictureCacheStatus::kOk);
  }
  ASSERT_EQ(cache.FinishWrite(), PictureCacheStatus::kOk);
}

// Reads the picture of `key` back in pieces of `rowsAtATime` rows: the
// last status, and the bytes read until then.
PictureCacheStatus ReadBack(PictureCache& cache, const PictureKey& key, Bytes& rows, std::size_t rowsAtATime = 2) {
  rows.clear();
  PictureCacheStatus status = cache.BeginRead(key);
  const std::size_t rowBytes = PictureCache::RowBytesFor(key);
  while (status == PictureCacheStatus::kOk && cache.Rows() < key.height) {
    const std::size_t count = std::min<std::size_t>(rowsAtATime, key.height - cache.Rows());
    Bytes piece(count * rowBytes);
    status = cache.ReadRows(piece);
    if (status == PictureCacheStatus::kOk) {
      rows.insert(rows.end(), piece.begin(), piece.end());
    }
  }
  return status;
}

TEST(PictureCache, GivesBackThePictureItWasGiven) {
  for (const std::uint16_t levels : {std::uint16_t{2}, std::uint16_t{4}, std::uint16_t{16}, std::uint16_t{256}}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    PictureCache cache(storage, kFolder);
    PictureKey key = Key();
    key.levels = levels;
    const Bytes rows = RowsFor(key);
    Keep(cache, key, rows);
    EXPECT_EQ(storage.OpenCount(), 0);
    for (const std::size_t atATime : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
      Bytes read;
      EXPECT_EQ(ReadBack(cache, key, read, atATime), PictureCacheStatus::kOk) << levels;
      EXPECT_EQ(read, rows) << levels;
      EXPECT_EQ(storage.OpenCount(), 0);
    }
  }
}

TEST(PictureCache, PacksEachPixelInAsFewBitsAsItsLevelsNeed) {
  EXPECT_EQ(PictureCache::BitsFor(1), 0U);
  EXPECT_EQ(PictureCache::BitsFor(2), 1U);
  EXPECT_EQ(PictureCache::BitsFor(3), 2U);
  EXPECT_EQ(PictureCache::BitsFor(4), 2U);
  EXPECT_EQ(PictureCache::BitsFor(5), 4U);
  EXPECT_EQ(PictureCache::BitsFor(16), 4U);
  EXPECT_EQ(PictureCache::BitsFor(17), 8U);
  EXPECT_EQ(PictureCache::BitsFor(256), 8U);
  EXPECT_EQ(PictureCache::BitsFor(257), 0U);
  PictureKey key = Key();  // 13 across.
  EXPECT_EQ(PictureCache::RowBytesFor(key), 2U);
  key.levels = 4;
  EXPECT_EQ(PictureCache::RowBytesFor(key), 4U);
  key.levels = 200;
  EXPECT_EQ(PictureCache::RowBytesFor(key), 13U);
}

TEST(PictureCache, SaysWhenNothingIsKept) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  Bytes read;
  EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kMissing);
  EXPECT_EQ(storage.OpenCount(), 0);
}

TEST(PictureCache, APictureOfAnythingElseIsNotGivenBack) {
  // Each part of the key: another file, the same file changed, another
  // size, other levels, turned otherwise. Where it goes in the same file,
  // it is another picture; where it does not, there is none.
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  Keep(cache, Key(), RowsFor(Key()));
  std::vector<PictureKey> others(7, Key());
  others[0].source = PictureKey::PathHash("/drawings/b.svg");
  others[1].sourceSize = 1235;
  others[2].sourceModified = 57;
  others[3].width = 14;
  others[4].height = 6;
  others[5].levels = 4;
  others[6].turn = 3;
  for (const PictureKey& other : others) {
    Bytes read;
    const PictureCacheStatus status = ReadBack(cache, other, read);
    EXPECT_TRUE(status == PictureCacheStatus::kOther || status == PictureCacheStatus::kMissing)
        << bn::draw::PictureCacheStatusName(status);
    EXPECT_TRUE(read.empty());
  }
  Bytes read;
  EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kOk);
}

TEST(PictureCache, KeepsUpToItsSlotsAndOneTakesTheLastOnesPlace) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  std::vector<PictureKey> keys;
  for (std::uint32_t at = 0; at < 3 * PictureCache::kSlots; ++at) {
    PictureKey key = Key();
    key.source = PictureKey::PathHash("/drawings/" + std::to_string(at) + ".svg");
    keys.push_back(key);
    Keep(cache, key, RowsFor(key, static_cast<std::uint8_t>(at)));
  }
  std::size_t files = 0;
  for (const auto& [path, entry] : storage.entries) {
    files += path.starts_with(std::string(kFolder) + "/") ? 1 : 0;
  }
  EXPECT_LE(files, PictureCache::kSlots);
  // The last is there, and anything there is what was put there.
  std::size_t found = 0;
  for (std::uint32_t at = 0; at < keys.size(); ++at) {
    Bytes read;
    const PictureCacheStatus status = ReadBack(cache, keys[at], read);
    if (status == PictureCacheStatus::kOk) {
      ++found;
      EXPECT_EQ(read, RowsFor(keys[at], static_cast<std::uint8_t>(at)));
    } else {
      EXPECT_EQ(status, PictureCacheStatus::kOther);
    }
  }
  EXPECT_EQ(found, files);
  Bytes read;
  EXPECT_EQ(ReadBack(cache, keys.back(), read), PictureCacheStatus::kOk);
}

TEST(PictureCache, APictureNotFinishedIsFoundDamaged) {
  // Given up after some rows, as by a press, or the power gone: there is
  // no header.
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  const Bytes rows = RowsFor(Key());
  ASSERT_EQ(cache.BeginWrite(Key()), PictureCacheStatus::kOk);
  ASSERT_EQ(cache.WriteRows(std::span(rows).first(2 * PictureCache::RowBytesFor(Key()))), PictureCacheStatus::kOk);
  cache.Close();
  EXPECT_EQ(storage.OpenCount(), 0);
  Bytes read;
  EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kDamaged);
}

TEST(PictureCache, AFileThatIsNotWhatWasWrittenIsFoundDamaged) {
  const Bytes rows = RowsFor(Key());
  struct Harm {
    const char* what;
    std::size_t at;  // A byte changed, or
    bool cut;        // the file cut short there.
  };
  for (const Harm harm : {Harm{.what = "magic", .at = 0, .cut = false}, Harm{.what = "version", .at = 4, .cut = false},
                          Harm{.what = "a row's checksum", .at = 33, .cut = false},
                          Harm{.what = "the header's checksum", .at = 37, .cut = false},
                          Harm{.what = "the first row", .at = 64, .cut = false},
                          Harm{.what = "the last row", .at = 64 + rows.size() - 1, .cut = false},
                          Harm{.what = "cut in the header", .at = 30, .cut = true},
                          Harm{.what = "cut in the rows", .at = 70, .cut = true}}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    PictureCache cache(storage, kFolder);
    Keep(cache, Key(), rows);
    for (auto& [path, entry] : storage.entries) {
      if (path.starts_with(std::string(kFolder) + "/")) {
        if (harm.cut) {
          entry.data.resize(harm.at);
        } else {
          entry.data[harm.at] ^= 0x10U;
        }
      }
    }
    Bytes read;
    EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kDamaged) << harm.what;
    EXPECT_EQ(storage.OpenCount(), 0) << harm.what;
  }
}

TEST(PictureCache, RefusesWhatCannotBe) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  PictureKey none = Key();
  none.width = 0;
  EXPECT_EQ(cache.BeginWrite(none), PictureCacheStatus::kMisused);
  PictureKey levels = Key();
  levels.levels = 1;
  EXPECT_EQ(cache.BeginRead(levels), PictureCacheStatus::kMisused);
  levels.levels = 257;
  EXPECT_EQ(cache.BeginWrite(levels), PictureCacheStatus::kMisused);
  // Rows with nothing begun, rows not whole, more rows than there are, and
  // finished too soon.
  const Bytes rows = RowsFor(Key());
  Bytes into(2);
  EXPECT_EQ(cache.WriteRows(std::span(rows).first(2)), PictureCacheStatus::kMisused);
  EXPECT_EQ(cache.ReadRows(into), PictureCacheStatus::kMisused);
  ASSERT_EQ(cache.BeginWrite(Key()), PictureCacheStatus::kOk);
  EXPECT_EQ(cache.WriteRows(std::span(rows).first(3)), PictureCacheStatus::kMisused);
  EXPECT_EQ(cache.FinishWrite(), PictureCacheStatus::kMisused);
  Bytes more = rows;
  more.resize(rows.size() + PictureCache::RowBytesFor(Key()));
  EXPECT_EQ(cache.WriteRows(more), PictureCacheStatus::kMisused);
  ASSERT_EQ(cache.WriteRows(rows), PictureCacheStatus::kOk);
  ASSERT_EQ(cache.FinishWrite(), PictureCacheStatus::kOk);
  ASSERT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kOk);
  Bytes tooMany(more.size());
  EXPECT_EQ(cache.ReadRows(tooMany), PictureCacheStatus::kMisused);
  cache.Close();
  EXPECT_EQ(storage.OpenCount(), 0);
}

TEST(PictureCache, SaysWhenStorageFails) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  storage.failWrites = bn::core::StorageStatus::kNoSpace;
  EXPECT_EQ(cache.BeginWrite(Key()), PictureCacheStatus::kStoreFailed);
  EXPECT_EQ(storage.OpenCount(), 0);
  storage.failWrites = bn::core::StorageStatus::kOk;
  Keep(cache, Key(), RowsFor(Key()));
  storage.failReads = true;
  Bytes read;
  EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kStoreFailed);
  EXPECT_EQ(storage.OpenCount(), 0);
}

TEST(PictureCache, NamesEachStatus) {
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kOk), "ok");
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kMissing), "missing");
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kOther), "other");
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kDamaged), "damaged");
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kStoreFailed), "store failed");
  EXPECT_STREQ(bn::draw::PictureCacheStatusName(PictureCacheStatus::kMisused), "misused");
}

}  // namespace
