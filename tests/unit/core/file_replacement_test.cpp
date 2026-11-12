#include "core/file_replacement.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "storage_fakes.h"

namespace {

using bn::core::FileReplacement;
using bn::core::RecoverReplacement;
using bn::core::StorageStatus;
using bn::test::FakeStorage;
using Bytes = FakeStorage::Bytes;

const std::string kPath = "/state/settings";
const Bytes kOld(300, 0x11);
const Bytes kNew(5'000, 0x22);

struct ReplacementFixture {
  ReplacementFixture() {
    storage.Put(kPath, kOld);
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
  }

  // Writes kNew in two pieces and commits. Returns how the commit ended,
  // or the first thing to go wrong before it.
  StorageStatus Replace() {
    if (const StorageStatus status = replacement.Begin(kPath); status != StorageStatus::kOk) {
      return status;
    }
    const std::span<const std::uint8_t> contents(kNew);
    if (!replacement.File().Write(contents.first(2'000)) || !replacement.File().Write(contents.subspan(2'000))) {
      return replacement.File().LastStatus();
    }
    return replacement.Commit();
  }

  // Files with the path's name and something after it.
  std::vector<std::string> Leftovers() const {
    std::vector<std::string> found;
    for (const auto& [path, entry] : storage.entries) {
      if (path.starts_with(kPath) && path != kPath) {
        found.push_back(path);
      }
    }
    return found;
  }

  FakeStorage storage;
  std::array<std::uint8_t, 512> buffer{};
  FileReplacement replacement{storage, buffer};
};

TEST(FileReplacement, PutsNewContentsInAFilesPlace) {
  ReplacementFixture fixture;
  ASSERT_EQ(fixture.replacement.Begin(kPath), StorageStatus::kOk);
  // Until it is committed the file is as it was, and the new contents are
  // being written beside it.
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
  EXPECT_EQ(fixture.Leftovers(), (std::vector<std::string>{kPath + ".tmp"}));
  ASSERT_TRUE(fixture.replacement.File().Write(kNew));
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);

  ASSERT_EQ(fixture.replacement.Commit(), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(kPath), kNew);
  EXPECT_TRUE(fixture.Leftovers().empty());
  EXPECT_EQ(fixture.storage.OpenCount(), 0);

  // In this order: safe on the card, named as complete, and only then is
  // the old file touched.
  EXPECT_EQ(fixture.storage.log,
            (std::vector<std::string>{"sync " + kPath + ".tmp", "rename " + kPath + ".tmp " + kPath + ".new",
                                      "remove " + kPath, "rename " + kPath + ".new " + kPath}));
}

TEST(FileReplacement, WritesAFileThatWasNotThereBefore) {
  ReplacementFixture fixture;
  fixture.storage.entries.erase(kPath);
  ASSERT_EQ(fixture.Replace(), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(kPath), kNew);
  EXPECT_TRUE(fixture.Leftovers().empty());
}

TEST(FileReplacement, CanBeUsedAgain) {
  ReplacementFixture fixture;
  ASSERT_EQ(fixture.Replace(), StorageStatus::kOk);
  ASSERT_EQ(fixture.replacement.Begin(kPath), StorageStatus::kOk);
  ASSERT_TRUE(fixture.replacement.File().Write(kOld));
  ASSERT_EQ(fixture.replacement.Commit(), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
  EXPECT_EQ(fixture.replacement.Commit(), StorageStatus::kFailed) << "nothing was begun";
}

TEST(FileReplacement, AbandoningLeavesTheFileAsItWas) {
  ReplacementFixture fixture;
  ASSERT_EQ(fixture.replacement.Begin(kPath), StorageStatus::kOk);
  ASSERT_TRUE(fixture.replacement.File().Write(kNew));
  fixture.replacement.Abandon();
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
  EXPECT_TRUE(fixture.Leftovers().empty());
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
  fixture.replacement.Abandon();  // Nothing to abandon: nothing happens.

  // Going out of scope, or beginning again, abandons too.
  {
    std::array<std::uint8_t, 64> buffer{};
    FileReplacement scoped(fixture.storage, buffer);
    ASSERT_EQ(scoped.Begin(kPath), StorageStatus::kOk);
    ASSERT_TRUE(scoped.File().Write(kNew));
  }
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
  EXPECT_TRUE(fixture.Leftovers().empty());
  ASSERT_EQ(fixture.replacement.Begin(kPath), StorageStatus::kOk);
  ASSERT_EQ(fixture.replacement.Begin("/state/other"), StorageStatus::kOk);
  EXPECT_EQ(fixture.Leftovers(), std::vector<std::string>{});
  EXPECT_TRUE(fixture.storage.Has("/state/other.tmp"));
}

TEST(FileReplacement, IsEitherTheOldFileOrTheNewHoweverEarlyThePowerGoes) {
  // The power is lost after every possible number of changes to the card,
  // with a file already there and without. Afterwards, once recovery has
  // run, the file must be whole: the old contents or the new, never a
  // mixture, never a part. And if the commit said it had succeeded, the
  // new.
  for (const bool hadOldFile : {true, false}) {
    int changesInAll = 0;
    {
      ReplacementFixture complete;
      if (!hadOldFile) {
        complete.storage.entries.erase(kPath);
      }
      ASSERT_EQ(complete.Replace(), StorageStatus::kOk);
      changesInAll = complete.storage.changes;
    }
    ASSERT_GE(changesInAll, 6);

    for (int survived = 0; survived <= changesInAll; ++survived) {
      SCOPED_TRACE(std::string(hadOldFile ? "over an old file" : "no file before") + ", power lost after " +
                   std::to_string(survived) + " of " + std::to_string(changesInAll) + " changes");
      ReplacementFixture fixture;
      if (!hadOldFile) {
        fixture.storage.entries.erase(kPath);
      }
      fixture.storage.changesUntilPowerOff = survived;
      const StorageStatus committed = fixture.Replace();
      if (survived < changesInAll) {
        EXPECT_NE(committed, StorageStatus::kOk);
      }
      fixture.storage.PowerOff();  // If it had not gone already.

      // The next time the device is switched on.
      fixture.storage.PowerOn();
      ASSERT_EQ(fixture.storage.Start(), StorageStatus::kOk);
      ASSERT_EQ(RecoverReplacement(fixture.storage, kPath), StorageStatus::kOk);
      EXPECT_TRUE(fixture.Leftovers().empty());
      if (committed == StorageStatus::kOk) {
        ASSERT_TRUE(fixture.storage.Has(kPath));
        EXPECT_EQ(fixture.storage.Contents(kPath), kNew);
      } else if (fixture.storage.Has(kPath)) {
        const Bytes& found = fixture.storage.Contents(kPath);
        EXPECT_TRUE(found == kNew || (hadOldFile && found == kOld)) << found.size() << " bytes";
      } else {
        EXPECT_FALSE(hadOldFile) << "the file is gone";
      }
    }
  }
}

TEST(FileReplacement, RecoveryFinishesWhatWasCompleteAndThrowsAwayWhatWasNot) {
  FakeStorage storage;
  ASSERT_EQ(storage.Start(), StorageStatus::kOk);

  // Nothing to do.
  storage.Put(kPath, kOld);
  ASSERT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(storage.Contents(kPath), kOld);
  ASSERT_EQ(RecoverReplacement(storage, "/state/never-written"), StorageStatus::kOk);
  EXPECT_FALSE(storage.Has("/state/never-written"));

  // Contents still being written when it stopped.
  storage.Put(kPath + ".tmp", Bytes(10, 0x33));
  ASSERT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(storage.Contents(kPath), kOld);
  EXPECT_FALSE(storage.Has(kPath + ".tmp"));

  // Complete contents, the old file not yet removed.
  storage.Put(kPath + ".new", kNew);
  ASSERT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(storage.Contents(kPath), kNew);
  EXPECT_FALSE(storage.Has(kPath + ".new"));

  // Complete contents, the old file already gone.
  storage.entries.erase(kPath);
  storage.Put(kPath + ".new", kOld);
  ASSERT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(storage.Contents(kPath), kOld);

  // Both kinds of leftover: the complete one is the newer.
  storage.Put(kPath + ".new", kNew);
  storage.Put(kPath + ".tmp", Bytes(10, 0x33));
  ASSERT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(storage.Contents(kPath), kNew);
  EXPECT_FALSE(storage.Has(kPath + ".tmp"));
  EXPECT_FALSE(storage.Has(kPath + ".new"));

  // If storage will not do it, it says so and the file stays whole.
  storage.Put(kPath + ".new", kOld);
  storage.failRemoves = true;
  EXPECT_EQ(RecoverReplacement(storage, kPath), StorageStatus::kFailed);
  EXPECT_EQ(storage.Contents(kPath), kNew);
}

TEST(FileReplacement, AFailureBeforeTheNewContentsAreCompleteLeavesTheOldFile) {
  {
    ReplacementFixture fixture;
    fixture.storage.failSyncs = true;
    EXPECT_EQ(fixture.Replace(), StorageStatus::kFailed);
    EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
    EXPECT_TRUE(fixture.Leftovers().empty());
    EXPECT_EQ(fixture.storage.OpenCount(), 0);
  }
  {
    ReplacementFixture fixture;
    fixture.storage.failRenames = true;
    EXPECT_EQ(fixture.Replace(), StorageStatus::kFailed);
    EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
    EXPECT_TRUE(fixture.Leftovers().empty());
  }
  {
    ReplacementFixture fixture;
    fixture.storage.failWrites = StorageStatus::kNoSpace;
    EXPECT_EQ(fixture.Replace(), StorageStatus::kNoSpace);
    fixture.replacement.Abandon();
    EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
    EXPECT_TRUE(fixture.Leftovers().empty());
  }
}

TEST(FileReplacement, AFailureAfterTheyAreCompleteIsFinishedByRecovery) {
  ReplacementFixture fixture;
  fixture.storage.failRemoves = true;  // The old file cannot be removed.
  EXPECT_EQ(fixture.Replace(), StorageStatus::kFailed);
  EXPECT_EQ(fixture.storage.Contents(kPath), kOld);
  EXPECT_EQ(fixture.Leftovers(), (std::vector<std::string>{kPath + ".new"}));

  fixture.storage.failRemoves = false;
  ASSERT_EQ(RecoverReplacement(fixture.storage, kPath), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(kPath), kNew);
  EXPECT_TRUE(fixture.Leftovers().empty());
}

TEST(FileReplacement, IsNotStoppedByWhatAnEarlierOneLeftBehind) {
  ReplacementFixture fixture;
  fixture.storage.Put(kPath + ".new", Bytes(7, 0x44));
  fixture.storage.Put(kPath + ".tmp", Bytes(9, 0x55));
  ASSERT_EQ(fixture.Replace(), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(kPath), kNew);
  EXPECT_TRUE(fixture.Leftovers().empty());
}

TEST(FileReplacement, RefusesPathsItCannotWorkWith) {
  ReplacementFixture fixture;
  EXPECT_EQ(fixture.replacement.Begin("/"), StorageStatus::kBadPath);
  EXPECT_EQ(fixture.replacement.Begin("settings"), StorageStatus::kBadPath);
  EXPECT_EQ(fixture.replacement.Begin("/no/such/dir/settings"), StorageStatus::kNotFound);
  EXPECT_EQ(RecoverReplacement(fixture.storage, "/"), StorageStatus::kBadPath);
  EXPECT_EQ(RecoverReplacement(fixture.storage, "settings"), StorageStatus::kBadPath);

  // The names used on the way are four bytes longer than the path.
  const std::string longest = "/" + std::string(FileReplacement::kMaxPath - 1, 'a');
  ASSERT_EQ(longest.size(), bn::core::Storage::kMaxPath - 4);
  ASSERT_EQ(fixture.replacement.Begin(longest), StorageStatus::kOk);
  ASSERT_TRUE(fixture.replacement.File().Write(kNew));
  ASSERT_EQ(fixture.replacement.Commit(), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage.Contents(longest), kNew);
  EXPECT_EQ(RecoverReplacement(fixture.storage, longest), StorageStatus::kOk);
  EXPECT_EQ(fixture.replacement.Begin(longest + "a"), StorageStatus::kBadPath);
  EXPECT_EQ(RecoverReplacement(fixture.storage, longest + "a"), StorageStatus::kBadPath);
}

}  // namespace
