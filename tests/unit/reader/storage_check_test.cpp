#include "reader/storage_check.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "reader/trace_ids.h"
#include "storage_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::StorageStatus;
using bn::reader::StorageCheck;
using bn::test::FakeStorage;
using State = bn::reader::StorageCheck::State;
using Step = bn::reader::StorageCheck::Step;

struct CheckFixture {
  CheckFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Turns until the check is no longer running. Returns how many it took.
  int RunToTheEnd() {
    int turns = 0;
    while (check.CurrentState() == State::kRunning && turns < 1'000) {
      check.Tick();
      ++turns;
    }
    return turns;
  }

  // Turns until it is at `step`, about to do its first piece of it.
  void RunUntilWriting() {
    for (int turn = 0; turn < 2; ++turn) {
      check.Tick();
    }
  }

  // What the trace holds under `id`, in order.
  std::vector<std::uint32_t> Recorded(bn::core::SpanId id) {
    std::array<bn::core::TraceRecord, 64> records{};
    const std::size_t count = trace.tracer.Drain(records);
    std::vector<std::uint32_t> numbers;
    for (std::size_t i = 0; i < count; ++i) {
      if (records[i].id == id) {
        numbers.push_back(records[i].arg);
      }
    }
    return numbers;
  }

  bn::test::TraceFixture<64> trace;
  FakeStorage storage;
  StorageCheck check{storage};
};

TEST(StorageCheck, DoesNothingUntilItIsBegun) {
  CheckFixture fixture;
  EXPECT_EQ(fixture.check.CurrentState(), State::kIdle);
  fixture.check.Tick();
  fixture.check.Tick();
  EXPECT_EQ(fixture.check.CurrentState(), State::kIdle);
  EXPECT_EQ(fixture.storage.changes, 0);
  EXPECT_EQ(fixture.storage.opens, 0);
}

TEST(StorageCheck, PassesOnStorageThatWorksAndLeavesNothingBehind) {
  CheckFixture fixture;
  fixture.check.Begin();
  EXPECT_EQ(fixture.check.CurrentState(), State::kRunning);
  EXPECT_EQ(fixture.storage.changes, 0) << "nothing happens until a turn";
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check.CurrentState(), State::kPassed);
  EXPECT_EQ(fixture.check.FailedStep(), Step::kNone);

  // Only the top directory is left, and nothing is open.
  EXPECT_EQ(fixture.storage.entries.size(), 1u);
  EXPECT_EQ(fixture.storage.OpenCount(), 0);

  // What it did on the way: 64 blocks written, one write of storage each;
  // the file read back in 64 reads, then 16 more all over it.
  EXPECT_EQ(fixture.storage.writes, 64 + 2) << "and the two small files";
  std::size_t wholeBlocks = 0;
  for (const FakeStorage::ReadCall& call : fixture.storage.reads) {
    wholeBlocks += call.size == StorageCheck::kBlockBytes ? 1 : 0;
  }
  EXPECT_GE(wholeBlocks, 64u);
  EXPECT_GE(fixture.storage.syncs, 3) << "the file, and each replacement";

  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanStorageCheck), (std::vector<std::uint32_t>{1}));
}

TEST(StorageCheck, DoesOnePieceEachTurn) {
  CheckFixture fixture;
  fixture.check.Begin();
  fixture.RunUntilWriting();  // The directory, and the file created.
  ASSERT_TRUE(fixture.storage.Has("/biblionaut-check/data.bin"));
  EXPECT_EQ(fixture.storage.writes, 0);
  for (int block = 1; block <= 64; ++block) {
    fixture.check.Tick();
    EXPECT_EQ(fixture.storage.writes, block);
  }
  EXPECT_EQ(fixture.storage.Contents("/biblionaut-check/data.bin").size(), 64u * 4096u);
  fixture.check.Tick();  // The sync.
  EXPECT_EQ(fixture.storage.syncs, 1);
  fixture.check.Tick();  // Opened again.
  const std::size_t readsBefore = fixture.storage.reads.size();
  fixture.check.Tick();
  EXPECT_EQ(fixture.storage.reads.size(), readsBefore + 1) << "a block a turn";
}

TEST(StorageCheck, CanBeRunAgainButNotWhileItIsRunning) {
  CheckFixture fixture;
  fixture.check.Begin();
  fixture.RunUntilWriting();
  fixture.check.Tick();
  fixture.check.Tick();
  ASSERT_EQ(fixture.storage.writes, 2);
  fixture.check.Begin();  // Nothing: it carries on.
  fixture.check.Tick();
  EXPECT_EQ(fixture.storage.writes, 3);
  const int first = 5 + fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check.CurrentState(), State::kPassed);

  fixture.check.Begin();
  EXPECT_EQ(fixture.check.CurrentState(), State::kRunning);
  EXPECT_EQ(fixture.RunToTheEnd(), first);
  EXPECT_EQ(fixture.check.CurrentState(), State::kPassed);
  EXPECT_EQ(fixture.storage.entries.size(), 1u);
}

