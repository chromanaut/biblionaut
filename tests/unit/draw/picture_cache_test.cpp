#include "draw/picture_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
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

// A cache, and the memory it gathers rows in: two sectors of it unless
// the test says otherwise.
struct Gathering {
  explicit Gathering(FakeStorage& storage, std::size_t bytes = 2 * PictureCache::kSectorBytes)
      : buffer(bytes), cache(storage, kFolder) {
    cache.GatherIn(buffer);
  }
  Bytes buffer;
  PictureCache cache;
};

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
    Gathering made(storage);
    PictureCache& cache = made.cache;
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
  Gathering made(storage);
  PictureCache& cache = made.cache;
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
  Gathering made(storage);
  PictureCache& cache = made.cache;
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
  Gathering made(storage);
  PictureCache& cache = made.cache;
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

// The file the picture of `key` goes in when the cache is empty.
std::string FileFor(const PictureKey& key) {
  FakeStorage storage;
  EXPECT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  Keep(cache, key, RowsFor(key));
  for (const auto& [path, entry] : storage.entries) {
    if (path.starts_with(std::string(kFolder) + "/")) {
      return path;
    }
  }
  return {};
}

// `count` keys with the same home as `first`, `first` among them.
std::vector<PictureKey> KeysOfOneHome(const PictureKey& first, std::size_t count) {
  // An empty cache puts a picture in its home, so keys with one home go in
  // one file then.
  const std::string file = FileFor(first);
  std::vector<PictureKey> keys = {first};
  for (int at = 0; keys.size() < count && at < 100000; ++at) {
    PictureKey key = first;
    key.source = PictureKey::PathHash("/drawings/" + std::to_string(at) + ".svg");
    if (FileFor(key) == file) {
      keys.push_back(key);
    }
  }
  EXPECT_EQ(keys.size(), count);
  return keys;
}

// Whether the picture of `key` is there to be read.
bool There(PictureCache& cache, const PictureKey& key) {
  Bytes read;
  return ReadBack(cache, key, read) == PictureCacheStatus::kOk;
}

TEST(PictureCache, PicturesWithOneHomeAreAllKeptWhileThereArePlaces) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  const std::vector<PictureKey> keys = KeysOfOneHome(Key(), PictureCache::kPlaces);
  ASSERT_EQ(keys.size(), PictureCache::kPlaces);
  for (std::size_t at = 0; at < keys.size(); ++at) {
    Keep(cache, keys[at], RowsFor(keys[at], static_cast<std::uint8_t>(at)));
  }
  for (std::size_t at = 0; at < keys.size(); ++at) {
    Bytes read;
    EXPECT_EQ(ReadBack(cache, keys[at], read), PictureCacheStatus::kOk) << at;
    EXPECT_EQ(read, RowsFor(keys[at], static_cast<std::uint8_t>(at))) << at;
  }
  // Kept again, a picture takes its own place, not another's.
  Keep(cache, keys[0], RowsFor(keys[0], 99));
  for (std::size_t at = 0; at < keys.size(); ++at) {
    Bytes read;
    EXPECT_EQ(ReadBack(cache, keys[at], read), PictureCacheStatus::kOk) << at;
    EXPECT_EQ(read, RowsFor(keys[at], static_cast<std::uint8_t>(at == 0 ? 99 : at))) << at;
  }
}

