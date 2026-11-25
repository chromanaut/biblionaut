#include "epub/bundle.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleMaker;
using bn::epub::BundleSlotBytes;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::BundleTotals;
using bn::epub::ChapterState;
using bn::test::FakeStorage;
using Bytes = FakeStorage::Bytes;

const char* const kPath = "/book.bundle";

// Numbers out of a file's bytes, by the places the format gives them, and
// nothing of the code that is being tested.
std::uint32_t U16(const Bytes& bytes, std::size_t at) { return bytes.at(at) | (std::uint32_t{bytes.at(at + 1)} << 8U); }
std::uint32_t U32(const Bytes& bytes, std::size_t at) { return U16(bytes, at) | (U16(bytes, at + 2) << 16U); }
std::uint32_t Crc(const Bytes& bytes, std::size_t from, std::size_t to) {
  return bn::core::Crc32(std::span(bytes).subspan(from, to - from));
}

// Some of a file's bytes.
Bytes Part(const Bytes& bytes, std::size_t from, std::size_t count) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(from + count)};
}

// Bytes that say which they are, so that ones in the wrong place show.
Bytes Pattern(std::size_t count, std::uint32_t seed) {
  Bytes bytes(count);
  for (std::size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<std::uint8_t>(((i + seed) * 2654435761U) >> 23U);
  }
  return bytes;
}

// The record of the n-th file of a table: every field its own.
BundleFile FileNumber(std::uint32_t n, std::uint8_t kind) {
  BundleFile file;
  file.entry.headerOffset = 1000 + (n * 7919);
  file.entry.compressedSize = 300 + n;
  file.entry.size = 900 + (n * 3);
  file.entry.crc32 = 0xC0000000U + n;
  file.entry.method = static_cast<std::uint16_t>(n % 2 == 0 ? 8 : 0);
  file.entry.flags = static_cast<std::uint16_t>(0x0800 + (n % 5));
  file.entry.nameLength = static_cast<std::uint16_t>(20 + (n % 40));
  file.kind = kind;
  file.flags = static_cast<std::uint8_t>(n % 3);
  return file;
}

void ExpectSameFile(const BundleFile& got, const BundleFile& expected, const std::string& what) {
  EXPECT_EQ(got.entry.headerOffset, expected.entry.headerOffset) << what;
  EXPECT_EQ(got.entry.compressedSize, expected.entry.compressedSize) << what;
  EXPECT_EQ(got.entry.size, expected.entry.size) << what;
  EXPECT_EQ(got.entry.crc32, expected.entry.crc32) << what;
  EXPECT_EQ(got.entry.method, expected.entry.method) << what;
  EXPECT_EQ(got.entry.flags, expected.entry.flags) << what;
  EXPECT_EQ(got.entry.nameLength, expected.entry.nameLength) << what;
  EXPECT_EQ(got.kind, expected.kind) << what;
  EXPECT_EQ(got.flags, expected.flags) << what;
}

BundleBook BookOf(std::uint16_t documents, std::uint16_t pictures) {
  BundleBook book;
  book.textRules = 3;
  book.importer = 7;
  book.epubBytes = 1'234'567;
  book.epubModified = 0x5B7A1234;
  book.epubDirectoryChecksum = 0xD1EC7000;
  book.packageChecksum = 0x9AC4A6E0;
  book.documents = documents;
  book.pictures = pictures;
  book.contents = 11;
  const std::string madeBy = "tests 1.0";
  std::copy(madeBy.begin(), madeBy.end(), book.madeBy.begin());
  return book;
}

// A chapter's record with a number in every field, for a segment of
// `bytes` at `offset`.
BundleChapter ChapterNumber(std::uint32_t n, std::uint32_t offset, std::uint32_t bytes) {
  BundleChapter chapter;
  chapter.state = n % 2 == 0 ? ChapterState::kWhole : ChapterState::kCutShort;
  chapter.reason = static_cast<std::uint8_t>(n % 2 == 0 ? 0 : 5);
  chapter.leftOut = static_cast<std::uint16_t>(n * 3);
  chapter.segmentOffset = offset;
  chapter.segmentBytes = bytes;
  chapter.directoryOffset = bytes - std::min<std::uint32_t>(bytes, 40);
  chapter.directoryBytes = std::min<std::uint32_t>(bytes, 40);
  chapter.directoryChecksum = 0xD0000000U + n;
  chapter.blocks = 10 + n;
  chapter.characters = 1000 + (n * 17);
  for (std::size_t i = 0; i < chapter.classCharacters.size(); ++i) {
    chapter.classCharacters[i] = (n * 100) + static_cast<std::uint32_t>(i);
  }
  for (std::size_t i = 0; i < chapter.characterRows.size(); ++i) {
    chapter.characterRows[i] = static_cast<std::uint8_t>(n + i);
  }
  return chapter;
}

// Storage, a file on it, and the memory a bundle wants.
struct BundleFixture {
  BundleFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Makes a bundle of a book of so many documents and pictures, with a
  // table of strings and of contents of the bytes given. Leaves the file
  // closed. Returns how the making ended.
  BundleStatus Make(std::uint16_t documents, std::uint16_t pictures, const Bytes& strings = Pattern(70, 1),
                    const Bytes& contents = Pattern(900, 2)) {
    BundleMaker maker(page);
    if (file.Open(storage, kPath, OpenMode::kCreate) != StorageStatus::kOk) {
      return BundleStatus::kStoreFailed;
    }
    BundleStatus status = maker.Begin(file, BookOf(documents, pictures));
    const auto good = [&status] { return status == BundleStatus::kOk; };
    status = good() ? maker.BeginTable(BundleTable::kStrings) : status;
    status = good() ? maker.Write(strings) : status;
    status = good() ? maker.BeginTable(BundleTable::kSpine) : status;
    for (std::uint32_t n = 0; n < documents && good(); ++n) {
      status = maker.WriteFile(FileNumber(n, 1));
    }
    status = good() ? maker.BeginTable(BundleTable::kPictures) : status;
    for (std::uint32_t n = 0; n < pictures && good(); ++n) {
      status = maker.WriteFile(FileNumber(5000 + n, 4));
    }
    status = good() ? maker.BeginTable(BundleTable::kContents) : status;
    status = good() ? maker.Write(contents) : status;
    status = good() ? maker.Finish() : status;
    if (good()) {
      while (maker.Step()) {
      }
      status = maker.Status();
    }
    file.Close();
    return status;
  }

  // Opens the file, to be changed, and the bundle it is.
  BundleStatus Open() {
    bundle.Close();
    if (file.Open(storage, kPath, OpenMode::kUpdate) != StorageStatus::kOk) {
      return BundleStatus::kStoreFailed;
    }
    return bundle.Open(file);
  }

  // Writes a segment where the segments end, in two writes, and gives the
  // record of it for the chapter numbered `n`. False if storage failed.
  bool WriteSegment(std::uint32_t n, const Bytes& bytes, BundleChapter& record) {
    record = ChapterNumber(n, bundle.DataEnd(), static_cast<std::uint32_t>(bytes.size()));
    const std::size_t half = bytes.size() / 2;
    return file.Seek(bundle.DataEnd()) && file.Write(std::span(bytes).first(half)) &&
           file.Write(std::span(bytes).subspan(half));
  }

  const Bytes& OnCard() const { return storage.Contents(kPath); }

  FakeStorage storage;
  alignas(4) std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  BlockFile file{fileBuffer};
  Bundle bundle{page};
};