TEST(StorageCheck, IsNotPutOffByWhatAnEarlierRunLeftBehind) {
  CheckFixture fixture;
  fixture.storage.Put("/biblionaut-check/data.bin", FakeStorage::Bytes(10, 0x77));
  fixture.storage.Put("/biblionaut-check/settings", FakeStorage::Bytes(10, 0x77));
  fixture.storage.Put("/biblionaut-check/settings.tmp", FakeStorage::Bytes(10, 0x77));
  fixture.check.Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check.CurrentState(), State::kPassed);
  EXPECT_EQ(fixture.storage.entries.size(), 1u);
}

TEST(StorageCheck, FailsWhereStorageFailsAndSaysWhereAndHow) {
  // No card.
  {
    CheckFixture fixture;
    fixture.storage.present = false;
    fixture.storage.started = false;
    fixture.check.Begin();
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kMakeDirectory);
    EXPECT_EQ(fixture.check.FailedStatus(), StorageStatus::kAbsent);
    EXPECT_EQ(fixture.Recorded(bn::reader::kInstantStorageCheckFailed), (std::vector<std::uint32_t>{101}));
  }
  // A full card.
  {
    CheckFixture fixture;
    fixture.storage.failWrites = StorageStatus::kNoSpace;
    fixture.check.Begin();
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kWrite);
    EXPECT_EQ(fixture.check.FailedStatus(), StorageStatus::kNoSpace);
    EXPECT_EQ(fixture.storage.OpenCount(), 0);
    EXPECT_EQ(fixture.storage.entries.size(), 1u) << "it still clears up";
    std::array<bn::core::TraceRecord, 64> records{};
    const std::size_t count = fixture.trace.tracer.Drain(records);
    bool failed = false;
    bool finished = false;
    for (std::size_t i = 0; i < count; ++i) {
      failed = failed || (records[i].id == bn::reader::kInstantStorageCheckFailed && records[i].arg == 307);
      finished = finished || (records[i].id == bn::reader::kSpanStorageCheck && records[i].arg == 0);
    }
    EXPECT_TRUE(failed);
    EXPECT_TRUE(finished);
  }
  // A sync, a read, a rename that fail.
  {
    CheckFixture fixture;
    fixture.storage.failSyncs = true;
    fixture.check.Begin();
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.FailedStep(), Step::kSync);
    EXPECT_EQ(fixture.check.FailedStatus(), StorageStatus::kFailed);
  }
  {
    CheckFixture fixture;
    fixture.check.Begin();
    fixture.RunUntilWriting();
    for (int turn = 0; turn < 66; ++turn) {
      fixture.check.Tick();  // Written, synced, opened again.
    }
    fixture.storage.failReads = true;
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kRead);
    EXPECT_EQ(fixture.check.FailedStatus(), StorageStatus::kFailed);
  }
  {
    CheckFixture fixture;
    fixture.storage.failRenames = true;
    fixture.check.Begin();
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kReplace);
  }
}

TEST(StorageCheck, FailsIfWhatComesBackIsNotWhatWasWritten) {
  // One byte wrong, late in the file: the read from end to end finds it.
  {
    CheckFixture fixture;
    fixture.check.Begin();
    fixture.RunUntilWriting();
    for (int turn = 0; turn < 66; ++turn) {
      fixture.check.Tick();
    }
    fixture.storage.entries.at("/biblionaut-check/data.bin").data[200'000] ^= 0x01U;
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kRead);
    EXPECT_EQ(fixture.check.FailedStatus(), StorageStatus::kOk) << "storage answered; the bytes were wrong";
    EXPECT_EQ(fixture.Recorded(bn::reader::kInstantStorageCheckFailed), (std::vector<std::uint32_t>{600}));
  }
  // Wrong only once the whole file has been read: the scattered reads go
  // back over it.
  {
    CheckFixture fixture;
    fixture.check.Begin();
    fixture.RunUntilWriting();
    for (int turn = 0; turn < 66 + 64; ++turn) {
      fixture.check.Tick();
    }
    FakeStorage::Bytes& data = fixture.storage.entries.at("/biblionaut-check/data.bin").data;
    std::fill(data.begin(), data.end(), std::uint8_t{0});
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kReadScattered);
  }
  // A file shorter than was written.
  {
    CheckFixture fixture;
    fixture.check.Begin();
    fixture.RunUntilWriting();
    for (int turn = 0; turn < 65; ++turn) {
      fixture.check.Tick();  // Written and synced, not yet opened again.
    }
    fixture.storage.entries.at("/biblionaut-check/data.bin").data.resize(1000);
    fixture.RunToTheEnd();
    EXPECT_EQ(fixture.check.CurrentState(), State::kFailed);
    EXPECT_EQ(fixture.check.FailedStep(), Step::kReopen);
  }
}

}  // namespace
