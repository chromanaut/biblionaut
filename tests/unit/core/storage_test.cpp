#include "core/storage.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using bn::core::IsStoragePath;
using bn::core::Storage;
using bn::core::StorageStatus;

TEST(StoragePath, StartsAtTheTopAndIsMadeOfNames) {
  EXPECT_TRUE(IsStoragePath("/"));
  EXPECT_TRUE(IsStoragePath("/a"));
  EXPECT_TRUE(IsStoragePath("/biblionaut/books/0123abcd.bundle"));
  EXPECT_TRUE(IsStoragePath("/My Books/War and Peace (vol. 1).epub"));
  EXPECT_TRUE(IsStoragePath("/.hidden"));
  EXPECT_TRUE(IsStoragePath("/caf\xC3\xA9.epub")) << "bytes above ASCII are left to the file system";

  EXPECT_FALSE(IsStoragePath(""));
  EXPECT_FALSE(IsStoragePath("a"));
  EXPECT_FALSE(IsStoragePath("a/b"));
  EXPECT_FALSE(IsStoragePath("/a/"));
  EXPECT_FALSE(IsStoragePath("//a"));
  EXPECT_FALSE(IsStoragePath("/a//b"));
}

TEST(StoragePath, CannotClimbOutOrStandStill) {
  EXPECT_FALSE(IsStoragePath("/.."));
  EXPECT_FALSE(IsStoragePath("/a/../b"));
  EXPECT_FALSE(IsStoragePath("/a/.."));
  EXPECT_FALSE(IsStoragePath("/."));
  EXPECT_FALSE(IsStoragePath("/a/./b"));
}

TEST(StoragePath, HoldsOnlyWhatAnSdCardCanHold) {
  for (const char* path : {"/a:b", "/a*b", "/a?b", "/a\"b", "/a<b", "/a>b", "/a|b", "/a\\b", "/dir/a:b"}) {
    EXPECT_FALSE(IsStoragePath(path)) << path;
  }
  EXPECT_FALSE(IsStoragePath("/name."));
  EXPECT_FALSE(IsStoragePath("/name "));
  EXPECT_FALSE(IsStoragePath("/dir./a"));
  EXPECT_FALSE(IsStoragePath("/dir /a"));
  EXPECT_FALSE(IsStoragePath(std::string_view("/a\0b", 4)));
  EXPECT_FALSE(IsStoragePath("/a\tb"));
  EXPECT_FALSE(IsStoragePath("/a\nb"));
  EXPECT_FALSE(IsStoragePath("/a\x7F"));
  EXPECT_TRUE(IsStoragePath("/a.b.c"));
  EXPECT_TRUE(IsStoragePath("/a b"));
}

TEST(StoragePath, HasALongestLength) {
  const std::string longest = "/" + std::string(Storage::kMaxPath - 1, 'a');
  ASSERT_EQ(longest.size(), Storage::kMaxPath);
  EXPECT_TRUE(IsStoragePath(longest));
  EXPECT_FALSE(IsStoragePath(longest + "a"));
}

TEST(StorageStatus, EveryStatusHasAName) {
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kOk), "ok");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kAbsent), "absent");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kNotFound), "not found");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kExists), "exists");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kBadPath), "bad path");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kTooManyOpen), "too many open");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kTooBig), "too big");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kNoSpace), "no space");
  EXPECT_STREQ(bn::core::StorageStatusName(StorageStatus::kFailed), "failed");
}

}  // namespace