TEST(Bundle, ANewBundleOpensAndSaysWhatItWasMadeOf) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 3), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  EXPECT_TRUE(fixture.bundle.IsOpen());
  EXPECT_EQ(fixture.bundle.Book(), BookOf(5, 3));
  EXPECT_EQ(fixture.bundle.DataEnd(), fixture.bundle.DataBegin()) << "no segment yet";
  EXPECT_EQ(fixture.bundle.DataBegin(), fixture.OnCard().size());

  // Every chapter not imported, and nothing counted.
  for (std::size_t chapter = 0; chapter < 5; ++chapter) {
    BundleChapter record = ChapterNumber(9, 0, 0);
    ASSERT_EQ(fixture.bundle.ReadChapter(chapter, record), BundleStatus::kOk);
    EXPECT_EQ(record, BundleChapter{}) << chapter;
  }
  EXPECT_EQ(fixture.bundle.Totals(), (BundleTotals{.notImported = 5, .characters = 0, .classCharacters = {}}));

  // The tables, each where the last ended, of the bytes given.
  const Bundle::Place strings = fixture.bundle.Table(BundleTable::kStrings);
  const Bundle::Place spine = fixture.bundle.Table(BundleTable::kSpine);
  const Bundle::Place pictures = fixture.bundle.Table(BundleTable::kPictures);
  const Bundle::Place contents = fixture.bundle.Table(BundleTable::kContents);
  EXPECT_EQ(strings.offset, 4096u);
  EXPECT_EQ(strings.bytes, 70u);
  EXPECT_EQ(spine.offset, 4096u + 70u);
  EXPECT_EQ(spine.bytes, 5u * 24u);
  EXPECT_EQ(pictures.offset, spine.offset + spine.bytes);
  EXPECT_EQ(pictures.bytes, 3u * 24u);
  EXPECT_EQ(contents.offset, pictures.offset + pictures.bytes);
  EXPECT_EQ(contents.bytes, 900u);
  Bytes got(900);
  ASSERT_EQ(fixture.bundle.ReadTable(BundleTable::kContents, 0, got), BundleStatus::kOk);
  EXPECT_EQ(got, Pattern(900, 2));
  got.resize(20);
  ASSERT_EQ(fixture.bundle.ReadTable(BundleTable::kStrings, 50, got), BundleStatus::kOk);
  EXPECT_EQ(got, Part(Pattern(70, 1), 50, 20));
  EXPECT_EQ(fixture.bundle.ReadTable(BundleTable::kStrings, 51, got), BundleStatus::kMisused) << "past its end";
}

TEST(Bundle, IsLaidOutAsTheFormatSays) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 3), BundleStatus::kOk);
  const Bytes& bytes = fixture.OnCard();

  // The header page.
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "BNBK");
  EXPECT_EQ(U16(bytes, 4), 1u) << "the format's version";
  EXPECT_EQ(U16(bytes, 6), 3u);
  EXPECT_EQ(U16(bytes, 8), 7u);
  EXPECT_EQ(U32(bytes, 12), 1'234'567u);
  EXPECT_EQ(U32(bytes, 16), 0x5B7A1234u);
  EXPECT_EQ(U32(bytes, 20), 0xD1EC7000u);
  EXPECT_EQ(U32(bytes, 24), 0x9AC4A6E0u);
  EXPECT_EQ(U16(bytes, 28), 5u);
  EXPECT_EQ(U16(bytes, 30), 3u);
  EXPECT_EQ(U16(bytes, 32), 11u);
  EXPECT_EQ(U32(bytes, 36), 8192u) << "the slots begin on the page after the tables";
  EXPECT_EQ(U32(bytes, 40), 4096u) << "a slot of five records is one page";
  EXPECT_EQ(U32(bytes, 44), 16384u);
  // The four tables: where, how long, checksum.
  const std::array<std::uint32_t, 4> lengths = {70, 120, 72, 900};
  std::uint32_t at = 4096;
  for (std::size_t table = 0; table < 4; ++table) {
    EXPECT_EQ(U32(bytes, 48 + (table * 12)), at) << table;
    EXPECT_EQ(U32(bytes, 52 + (table * 12)), lengths[table]) << table;
    EXPECT_EQ(U32(bytes, 56 + (table * 12)), Crc(bytes, at, at + lengths[table])) << table;
    at += lengths[table];
  }
  EXPECT_EQ(std::string(bytes.begin() + 96, bytes.begin() + 105), "tests 1.0");
  EXPECT_TRUE(std::all_of(bytes.begin() + 105, bytes.begin() + 4092, [](std::uint8_t byte) { return byte == 0; }));
  EXPECT_EQ(U32(bytes, 4092), Crc(bytes, 0, 4092));
  EXPECT_TRUE(std::all_of(bytes.begin() + at, bytes.begin() + 8192, [](std::uint8_t byte) { return byte == 0; }))
      << "nothing between the tables and the slots";

  // A file's record: the third document of the spine.
  const std::size_t third = 4096 + 70 + (2 * 24);
  const BundleFile file = FileNumber(2, 1);
  EXPECT_EQ(U32(bytes, third), file.entry.headerOffset);
  EXPECT_EQ(U32(bytes, third + 4), file.entry.compressedSize);
  EXPECT_EQ(U32(bytes, third + 8), file.entry.size);
  EXPECT_EQ(U32(bytes, third + 12), file.entry.crc32);
  EXPECT_EQ(U16(bytes, third + 16), file.entry.method);
  EXPECT_EQ(U16(bytes, third + 18), file.entry.flags);
  EXPECT_EQ(U16(bytes, third + 20), file.entry.nameLength);
  EXPECT_EQ(bytes[third + 22], 1);
  EXPECT_EQ(bytes[third + 23], 2);

  // The slots: the first is the one in force.
  const std::array<std::uint32_t, 2> sequences = {2, 1};
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const std::size_t begin = 8192 + (slot * 4096);
    EXPECT_EQ(Part(bytes, begin, 4), (Bytes{'B', 'N', 'S', 'L'}));
    EXPECT_EQ(U32(bytes, begin + 4), sequences[slot]);
    EXPECT_EQ(U32(bytes, begin + 8), 24u + (5u * 88u)) << "the bytes it uses";
    EXPECT_EQ(U32(bytes, begin + 12), 16384u) << "where the segments end";
    EXPECT_EQ(U16(bytes, begin + 16), 5u);
    EXPECT_EQ(U16(bytes, begin + 18), 88u);
    EXPECT_EQ(U32(bytes, begin + 460), Crc(bytes, begin, begin + 460));
  }
  EXPECT_EQ(bytes.size(), 16384u);

  // And a chapter's record, once one is committed.
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter record;
  ASSERT_TRUE(fixture.WriteSegment(3, Pattern(700, 9), record));
  ASSERT_EQ(fixture.bundle.Commit(3, record), BundleStatus::kOk);
  const Bytes& after = fixture.OnCard();
  const std::size_t begin = 8192 + 4096 + 20 + (3 * 88);
  EXPECT_EQ(U32(after, 8192 + 4096 + 4), 3u) << "the other slot, with the next number";
  EXPECT_EQ(after[begin], 2) << "cut short";
  EXPECT_EQ(after[begin + 1], 5);
  EXPECT_EQ(U16(after, begin + 2), 9u);
  EXPECT_EQ(U32(after, begin + 4), 16384u);
  EXPECT_EQ(U32(after, begin + 8), 700u);
  EXPECT_EQ(U32(after, begin + 12), 660u);
  EXPECT_EQ(U32(after, begin + 16), 40u);
  EXPECT_EQ(U32(after, begin + 20), 0xD0000003u);
  EXPECT_EQ(U32(after, begin + 24), 13u);
  EXPECT_EQ(U32(after, begin + 28), 1051u);
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(U32(after, begin + 32 + (i * 4)), 300u + i);
  }
  for (std::size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(after[begin + 56 + i], 3 + i);
  }
  EXPECT_EQ(U32(after, 8192 + 4096 + 12), 16384u + 1024u) << "the segments end where its last sector does";
  EXPECT_EQ(U32(after, 8192 + 4096 + 460), Crc(after, 8192 + 4096, 8192 + 4096 + 460));
}

