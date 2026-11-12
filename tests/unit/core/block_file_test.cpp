#include "core/block_file.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::test::FakeStorage;
using Bytes = FakeStorage::Bytes;

// Bytes that say where in the file they are, so a misplaced read shows.
Bytes Numbered(std::size_t count, std::size_t from = 0) {
  Bytes bytes(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t at = from + i;
    bytes[i] = static_cast<std::uint8_t>((at * 31) ^ (at >> 8U));
  }
  return bytes;
}

// The present value of one of the counters kept for every BlockFile.
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

// A started storage holding one file of 10,000 numbered bytes, and a file
// object with a 4 KB buffer.
struct FileFixture {
  FileFixture() {
    storage.Put("/book", Numbered(10'000));
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
  }

  // Reads `count` bytes and checks they are the file's bytes from `from`.
  void ExpectRead(std::size_t count, std::size_t from) {
    Bytes got(count);
    ASSERT_EQ(file.Read(got), static_cast<std::ptrdiff_t>(count)) << "at " << from;
    EXPECT_EQ(got, Numbered(count, from)) << "at " << from;
  }

  FakeStorage storage;
  std::array<std::uint8_t, 4096> buffer{};
  BlockFile file{buffer};
};

TEST(BlockFile, OpensAFileAndKnowsItsSize) {
  FileFixture fixture;
  EXPECT_FALSE(fixture.file.IsOpen());
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_TRUE(fixture.file.IsOpen());
  EXPECT_EQ(fixture.file.Size(), 10'000u);
  EXPECT_EQ(fixture.file.Position(), 0u);
  EXPECT_EQ(fixture.file.Remaining(), 10'000u);
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kOk);
  EXPECT_TRUE(fixture.storage.reads.empty()) << "opening reads nothing";

  fixture.file.Close();
  EXPECT_FALSE(fixture.file.IsOpen());
  EXPECT_EQ(fixture.file.Size(), 0u);
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
}

TEST(BlockFile, SaysWhyAFileCouldNotBeOpened) {
  FileFixture fixture;
  EXPECT_EQ(fixture.file.Open(fixture.storage, "/nothing", OpenMode::kRead), StorageStatus::kNotFound);
  EXPECT_FALSE(fixture.file.IsOpen());
  EXPECT_EQ(fixture.file.Open(fixture.storage, "book", OpenMode::kRead), StorageStatus::kBadPath);
  EXPECT_EQ(fixture.file.Open(fixture.storage, "/no/such/dir/file", OpenMode::kCreate), StorageStatus::kNotFound);

  FakeStorage absent;
  absent.present = false;
  EXPECT_EQ(absent.Start(), StorageStatus::kAbsent);
  EXPECT_EQ(fixture.file.Open(absent, "/book", OpenMode::kRead), StorageStatus::kAbsent);

  // If its size cannot be had, it is not left open.
  fixture.storage.failSize = true;
  EXPECT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kFailed);
  EXPECT_FALSE(fixture.file.IsOpen());
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
}

TEST(BlockFile, ServesSmallReadsFromOneBlockAtATime) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  for (std::size_t at = 0; at < 5'000; at += 100) {
    fixture.ExpectRead(100, at);
  }
  EXPECT_EQ(fixture.file.Position(), 5'000u);
  EXPECT_EQ(fixture.file.Remaining(), 5'000u);
  // Fifty reads of the file: two of the card, a block each.
  ASSERT_EQ(fixture.storage.reads.size(), 2u);
  EXPECT_EQ(fixture.storage.reads[0].offset, 0u);
  EXPECT_EQ(fixture.storage.reads[0].size, 4096u);
  EXPECT_EQ(fixture.storage.reads[1].offset, 4096u);
  EXPECT_EQ(fixture.storage.reads[1].size, 4096u);
}

TEST(BlockFile, AsksForBlocksAtMultiplesOfTheBlockSize) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  ASSERT_TRUE(fixture.file.Seek(5'000));
  EXPECT_TRUE(fixture.storage.reads.empty()) << "moving reads nothing";
  fixture.ExpectRead(10, 5'000);
  ASSERT_EQ(fixture.storage.reads.size(), 1u);
  EXPECT_EQ(fixture.storage.reads[0].offset, 4096u);
  EXPECT_EQ(fixture.storage.reads[0].size, 4096u);

  // The last block is only as long as the file.
  ASSERT_TRUE(fixture.file.Seek(9'000));
  Bytes rest(2'000);
  EXPECT_EQ(fixture.file.Read(rest), 1'000);
  rest.resize(1'000);
  EXPECT_EQ(rest, Numbered(1'000, 9'000));
  ASSERT_EQ(fixture.storage.reads.size(), 2u);
  EXPECT_EQ(fixture.storage.reads[1].offset, 8192u);
  EXPECT_EQ(fixture.storage.reads[1].size, 1808u);

  // And at the end there is nothing more.
  EXPECT_EQ(fixture.file.Read(rest), 0);
  EXPECT_EQ(fixture.file.Remaining(), 0u);
  EXPECT_EQ(fixture.storage.reads.size(), 2u);
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kOk);
}

TEST(BlockFile, MovingAboutInsideTheBufferCostsNothing) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  fixture.ExpectRead(10, 0);
  ASSERT_TRUE(fixture.file.Seek(2'000));
  fixture.ExpectRead(10, 2'000);
  ASSERT_TRUE(fixture.file.Seek(5));
  fixture.ExpectRead(5, 5);
  ASSERT_TRUE(fixture.file.Seek(4'090));
  fixture.ExpectRead(6, 4'090);
  EXPECT_EQ(fixture.storage.reads.size(), 1u);

  EXPECT_TRUE(fixture.file.Seek(10'000)) << "the end is a place";
  EXPECT_FALSE(fixture.file.Seek(10'001));
  EXPECT_EQ(fixture.file.Position(), 10'000u);
}

TEST(BlockFile, LongReadsGoStraightToTheCallerInWholeBlocks) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  fixture.ExpectRead(10'000, 0);
  // Two whole blocks in one go, then the tail through the buffer.
  ASSERT_EQ(fixture.storage.reads.size(), 2u);
  EXPECT_EQ(fixture.storage.reads[0].offset, 0u);
  EXPECT_EQ(fixture.storage.reads[0].size, 8192u);
  EXPECT_EQ(fixture.storage.reads[1].offset, 8192u);
  EXPECT_EQ(fixture.storage.reads[1].size, 1808u);

  // Starting off a block: up to the next block through the buffer, whole
  // blocks straight in, and the rest through the buffer again.
  fixture.storage.reads.clear();
  ASSERT_TRUE(fixture.file.Seek(100));
  fixture.ExpectRead(8'192, 100);
  ASSERT_EQ(fixture.storage.reads.size(), 3u);
  EXPECT_EQ(fixture.storage.reads[0].offset, 0u);
  EXPECT_EQ(fixture.storage.reads[0].size, 4096u);
  EXPECT_EQ(fixture.storage.reads[1].offset, 4096u);
  EXPECT_EQ(fixture.storage.reads[1].size, 4096u);
  EXPECT_EQ(fixture.storage.reads[2].offset, 8192u);
  EXPECT_EQ(fixture.storage.reads[2].size, 1808u);
}

TEST(BlockFile, CountsReadsOfTheCardTheirBytesAndTheJumpsBetweenThem) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  const std::uint32_t reads = CounterNow("sd_reads");
  const std::uint32_t bytes = CounterNow("sd_read_bytes");
  const std::uint32_t seeks = CounterNow("sd_seeks");

  // From one end to the other: three blocks, each starting where the last
  // ended.
  for (std::size_t at = 0; at < 10'000; at += 100) {
    fixture.ExpectRead(100, at);
  }
  EXPECT_EQ(CounterNow("sd_reads"), reads + 3);
  EXPECT_EQ(CounterNow("sd_read_bytes"), bytes + 10'000);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks);

  // Back to the start is a jump; so is skipping a block.
  ASSERT_TRUE(fixture.file.Seek(0));
  fixture.ExpectRead(1, 0);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks + 1);
  ASSERT_TRUE(fixture.file.Seek(8'192));
  fixture.ExpectRead(1, 8'192);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks + 2);
  EXPECT_EQ(CounterNow("sd_reads"), reads + 5);

  // A second file keeps its own place.
  std::array<std::uint8_t, 512> otherBuffer{};
  BlockFile other(otherBuffer);
  ASSERT_EQ(other.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  std::array<std::uint8_t, 4> few{};
  ASSERT_EQ(other.Read(few), 4);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks + 2) << "the start of a file is where it begins";
}

TEST(BlockFile, ReadsExactlyOrNotAtAll) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  Bytes ten(10);
  ASSERT_TRUE(fixture.file.ReadAt(9'990, ten));
  EXPECT_EQ(ten, Numbered(10, 9'990));
  EXPECT_EQ(fixture.file.Position(), 10'000u);

  EXPECT_FALSE(fixture.file.ReadAt(9'995, ten)) << "the file ends first";
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kFailed);
  EXPECT_FALSE(fixture.file.ReadAt(20'000, ten)) << "past the end";

  ASSERT_TRUE(fixture.file.Seek(0));
  ASSERT_TRUE(fixture.file.ReadExactly(ten));
  EXPECT_EQ(ten, Numbered(10, 0));
  EXPECT_TRUE(fixture.file.ReadExactly({})) << "nothing is always there";
}

TEST(BlockFile, ReportsAReadThatFailed) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  fixture.storage.failReads = true;
  Bytes some(10);
  EXPECT_EQ(fixture.file.Read(some), bn::core::ByteSource::kError);
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kFailed);
  EXPECT_FALSE(fixture.file.ReadExactly(some));
  Bytes many(8'192);
  EXPECT_EQ(fixture.file.Read(many), bn::core::ByteSource::kError) << "straight to the caller, too";

  // It can be tried again.
  fixture.storage.failReads = false;
  fixture.ExpectRead(10, 0);
}

TEST(BlockFile, AFileThatHasShrunkSinceItWasOpenedReadsShort) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  fixture.storage.entries.at("/book").data.resize(6'000);
  Bytes all(10'000);
  EXPECT_EQ(fixture.file.Read(all), 6'000);
  all.resize(6'000);
  EXPECT_EQ(all, Numbered(6'000));
  EXPECT_FALSE(fixture.file.ReadExactly(std::span(all).first(10)));
}

TEST(BlockFile, WritesAtThePositionAndReadsBack) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/new", OpenMode::kCreate), StorageStatus::kOk);
  EXPECT_EQ(fixture.file.Size(), 0u);
  const std::uint32_t writes = CounterNow("sd_writes");
  const std::uint32_t bytes = CounterNow("sd_write_bytes");
  const std::uint32_t seeks = CounterNow("sd_seeks");

  ASSERT_TRUE(fixture.file.Write(Numbered(100)));
  ASSERT_TRUE(fixture.file.Write(Numbered(5'000, 100)));
  EXPECT_TRUE(fixture.file.Write({})) << "nothing to write is no trouble";
  EXPECT_EQ(fixture.file.Size(), 5'100u);
  EXPECT_EQ(fixture.file.Position(), 5'100u);
  EXPECT_EQ(fixture.storage.writes, 2) << "one write of storage for each, however long";
  EXPECT_EQ(CounterNow("sd_writes"), writes + 2);
  EXPECT_EQ(CounterNow("sd_write_bytes"), bytes + 5'100);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks);

  // Over what is there, which is a jump back.
  ASSERT_TRUE(fixture.file.Seek(50));
  ASSERT_TRUE(fixture.file.Write(Bytes(10, 0xEE)));
  EXPECT_EQ(fixture.file.Size(), 5'100u);
  EXPECT_EQ(fixture.file.Position(), 60u);
  EXPECT_EQ(CounterNow("sd_seeks"), seeks + 1);

  Bytes expected = Numbered(5'100);
  std::fill_n(expected.begin() + 50, 10, std::uint8_t{0xEE});
  EXPECT_EQ(fixture.storage.Contents("/new"), expected);
  ASSERT_TRUE(fixture.file.Seek(0));
  Bytes got(5'100);
  ASSERT_EQ(fixture.file.Read(got), 5'100);
  EXPECT_EQ(got, expected);
}

TEST(BlockFile, AWriteIsSeenByTheNextReadWhateverTheBufferHeld) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/new", OpenMode::kCreate), StorageStatus::kOk);
  ASSERT_TRUE(fixture.file.Write(Numbered(8'192)));
  ASSERT_TRUE(fixture.file.Seek(0));
  fixture.ExpectRead(10, 0);  // The first block is now in the buffer.
  ASSERT_TRUE(fixture.file.Seek(4));
  ASSERT_TRUE(fixture.file.Write(Bytes(2, 0xEE)));
  ASSERT_TRUE(fixture.file.Seek(0));
  Bytes got(8);
  ASSERT_EQ(fixture.file.Read(got), 8);
  Bytes expected = Numbered(8);
  expected[4] = 0xEE;
  expected[5] = 0xEE;
  EXPECT_EQ(got, expected);
}

TEST(BlockFile, AFileOpenedForReadingCannotBeWritten) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_FALSE(fixture.file.Write(Numbered(10)));
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kFailed);
  EXPECT_EQ(fixture.storage.writes, 0);
  EXPECT_EQ(fixture.storage.Contents("/book"), Numbered(10'000));
}

TEST(BlockFile, ReportsAWriteOrASyncThatFailed) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/new", OpenMode::kCreate), StorageStatus::kOk);
  fixture.storage.failWrites = StorageStatus::kNoSpace;
  EXPECT_FALSE(fixture.file.Write(Numbered(10)));
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kNoSpace);
  EXPECT_EQ(fixture.file.Size(), 0u);
  EXPECT_EQ(fixture.file.Position(), 0u);

  fixture.storage.failWrites = StorageStatus::kOk;
  ASSERT_TRUE(fixture.file.Write(Numbered(10)));
  const std::uint32_t syncs = CounterNow("sd_syncs");
  EXPECT_TRUE(fixture.file.Sync());
  EXPECT_EQ(fixture.storage.syncs, 1);
  EXPECT_EQ(CounterNow("sd_syncs"), syncs + 1);
  fixture.storage.failSyncs = true;
  EXPECT_FALSE(fixture.file.Sync());
  EXPECT_EQ(fixture.file.LastStatus(), StorageStatus::kFailed);
}

TEST(BlockFile, ClosesWhenItIsDoneWith) {
  FileFixture fixture;
  {
    std::array<std::uint8_t, 64> buffer{};
    BlockFile scoped(buffer);
    ASSERT_EQ(scoped.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(fixture.storage.OpenCount(), 1);
  }
  EXPECT_EQ(fixture.storage.OpenCount(), 0);

  // Opening another file closes the one before.
  fixture.storage.Put("/other", Numbered(3));
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/other", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.OpenCount(), 1);
  EXPECT_EQ(fixture.file.Size(), 3u);
  // So does failing to.
  EXPECT_EQ(fixture.file.Open(fixture.storage, "/nothing", OpenMode::kRead), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
}

TEST(BlockFile, DoesNothingWhileClosed) {
  FileFixture fixture;
  Bytes some(10);
  EXPECT_EQ(fixture.file.Read(some), bn::core::ByteSource::kError);
  EXPECT_FALSE(fixture.file.ReadExactly(some));
  EXPECT_FALSE(fixture.file.ReadAt(0, some));
  EXPECT_FALSE(fixture.file.Write(some));
  EXPECT_FALSE(fixture.file.Sync());
  fixture.file.Close();
  EXPECT_EQ(fixture.storage.closes, 0);
}

TEST(BlockFile, OnlyAFewFilesCanBeOpenAtOnce) {
  FileFixture fixture;
  std::array<std::array<std::uint8_t, 16>, 5> buffers{};
  BlockFile a(buffers[0]);
  BlockFile b(buffers[1]);
  BlockFile c(buffers[2]);
  BlockFile d(buffers[3]);
  BlockFile e(buffers[4]);
  for (BlockFile* file : {&a, &b, &c, &d}) {
    ASSERT_EQ(file->Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  }
  EXPECT_EQ(e.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kTooManyOpen);
  a.Close();
  EXPECT_EQ(e.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
}

TEST(BlockFile, WorksWithABufferOfAnySizeOrNone) {
  FakeStorage storage;
  storage.Put("/book", Numbered(1'000));
  ASSERT_EQ(storage.Start(), StorageStatus::kOk);

  // Seven bytes: blocks of seven, at multiples of seven.
  std::array<std::uint8_t, 7> small{};
  BlockFile file(small);
  ASSERT_EQ(file.Open(storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  Bytes got(1'000);
  std::size_t at = 0;
  for (const std::size_t piece : {1u, 2u, 3u, 5u, 8u, 13u, 21u, 34u, 55u, 89u, 144u, 233u, 392u}) {
    ASSERT_EQ(file.Read(std::span(got).subspan(at, piece)), static_cast<std::ptrdiff_t>(piece));
    at += piece;
  }
  ASSERT_EQ(at, 1'000u);
  EXPECT_EQ(got, Numbered(1'000));
  for (const FakeStorage::ReadCall& call : storage.reads) {
    EXPECT_EQ(call.offset % 7, 0u);
  }

  // None: every read goes to storage as it is.
  storage.reads.clear();
  BlockFile bare({});
  ASSERT_EQ(bare.Open(storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  ASSERT_TRUE(bare.Seek(3));
  Bytes five(5);
  ASSERT_EQ(bare.Read(five), 5);
  EXPECT_EQ(five, Numbered(5, 3));
  ASSERT_EQ(storage.reads.size(), 1u);
  EXPECT_EQ(storage.reads[0].offset, 3u);
  EXPECT_EQ(storage.reads[0].size, 5u);
}

// Reads a source to its end, the way a parser would.
Bytes Drain(bn::core::ByteSource& source) {
  Bytes all;
  std::array<std::uint8_t, 300> piece{};
  for (;;) {
    const std::ptrdiff_t read = source.Read(piece);
    if (read <= 0) {
      break;
    }
    all.insert(all.end(), piece.begin(), piece.begin() + read);
  }
  return all;
}

TEST(BlockFile, IsAByteSource) {
  FileFixture fixture;
  ASSERT_EQ(fixture.file.Open(fixture.storage, "/book", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(Drain(fixture.file), Numbered(10'000));
}

}  // namespace