TEST(PictureCache, APictureTakesThePlaceOfWhatWasKeptLongestAgo) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  const std::vector<PictureKey> keys = KeysOfOneHome(Key(), PictureCache::kPlaces + 2);
  ASSERT_EQ(keys.size(), PictureCache::kPlaces + 2);
  for (std::size_t at = 0; at < PictureCache::kPlaces; ++at) {
    Keep(cache, keys[at], RowsFor(keys[at]));
  }
  Keep(cache, keys[PictureCache::kPlaces], RowsFor(keys[PictureCache::kPlaces]));  // In place of the first,
  EXPECT_FALSE(There(cache, keys[0]));
  EXPECT_TRUE(There(cache, keys[1]));
  Keep(cache, keys[PictureCache::kPlaces + 1], RowsFor(keys[PictureCache::kPlaces + 1]));  // then of the second,
  EXPECT_FALSE(There(cache, keys[1]));
  EXPECT_TRUE(There(cache, keys[2]));
  Keep(cache, keys[0], RowsFor(keys[0]));  // then of the third.
  EXPECT_FALSE(There(cache, keys[2]));
  for (std::size_t at = 3; at < keys.size(); ++at) {
    EXPECT_TRUE(There(cache, keys[at])) << at;
  }
  EXPECT_TRUE(There(cache, keys[0]));
  // And by a cache that has only just begun, the same.
  Gathering again(storage);
  PictureCache& later = again.cache;
  Keep(later, keys[1], RowsFor(keys[1]));  // In place of the fourth.
  EXPECT_FALSE(There(later, keys[3]));
  EXPECT_TRUE(There(later, keys[1]));
  EXPECT_TRUE(There(later, keys[4]));
}

TEST(PictureCache, AFileThatHoldsNothingWholeIsTakenFirst) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  const std::vector<PictureKey> keys = KeysOfOneHome(Key(), PictureCache::kPlaces + 1);
  ASSERT_EQ(keys.size(), PictureCache::kPlaces + 1);
  for (std::size_t at = 0; at < PictureCache::kPlaces; ++at) {
    Keep(cache, keys[at], RowsFor(keys[at]));
  }
  // The last kept again, and given up partway: its file has no header.
  const PictureKey& halfKept = keys[PictureCache::kPlaces - 1];
  const Bytes rows = RowsFor(halfKept);
  ASSERT_EQ(cache.BeginWrite(halfKept), PictureCacheStatus::kOk);
  ASSERT_EQ(cache.WriteRows(std::span(rows).first(PictureCache::RowBytesFor(halfKept))), PictureCacheStatus::kOk);
  cache.Close();
  Bytes read;
  EXPECT_EQ(ReadBack(cache, halfKept, read), PictureCacheStatus::kDamaged);
  // The next takes its place, not that of the one kept longest ago.
  Keep(cache, keys[PictureCache::kPlaces], RowsFor(keys[PictureCache::kPlaces]));
  for (std::size_t at = 0; at + 1 < PictureCache::kPlaces; ++at) {
    EXPECT_TRUE(There(cache, keys[at])) << at;
  }
  EXPECT_TRUE(There(cache, keys[PictureCache::kPlaces]));
  EXPECT_EQ(ReadBack(cache, halfKept, read), PictureCacheStatus::kOther);
}