TEST(Bundle, ASlotIsItsRecordsFilledOutToPages) {
  EXPECT_EQ(BundleSlotBytes(0), 4096u);
  EXPECT_EQ(BundleSlotBytes(1), 4096u);
  EXPECT_EQ(BundleSlotBytes(46), 4096u) << "24 and 46 of 88 are 4,072";
  EXPECT_EQ(BundleSlotBytes(47), 8192u);
  EXPECT_EQ(BundleSlotBytes(279), 24576u) << "six pages to the byte";
  EXPECT_EQ(BundleSlotBytes(280), 28672u);
  EXPECT_EQ(BundleSlotBytes(2014), 180224u);
  EXPECT_EQ(BundleSlotBytes(65535), 5767168u);
}

TEST(Bundle, GivesBackTheRecordsOfTheSpineAndOfThePictures) {
  BundleFixture fixture;
  // Enough of each that the tables run over several pages of the file.
  ASSERT_EQ(fixture.Make(400, 300), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  for (const std::uint32_t n : {0U, 1U, 166U, 167U, 168U, 399U}) {
    BundleFile file;
    ASSERT_EQ(fixture.bundle.ReadSpine(n, file), BundleStatus::kOk);
    ExpectSameFile(file, FileNumber(n, 1), "document " + std::to_string(n));
  }
  for (const std::uint32_t n : {0U, 150U, 299U}) {
    BundleFile file;
    ASSERT_EQ(fixture.bundle.ReadPicture(n, file), BundleStatus::kOk);
    ExpectSameFile(file, FileNumber(5000 + n, 4), "picture " + std::to_string(n));
  }
  BundleFile file;
  BundleChapter chapter;
  EXPECT_EQ(fixture.bundle.ReadSpine(400, file), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.ReadPicture(300, file), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.ReadChapter(400, chapter), BundleStatus::kMisused);
}

TEST(Bundle, ChecksATableAgainstItsChecksum) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(400, 300, Pattern(70, 1), Pattern(9'000, 2)), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  for (const BundleTable table :
       {BundleTable::kStrings, BundleTable::kSpine, BundleTable::kPictures, BundleTable::kContents}) {
    EXPECT_EQ(fixture.bundle.Check(table), BundleStatus::kOk) << static_cast<int>(table);
  }
  // One bit of each, in turn, and that table alone is found damaged. The
  // bundle stays open: its header and slots are as they were.
  for (std::size_t spoiled = 0; spoiled < 4; ++spoiled) {
    const Bundle::Place place = fixture.bundle.Table(static_cast<BundleTable>(spoiled));
    Bytes& bytes = fixture.storage.entries.at(kPath).data;
    bytes[place.offset + place.bytes - 1] ^= 0x10U;
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << "opening does not read the tables";
    for (std::size_t table = 0; table < 4; ++table) {
      EXPECT_EQ(fixture.bundle.Check(static_cast<BundleTable>(table)),
                table == spoiled ? BundleStatus::kDamaged : BundleStatus::kOk)
          << spoiled << " " << table;
    }
    EXPECT_TRUE(fixture.bundle.IsOpen());
    bytes[place.offset + place.bytes - 1] ^= 0x10U;
  }
  // A table of nothing is right.
  BundleFixture empty;
  ASSERT_EQ(empty.Make(0, 0, {}, {}), BundleStatus::kOk);
  ASSERT_EQ(empty.Open(), BundleStatus::kOk);
  EXPECT_EQ(empty.bundle.Check(BundleTable::kSpine), BundleStatus::kOk);
  EXPECT_EQ(empty.bundle.Totals().notImported, 0u);
}

TEST(Bundle, CommitsChaptersAndFindsThemAgainWhenOpened) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  const std::uint32_t begin = fixture.bundle.DataBegin();

  // In no order, each segment where the last ended, on a sector.
  std::map<std::size_t, BundleChapter> committed;
  std::map<std::size_t, Bytes> segments;
  std::uint32_t end = begin;
  std::uint32_t sequence = 2;
  for (const auto& [chapter, bytes] : std::vector<std::pair<std::size_t, std::size_t>>{{2, 700}, {0, 512}, {4, 3000}}) {
    BundleChapter record;
    segments[chapter] = Pattern(bytes, static_cast<std::uint32_t>(chapter));
    ASSERT_EQ(fixture.bundle.DataEnd(), end);
    ASSERT_TRUE(fixture.WriteSegment(static_cast<std::uint32_t>(chapter), segments[chapter], record));
    EXPECT_EQ(record.segmentOffset, end);
    ASSERT_EQ(fixture.bundle.Commit(chapter, record), BundleStatus::kOk);
    committed[chapter] = record;
    end += static_cast<std::uint32_t>(((bytes + 511) / 512) * 512);
    EXPECT_EQ(fixture.bundle.DataEnd(), end) << "filled out to its sector";
    EXPECT_EQ(fixture.bundle.Sequence(), ++sequence);
    EXPECT_EQ(fixture.OnCard().size(), end);
  }

  for (int opened = 0; opened < 2; ++opened) {
    for (std::size_t chapter = 0; chapter < 5; ++chapter) {
      BundleChapter record;
      ASSERT_EQ(fixture.bundle.ReadChapter(chapter, record), BundleStatus::kOk);
      EXPECT_EQ(record, committed.contains(chapter) ? committed[chapter] : BundleChapter{}) << chapter;
      if (committed.contains(chapter)) {
        const Bytes& bytes = fixture.OnCard();
        EXPECT_EQ(
            Bytes(bytes.begin() + record.segmentOffset, bytes.begin() + record.segmentOffset + record.segmentBytes),
            segments[chapter]);
        // And nothing but zeros from its end to the end of its sector.
        const std::size_t sectorEnd = ((std::size_t{record.segmentOffset} + record.segmentBytes + 511) / 512) * 512;
        EXPECT_TRUE(std::all_of(bytes.begin() + record.segmentOffset + record.segmentBytes,
                                bytes.begin() + static_cast<std::ptrdiff_t>(sectorEnd),
                                [](std::uint8_t byte) { return byte == 0; }));
      }
    }
    EXPECT_EQ(fixture.bundle.DataEnd(), end);
    EXPECT_EQ(fixture.bundle.Sequence(), 5u);
    EXPECT_EQ(fixture.bundle.Totals().notImported, 2u);
    EXPECT_EQ(fixture.bundle.Totals().characters, 1034u + 1000u + 1068u);
    EXPECT_EQ(fixture.bundle.Totals().classCharacters[3], 203u + 3u + 403u);
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);  // And all of it again, from the card.
  }
}

TEST(Bundle, WritesTheSlotsInTurnAndNeverTheOneInForce) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  const std::array<std::size_t, 2> slots = {8192, 12288};
  std::size_t inForce = 0;
  for (std::uint32_t n = 0; n < 5; ++n) {
    const Bytes before = fixture.OnCard();
    BundleChapter record;
    ASSERT_TRUE(fixture.WriteSegment(n, Pattern(100 + n, n), record));
    fixture.storage.written.clear();
    ASSERT_EQ(fixture.bundle.Commit(n, record), BundleStatus::kOk);
    const Bytes& after = fixture.OnCard();
    EXPECT_EQ(Part(after, slots[inForce], 4096), Part(before, slots[inForce], 4096))
        << "the slot in force is left as it is";
    EXPECT_EQ(Part(after, 0, 8192), Part(before, 0, 8192)) << "and so are the header and tables";
    // What a commit writes: the rest of the segment's sector, and one slot.
    ASSERT_EQ(fixture.storage.written.size(), 2u);
    EXPECT_EQ(fixture.storage.written[0].offset, record.segmentOffset + record.segmentBytes);
    EXPECT_EQ(fixture.storage.written[1].offset, slots[1 - inForce]);
    EXPECT_EQ(fixture.storage.written[1].size, 4096u);
    inForce = 1 - inForce;
    EXPECT_EQ(U32(after, slots[inForce] + 4), 3u + n);
  }
}

