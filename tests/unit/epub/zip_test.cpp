#include "epub/zip.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "core/metrics.h"
#include "storage_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::BlockFile;
using bn::core::ByteSource;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::ZipArchive;
using bn::epub::ZipEntry;
using bn::epub::ZipEntryReader;
using bn::epub::ZipStatus;
using bn::test::FakeStorage;
using bn::test::Put16;
using bn::test::Put32;
using bn::test::ZipBuilder;
using Bytes = std::vector<std::uint8_t>;
namespace field = bn::test::zip_field;

Bytes BytesOf(std::string_view text) { return {text.begin(), text.end()}; }

// Words in no particular order: bytes that compress well.
Bytes Text(std::size_t size, std::uint32_t seed) {
  static constexpr std::array<std::string_view, 8> kWords = {"the ",   "page ", "turned ",  "and ",
                                                             "night ", "fell ", "quietly ", "\n"};
  std::mt19937 random(seed);
  Bytes bytes;
  bytes.reserve(size + 16);
  while (bytes.size() < size) {
    const std::string_view word = kWords[random() % kWords.size()];
    bytes.insert(bytes.end(), word.begin(), word.end());
  }
  bytes.resize(size);
  return bytes;
}

// Bytes that do not compress at all.
Bytes Noise(std::size_t size, std::uint32_t seed) {
  std::mt19937 random(seed);
  Bytes bytes(size);
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  return bytes;
}

// The present value of one of the counters.
std::uint32_t CounterNow(const std::string& name) {
  std::array<char, 2048> line{};
  bn::core::DefaultMetrics().FormatLine(line);
  const std::string text(line.data());
  const std::size_t at = text.find(name + "=");
  EXPECT_NE(at, std::string::npos) << text;
  return at == std::string::npos
             ? 0
             : static_cast<std::uint32_t>(std::strtoul(text.c_str() + at + name.size() + 1, nullptr, 10));
}

// One window for every reader the tests make, left as the last one filled
// it: nothing already in it may ever come out.
Bytes& Window() {
  static Bytes window(bn::core::Inflater::kWindowBytes);
  return window;
}