TEST(PictureCache, APictureNotFinishedIsFoundDamaged) {
  // Given up after some rows, as by a press, or the power gone: there is
  // no header.
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
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
  for (const Harm harm :
       {Harm{.what = "magic", .at = 0, .cut = false}, Harm{.what = "version", .at = 4, .cut = false},
        Harm{.what = "a row's checksum", .at = 33, .cut = false},
        Harm{.what = "when it was kept", .at = 37, .cut = false},
        Harm{.what = "the header's checksum", .at = 41, .cut = false},
        Harm{.what = "the first row", .at = PictureCache::kHeaderBytes, .cut = false},
        Harm{.what = "the last row", .at = PictureCache::kHeaderBytes + rows.size() - 1, .cut = false},
        Harm{.what = "cut in the header", .at = 30, .cut = true},
        Harm{.what = "cut at the header's end", .at = PictureCache::kHeaderBytes, .cut = true},
        Harm{.what = "cut in the rows", .at = PictureCache::kHeaderBytes + 6, .cut = true}}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    Gathering made(storage);
    PictureCache& cache = made.cache;
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
  Gathering made(storage);
  PictureCache& cache = made.cache;
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
  Gathering made(storage);
  PictureCache& cache = made.cache;
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

TEST(PictureCache, AFileOfAnyOtherLengthIsFoundDamagedBeforeARowIsRead) {
  // The header is whole and says what the picture is, so how long the file
  // has to be is known before anything of the rows is asked for.
  const Bytes rows = RowsFor(Key());
  for (const std::ptrdiff_t change : {std::ptrdiff_t{-1}, std::ptrdiff_t{1}, std::ptrdiff_t{-6}, std::ptrdiff_t{700}}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    Gathering made(storage);
    PictureCache& cache = made.cache;
    Keep(cache, Key(), rows);
    for (auto& [path, entry] : storage.entries) {
      if (path.starts_with(std::string(kFolder) + "/")) {
        entry.data.resize(static_cast<std::size_t>(static_cast<std::ptrdiff_t>(entry.data.size()) + change));
      }
    }
    EXPECT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kDamaged) << change;
    EXPECT_EQ(storage.OpenCount(), 0) << change;
  }
}

TEST(PictureCache, AFileThatGrowsShorterWhileItIsReadIsFoundDamagedAtOnce) {
  // Storage that gives fewer bytes than the file was found to hold: said
  // at that read, and nothing of what the memory held before is given out
  // as rows.
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  const Bytes rows = RowsFor(Key());
  Keep(cache, Key(), rows);
  ASSERT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kOk);
  for (auto& [path, entry] : storage.entries) {
    if (path.starts_with(std::string(kFolder) + "/")) {
      entry.data.resize(entry.data.size() - 3);
    }
  }
  Bytes row(PictureCache::RowBytesFor(Key()), 0xEE);
  EXPECT_EQ(cache.ReadRows(row), PictureCacheStatus::kDamaged);
  EXPECT_EQ(storage.OpenCount(), 0);
}

TEST(PictureCache, AFileCutShortInTheHeadersSectorHoldsNothingWhole) {
  // All of what the header says is there, and its checksum is right, but
  // the sector it is in is not all there: no file the cache finished. It
  // is the first to be written over, like any other that holds nothing
  // whole.
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  Gathering made(storage);
  PictureCache& cache = made.cache;
  const std::vector<PictureKey> keys = KeysOfOneHome(Key(), PictureCache::kPlaces + 1);
  ASSERT_EQ(keys.size(), PictureCache::kPlaces + 1);
  std::set<std::string> before;
  for (std::size_t at = 0; at < PictureCache::kPlaces; ++at) {
    if (at + 1 == PictureCache::kPlaces) {
      for (const auto& [path, entry] : storage.entries) {
        before.insert(path);
      }
    }
    Keep(cache, keys[at], RowsFor(keys[at]));
  }
  const PictureKey& cut = keys[PictureCache::kPlaces - 1];
  std::size_t found = 0;
  for (auto& [path, entry] : storage.entries) {
    if (!before.contains(path)) {
      ASSERT_GT(entry.data.size(), PictureCache::kHeaderBytes);
      entry.data.resize(100);
      ++found;
    }
  }
  ASSERT_EQ(found, 1U);
  Bytes read;
  EXPECT_EQ(ReadBack(cache, cut, read), PictureCacheStatus::kDamaged);
  // The next takes its place, not that of the one kept longest ago.
  Keep(cache, keys[PictureCache::kPlaces], RowsFor(keys[PictureCache::kPlaces]));
  for (std::size_t at = 0; at + 1 < PictureCache::kPlaces; ++at) {
    EXPECT_TRUE(There(cache, keys[at])) << at;
  }
  EXPECT_TRUE(There(cache, keys[PictureCache::kPlaces]));
  EXPECT_EQ(ReadBack(cache, cut, read), PictureCacheStatus::kOther);
}