TEST(Bundle, KeepsARecordThatLiesAcrossTwoPagesAndASlotThatEndsWithItsPage) {
  // The record of the 47th chapter begins 4,068 bytes into the slot. And
  // a slot of 279 records is six pages to the byte, its checksum the last
  // four of them.
  for (const std::uint16_t documents : {std::uint16_t{100}, std::uint16_t{279}}) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(documents, 0), BundleStatus::kOk);
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    std::map<std::size_t, BundleChapter> committed;
    for (const std::uint32_t chapter : {46U, 45U, 47U, 93U, static_cast<std::uint32_t>(documents - 1), 46U}) {
      BundleChapter record;
      ASSERT_TRUE(
          fixture.WriteSegment(chapter + static_cast<std::uint32_t>(committed.size()), Pattern(600, chapter), record));
      ASSERT_EQ(fixture.bundle.Commit(chapter, record), BundleStatus::kOk) << chapter;
      committed[chapter] = record;
    }
    for (int opened = 0; opened < 2; ++opened) {
      for (std::size_t chapter = 0; chapter < documents; ++chapter) {
        BundleChapter record;
        ASSERT_EQ(fixture.bundle.ReadChapter(chapter, record), BundleStatus::kOk);
        ASSERT_EQ(record, committed.contains(chapter) ? committed[chapter] : BundleChapter{}) << chapter;
      }
      EXPECT_EQ(fixture.bundle.Totals().notImported, documents - 5u);
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    }
  }
}

TEST(Bundle, AChapterCommittedAgainTakesTheOldOnesPlaceInTheCount) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(3, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter first;
  ASSERT_TRUE(fixture.WriteSegment(4, Pattern(900, 1), first));
  ASSERT_EQ(fixture.bundle.Commit(1, first), BundleStatus::kOk);
  BundleChapter second;
  ASSERT_TRUE(fixture.WriteSegment(6, Pattern(300, 2), second));
  ASSERT_EQ(fixture.bundle.Commit(1, second), BundleStatus::kOk);
  EXPECT_EQ(second.segmentOffset, first.segmentOffset + 1024) << "the old segment is left where it is";

  for (int opened = 0; opened < 2; ++opened) {
    BundleChapter record;
    ASSERT_EQ(fixture.bundle.ReadChapter(1, record), BundleStatus::kOk);
    EXPECT_EQ(record, second);
    BundleTotals expected{.notImported = 2, .characters = second.characters, .classCharacters = {}};
    std::copy(second.classCharacters.begin(), second.classCharacters.end(), expected.classCharacters.begin());
    EXPECT_EQ(fixture.bundle.Totals(), expected) << "as counted when opened, and as kept while open";
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  }
}

TEST(Bundle, CommitsAChapterThatHasNoSegment) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(3, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  const std::uint32_t end = fixture.bundle.DataEnd();
  BundleChapter none;
  none.state = ChapterState::kNotDocument;
  fixture.storage.written.clear();
  const int syncs = fixture.storage.syncs;
  ASSERT_EQ(fixture.bundle.Commit(2, none), BundleStatus::kOk);
  EXPECT_EQ(fixture.bundle.DataEnd(), end);
  EXPECT_EQ(fixture.storage.written.size(), 1u) << "the slot, and nothing else";
  EXPECT_EQ(fixture.storage.syncs, syncs + 1);
  BundleChapter refused;
  refused.state = ChapterState::kRefused;
  refused.reason = 4;
  ASSERT_EQ(fixture.bundle.Commit(0, refused), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter record;
  ASSERT_EQ(fixture.bundle.ReadChapter(2, record), BundleStatus::kOk);
  EXPECT_EQ(record, none);
  ASSERT_EQ(fixture.bundle.ReadChapter(0, record), BundleStatus::kOk);
  EXPECT_EQ(record, refused);
  EXPECT_EQ(fixture.bundle.Totals().notImported, 1u);
}

TEST(Bundle, RefusesToCommitWhatCannotBe) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(3, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter record;
  ASSERT_TRUE(fixture.WriteSegment(1, Pattern(700, 1), record));
  const Bytes before = fixture.OnCard();

  EXPECT_EQ(fixture.bundle.Commit(3, record), BundleStatus::kMisused) << "no such chapter";
  BundleChapter wrong = record;
  wrong.segmentOffset += 512;
  wrong.segmentBytes = 100;
  wrong.directoryOffset = 0;
  ASSERT_LE(wrong.segmentOffset + wrong.segmentBytes, fixture.OnCard().size()) << "it is all in the file";
  EXPECT_EQ(fixture.bundle.Commit(1, wrong), BundleStatus::kMisused) << "not where the segments end";
  wrong = record;
  wrong.segmentBytes = 701;
  EXPECT_EQ(fixture.bundle.Commit(1, wrong), BundleStatus::kMisused) << "longer than was written";
  wrong = record;
  wrong.directoryOffset = 661;
  EXPECT_EQ(fixture.bundle.Commit(1, wrong), BundleStatus::kMisused) << "a directory that runs out of its segment";
  wrong = record;
  wrong.state = static_cast<ChapterState>(5);
  EXPECT_EQ(fixture.bundle.Commit(1, wrong), BundleStatus::kMisused) << "a state there is none of";
  wrong = BundleChapter{};
  wrong.segmentOffset = record.segmentOffset;
  EXPECT_EQ(fixture.bundle.Commit(1, wrong), BundleStatus::kMisused) << "a place, and no segment";

  EXPECT_EQ(fixture.OnCard(), before) << "none of which wrote anything";
  EXPECT_TRUE(fixture.bundle.IsOpen());
  EXPECT_EQ(fixture.bundle.Commit(1, record), BundleStatus::kOk) << "and the bundle is as good as it was";
}

TEST(Bundle, TakesTheOtherSlotWhenTheNewerIsSpoiled) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter first;
  ASSERT_TRUE(fixture.WriteSegment(0, Pattern(700, 1), first));
  ASSERT_EQ(fixture.bundle.Commit(0, first), BundleStatus::kOk);
  BundleChapter second;
  ASSERT_TRUE(fixture.WriteSegment(1, Pattern(700, 2), second));
  ASSERT_EQ(fixture.bundle.Commit(1, second), BundleStatus::kOk);
  // The second commit wrote slot 0, which is now the newer.
  Bytes& bytes = fixture.storage.entries.at(kPath).data;
  ASSERT_EQ(U32(bytes, 8192 + 4), 4u);
  bytes[8192 + 300] ^= 0x01U;

  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  EXPECT_EQ(fixture.bundle.Sequence(), 3u);
  BundleChapter record;
  ASSERT_EQ(fixture.bundle.ReadChapter(0, record), BundleStatus::kOk);
  EXPECT_EQ(record, first);
  ASSERT_EQ(fixture.bundle.ReadChapter(1, record), BundleStatus::kOk);
  EXPECT_EQ(record, BundleChapter{}) << "the chapter the spoiled slot recorded is not imported";
  EXPECT_EQ(fixture.bundle.DataEnd(), second.segmentOffset) << "and its segment's place is where the next is written";
  EXPECT_EQ(fixture.bundle.Totals().notImported, 4u);

  // The next commit writes over the spoiled slot, and all is well again.
  BundleChapter third;
  ASSERT_TRUE(fixture.WriteSegment(2, Pattern(100, 3), third));
  ASSERT_EQ(fixture.bundle.Commit(1, third), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  EXPECT_EQ(fixture.bundle.Sequence(), 4u);
  ASSERT_EQ(fixture.bundle.ReadChapter(1, record), BundleStatus::kOk);
  EXPECT_EQ(record, third);

  // Both spoiled: there is nothing to go by.
  bytes[8192 + 300] ^= 0x01U;
  bytes[12288 + 300] ^= 0x01U;
  EXPECT_EQ(fixture.Open(), BundleStatus::kDamaged);
  EXPECT_FALSE(fixture.bundle.IsOpen());
}

TEST(Bundle, FindsAnyByteOfItsHeaderOrOfTheSlotInForceThatHasChanged) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(60, 2), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter committed;
  ASSERT_TRUE(fixture.WriteSegment(7, Pattern(700, 1), committed));
  ASSERT_EQ(fixture.bundle.Commit(50, committed), BundleStatus::kOk);
  const std::uint32_t slot =
      fixture.bundle.Table(BundleTable::kContents).offset + fixture.bundle.Table(BundleTable::kContents).bytes;
  const std::uint32_t slots = ((slot + 4095) / 4096) * 4096;
  const std::uint32_t used = 24 + (60 * 88);
  Bytes& bytes = fixture.storage.entries.at(kPath).data;

  // The header page: nothing in it can change and the file still open.
  for (std::size_t at = 0; at < 4096; ++at) {
    bytes[at] ^= 0x40U;
    const BundleStatus status = fixture.Open();
    ASSERT_NE(status, BundleStatus::kOk) << at;
    ASSERT_NE(status, BundleStatus::kStoreFailed) << at;
    bytes[at] ^= 0x40U;
  }
  // The slot in force is the second, after one commit: with any byte it
  // uses changed, the first is taken, which has no chapter.
  for (std::size_t at = 0; at < used; ++at) {
    bytes[slots + 8192 + at] ^= 0x40U;
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << at;
    ASSERT_EQ(fixture.bundle.Sequence(), 2u) << at;
    ASSERT_EQ(fixture.bundle.Totals().notImported, 60u) << at;
    bytes[slots + 8192 + at] ^= 0x40U;
  }
  // What it does not use is no part of it.
  bytes[slots + 8192 + used] ^= 0x40U;
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  EXPECT_EQ(fixture.bundle.Sequence(), 3u);
}