// A ZIP file on a started storage, open for reading through a buffer of
// `blockBytes`, with a directory reader and an entry reader to use on it.
struct ZipFixture {
  explicit ZipFixture(const Bytes& zip, std::size_t blockBytes = 4096) : buffer(blockBytes), file(buffer) {
    storage.Put("/book.epub", zip);
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
    EXPECT_EQ(file.Open(storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
  }

  struct Result {
    Bytes out;                          // Everything that was given out.
    ZipStatus status = ZipStatus::kOk;  // How it ended.
    bool finished = false;              // Whether it was read to its end and found sound.
  };

  // Reads `entry` to its end, or to where it fails, `piece` bytes at a time.
  Result ReadAll(const ZipEntry& entry, std::size_t piece = 1000) {
    Result result;
    result.status = reader.Open(file, entry);
    if (result.status != ZipStatus::kOk) {
      return result;
    }
    Bytes part(piece);
    for (;;) {
      const std::ptrdiff_t read = reader.Read(part);
      if (read <= 0) {
        break;
      }
      result.out.insert(result.out.end(), part.begin(), part.begin() + read);
    }
    result.status = reader.Status();
    result.finished = reader.Finished();
    return result;
  }

  // Finds the entry called `name` and reads it.
  Result ReadNamed(std::string_view name, std::size_t piece = 1000) {
    ZipEntry entry;
    const ZipStatus status = archive.Find(name, entry);
    if (status != ZipStatus::kOk) {
      Result result;
      result.status = status;
      return result;
    }
    return ReadAll(entry, piece);
  }

  FakeStorage storage;
  Bytes buffer;
  BlockFile file;
  ZipArchive archive;
  ZipEntryReader reader{Window()};
};

// A small ZIP file laid out as a book's is.
struct Sample {
  Sample() {
    builder.Add("mimetype", mimetype);
    builder.Add("META-INF/container.xml", container, {.deflate = true});
    builder.Add("OEBPS/chapter one.xhtml", chapter, {.deflate = true});
    builder.Add("OEBPS/images/cover.jpg", picture);
    zip = builder.Finish();
  }

  static constexpr std::array<std::string_view, 4> kNames = {"mimetype", "META-INF/container.xml",
                                                             "OEBPS/chapter one.xhtml", "OEBPS/images/cover.jpg"};
  Bytes mimetype = BytesOf("application/epub+zip");
  Bytes container = Text(300, 1);
  Bytes chapter = Text(5'000, 2);
  Bytes picture = Noise(3'000, 3);
  ZipBuilder builder;
  Bytes zip;
};

// ---- The directory ----

TEST(ZipArchive, FindsTheDirectoryAndCountsTheEntries) {
  const Sample sample;
  ZipFixture f(sample.zip);
  EXPECT_FALSE(f.archive.IsOpen());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  EXPECT_TRUE(f.archive.IsOpen());
  EXPECT_EQ(f.archive.EntryCount(), 4u);

  f.archive.Close();
  EXPECT_FALSE(f.archive.IsOpen());
  EXPECT_EQ(f.archive.EntryCount(), 0u);
  ZipEntry entry;
  EXPECT_EQ(f.archive.Find("mimetype", entry), ZipStatus::kNotOpen);
  ZipArchive::Cursor cursor;
  EXPECT_EQ(f.archive.Next(cursor, entry, {}), ZipStatus::kNotOpen);
}

TEST(ZipArchive, SaysWhatTheDirectorySaysOfAnEntry) {
  const Sample sample;
  ZipFixture f(sample.zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

  ZipEntry chapter;
  ASSERT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", chapter), ZipStatus::kOk);
  EXPECT_EQ(chapter.method, bn::epub::kZipDeflated);
  EXPECT_EQ(chapter.size, 5'000u);
  EXPECT_EQ(chapter.compressedSize, sample.builder.At(2).dataSize);
  EXPECT_LT(chapter.compressedSize, 2'500u);
  EXPECT_EQ(chapter.headerOffset, sample.builder.At(2).header);
  EXPECT_EQ(chapter.crc32, crc32(0, sample.chapter.data(), static_cast<uInt>(sample.chapter.size())));
  EXPECT_EQ(chapter.nameLength, 23u);
  EXPECT_EQ(chapter.flags, 0u);

  ZipEntry mimetype;
  ASSERT_EQ(f.archive.Find("mimetype", mimetype), ZipStatus::kOk);
  EXPECT_EQ(mimetype.method, bn::epub::kZipStored);
  EXPECT_EQ(mimetype.size, 20u);
  EXPECT_EQ(mimetype.compressedSize, 20u);
  EXPECT_EQ(mimetype.headerOffset, 0u);
}

TEST(ZipArchive, MatchesNamesByteForByte) {
  const std::string longOne = std::string(300, 'x') + "1";
  const std::string longTwo = std::string(300, 'x') + "2";
  // The same word with its accent as one letter, and as a letter and a mark.
  const std::string composed = "caf\xC3\xA9";
  const std::string decomposed = "cafe\xCC\x81";
  const std::vector<std::string> names = {
      "a", "A", "ab", "chapter.xhtml", "Chapter.xhtml", "text/chapter.xhtml", longOne, longTwo, composed, decomposed};
  ZipBuilder builder;
  for (const std::string& name : names) {
    builder.Add(name, BytesOf(name));
  }
  ZipFixture f(builder.Finish());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

  // Each name finds its own entry, which holds the name.
  for (const std::string& name : names) {
    const ZipFixture::Result result = f.ReadNamed(name);
    EXPECT_EQ(result.status, ZipStatus::kOk) << name;
    EXPECT_EQ(result.out, BytesOf(name)) << name;
  }
  // And nothing else finds anything.
  ZipEntry entry;
  for (const std::string& name :
       {std::string(), std::string("b"), std::string("AB"), std::string("chapter.xhtm"), std::string("chapter.xhtml "),
        std::string("CHAPTER.XHTML"), std::string("/chapter.xhtml"), std::string("text\\chapter.xhtml"),
        std::string(300, 'x') + "3", std::string(300, 'x'), std::string(70'000, 'x')}) {
    EXPECT_EQ(f.archive.Find(name, entry), ZipStatus::kNotFound) << name.substr(0, 40);
  }
}

TEST(ZipArchive, GoesThroughTheEntriesInOrder) {
  const Sample sample;
  ZipFixture f(sample.zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

  ZipArchive::Cursor cursor;
  std::array<char, 64> name{};
  ZipEntry entry;
  for (const std::string_view expected : Sample::kNames) {
    ASSERT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk) << expected;
    EXPECT_EQ(std::string_view(name.data(), entry.nameLength), expected);
  }
  EXPECT_EQ(entry.size, 3'000u) << "the last one's";
  EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kEnd);
  EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kEnd);

  // What Next() gives is what Find() gives.
  ZipArchive::Cursor again;
  ASSERT_EQ(f.archive.Next(again, entry, name), ZipStatus::kOk);
  ASSERT_EQ(f.archive.Next(again, entry, name), ZipStatus::kOk);
  ZipEntry found;
  ASSERT_EQ(f.archive.Find("META-INF/container.xml", found), ZipStatus::kOk);
  EXPECT_EQ(found.headerOffset, entry.headerOffset);
  EXPECT_EQ(found.crc32, entry.crc32);
  EXPECT_EQ(f.ReadAll(entry).out, sample.container);

  // A name with less room than it needs: as much as fits, and its length.
  ZipArchive::Cursor third;
  std::array<char, 5> small{};
  ASSERT_EQ(f.archive.Next(third, entry, small), ZipStatus::kOk);
  EXPECT_EQ(std::string_view(small.data(), small.size()), "mimet");
  EXPECT_EQ(entry.nameLength, 8u);
  // And with no room at all.
  ASSERT_EQ(f.archive.Next(third, entry, {}), ZipStatus::kOk);
  EXPECT_EQ(entry.nameLength, 22u);
}

TEST(ZipArchive, StartsEachSearchWhereTheLastOneEnded) {
  ZipBuilder builder;
  const auto name = [](int number) { return "item" + std::to_string(1000 + number); };
  for (int i = 0; i < 100; ++i) {
    builder.Add(name(i), BytesOf(name(i)));
  }
  ZipFixture f(builder.Finish());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  ZipEntry entry;
  // How many directory records a search for `wanted` reads.
  const auto cost = [&](const std::string& wanted, ZipStatus expected) {
    const std::uint32_t before = CounterNow("zip_records");
    EXPECT_EQ(f.archive.Find(wanted, entry), expected) << wanted;
    return CounterNow("zip_records") - before;
  };

  // In the file's own order, each is the first one looked at.
  const std::uint32_t finds = CounterNow("zip_finds");
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(cost(name(i), ZipStatus::kOk), 1u) << i;
  }
  EXPECT_EQ(CounterNow("zip_finds") - finds, 100u);
  // From the end it goes round to the start.
  EXPECT_EQ(cost(name(0), ZipStatus::kOk), 1u);
  // Forward: every entry from the one after the last found, to the one
  // wanted.
  EXPECT_EQ(cost(name(50), ZipStatus::kOk), 50u);
  // One back is the longest way round.
  EXPECT_EQ(cost(name(49), ZipStatus::kOk), 99u);
  // A name that is not there costs the whole directory, and leaves the
  // next search starting where it would have.
  EXPECT_EQ(cost("nothing", ZipStatus::kNotFound), 100u);
  EXPECT_EQ(cost(name(50), ZipStatus::kOk), 1u);
  // The entry just found is the furthest away of all.
  EXPECT_EQ(cost(name(50), ZipStatus::kOk), 100u);
}

TEST(ZipArchive, ReadsTheDirectoryOnceToFindEveryEntryInOrder) {
  ZipBuilder builder;
  const auto name = [](int number) { return "OEBPS/text/chapter-" + std::to_string(10'000 + number) + ".xhtml"; };
  for (int i = 0; i < 1'000; ++i) {
    builder.Add(name(i), BytesOf("x"));
  }
  ZipFixture f(builder.Finish());
  const std::size_t directoryBytes = builder.EndRecord() - builder.Directory();
  ASSERT_GT(directoryBytes, 70'000u);

  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  EXPECT_LE(f.storage.reads.size(), 2u) << "to find the directory";
  const std::size_t before = f.storage.reads.size();
  ZipEntry entry;
  for (int i = 0; i < 1'000; ++i) {
    ASSERT_EQ(f.archive.Find(name(i), entry), ZipStatus::kOk) << i;
  }
  EXPECT_LE(f.storage.reads.size() - before, (directoryBytes / 4096) + 2);
}

TEST(ZipArchive, FindsTheEndRecordBehindAComment) {
  const Bytes contents = Text(900, 4);
  for (const std::size_t length : {1U, 21U, 22U, 23U, 255U, 256U, 257U, 4096U, 65534U, 65535U}) {
    ZipBuilder builder;
    builder.Add("only", contents, {.deflate = true});
    ZipFixture f(builder.Finish(Bytes(length, 'c')));
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk) << length;
    EXPECT_EQ(f.archive.EntryCount(), 1u) << length;
    EXPECT_EQ(f.ReadNamed("only").out, contents) << length;
  }

  // A comment that holds what looks like the start of an end record, but
  // one whose own comment would run off the end of the file.
  const Bytes comment = {'P', 'K', 5, 6, 0, 0, 0, 0, 1, 0, 1, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 'x', 'y', 'z'};
  ZipBuilder builder;
  builder.Add("only", contents);
  ZipFixture withFake(builder.Finish(comment));
  ASSERT_EQ(withFake.archive.Open(withFake.file), ZipStatus::kOk);
  EXPECT_EQ(withFake.ReadNamed("only").out, contents);

  // Bytes after the end of the ZIP file proper, which its comment length
  // does not own up to.
  ZipBuilder other;
  other.Add("only", contents);
  Bytes padded = other.Finish();
  padded.resize(padded.size() + 700, 0);
  ZipFixture withMore(padded);
  ASSERT_EQ(withMore.archive.Open(withMore.file), ZipStatus::kOk);
  EXPECT_EQ(withMore.ReadNamed("only").out, contents);
}

TEST(ZipArchive, FindsTheEndRecordWhereverItFalls) {
  // The search behind a comment goes through the end of the file a piece
  // at a time. Here the record is moved along a byte at a time, so that it
  // lies across the join between two pieces in every way it can.
  for (std::size_t pad = 0; pad < 300; ++pad) {
    ZipBuilder builder;
    builder.Add("pad", Bytes(pad, 'p'));
    ZipFixture f(builder.Finish(BytesOf("c")), 512);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk) << pad;
    ASSERT_EQ(f.archive.EntryCount(), 1u) << pad;
  }
}

TEST(ZipArchive, ReadsAFileWithNoEntries) {
  const Bytes zip = ZipBuilder().Finish();
  EXPECT_EQ(zip.size(), 22u);
  ZipFixture f(zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  EXPECT_EQ(f.archive.EntryCount(), 0u);
  ZipEntry entry;
  EXPECT_EQ(f.archive.Find("mimetype", entry), ZipStatus::kNotFound);
  ZipArchive::Cursor cursor;
  EXPECT_EQ(f.archive.Next(cursor, entry, {}), ZipStatus::kEnd);
}

TEST(ZipArchive, RefusesAFileThatIsNotAZipFile) {
  const Sample sample;
  Bytes noSignature = sample.zip;
  noSignature[sample.builder.EndRecord()] = 'Q';
  // One byte short: the end record is there, but not all of it.
  const Bytes cut(sample.zip.begin(), sample.zip.end() - 1);

  for (const Bytes& bytes :
       {Bytes(), Bytes(21, 0), Bytes(22, 0), Text(5'000, 5), Noise(100'000, 6), noSignature, cut}) {
    ZipFixture f(bytes);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kNotZip) << bytes.size();
    EXPECT_FALSE(f.archive.IsOpen());
  }

  // And a file that is not open at all.
  Bytes buffer(4096);
  BlockFile closed(buffer);
  ZipArchive archive;
  EXPECT_EQ(archive.Open(closed), ZipStatus::kNotOpen);
}

TEST(ZipArchive, RefusesZip64AndFilesInSeveralParts) {
  const Sample sample;
  const std::size_t end = sample.builder.EndRecord();
  const auto opened = [&](std::size_t at, std::uint32_t value, bool wide) {
    Bytes zip = sample.zip;
    if (wide) {
      Put32(zip, end + at, value);
    } else {
      Put16(zip, end + at, value);
    }
    ZipFixture f(zip);
    return f.archive.Open(f.file);
  };
  // The marks that say "the real number is elsewhere".
  EXPECT_EQ(opened(field::kEndEntries, 0xFFFF, false), ZipStatus::kUnsupported);
  EXPECT_EQ(opened(field::kEndEntriesHere, 0xFFFF, false), ZipStatus::kUnsupported);
  EXPECT_EQ(opened(field::kEndDirectorySize, 0xFFFFFFFF, true), ZipStatus::kUnsupported);
  EXPECT_EQ(opened(field::kEndDirectoryOffset, 0xFFFFFFFF, true), ZipStatus::kUnsupported);
  // Both counts of entries marked so, as a ZIP64 file has them.
  {
    Bytes zip = sample.zip;
    Put16(zip, end + field::kEndEntries, 0xFFFF);
    Put16(zip, end + field::kEndEntriesHere, 0xFFFF);
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kUnsupported);
  }
  // One part of several.
  EXPECT_EQ(opened(field::kEndDisk, 1, false), ZipStatus::kUnsupported);
  EXPECT_EQ(opened(field::kEndDirectoryDisk, 1, false), ZipStatus::kUnsupported);
  EXPECT_EQ(opened(field::kEndEntriesHere, 3, false), ZipStatus::kUnsupported);
}

TEST(ZipArchive, ReportsAnEntryThatNeedsZip64AndCarriesOn) {
  const Sample sample;
  for (const std::size_t at : {field::kRecordSize, field::kRecordCompressedSize, field::kRecordHeaderOffset}) {
    Bytes zip = sample.zip;
    Put32(zip, sample.builder.At(1).record + at, 0xFFFFFFFF);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

    ZipEntry entry;
    EXPECT_EQ(f.archive.Find("META-INF/container.xml", entry), ZipStatus::kUnsupported);
    EXPECT_EQ(f.ReadNamed("OEBPS/chapter one.xhtml").out, sample.chapter);
    EXPECT_EQ(f.ReadNamed("mimetype").out, sample.mimetype);

    ZipArchive::Cursor cursor;
    std::array<char, 64> name{};
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kUnsupported);
    EXPECT_EQ(std::string_view(name.data(), entry.nameLength), "META-INF/container.xml");
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk);
    EXPECT_EQ(std::string_view(name.data(), entry.nameLength), "OEBPS/chapter one.xhtml");
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kEnd);
  }
}

TEST(ZipArchive, RefusesADirectoryThatContradictsItself) {
  const Sample sample;
  const std::size_t end = sample.builder.EndRecord();
  const auto directoryBytes = static_cast<std::uint32_t>(end - sample.builder.Directory());
  ZipEntry entry;
  std::array<char, 64> name{};

  // A directory that runs into the end record.
  {
    Bytes zip = sample.zip;
    Put32(zip, end + field::kEndDirectorySize, directoryBytes + 1);
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kDamaged);
  }
  // One that starts beyond the end of the file.
  {
    Bytes zip = sample.zip;
    Put32(zip, end + field::kEndDirectoryOffset, static_cast<std::uint32_t>(zip.size()) + 5);
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kDamaged);
  }
  // More entries than the directory could hold.
  {
    Bytes zip = sample.zip;
    Put16(zip, end + field::kEndEntries, 1'000);
    Put16(zip, end + field::kEndEntriesHere, 1'000);
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kDamaged);
  }
  // A record that does not begin as one should: what comes before it can
  // still be found, and nothing after it.
  {
    Bytes zip = sample.zip;
    zip[sample.builder.At(2).record] = 'Q';
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("META-INF/container.xml", entry), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/images/cover.jpg", entry), ZipStatus::kDamaged);
    EXPECT_EQ(f.archive.Find("nothing", entry), ZipStatus::kDamaged);
    ZipArchive::Cursor cursor;
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kDamaged);
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kDamaged) << "and it stays where it was";
  }
  // A record whose name would run past the end of the directory.
  {
    Bytes zip = sample.zip;
    Put16(zip, sample.builder.At(3).record + field::kRecordNameLength, 60'000);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/images/cover.jpg", entry), ZipStatus::kDamaged);
  }
  // Likewise the two things after its name.
  for (const std::size_t at : {field::kRecordExtraLength, field::kRecordCommentLength}) {
    Bytes zip = sample.zip;
    Put16(zip, sample.builder.At(3).record + at, 1);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/images/cover.jpg", entry), ZipStatus::kDamaged) << at;
  }
  // A directory said to end before its last record does. The record is
  // there to be read, but it is not the directory's.
  {
    Bytes zip = sample.zip;
    Put32(zip, end + field::kEndDirectorySize,
          static_cast<std::uint32_t>(sample.builder.At(3).record - sample.builder.Directory()));
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/images/cover.jpg", entry), ZipStatus::kDamaged);
  }
  // One more entry promised than there are records.
  {
    Bytes zip = sample.zip;
    Put16(zip, end + field::kEndEntries, 5);
    Put16(zip, end + field::kEndEntriesHere, 5);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.EntryCount(), 5u);
    EXPECT_EQ(f.ReadNamed("OEBPS/images/cover.jpg").out, sample.picture);
    EXPECT_EQ(f.archive.Find("nothing", entry), ZipStatus::kDamaged);
    ZipArchive::Cursor cursor;
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kOk) << i;
    }
    EXPECT_EQ(f.archive.Next(cursor, entry, name), ZipStatus::kDamaged);
  }
  // Fewer promised than there are: the file is taken at its word.
  {
    Bytes zip = sample.zip;
    Put16(zip, end + field::kEndEntries, 2);
    Put16(zip, end + field::kEndEntriesHere, 2);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("META-INF/container.xml", entry), ZipStatus::kOk);
    EXPECT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kNotFound);
  }
}