TEST(PictureCache, LeavesNothingInTheRestOfTheHeadersSector) {
  // The header goes to storage from the memory the rows were gathered in,
  // and what it does not use of its sector is nothing: a file is the same
  // bytes whatever was in that memory before.
  PictureKey key = Key();
  key.width = 480;
  key.height = 40;
  key.levels = 4;
  for (const std::size_t memory : {PictureCache::kSectorBytes, std::size_t{4096}}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    Gathering made(storage, memory);
    std::fill(made.buffer.begin(), made.buffer.end(), std::uint8_t{0xEE});
    Keep(made.cache, key, RowsFor(key, 1));
    std::size_t files = 0;
    for (const auto& [path, entry] : storage.entries) {
      if (path.starts_with(std::string(kFolder) + "/")) {
        ++files;
        ASSERT_GT(entry.data.size(), PictureCache::kHeaderBytes);
        const auto rest = std::span(entry.data).subspan(44, PictureCache::kHeaderBytes - 44);
        EXPECT_TRUE(std::all_of(rest.begin(), rest.end(), [](std::uint8_t byte) { return byte == 0; })) << memory;
        // And the header's own bytes are not all nothing.
        EXPECT_EQ(entry.data[0], 'B');
      }
    }
    EXPECT_EQ(files, 1U);
  }
}

TEST(PictureCache, ReadsAndWritesWholeSectorsEachBegunWhereASectorBegins) {
  // A write that begins or ends part way through a sector costs a card a
  // read of that sector and a write of all of it. So whatever pieces the
  // rows come in, they go to storage as many whole sectors at a time as
  // the cache has memory for, each write begun where a sector begins, and
  // only the last of the rows may end part way through one. A page of the
  // panel in four levels, given a band at a time, a row at a time and all
  // at once, to caches with a sector of memory, with eight, with a size
  // that is no whole number of them, and with more than the picture. And
  // a picture whose rows are 13 bytes, which given a row at a time end
  // at every place in a sector there is: one byte short of its end, and
  // of the memory's, among them.
  struct Page {
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t levels;
    std::size_t rowBytes;
  };
  const std::size_t sector = PictureCache::kSectorBytes;
  for (const Page page : {Page{.width = 480, .height = 800, .levels = 4, .rowBytes = 120},
                          Page{.width = 100, .height = 700, .levels = 2, .rowBytes = 13}}) {
    PictureKey key = Key();
    key.width = page.width;
    key.height = page.height;
    key.levels = page.levels;
    const std::size_t rowBytes = PictureCache::RowBytesFor(key);
    ASSERT_EQ(rowBytes, page.rowBytes);
    const Bytes rows = RowsFor(key);
    for (const std::size_t memory : {sector, std::size_t{4096}, std::size_t{4500}, std::size_t{200000}}) {
      const std::size_t used = memory - (memory % sector);
      for (const std::size_t rowsAtATime : {std::size_t{26}, std::size_t{1}, std::size_t{800}}) {
        FakeStorage storage;
        ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
        Gathering made(storage, memory);
        PictureCache& cache = made.cache;
        ASSERT_EQ(cache.BeginWrite(key), PictureCacheStatus::kOk);
        for (std::size_t at = 0; at < rows.size(); at += rowsAtATime * rowBytes) {
          const std::size_t count = std::min(rowsAtATime * rowBytes, rows.size() - at);
          ASSERT_EQ(cache.WriteRows(std::span(rows).subspan(at, count)), PictureCacheStatus::kOk);
        }
        ASSERT_EQ(cache.FinishWrite(), PictureCacheStatus::kOk);

        // The header's sector, empty; the rows; the header's sector again.
        const std::size_t rowWrites = (rows.size() + used - 1) / used;
        ASSERT_EQ(storage.written.size(), rowWrites + 2)
            << memory << " of memory, " << rowsAtATime << " rows at a time";
        EXPECT_EQ(storage.written.front().offset, 0U);
        EXPECT_EQ(storage.written.front().size, sector);
        EXPECT_EQ(storage.written.back().offset, 0U);
        EXPECT_EQ(storage.written.back().size, sector);
        for (std::size_t write = 1; write <= rowWrites; ++write) {
          const FakeStorage::ReadCall& call = storage.written[write];
          EXPECT_EQ(call.offset, sector + ((write - 1) * used)) << "write " << write;
          EXPECT_EQ(call.offset % sector, 0U) << "write " << write;
          if (write < rowWrites) {
            EXPECT_EQ(call.size, used) << "write " << write;
          } else {
            EXPECT_EQ(call.size, rows.size() - ((rowWrites - 1) * used)) << "the last write";
          }
        }

        // And read back the same way: the header's sector, then the rows.
        storage.reads.clear();
        Bytes read;
        ASSERT_EQ(ReadBack(cache, key, read, rowsAtATime), PictureCacheStatus::kOk);
        EXPECT_TRUE(read == rows);
        ASSERT_EQ(storage.reads.size(), rowWrites + 1) << memory << " of memory, " << rowsAtATime << " rows at a time";
        EXPECT_EQ(storage.reads.front().offset, 0U);
        EXPECT_EQ(storage.reads.front().size, sector);
        for (std::size_t call = 1; call <= rowWrites; ++call) {
          EXPECT_EQ(storage.reads[call].offset, sector + ((call - 1) * used)) << "read " << call;
          EXPECT_EQ(storage.reads[call].size, call < rowWrites ? used : rows.size() - ((rowWrites - 1) * used));
        }
      }
    }
  }
}