TEST(Bundle, SaysWhenAFileIsNoBundleOrOneOfAnotherVersion) {
  BundleFixture fixture;
  fixture.storage.Put(kPath, {});
  EXPECT_EQ(fixture.Open(), BundleStatus::kNotBundle);
  fixture.storage.Put(kPath, Pattern(4095, 3));
  EXPECT_EQ(fixture.Open(), BundleStatus::kNotBundle);
  fixture.storage.Put(kPath, Pattern(50'000, 3));
  EXPECT_EQ(fixture.Open(), BundleStatus::kNotBundle);
  fixture.storage.Put(kPath, Bytes(50'000, 0));
  EXPECT_EQ(fixture.Open(), BundleStatus::kNotBundle) << "the page a bundle being made has for a header";

  ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
  Bytes& bytes = fixture.storage.entries.at(kPath).data;
  bytes[4] = 2;
  EXPECT_EQ(fixture.Open(), BundleStatus::kOtherVersion) << "whatever else it holds";
  bytes[4] = 1;
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  // Cut off part way through the second slot, and before the first.
  fixture.bundle.Close();
  fixture.file.Close();
  bytes.resize(16'000);
  EXPECT_EQ(fixture.Open(), BundleStatus::kDamaged);
  fixture.bundle.Close();
  fixture.file.Close();
  bytes.resize(8'000);
  EXPECT_EQ(fixture.Open(), BundleStatus::kDamaged);
  EXPECT_FALSE(fixture.bundle.IsOpen());
  BundleChapter record;
  EXPECT_EQ(fixture.bundle.ReadChapter(0, record), BundleStatus::kMisused) << "nothing is to be had of it";
}

TEST(Bundle, AHeaderThatPassesItsChecksumMustStillSayWhatAHeaderSays) {
  // Each of these is a header with one number changed and its checksum put
  // right: what a bundle made by a faulty maker would be.
  struct Change {
    std::size_t at;
    std::uint32_t add;
    const char* what;
  };
  for (const Change& change :
       {Change{48, 1, "the first table does not follow the header page"},
        Change{52, 1, "a table is longer, so the next does not follow it"},
        Change{28, 1, "more documents than the spine's table has records"},
        Change{30, 1, "more pictures than their table has records"}, Change{36, 4096, "the slots a page further on"},
        Change{40, 4096, "slots a page longer than their records want"},
        Change{44, 512, "segments that do not begin after the slots"}}) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(5, 3), BundleStatus::kOk);
    Bytes& bytes = fixture.storage.entries.at(kPath).data;
    const std::uint32_t value = U32(bytes, change.at) + change.add;
    for (std::size_t i = 0; i < (change.at == 28 || change.at == 30 ? 2u : 4u); ++i) {
      bytes[change.at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    const std::uint32_t checksum = Crc(bytes, 0, 4092);
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[4092 + i] = static_cast<std::uint8_t>(checksum >> (8 * i));
    }
    EXPECT_EQ(fixture.Open(), BundleStatus::kDamaged) << change.what;
  }
}

TEST(Bundle, ASlotThatPassesItsChecksumMustStillHoldRecords) {
  // The same for a slot: a record with one number changed, and the slot's
  // checksum put right. The other slot is then the one taken.
  struct Change {
    std::size_t at;
    std::uint32_t value;
    const char* what;
  };
  for (const Change& change :
       {Change{0, 5, "a state there is none of"}, Change{4, 16384 + 4096, "a segment past where the segments end"},
        Change{4, 16384 - 512, "a segment among the slots"},
        Change{4, 16384 + 100, "a segment that begins part way through a sector"},
        Change{8, 2000, "a segment that runs past where the segments end"},
        Change{16, 701, "a directory that runs out of its segment"}}) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(5, 3), BundleStatus::kOk);
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    BundleChapter record;
    ASSERT_TRUE(fixture.WriteSegment(2, Pattern(700, 1), record));
    record.directoryOffset = 0;
    ASSERT_EQ(fixture.bundle.Commit(2, record), BundleStatus::kOk);
    Bytes& bytes = fixture.storage.entries.at(kPath).data;
    const std::size_t slot = 12288;
    const std::size_t at = slot + 20 + 176 + change.at;
    for (std::size_t i = 0; i < (change.at == 0 ? 1u : 4u); ++i) {
      bytes[at + i] = static_cast<std::uint8_t>(change.value >> (8 * i));
    }
    const std::uint32_t checksum = Crc(bytes, slot, slot + 460);
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[slot + 460 + i] = static_cast<std::uint8_t>(checksum >> (8 * i));
    }
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << change.what;
    EXPECT_EQ(fixture.bundle.Sequence(), 2u) << change.what;
  }
  // And what the start of a slot says: its segments' end out of the file,
  // or not on a sector.
  for (const std::uint32_t end : {16384U + 2048U, 16384U + 1000U, 8192U}) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(5, 3), BundleStatus::kOk);
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    BundleChapter record;
    ASSERT_TRUE(fixture.WriteSegment(2, Pattern(700, 1), record));
    ASSERT_EQ(fixture.bundle.Commit(2, record), BundleStatus::kOk);
    Bytes& bytes = fixture.storage.entries.at(kPath).data;
    const std::size_t slot = 12288;
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[slot + 12 + i] = static_cast<std::uint8_t>(end >> (8 * i));
    }
    const std::uint32_t checksum = Crc(bytes, slot, slot + 460);
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[slot + 460 + i] = static_cast<std::uint8_t>(checksum >> (8 * i));
    }
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << end;
    EXPECT_EQ(fixture.bundle.Sequence(), 2u) << end;
  }
}