TEST(ZipArchive, ReportsStorageThatFails) {
  const Sample sample;
  // A buffer so small that nearly everything asked for has to be fetched.
  {
    ZipFixture f(sample.zip, 64);
    f.storage.failReads = true;
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kStorageFailed);
    EXPECT_FALSE(f.archive.IsOpen());
  }
  {
    ZipFixture f(sample.zip, 64);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    ZipEntry entry;
    ASSERT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kOk);

    f.storage.failReads = true;
    ZipEntry other;
    EXPECT_EQ(f.archive.Find("mimetype", other), ZipStatus::kStorageFailed);
    ZipArchive::Cursor cursor;
    EXPECT_EQ(f.archive.Next(cursor, other, {}), ZipStatus::kStorageFailed);
    EXPECT_EQ(f.reader.Open(f.file, entry), ZipStatus::kStorageFailed);
    EXPECT_EQ(f.reader.Status(), ZipStatus::kStorageFailed);

    // And part of the way through an entry, kept either way.
    for (const std::string_view name : {"OEBPS/chapter one.xhtml", "OEBPS/images/cover.jpg"}) {
      f.storage.failReads = false;
      ASSERT_EQ(f.archive.Find(name, entry), ZipStatus::kOk);
      ASSERT_EQ(f.reader.Open(f.file, entry), ZipStatus::kOk);
      Bytes part(100);
      ASSERT_EQ(f.reader.Read(part), 100);
      f.storage.failReads = true;
      Bytes rest(5'000);
      std::ptrdiff_t read = f.reader.Read(rest);
      if (read > 0) {
        read = f.reader.Read(rest);  // What it already had came first.
      }
      EXPECT_EQ(read, ByteSource::kError) << name;
      EXPECT_EQ(f.reader.Status(), ZipStatus::kStorageFailed) << name;
      EXPECT_FALSE(f.reader.Finished());
    }
  }
}