TEST(PictureCache, RefusesEverythingUntilItHasASectorToGatherIn) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
  PictureCache cache(storage, kFolder);
  EXPECT_EQ(cache.BeginWrite(Key()), PictureCacheStatus::kMisused);
  EXPECT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kMisused);
  Bytes small(PictureCache::kSectorBytes - 1);
  cache.GatherIn(small);
  EXPECT_EQ(cache.BeginWrite(Key()), PictureCacheStatus::kMisused);
  EXPECT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kMisused);
  EXPECT_FALSE(storage.Has(std::string(kFolder)));
  Bytes enough(PictureCache::kSectorBytes);
  cache.GatherIn(enough);
  Keep(cache, Key(), RowsFor(Key()));
  Bytes read;
  EXPECT_EQ(ReadBack(cache, Key(), read), PictureCacheStatus::kOk);
  EXPECT_EQ(read, RowsFor(Key()));
  // Given other memory part way through, it has ended what was begun.
  ASSERT_EQ(cache.BeginRead(Key()), PictureCacheStatus::kOk);
  cache.GatherIn(small);
  EXPECT_EQ(storage.OpenCount(), 0);
  Bytes into(PictureCache::RowBytesFor(Key()));
  EXPECT_EQ(cache.ReadRows(into), PictureCacheStatus::kMisused);
}

TEST(PictureCache, SaysWhenARowCannotBeWrittenHoweverLateItFindsOut) {
  // Rows are not written as they are given, so a card that fails, or is
  // full, is found out when the memory is full or the picture finished.
  PictureKey key = Key();
  key.width = 480;
  key.height = 20;
  key.levels = 4;
  const Bytes rows = RowsFor(key);  // 2,400 bytes: more than the two sectors of memory.
  for (const bool lateFailure : {false, true}) {
    FakeStorage storage;
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    Gathering made(storage);
    PictureCache& cache = made.cache;
    ASSERT_EQ(cache.BeginWrite(key), PictureCacheStatus::kOk);
    if (lateFailure) {
      // The first two sectors' worth goes; what is left fails at the end.
      ASSERT_EQ(cache.WriteRows(std::span(rows).first(1200)), PictureCacheStatus::kOk);
      ASSERT_EQ(cache.WriteRows(std::span(rows).subspan(1200)), PictureCacheStatus::kOk);
      storage.failWrites = bn::core::StorageStatus::kNoSpace;
      EXPECT_EQ(cache.FinishWrite(), PictureCacheStatus::kStoreFailed);
    } else {
      storage.failWrites = bn::core::StorageStatus::kNoSpace;
      EXPECT_EQ(cache.WriteRows(rows), PictureCacheStatus::kStoreFailed);
      EXPECT_EQ(cache.FinishWrite(), PictureCacheStatus::kMisused);  // Nothing is begun any more.
    }
    EXPECT_EQ(storage.OpenCount(), 0);
    storage.failWrites = bn::core::StorageStatus::kOk;
    Bytes read;
    EXPECT_EQ(ReadBack(cache, key, read), PictureCacheStatus::kDamaged);
  }
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