TEST(Bundle, DoesNoMoreThanAPageOfWorkAStep) {
  // A book of 2,014 documents: slots of 44 pages.
  BundleFixture fixture;
  BundleMaker maker(fixture.page);
  ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kCreate), StorageStatus::kOk);
  ASSERT_EQ(maker.Begin(fixture.file, BookOf(2014, 0)), BundleStatus::kOk);
  ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
  // Ten bytes of strings, so that the spine's table begins part way through a page of the file.
  ASSERT_EQ(maker.Write(Pattern(10, 1)), BundleStatus::kOk);
  ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
  for (std::uint32_t n = 0; n < 2014; ++n) {
    ASSERT_EQ(maker.WriteFile(FileNumber(n, 1)), BundleStatus::kOk);
  }
  ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
  ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
  ASSERT_EQ(maker.Finish(), BundleStatus::kOk);
  FakeStorage& storage = fixture.storage;
  const auto stepped = [&storage](const auto& step, const char* what) {
    int steps = 0;
    for (bool more = true; more; ++steps) {
      const std::size_t reads = storage.reads.size();
      const int writes = storage.writes;
      const int syncs = storage.syncs;
      more = step();
      EXPECT_LE(storage.reads.size(), reads + 1) << what;
      EXPECT_LE(storage.writes, writes + 1) << what;
      EXPECT_LE(storage.syncs, syncs + 1) << what;
      EXPECT_LE(storage.reads.empty() ? 0u : storage.reads.back().size, 4096u) << what;
      EXPECT_LE(storage.written.empty() ? 0u : storage.written.back().size, 4096u) << what;
    }
    return steps;
  };
  EXPECT_EQ(stepped([&maker] { return maker.Step(); }, "making"), 44 + 44 + 1 + 1)
      << "a page of a slot a step, the header page, the sync";
  ASSERT_EQ(maker.Status(), BundleStatus::kOk);
  fixture.file.Close();

  ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kUpdate), StorageStatus::kOk);
  Bundle& bundle = fixture.bundle;
  ASSERT_EQ(bundle.BeginOpen(fixture.file), BundleStatus::kOk);
  EXPECT_EQ(stepped([&bundle] { return bundle.Step(); }, "opening"), 1 + 2 + 44)
      << "the header page, the start of each slot, the slot";
  ASSERT_EQ(bundle.Status(), BundleStatus::kOk);

  ASSERT_EQ(bundle.BeginCheck(BundleTable::kSpine), BundleStatus::kOk);
  EXPECT_EQ(stepped([&bundle] { return bundle.Step(); }, "checking"), 12)
      << "48,336 bytes from ten bytes into a page: to the end of that page, then a page of the file a step";
  ASSERT_EQ(bundle.Status(), BundleStatus::kOk);

  BundleChapter record;
  ASSERT_TRUE(fixture.WriteSegment(1, Pattern(700, 1), record));
  ASSERT_EQ(bundle.BeginCommit(2013, record), BundleStatus::kOk);
  EXPECT_EQ(stepped([&bundle] { return bundle.Step(); }, "committing"), 1 + 1 + 44 + 1)
      << "the segment's sector filled, a sync, the slot, a sync";
  ASSERT_EQ(bundle.Status(), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter got;
  ASSERT_EQ(fixture.bundle.ReadChapter(2013, got), BundleStatus::kOk);
  EXPECT_EQ(got, record);
}

TEST(Bundle, IsNotToBeAskedAnythingWhileWorkIsUnderWay) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(100, 2), BundleStatus::kOk);
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  BundleChapter record;
  ASSERT_TRUE(fixture.WriteSegment(1, Pattern(700, 1), record));
  ASSERT_EQ(fixture.bundle.BeginCommit(3, record), BundleStatus::kOk);
  ASSERT_TRUE(fixture.bundle.Step());
  BundleFile file;
  BundleChapter chapter;
  std::array<std::uint8_t, 4> some{};
  EXPECT_EQ(fixture.bundle.ReadSpine(0, file), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.ReadPicture(0, file), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.ReadChapter(0, chapter), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.ReadTable(BundleTable::kStrings, 0, some), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.BeginCheck(BundleTable::kSpine), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.BeginCommit(4, BundleChapter{}), BundleStatus::kMisused);
  EXPECT_EQ(fixture.bundle.Status(), BundleStatus::kOk) << "and the work goes on";
  while (fixture.bundle.Step()) {
  }
  EXPECT_EQ(fixture.bundle.Status(), BundleStatus::kOk);
  EXPECT_EQ(fixture.bundle.ReadChapter(3, chapter), BundleStatus::kOk);
  EXPECT_EQ(chapter, record);

  // And nothing is to be asked of one that is not open, or was given no
  // page to work in.
  Bundle closed(fixture.page);
  EXPECT_EQ(closed.ReadChapter(0, chapter), BundleStatus::kMisused);
  EXPECT_EQ(closed.BeginCheck(BundleTable::kSpine), BundleStatus::kMisused);
  EXPECT_EQ(closed.BeginCommit(0, BundleChapter{}), BundleStatus::kMisused);
  EXPECT_FALSE(closed.Step());
  std::array<std::uint8_t, 512> small{};
  Bundle cramped(small);
  EXPECT_EQ(cramped.Open(fixture.file), BundleStatus::kMisused);
  BundleMaker crampedMaker(small);
  EXPECT_EQ(crampedMaker.Begin(fixture.file, BookOf(1, 0)), BundleStatus::kMisused);
}

TEST(Bundle, SaysWhichEpubItIsOf) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
  EXPECT_FALSE(fixture.bundle.IsOf(1'234'567, 0x5B7A1234, 0xD1EC7000)) << "not open";
  ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
  EXPECT_TRUE(fixture.bundle.IsOf(1'234'567, 0x5B7A1234, 0xD1EC7000));
  EXPECT_FALSE(fixture.bundle.IsOf(1'234'568, 0x5B7A1234, 0xD1EC7000)) << "another size";
  EXPECT_FALSE(fixture.bundle.IsOf(1'234'567, 0x5B7A1235, 0xD1EC7000)) << "written since";
  EXPECT_FALSE(fixture.bundle.IsOf(1'234'567, 0x5B7A1234, 0xD1EC7001)) << "another directory";
}

TEST(Bundle, EndsInAStatusWhenStorageFails) {
  // Reading, at each read of opening.
  for (int failAt = 0; failAt < 4; ++failAt) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
    ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kUpdate), StorageStatus::kOk);
    ASSERT_EQ(fixture.bundle.BeginOpen(fixture.file), BundleStatus::kOk);
    for (int step = 0; step < failAt; ++step) {
      ASSERT_TRUE(fixture.bundle.Step());
    }
    fixture.storage.failReads = true;
    while (fixture.bundle.Step()) {
    }
    EXPECT_EQ(fixture.bundle.Status(), BundleStatus::kStoreFailed) << failAt;
    EXPECT_FALSE(fixture.bundle.IsOpen());
  }
  // Writing and syncing, in a commit: the bundle is then no longer open,
  // and opening it again finds what the card has.
  for (const int fault : {0, 1}) {
    BundleFixture fixture;
    ASSERT_EQ(fixture.Make(5, 0), BundleStatus::kOk);
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    BundleChapter record;
    ASSERT_TRUE(fixture.WriteSegment(1, Pattern(700, 1), record));
    fixture.storage.failWrites = fault == 0 ? StorageStatus::kNoSpace : StorageStatus::kOk;
    fixture.storage.failSyncs = fault == 1;
    EXPECT_EQ(fixture.bundle.Commit(1, record), BundleStatus::kStoreFailed) << fault;
    EXPECT_FALSE(fixture.bundle.IsOpen());
    fixture.storage.failWrites = StorageStatus::kOk;
    fixture.storage.failSyncs = false;
    ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
    EXPECT_EQ(fixture.bundle.Totals().notImported, 5u);
  }
  // And at every step of a commit, each of the three things it asks of
  // storage: a commit that does not end well leaves the bundle not open,
  // and the card with every chapter committed before.
  for (const int fault : {0, 1, 2}) {
    for (int stepsBefore = 0;; ++stepsBefore) {
      BundleFixture fixture;
      ASSERT_EQ(fixture.Make(100, 0), BundleStatus::kOk);
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
      BundleChapter earlier;
      ASSERT_TRUE(fixture.WriteSegment(1, Pattern(300, 1), earlier));
      ASSERT_EQ(fixture.bundle.Commit(7, earlier), BundleStatus::kOk);
      BundleChapter record;
      ASSERT_TRUE(fixture.WriteSegment(2, Pattern(700, 2), record));
      ASSERT_EQ(fixture.bundle.BeginCommit(50, record), BundleStatus::kOk);
      bool more = true;
      for (int step = 0; step < stepsBefore && more; ++step) {
        more = fixture.bundle.Step();
      }
      if (!more) {
        EXPECT_EQ(stepsBefore, 1 + 1 + 3 + 1) << "the segment filled out and synced, three pages of slot, a sync";
        break;
      }
      fixture.storage.failReads = fault == 0;
      fixture.storage.failWrites = fault == 1 ? StorageStatus::kNoSpace : StorageStatus::kOk;
      fixture.storage.failSyncs = fault == 2;
      while (fixture.bundle.Step()) {
      }
      // It failed, unless what was left of it asked no such thing.
      const bool failed = fixture.bundle.Status() != BundleStatus::kOk;
      EXPECT_TRUE(fixture.bundle.Status() == BundleStatus::kOk ||
                  fixture.bundle.Status() == BundleStatus::kStoreFailed);
      EXPECT_EQ(fixture.bundle.IsOpen(), !failed) << fault << " after " << stepsBefore << " steps";
      fixture.storage.failReads = false;
      fixture.storage.failWrites = StorageStatus::kOk;
      fixture.storage.failSyncs = false;
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << fault << " after " << stepsBefore << " steps";
      BundleChapter got;
      ASSERT_EQ(fixture.bundle.ReadChapter(7, got), BundleStatus::kOk);
      EXPECT_EQ(got, earlier) << fault << " after " << stepsBefore << " steps";
      ASSERT_EQ(fixture.bundle.ReadChapter(50, got), BundleStatus::kOk);
      EXPECT_TRUE(got == BundleChapter{} || got == record);
      if (!failed) {
        EXPECT_EQ(got, record);
      }
    }
  }
  // And in the making.
  BundleFixture fixture;
  fixture.storage.failSyncs = true;
  EXPECT_EQ(fixture.Make(5, 0), BundleStatus::kStoreFailed);
  fixture.storage.failSyncs = false;
  fixture.storage.failWrites = StorageStatus::kNoSpace;
  EXPECT_EQ(fixture.Make(5, 0), BundleStatus::kStoreFailed);
}