TEST(ZipArchive, NamesEveryStatus) {
  std::set<std::string> names;
  for (const ZipStatus status : {ZipStatus::kOk, ZipStatus::kEnd, ZipStatus::kNotFound, ZipStatus::kNotOpen,
                                 ZipStatus::kNotZip, ZipStatus::kDamaged, ZipStatus::kBadChecksum,
                                 ZipStatus::kUnsupported, ZipStatus::kStorageFailed, ZipStatus::kNoWindow}) {
    names.insert(bn::epub::ZipStatusName(status));
  }
  EXPECT_EQ(names.size(), 10u);
  EXPECT_FALSE(names.contains("unknown"));
}

// ---- One entry ----

TEST(ZipEntryReader, ReadsEntriesKeptEitherWay) {
  int read = 0;
  for (const std::size_t size : {0U, 1U, 2U, 4095U, 4096U, 4097U, 40'000U, 100'000U}) {
    for (const bool deflate : {false, true}) {
      for (const bool noise : {false, true}) {
        const Bytes contents = noise ? Noise(size, 7) : Text(size, 8);
        ZipBuilder builder;
        builder.Add("before", Text(700, 9), {.deflate = true});
        builder.Add("it", contents, {.deflate = deflate});
        builder.Add("after", Text(700, 10));
        const Bytes zip = builder.Finish();
        for (const std::size_t blockBytes : {512U, 4096U}) {
          for (const std::size_t piece : {1U, 7U, 1'000U, 4'096U, 200'000U}) {
            if (piece < 1'000 && size > 4'097) {
              continue;  // A byte at a time is tried on the small ones.
            }
            ZipFixture f(zip, blockBytes);
            ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
            const ZipFixture::Result result = f.ReadNamed("it", piece);
            const auto what = [&] {
              return std::to_string(size) + " bytes, " + (deflate ? "deflated" : "stored") + ", blocks of " +
                     std::to_string(blockBytes) + ", " + std::to_string(piece) + " at a time";
            };
            ASSERT_EQ(result.status, ZipStatus::kOk) << what();
            ASSERT_TRUE(result.finished) << what();
            ASSERT_TRUE(result.out == contents) << what();
            EXPECT_EQ(f.reader.Remaining(), 0u);
            ++read;
          }
        }
      }
    }
  }
  EXPECT_EQ(read, (6 * 2 * 2 * 2 * 5) + (2 * 2 * 2 * 2 * 3));
}

TEST(ZipEntryReader, SaysHowMuchIsLeftAndWhenItIsDone) {
  const Bytes contents = Text(2'500, 11);
  ZipBuilder builder;
  builder.Add("it", contents, {.deflate = true});
  ZipFixture f(builder.Finish());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  ZipEntry entry;
  ASSERT_EQ(f.archive.Find("it", entry), ZipStatus::kOk);

  // Before it has been given anything to read.
  Bytes part(1'000);
  EXPECT_EQ(f.reader.Status(), ZipStatus::kNotOpen);
  EXPECT_EQ(f.reader.Read(part), ByteSource::kError);
  EXPECT_FALSE(f.reader.Finished());

  ASSERT_EQ(f.reader.Open(f.file, entry), ZipStatus::kOk);
  EXPECT_EQ(f.reader.Status(), ZipStatus::kOk);
  EXPECT_EQ(f.reader.Remaining(), 2'500u);
  EXPECT_FALSE(f.reader.Finished());

  EXPECT_EQ(f.reader.Read(part), 1'000);
  EXPECT_EQ(f.reader.Remaining(), 1'500u);
  EXPECT_EQ(f.reader.Read({}), 0) << "nothing asked for, nothing given";
  EXPECT_EQ(f.reader.Remaining(), 1'500u);
  EXPECT_EQ(f.reader.Read(part), 1'000);
  EXPECT_FALSE(f.reader.Finished());

  // The last bytes: it is checked as they are handed over.
  EXPECT_EQ(f.reader.Read(part), 500);
  EXPECT_TRUE(f.reader.Finished());
  EXPECT_EQ(f.reader.Status(), ZipStatus::kOk);
  EXPECT_EQ(f.reader.Read(part), 0);
  EXPECT_EQ(f.reader.Read(part), 0);
  EXPECT_TRUE(f.reader.Finished());
}

TEST(ZipEntryReader, ReadsAnEntryWhoseSizesFollowItsData) {
  const Bytes kept = Noise(1'500, 12);
  const Bytes squeezed = Text(9'000, 13);
  ZipBuilder builder;
  builder.Add("kept", kept, {.descriptor = true});
  builder.Add("squeezed", squeezed, {.deflate = true, .descriptor = true});
  builder.Add("last", BytesOf("last"));
  ZipFixture f(builder.Finish());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  for (const auto& [name, contents] :
       {std::pair{"kept", kept}, std::pair{"squeezed", squeezed}, std::pair{"last", BytesOf("last")}}) {
    const ZipFixture::Result result = f.ReadNamed(name);
    EXPECT_TRUE(result.finished) << name;
    EXPECT_TRUE(result.out == contents) << name;
  }
}

TEST(ZipEntryReader, StepsOverWhatAnEntrysOwnHeaderCarries) {
  // The header and the directory record each carry an "extra field", and
  // need not carry the same one; the record may have a comment too.
  const Bytes first = Text(3'000, 14);
  const Bytes second = Noise(800, 15);
  ZipBuilder builder;
  builder.Add("first", first, {.deflate = true, .headerExtra = Noise(37, 16), .recordExtra = Noise(5, 17)});
  builder.Add("second", second, {.headerExtra = Bytes(), .recordExtra = Noise(300, 18), .comment = "a note on it"});
  builder.Add("third", BytesOf("third"), {.headerExtra = Noise(9, 19)});
  ZipFixture f(builder.Finish());
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  EXPECT_TRUE(f.ReadNamed("third").out == BytesOf("third"));
  EXPECT_TRUE(f.ReadNamed("first").out == first);
  EXPECT_TRUE(f.ReadNamed("second").out == second);
  EXPECT_TRUE(f.reader.Finished());
}

TEST(ZipEntryReader, ChecksTheChecksum) {
  const Bytes contents = Text(6'000, 20);
  for (const bool deflate : {false, true}) {
    ZipBuilder builder;
    builder.Add("it", contents, {.deflate = deflate});
    builder.Add("empty", {});
    Bytes zip = builder.Finish();
    // The directory's idea of the checksum, one bit out.
    zip[builder.At(0).record + field::kRecordCrc] ^= 0x01U;
    // And an entry with nothing in it, said to have a checksum that
    // nothing has.
    Put32(zip, builder.At(1).record + field::kRecordCrc, 0xDEADBEEF);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

    const ZipFixture::Result result = f.ReadNamed("it");
    EXPECT_TRUE(result.out == contents) << "the bytes all come; then the news that they cannot be trusted";
    EXPECT_EQ(result.status, ZipStatus::kBadChecksum);
    EXPECT_FALSE(result.finished);
    Bytes part(10);
    EXPECT_EQ(f.reader.Read(part), ByteSource::kError) << "and it stays that way";

    const ZipFixture::Result empty = f.ReadNamed("empty");
    EXPECT_TRUE(empty.out.empty());
    EXPECT_EQ(empty.status, ZipStatus::kBadChecksum);
  }

  // A byte of an entry's own data changed, where no compression notices.
  ZipBuilder builder;
  builder.Add("it", contents);
  Bytes zip = builder.Finish();
  zip[builder.At(0).data + 4'000] ^= 0x10U;
  ZipFixture f(zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  const ZipFixture::Result result = f.ReadNamed("it");
  EXPECT_EQ(result.out.size(), contents.size());
  EXPECT_FALSE(result.out == contents);
  EXPECT_EQ(result.status, ZipStatus::kBadChecksum);
}

TEST(ZipEntryReader, GivesNoMoreAndNoLessThanTheDirectorySays) {
  const Bytes contents = Text(5'000, 21);
  ZipBuilder builder;
  builder.Add("it", contents, {.deflate = true});
  builder.Add("kept", contents);
  const Bytes good = builder.Finish();
  const std::size_t record = builder.At(0).record;

  // The data holds more than the directory says: what it says, and no more.
  {
    Bytes zip = good;
    Put32(zip, record + field::kRecordSize, 4'000);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("it");
    EXPECT_TRUE(result.out == Bytes(contents.begin(), contents.begin() + 4'000));
    EXPECT_EQ(result.status, ZipStatus::kDamaged);
    EXPECT_FALSE(result.finished);
  }
  // It holds less.
  {
    Bytes zip = good;
    Put32(zip, record + field::kRecordSize, 6'000);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("it");
    EXPECT_TRUE(result.out == contents);
    EXPECT_EQ(result.status, ZipStatus::kDamaged);
  }
  // It is said to hold nothing.
  {
    Bytes zip = good;
    Put32(zip, record + field::kRecordSize, 0);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("it");
    EXPECT_TRUE(result.out.empty());
    EXPECT_EQ(result.status, ZipStatus::kDamaged);
  }
  // The compressed data is cut off part of the way through.
  {
    Bytes zip = good;
    Put32(zip, record + field::kRecordCompressedSize, static_cast<std::uint32_t>(builder.At(0).dataSize / 2));
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("it");
    EXPECT_LT(result.out.size(), contents.size());
    EXPECT_TRUE(std::equal(result.out.begin(), result.out.end(), contents.begin()));
    EXPECT_EQ(result.status, ZipStatus::kDamaged);
  }
  // An entry kept as it is cannot be two sizes at once.
  for (const std::size_t at : {field::kRecordSize, field::kRecordCompressedSize}) {
    Bytes zip = good;
    Put32(zip, builder.At(1).record + at, 4'999);
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("kept");
    EXPECT_TRUE(result.out.empty());
    EXPECT_EQ(result.status, ZipStatus::kDamaged) << at;
  }
}

TEST(ZipEntryReader, RefusesWhatItCannotRead) {
  const Sample sample;
  const std::size_t record = sample.builder.At(2).record;
  const auto read = [&](std::size_t at, std::uint32_t value) {
    Bytes zip = sample.zip;
    Put16(zip, record + at, value);
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    ZipFixture::Result result = f.ReadNamed("OEBPS/chapter one.xhtml");
    if (result.status != ZipStatus::kOk) {
      Bytes part(10);
      EXPECT_EQ(f.reader.Read(part), ByteSource::kError);
      EXPECT_EQ(f.reader.Status(), result.status);
    }
    return result;
  };
  // Encrypted, one way or another.
  for (const std::uint32_t flags : {0x0001U, 0x0040U, 0x2000U, 0x0041U, 0x0809U}) {
    const ZipFixture::Result result = read(field::kRecordFlags, flags);
    EXPECT_EQ(result.status, ZipStatus::kUnsupported) << flags;
    EXPECT_TRUE(result.out.empty());
  }
  // Compressed some other way.
  for (const std::uint32_t method : {1U, 6U, 9U, 12U, 14U, 93U, 95U, 99U, 0xFFFFU}) {
    EXPECT_EQ(read(field::kRecordMethod, method).status, ZipStatus::kUnsupported) << method;
  }
  // Flags that are no obstacle: names in UTF-8, sizes after the data, and
  // the two that say how hard the compressor tried.
  for (const std::uint32_t flags : {0x0800U, 0x0008U, 0x0002U, 0x0006U, 0x080EU}) {
    const ZipFixture::Result result = read(field::kRecordFlags, flags);
    EXPECT_TRUE(result.finished) << flags;
    EXPECT_TRUE(result.out == sample.chapter) << flags;
  }
}

TEST(ZipEntryReader, NeedsAWindowOnlyForCompressedEntries) {
  const Sample sample;
  ZipFixture f(sample.zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  ZipEntry kept;
  ZipEntry squeezed;
  ASSERT_EQ(f.archive.Find("OEBPS/images/cover.jpg", kept), ZipStatus::kOk);
  ASSERT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", squeezed), ZipStatus::kOk);

  Bytes shortWindow(bn::core::Inflater::kWindowBytes - 1);
  ZipEntryReader none({});
  ZipEntryReader tooSmall(shortWindow);
  for (ZipEntryReader* reader : {&none, &tooSmall}) {
    EXPECT_EQ(reader->Open(f.file, squeezed), ZipStatus::kNoWindow);
    EXPECT_EQ(reader->Status(), ZipStatus::kNoWindow);
    Bytes out(4'000);
    EXPECT_EQ(reader->Read(out), ByteSource::kError);

    ASSERT_EQ(reader->Open(f.file, kept), ZipStatus::kOk);
    EXPECT_EQ(reader->Read(out), 3'000);
    EXPECT_TRUE(Bytes(out.begin(), out.begin() + 3'000) == sample.picture);
    EXPECT_TRUE(reader->Finished());
  }
}

TEST(ZipEntryReader, RefusesAnEntryThatIsNotWhereTheDirectorySays) {
  const Sample sample;
  const auto size = static_cast<std::uint32_t>(sample.zip.size());
  const std::size_t record = sample.builder.At(2).record;
  const std::size_t header = sample.builder.At(2).header;
  const auto status = [&](const Bytes& zip) {
    ZipFixture f(zip);
    EXPECT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("OEBPS/chapter one.xhtml");
    EXPECT_TRUE(result.out.empty());
    return result.status;
  };
  // Its header would start too near the end of the file to fit, or beyond
  // the end, or as far beyond as can be said.
  for (const std::uint32_t offset : {size - 29, size, size + 1'000, 0xFFFFFFFEU}) {
    Bytes zip = sample.zip;
    Put32(zip, record + field::kRecordHeaderOffset, offset);
    EXPECT_EQ(status(zip), ZipStatus::kDamaged) << offset;
  }
  // What is there is not a header: another kind of record, or nothing
  // that is one.
  {
    Bytes zip = sample.zip;
    Put32(zip, record + field::kRecordHeaderOffset, static_cast<std::uint32_t>(sample.builder.Directory()));
    EXPECT_EQ(status(zip), ZipStatus::kDamaged);
    zip = sample.zip;
    zip[header + field::kHeaderSignature] = 'Q';
    EXPECT_EQ(status(zip), ZipStatus::kDamaged);
  }
  // The header says it is itself so long that the data would run off the
  // end of the file.
  for (const std::size_t at : {field::kHeaderNameLength, field::kHeaderExtraLength}) {
    Bytes zip = sample.zip;
    Put16(zip, header + at, 60'000);
    EXPECT_EQ(status(zip), ZipStatus::kDamaged) << at;
  }
  // The directory says the data is longer than the rest of the file.
  {
    Bytes zip = sample.zip;
    Put32(zip, record + field::kRecordCompressedSize, size);
    EXPECT_EQ(status(zip), ZipStatus::kDamaged);
    Put32(zip, record + field::kRecordCompressedSize, 0xFFFFFFFEU);
    EXPECT_EQ(status(zip), ZipStatus::kDamaged);
  }
}

TEST(ZipEntryReader, ReportsCompressedDataThatIsDamaged) {
  // Every bit of an entry's compressed data turned over, one at a time.
  // Either the bytes still come out right (a few bits are padding) or the
  // damage is reported: never wrong bytes passed off as sound.
  const Bytes contents = Text(1'200, 22);
  ZipBuilder builder;
  builder.Add("it", contents, {.deflate = true});
  const Bytes good = builder.Finish();
  const ZipBuilder::Placed& placed = builder.At(0);
  ASSERT_LT(placed.dataSize, 600u);
  int harmless = 0;
  for (std::size_t bit = 0; bit < placed.dataSize * 8; ++bit) {
    Bytes zip = good;
    zip[placed.data + (bit / 8)] = static_cast<std::uint8_t>(zip[placed.data + (bit / 8)] ^ (1U << (bit % 8)));
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    const ZipFixture::Result result = f.ReadNamed("it", 500);
    ASSERT_LE(result.out.size(), contents.size()) << "bit " << bit;
    if (result.finished) {
      ASSERT_TRUE(result.out == contents) << "bit " << bit;
      ++harmless;
    } else {
      ASSERT_TRUE(result.status == ZipStatus::kDamaged || result.status == ZipStatus::kBadChecksum)
          << "bit " << bit << ": " << bn::epub::ZipStatusName(result.status);
    }
  }
  EXPECT_LT(harmless, 16);
}

TEST(ZipEntryName, IsReadBackFromTheEntrysOwnHeader) {
  const Sample sample;
  ZipFixture f(sample.zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  // Every entry of the file: the name the directory gives it is the name
  // read back from where the entry is.
  ZipArchive::Cursor cursor;
  std::array<char, 300> listed{};
  std::array<char, 300> name{};
  int entries = 0;
  for (;;) {
    ZipEntry entry;
    if (f.archive.Next(cursor, entry, listed) != ZipStatus::kOk) {
      break;
    }
    std::size_t length = 99;
    ASSERT_EQ(bn::epub::ReadZipEntryName(f.file, entry, name, length), ZipStatus::kOk);
    EXPECT_EQ(std::string_view(name.data(), length), std::string_view(listed.data(), entry.nameLength));
    ++entries;
  }
  EXPECT_EQ(static_cast<std::uint32_t>(entries), f.archive.EntryCount());
  EXPECT_GT(entries, 3);

  // Into less room than it needs: its start, and how long all of it is.
  ZipEntry entry;
  ASSERT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kOk);
  std::array<char, 8> small{};
  small.fill('#');
  std::size_t length = 0;
  ASSERT_EQ(bn::epub::ReadZipEntryName(f.file, entry, std::span(small).first(5), length), ZipStatus::kOk);
  EXPECT_EQ(length, 23u);
  EXPECT_EQ(std::string_view(small.data(), 8), "OEBPS###");
  ASSERT_EQ(bn::epub::ReadZipEntryName(f.file, entry, std::span<char>(), length), ZipStatus::kOk);
  EXPECT_EQ(length, 23u);
}

TEST(ZipEntryName, IsNotReadFromWhatIsNotAnEntry) {
  const Sample sample;
  const auto size = static_cast<std::uint32_t>(sample.zip.size());
  std::array<char, 300> name{};
  std::size_t length = 99;
  ZipFixture f(sample.zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
  ZipEntry entry;
  ASSERT_EQ(f.archive.Find("OEBPS/chapter one.xhtml", entry), ZipStatus::kOk);

  // Where there is no room for a header, or no header.
  for (const std::uint32_t offset :
       {size - 29, size, 0xFFFFFFFEU, static_cast<std::uint32_t>(sample.builder.Directory())}) {
    ZipEntry elsewhere = entry;
    elsewhere.headerOffset = offset;
    EXPECT_EQ(bn::epub::ReadZipEntryName(f.file, elsewhere, name, length), ZipStatus::kDamaged) << offset;
    EXPECT_EQ(length, 0u);
  }
  // A header that says its name runs past the end of the file.
  {
    Bytes zip = sample.zip;
    Put16(zip, sample.builder.At(2).header + field::kHeaderNameLength, 0xFFFF);
    ZipFixture damaged(zip);
    EXPECT_EQ(bn::epub::ReadZipEntryName(damaged.file, entry, name, length), ZipStatus::kDamaged);
  }
  // Storage that fails, and a file that is not open.
  f.storage.failReads = true;
  f.file.Close();
  ASSERT_EQ(f.file.Open(f.storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(bn::epub::ReadZipEntryName(f.file, entry, name, length), ZipStatus::kStorageFailed);
  f.file.Close();
  EXPECT_EQ(bn::epub::ReadZipEntryName(f.file, entry, name, length), ZipStatus::kNotOpen);
}

TEST(ZipEntryReader, ReadsOneEntryAfterAnother) {
  const Bytes first = Text(50'000, 23);
  const Bytes second = Text(20'000, 24);
  const Bytes third = Text(8'000, 25);
  const Bytes fourth = Text(45'000, 26);
  ZipBuilder builder;
  builder.Add("first", first, {.deflate = true});
  builder.Add("second", second, {.deflate = true});
  builder.Add("third", third, {.deflate = true});
  builder.Add("fourth", fourth, {.deflate = true});
  builder.Add("fifth", BytesOf("fifth"));
  Bytes zip = builder.Finish();
  zip[builder.At(2).data + 100] ^= 0xFFU;  // The third is damaged.
  ZipFixture f(zip);
  ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);

  EXPECT_TRUE(f.ReadNamed("first").out == first);

  // Left part of the way through.
  ZipEntry entry;
  ASSERT_EQ(f.archive.Find("second", entry), ZipStatus::kOk);
  ASSERT_EQ(f.reader.Open(f.file, entry), ZipStatus::kOk);
  Bytes part(7'000);
  ASSERT_EQ(f.reader.Read(part), 7'000);
  EXPECT_FALSE(f.reader.Finished());

  // One that fails.
  const ZipFixture::Result broken = f.ReadNamed("third");
  EXPECT_FALSE(broken.finished);
  EXPECT_NE(broken.status, ZipStatus::kOk);

  // And the reader is none the worse for either.
  const ZipFixture::Result last = f.ReadNamed("fourth");
  EXPECT_TRUE(last.finished);
  EXPECT_TRUE(last.out == fourth);
  EXPECT_TRUE(f.ReadNamed("fifth").out == BytesOf("fifth"));
  EXPECT_TRUE(f.ReadNamed("second").out == second);
}

TEST(ZipEntryReader, SharesItsFileWithTheDirectory) {
  const Bytes chapter = Text(30'000, 27);
  ZipBuilder builder;
  builder.Add("first", Text(2'000, 28));
  builder.Add("chapter", chapter, {.deflate = true});
  for (int i = 0; i < 40; ++i) {
    builder.Add("picture" + std::to_string(i), Noise(100, 29));
  }
  const Bytes zip = builder.Finish();

  // The directory searched, and gone through, between the pieces of an
  // entry being read through the same file and buffer.
  {
    ZipFixture f(zip, 512);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    ZipEntry entry;
    ASSERT_EQ(f.archive.Find("chapter", entry), ZipStatus::kOk);
    ASSERT_EQ(f.reader.Open(f.file, entry), ZipStatus::kOk);
    Bytes out;
    Bytes part(700);
    ZipArchive::Cursor cursor;
    int turn = 0;
    for (;;) {
      const std::ptrdiff_t read = f.reader.Read(part);
      ASSERT_GE(read, 0);
      if (read == 0) {
        break;
      }
      out.insert(out.end(), part.begin(), part.begin() + read);
      ZipEntry other;
      EXPECT_EQ(f.archive.Find("picture" + std::to_string(turn % 40), other), ZipStatus::kOk);
      std::array<char, 16> name{};
      (void)f.archive.Next(cursor, other, name);
      ++turn;
    }
    EXPECT_TRUE(out == chapter);
    EXPECT_TRUE(f.reader.Finished());
    EXPECT_GT(turn, 40);
  }
  // The entry read through a second file object, open on the same file.
  {
    ZipFixture f(zip);
    ASSERT_EQ(f.archive.Open(f.file), ZipStatus::kOk);
    Bytes otherBuffer(4096);
    BlockFile other(otherBuffer);
    ASSERT_EQ(other.Open(f.storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
    ZipEntry entry;
    ASSERT_EQ(f.archive.Find("chapter", entry), ZipStatus::kOk);
    ASSERT_EQ(f.reader.Open(other, entry), ZipStatus::kOk);
    Bytes out(40'000);
    EXPECT_EQ(f.reader.Read(out), 30'000);
    out.resize(30'000);
    EXPECT_TRUE(out == chapter);
    EXPECT_TRUE(f.reader.Finished());
  }
}

// ---- Files that are not to be trusted ----

// Does everything that can be done with a ZIP file. Nothing is asked of the
// answers but that there are some, in bounded time, and that no entry gives
// out more than it is said to hold; the sanitizers watch for anything read
// or written out of place.
void Exercise(const Bytes& zip) {
  ZipFixture f(zip, 512);
  if (f.archive.Open(f.file) != ZipStatus::kOk) {
    return;
  }
  ZipArchive::Cursor cursor;
  std::array<char, 24> name{};
  for (std::uint32_t count = 0; count <= f.archive.EntryCount(); ++count) {
    ZipEntry entry;
    const ZipStatus status = f.archive.Next(cursor, entry, name);
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      break;
    }
    if (status == ZipStatus::kOk) {
      const ZipFixture::Result result = f.ReadAll(entry, 300);
      EXPECT_LE(result.out.size(), entry.size);
      ZipEntry found;
      (void)f.archive.Find(std::string_view(name.data(), std::min<std::size_t>(entry.nameLength, name.size())), found);
    }
  }
  ZipEntry found;
  (void)f.archive.Find("no such entry", found);
}

// A small ZIP file with something of everything in it.
Bytes Assorted() {
  ZipBuilder builder;
  builder.Add("mimetype", BytesOf("application/epub+zip"));
  builder.Add("a.xml", Text(300, 30), {.deflate = true});
  builder.Add("b.txt", Text(60, 31), {.descriptor = true});
  builder.Add("c", Text(200, 32), {.deflate = true, .headerExtra = Noise(6, 33), .comment = "c"});
  return builder.Finish(BytesOf("end"));
}

TEST(Zip, SurvivesDamageAnywhereInAFile) {
  const Bytes good = Assorted();
  ASSERT_LT(good.size(), 900u);
  for (std::size_t bit = 0; bit < good.size() * 8; ++bit) {
    Bytes zip = good;
    zip[bit / 8] = static_cast<std::uint8_t>(zip[bit / 8] ^ (1U << (bit % 8)));
    Exercise(zip);
  }
  for (std::size_t length = 0; length < good.size(); ++length) {
    Exercise(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length)));
  }
  // Each field of every record set to the numbers most likely to upset
  // arithmetic.
  for (std::size_t at = 0; at + 4 <= good.size(); ++at) {
    for (const std::uint32_t value : {0U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFEU, 0xFFFFFFFFU}) {
      Bytes zip = good;
      Put32(zip, at, value);
      Exercise(zip);
    }
  }
}

TEST(Zip, SurvivesFilesThatWereNeverZipFiles) {
  // Noise, given just enough of a ZIP file's shape to be let in: an end
  // record at the end, and the start of a record wherever one is looked for.
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same input every run, so that a failure can be found again.
  std::mt19937 random(2026);
  // A number below `limit`.
  const auto below = [&random](std::size_t limit) { return static_cast<std::uint32_t>(random() % limit); };
  for (int trial = 0; trial < 3'000; ++trial) {
    Bytes zip = Noise(60 + below(600), below(1U << 30U));
    const std::size_t end = zip.size() - 22;
    const std::uint32_t entries = below(6);
    const std::uint32_t directory = below(end);
    const std::uint32_t directorySize = below(end - directory + 1);
    Put32(zip, end, 0x06054B50);
    Put16(zip, end + field::kEndDisk, 0);
    Put16(zip, end + field::kEndDirectoryDisk, 0);
    Put16(zip, end + field::kEndEntriesHere, entries);
    Put16(zip, end + field::kEndEntries, entries);
    Put32(zip, end + field::kEndDirectorySize, directorySize);
    Put32(zip, end + field::kEndDirectoryOffset, directory);
    Put16(zip, end + field::kEndCommentLength, 0);
    if (directory + 46 <= end) {
      Put32(zip, directory, 0x02014B50);
      // Small lengths, so that a second record is looked for too.
      Put16(zip, directory + field::kRecordNameLength, below(8));
      Put16(zip, directory + field::kRecordExtraLength, below(8));
      Put16(zip, directory + field::kRecordCommentLength, below(8));
      Put16(zip, directory + field::kRecordMethod, below(2) * 8);
      Put16(zip, directory + field::kRecordFlags, 0);
      const std::uint32_t header = below(end);
      Put32(zip, directory + field::kRecordHeaderOffset, header);
      if (header + 4 <= end) {
        Put32(zip, header, 0x04034B50);
      }
    }
    Exercise(zip);
  }
}

}  // namespace