TEST(BundleMaker, TakesItsTablesInOrderAndARecordForEveryFile) {
  BundleFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kCreate), StorageStatus::kOk);
  BundleMaker maker(fixture.page);
  EXPECT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kMisused) << "nothing begun";
  EXPECT_EQ(maker.Finish(), BundleStatus::kMisused);
  EXPECT_FALSE(maker.Step());
  ASSERT_EQ(maker.Begin(fixture.file, BookOf(2, 1)), BundleStatus::kOk);
  EXPECT_EQ(maker.Write(Pattern(4, 1)), BundleStatus::kMisused) << "no table begun";
  EXPECT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kMisused) << "the strings come first";
  ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
  EXPECT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kMisused) << "once each";
  ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
  ASSERT_EQ(maker.WriteFile(FileNumber(0, 1)), BundleStatus::kOk);
  ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
  ASSERT_EQ(maker.WriteFile(FileNumber(0, 4)), BundleStatus::kOk);
  EXPECT_EQ(maker.Finish(), BundleStatus::kMisused) << "the contents not begun";
  ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
  EXPECT_EQ(maker.Finish(), BundleStatus::kMisused) << "one document's record where the book has two";
  EXPECT_FALSE(maker.Step());

  // A file that has something in it is not one to make a bundle in.
  fixture.file.Close();
  fixture.storage.Put(kPath, Pattern(10, 1));
  ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kUpdate), StorageStatus::kOk);
  EXPECT_EQ(maker.Begin(fixture.file, BookOf(2, 1)), BundleStatus::kMisused);
}

TEST(BundleMaker, LeavesNoBundleUntilItsLastStep) {
  // Stopped after any number of steps, and the file closed as it is: what
  // is there is not a bundle. The header page is written last.
  for (int steps = 0;; ++steps) {
    BundleFixture fixture;
    BundleMaker maker(fixture.page);
    ASSERT_EQ(fixture.file.Open(fixture.storage, kPath, OpenMode::kCreate), StorageStatus::kOk);
    ASSERT_EQ(maker.Begin(fixture.file, BookOf(100, 0)), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
    for (std::uint32_t n = 0; n < 100; ++n) {
      ASSERT_EQ(maker.WriteFile(FileNumber(n, 1)), BundleStatus::kOk);
    }
    ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
    ASSERT_EQ(maker.Finish(), BundleStatus::kOk);
    bool more = true;
    for (int step = 0; step < steps && more; ++step) {
      more = maker.Step();
    }
    fixture.file.Close();
    const BundleStatus status = fixture.Open();
    if (!more) {
      EXPECT_EQ(status, BundleStatus::kOk);
      EXPECT_EQ(steps, 3 + 3 + 1 + 1) << "two slots of three pages, the header page, the sync";
      break;
    }
    // The last step but one has written the header page, and only the
    // sync is to come: by then it is a bundle, if the card has it all.
    EXPECT_EQ(status, steps == 7 ? BundleStatus::kOk : BundleStatus::kNotBundle) << steps;
  }
}

// ---- The power going ----

// What was asked of a bundle up to the moment the power went, and what
// must be found of it afterwards.
struct Told {
  bool made = false;                               // The making returned, and said ok.
  std::map<std::size_t, BundleChapter> committed;  // The record of each chapter whose commit returned ok.
  std::map<std::uint32_t, Bytes> segments;         // Every segment that was written whole, by where it begins.
  std::optional<std::pair<std::size_t, BundleChapter>> inHand;  // The commit that was under way, if one was.
};

// Makes a bundle and commits chapters to it until storage fails, which is
// the power going.
void MakeAndCommit(BundleFixture& fixture, std::uint16_t documents, Told& told) {
  if (fixture.Make(documents, 2) != BundleStatus::kOk) {
    return;
  }
  told.made = true;
  if (fixture.Open() != BundleStatus::kOk) {
    return;
  }
  // Chapters in no order; one twice; one with no segment; segments that
  // end on a sector, short of one, and over several.
  const std::array<std::pair<std::size_t, std::size_t>, 6> chapters = {
      {{2, 700}, {0, 512}, {static_cast<std::size_t>(documents - 1), 3000}, {2, 100}, {1, 0}, {3, 1}}};
  std::uint32_t n = 0;
  for (const auto& [chapter, bytes] : chapters) {
    BundleChapter record;
    if (bytes == 0) {
      record.state = ChapterState::kNotDocument;
    } else {
      Bytes segment = Pattern(bytes, 100 + n);
      const std::uint32_t at = fixture.bundle.DataEnd();
      if (!fixture.WriteSegment(n, segment, record)) {
        return;
      }
      told.segments[at] = std::move(segment);
    }
    told.inHand = {chapter, record};
    if (fixture.bundle.Commit(chapter, record) != BundleStatus::kOk) {
      return;
    }
    told.committed[chapter] = record;
    told.inHand.reset();
    ++n;
  }
}

TEST(Bundle, LosesNoCommittedChapterWheneverThePowerGoes) {
  struct Torn {
    std::size_t bytes;
    bool spoils;
  };
  // What the write the power goes at leaves: nothing of itself; some of
  // its first bytes; and those with the rest of its sectors spoiled.
  const std::array<Torn, 8> torn = {
      {{0, false}, {1, false}, {511, false}, {512, false}, {513, false}, {4095, false}, {0, true}, {600, true}}};
  int runs = 0;
  int opened = 0;
  int notOpened = 0;
  for (const std::uint16_t documents : {std::uint16_t{5}, std::uint16_t{100}}) {
    // How many changes the whole of it makes to the card, uninterrupted.
    int all = 0;
    {
      BundleFixture fixture;
      Told told;
      MakeAndCommit(fixture, documents, told);
      ASSERT_EQ(told.committed.size(), 5u);
      all = fixture.storage.changes;
    }
    for (const FakeStorage::Unsynced unsynced : {FakeStorage::Unsynced::kLost, FakeStorage::Unsynced::kKept,
                                                 FakeStorage::Unsynced::kInPlace, FakeStorage::Unsynced::kLastOnly}) {
      for (int after = 0; after < all; ++after) {
        for (const Torn& tear : torn) {
          // The longer book is there for slots of several pages: three of
          // the ways a write is cut short are enough of them for it.
          if (documents == 100 && tear.bytes != 0 && tear.bytes != 513 && tear.bytes != 600) {
            continue;
          }
          const std::string what = "documents " + std::to_string(documents) + ", unsynced " +
                                   std::to_string(static_cast<int>(unsynced)) + ", the power going after " +
                                   std::to_string(after) + " changes, torn " + std::to_string(tear.bytes) +
                                   (tear.spoils ? " and spoiling" : "");
          BundleFixture fixture;
          fixture.storage.changesUntilPowerOff = after;
          fixture.storage.unsynced = unsynced;
          fixture.storage.tornBytes = tear.bytes;
          fixture.storage.tornSpoilsSectors = tear.spoils;
          Told told;
          MakeAndCommit(fixture, documents, told);
          ASSERT_FALSE(fixture.storage.powered) << what;
          ++runs;

          fixture.storage.PowerOn();
          ASSERT_EQ(fixture.storage.Start(), StorageStatus::kOk);
          fixture.bundle.Close();
          fixture.file.Close();
          if (!fixture.storage.Has(kPath)) {
            ASSERT_FALSE(told.made) << what;
            continue;
          }
          const BundleStatus status = fixture.Open();
          if (status != BundleStatus::kOk) {
            // No bundle: only if its making had not been seen through.
            ASSERT_FALSE(told.made) << what << ": " << bn::epub::BundleStatusName(status);
            ASSERT_TRUE(status == BundleStatus::kNotBundle || status == BundleStatus::kDamaged) << what;
            ++notOpened;
            continue;
          }
          ++opened;
          // A bundle: every chapter is as its last commit that returned
          // left it, or as the commit that was under way would have; and
          // a chapter that has a segment has all of it.
          const Bytes& bytes = fixture.OnCard();
          for (std::size_t chapter = 0; chapter < documents; ++chapter) {
            BundleChapter record;
            ASSERT_EQ(fixture.bundle.ReadChapter(chapter, record), BundleStatus::kOk) << what;
            const BundleChapter kept = told.committed.contains(chapter) ? told.committed[chapter] : BundleChapter{};
            const bool asInHand =
                told.inHand.has_value() && told.inHand->first == chapter && record == told.inHand->second;
            ASSERT_TRUE(record == kept || asInHand) << what << ", chapter " << chapter;
            if (record.segmentBytes != 0) {
              ASSERT_LE(record.segmentOffset + record.segmentBytes, fixture.bundle.DataEnd()) << what;
              ASSERT_LE(fixture.bundle.DataEnd(), bytes.size()) << what;
              ASSERT_TRUE(told.segments.contains(record.segmentOffset)) << what;
              ASSERT_EQ(Bytes(bytes.begin() + record.segmentOffset,
                              bytes.begin() + record.segmentOffset + record.segmentBytes),
                        told.segments[record.segmentOffset])
                  << what << ", chapter " << chapter;
            }
          }
          ASSERT_EQ(fixture.bundle.DataEnd() % 512, 0u) << what;
          for (const BundleTable table :
               {BundleTable::kStrings, BundleTable::kSpine, BundleTable::kPictures, BundleTable::kContents}) {
            ASSERT_EQ(fixture.bundle.Check(table), BundleStatus::kOk) << what;
          }

          // And it can be gone on with: another chapter, over whatever
          // the power left past where the segments end.
          BundleChapter more;
          const Bytes segment = Pattern(900, 77);
          const std::uint32_t at = fixture.bundle.DataEnd();
          ASSERT_TRUE(fixture.WriteSegment(50, segment, more)) << what;
          ASSERT_EQ(fixture.bundle.Commit(4, more), BundleStatus::kOk) << what;
          ASSERT_EQ(fixture.Open(), BundleStatus::kOk) << what;
          BundleChapter record;
          ASSERT_EQ(fixture.bundle.ReadChapter(4, record), BundleStatus::kOk) << what;
          ASSERT_EQ(record, more) << what;
          ASSERT_EQ(Bytes(fixture.OnCard().begin() + at, fixture.OnCard().begin() + at + 900), segment) << what;
          for (const auto& [chapter, kept] : told.committed) {
            if (chapter == 4 || (told.inHand.has_value() && told.inHand->first == chapter)) {
              continue;
            }
            ASSERT_EQ(fixture.bundle.ReadChapter(chapter, record), BundleStatus::kOk) << what;
            ASSERT_EQ(record, kept) << what << ", chapter " << chapter << " after another was committed";
          }
        }
      }
    }
  }
  // It was a trial of both outcomes, many times.
  EXPECT_GT(runs, 1'000);
  EXPECT_GT(opened, 500);
  EXPECT_GT(notOpened, 50);
}

TEST(Bundle, RecordsNoChapterWhoseSegmentTheCardMayNotHave) {
  // The power goes once with a segment written and synced and its chapter
  // not yet recorded: the segment's bytes lie past where the segments end.
  // Another segment is then written over them, and the power goes again,
  // the card having kept only the last thing it was given. If the chapter
  // is recorded then, its segment has to be the one that was written for
  // it: which is why a segment is synced before its slot is written.
  int leftOver = 0;
  int recorded = 0;
  for (int first = 0; first < 12; ++first) {
    for (int second = 0; second < 12; ++second) {
      BundleFixture fixture;
      ASSERT_EQ(fixture.Make(3, 0), BundleStatus::kOk);
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
      const std::uint32_t at = fixture.bundle.DataEnd();
      BundleChapter record;
      fixture.storage.changesUntilPowerOff = first;
      if (fixture.WriteSegment(1, Pattern(700, 1), record)) {
        (void)fixture.bundle.Commit(0, record);
      }
      if (fixture.storage.powered) {
        continue;  // The commit was over before the power went.
      }
      fixture.storage.PowerOn();
      ASSERT_EQ(fixture.storage.Start(), StorageStatus::kOk);
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
      BundleChapter got;
      ASSERT_EQ(fixture.bundle.ReadChapter(0, got), BundleStatus::kOk);
      if (got != BundleChapter{} || fixture.OnCard().size() < at + 700) {
        continue;  // Recorded after all, or nothing of the segment is on the card.
      }
      leftOver += second == 0 ? 1 : 0;

      const Bytes now = Pattern(700, 2);
      fixture.storage.unsynced = FakeStorage::Unsynced::kLastOnly;
      fixture.storage.changesUntilPowerOff = second;
      if (fixture.WriteSegment(2, now, record)) {
        (void)fixture.bundle.Commit(0, record);
      }
      if (fixture.storage.powered) {
        continue;
      }
      fixture.storage.PowerOn();
      ASSERT_EQ(fixture.storage.Start(), StorageStatus::kOk);
      ASSERT_EQ(fixture.Open(), BundleStatus::kOk);
      ASSERT_EQ(fixture.bundle.ReadChapter(0, got), BundleStatus::kOk);
      if (got == BundleChapter{}) {
        continue;
      }
      ++recorded;
      ASSERT_EQ(got, record) << first << " " << second;
      ASSERT_EQ(Part(fixture.OnCard(), at, 700), now)
          << "a chapter is recorded, and its segment is another's: the power went after " << first
          << " changes and then " << second;
    }
  }
  EXPECT_GT(leftOver, 0) << "the first cut never left a segment unrecorded";
  EXPECT_GT(recorded, 0) << "the second never left the chapter recorded";
}

}  // namespace
