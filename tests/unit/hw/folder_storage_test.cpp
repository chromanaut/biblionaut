#include "desktop/folder_storage.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "core/block_file.h"
#include "core/file_replacement.h"

namespace {

using bn::core::FileHandle;
using bn::core::FileInfo;
using bn::core::kNoFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::hw::FolderStorage;
using Bytes = std::vector<std::uint8_t>;

Bytes BytesOf(const std::string& text) { return {text.begin(), text.end()}; }

// A folder of its own for each test, removed afterwards, with a started
// storage over it.
struct FolderFixture {
  FolderFixture() {
    std::string pattern = (std::filesystem::temp_directory_path() / "bn-storage-XXXXXX").string();
    const char* made = ::mkdtemp(pattern.data());
    EXPECT_NE(made, nullptr);
    folder = pattern;
    storage = std::make_unique<FolderStorage>(folder.string());
    EXPECT_EQ(storage->Start(), StorageStatus::kOk);
  }
  ~FolderFixture() {
    storage.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
  }
  FolderFixture(const FolderFixture&) = delete;
  FolderFixture& operator=(const FolderFixture&) = delete;

  // A file put straight into the folder, behind the storage's back.
  void Put(const std::string& name, const std::string& contents) const {
    std::ofstream(folder / name, std::ios::binary) << contents;
  }
  // What a file in the folder really holds.
  std::string OnDisk(const std::string& name) const {
    std::ifstream file(folder / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  FileHandle MustOpen(const std::string& path, OpenMode mode) const {
    FileHandle file = kNoFile;
    EXPECT_EQ(storage->Open(path, mode, file), StorageStatus::kOk) << path;
    return file;
  }

  std::filesystem::path folder;
  std::unique_ptr<FolderStorage> storage;
};

TEST(FolderStorage, IsAbsentUntilItsFolderIsThere) {
  const FolderFixture fixture;
  const std::filesystem::path later = fixture.folder / "card";
  FolderStorage storage(later.string());
  EXPECT_EQ(storage.Start(), StorageStatus::kAbsent);
  FileHandle file = kNoFile;
  FileInfo info;
  EXPECT_EQ(storage.Open("/a", OpenMode::kCreate, file), StorageStatus::kAbsent);
  EXPECT_EQ(file, kNoFile);
  EXPECT_EQ(storage.Stat("/", info), StorageStatus::kAbsent);
  EXPECT_EQ(storage.Remove("/a"), StorageStatus::kAbsent);
  EXPECT_EQ(storage.Rename("/a", "/b"), StorageStatus::kAbsent);
  EXPECT_EQ(storage.MakeDirectory("/d"), StorageStatus::kAbsent);

  // The card goes in.
  std::filesystem::create_directory(later);
  EXPECT_EQ(storage.Start(), StorageStatus::kOk);
  EXPECT_EQ(storage.Stat("/", info), StorageStatus::kOk);
  EXPECT_TRUE(info.isDirectory);

  // A file is not a card, and neither is nothing.
  fixture.Put("plain", "x");
  FolderStorage overAFile((fixture.folder / "plain").string());
  EXPECT_EQ(overAFile.Start(), StorageStatus::kAbsent);
  FolderStorage overNothing("");
  EXPECT_EQ(overNothing.Start(), StorageStatus::kAbsent);
}

TEST(FolderStorage, CreatesWritesReadsAndSizesAFile) {
  FolderFixture fixture;
  const FileHandle file = fixture.MustOpen("/a.bin", OpenMode::kCreate);
  std::uint32_t size = 99;
  ASSERT_EQ(fixture.storage->Size(file, size), StorageStatus::kOk);
  EXPECT_EQ(size, 0u);

  ASSERT_EQ(fixture.storage->WriteAt(file, 0, BytesOf("hello")), StorageStatus::kOk);
  ASSERT_EQ(fixture.storage->WriteAt(file, 5, BytesOf(" world")), StorageStatus::kOk);
  ASSERT_EQ(fixture.storage->WriteAt(file, 0, BytesOf("J")), StorageStatus::kOk);
  ASSERT_EQ(fixture.storage->Size(file, size), StorageStatus::kOk);
  EXPECT_EQ(size, 11u);
  EXPECT_EQ(fixture.storage->Sync(file), StorageStatus::kOk);
  EXPECT_EQ(fixture.OnDisk("a.bin"), "Jello world");

  // A file being written can be read back.
  Bytes got(5);
  std::size_t read = 0;
  ASSERT_EQ(fixture.storage->ReadAt(file, 6, got, read), StorageStatus::kOk);
  EXPECT_EQ(read, 5u);
  EXPECT_EQ(got, BytesOf("world"));
  fixture.storage->Close(file);

  // And again once it is closed: short at the end, nothing past it.
  const FileHandle again = fixture.MustOpen("/a.bin", OpenMode::kRead);
  Bytes more(20);
  ASSERT_EQ(fixture.storage->ReadAt(again, 6, more, read), StorageStatus::kOk);
  EXPECT_EQ(read, 5u);
  ASSERT_EQ(fixture.storage->ReadAt(again, 11, more, read), StorageStatus::kOk);
  EXPECT_EQ(read, 0u);
  ASSERT_EQ(fixture.storage->ReadAt(again, 5'000, more, read), StorageStatus::kOk);
  EXPECT_EQ(read, 0u);
  EXPECT_EQ(fixture.storage->WriteAt(again, 0, BytesOf("no")), StorageStatus::kFailed) << "opened for reading";
  EXPECT_EQ(fixture.OnDisk("a.bin"), "Jello world");
  fixture.storage->Close(again);

  // Creating it again empties it.
  const FileHandle emptied = fixture.MustOpen("/a.bin", OpenMode::kCreate);
  ASSERT_EQ(fixture.storage->Size(emptied, size), StorageStatus::kOk);
  EXPECT_EQ(size, 0u);
  fixture.storage->Close(emptied);
}

TEST(FolderStorage, WillNotLeaveGapsOrUseAFileThatIsNotOpen) {
  FolderFixture fixture;
  const FileHandle file = fixture.MustOpen("/a", OpenMode::kCreate);
  ASSERT_EQ(fixture.storage->WriteAt(file, 0, BytesOf("abc")), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage->WriteAt(file, 4, BytesOf("x")), StorageStatus::kFailed);
  EXPECT_EQ(fixture.storage->WriteAt(file, 3, BytesOf("d")), StorageStatus::kOk);
  fixture.storage->Close(file);
  EXPECT_EQ(fixture.OnDisk("a"), "abcd");

  Bytes some(4);
  std::size_t read = 7;
  std::uint32_t size = 0;
  for (const FileHandle bad : {file, kNoFile, FileHandle{100}}) {
    EXPECT_EQ(fixture.storage->ReadAt(bad, 0, some, read), StorageStatus::kFailed);
    EXPECT_EQ(read, 0u);
    EXPECT_EQ(fixture.storage->WriteAt(bad, 0, some), StorageStatus::kFailed);
    EXPECT_EQ(fixture.storage->Size(bad, size), StorageStatus::kFailed);
    EXPECT_EQ(fixture.storage->Sync(bad), StorageStatus::kFailed);
    fixture.storage->Close(bad);  // Harmless.
  }
}

TEST(FolderStorage, OpensOnlyFilesThatAreThere) {
  FolderFixture fixture;
  FileHandle file = 5;
  EXPECT_EQ(fixture.storage->Open("/nothing", OpenMode::kRead, file), StorageStatus::kNotFound);
  EXPECT_EQ(file, kNoFile);
  EXPECT_EQ(fixture.storage->Open("/no/such/dir/a", OpenMode::kRead, file), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->Open("/no-such-dir/a", OpenMode::kCreate, file), StorageStatus::kNotFound);

  ASSERT_EQ(fixture.storage->MakeDirectory("/dir"), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage->Open("/dir", OpenMode::kRead, file), StorageStatus::kNotFound) << "not a file";
  EXPECT_EQ(fixture.storage->Open("/", OpenMode::kRead, file), StorageStatus::kNotFound);

  fixture.Put("a", "x");
  EXPECT_EQ(fixture.storage->Open("/a/b", OpenMode::kRead, file), StorageStatus::kNotFound)
      << "a file is not a directory";
}

TEST(FolderStorage, OnlyAFewFilesCanBeOpenAtOnce) {
  FolderFixture fixture;
  fixture.Put("a", "x");
  std::array<FileHandle, FolderStorage::kMaxOpenFiles> files{};
  for (FileHandle& file : files) {
    file = fixture.MustOpen("/a", OpenMode::kRead);
  }
  FileHandle extra = kNoFile;
  EXPECT_EQ(fixture.storage->Open("/a", OpenMode::kRead, extra), StorageStatus::kTooManyOpen);
  fixture.storage->Close(files[1]);
  EXPECT_EQ(fixture.storage->Open("/a", OpenMode::kRead, extra), StorageStatus::kOk);
  EXPECT_EQ(extra, files[1]) << "the freed place is used again";

  // A file in use is not emptied under its reader.
  EXPECT_EQ(fixture.storage->Open("/a", OpenMode::kCreate, extra), StorageStatus::kTooManyOpen);
  fixture.storage->Close(files[2]);
  EXPECT_EQ(fixture.storage->Open("/a", OpenMode::kCreate, extra), StorageStatus::kFailed);
  EXPECT_EQ(fixture.OnDisk("a"), "x");
}

TEST(FolderStorage, SaysWhatIsAtAPath) {
  FolderFixture fixture;
  fixture.Put("book.epub", std::string(1234, 'x'));
  ASSERT_EQ(fixture.storage->MakeDirectory("/dir"), StorageStatus::kOk);
  FileInfo info;
  ASSERT_EQ(fixture.storage->Stat("/book.epub", info), StorageStatus::kOk);
  EXPECT_FALSE(info.isDirectory);
  EXPECT_EQ(info.size, 1234u);
  ASSERT_EQ(fixture.storage->Stat("/dir", info), StorageStatus::kOk);
  EXPECT_TRUE(info.isDirectory);
  EXPECT_EQ(info.size, 0u);
  EXPECT_EQ(fixture.storage->Stat("/nothing", info), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->Stat("/book.epub/inside", info), StorageStatus::kNotFound);
}

TEST(FolderStorage, RemovesFilesAndEmptyDirectories) {
  FolderFixture fixture;
  fixture.Put("a", "x");
  ASSERT_EQ(fixture.storage->MakeDirectory("/dir"), StorageStatus::kOk);
  ASSERT_EQ(fixture.storage->MakeDirectory("/dir/inner"), StorageStatus::kOk);

  EXPECT_EQ(fixture.storage->Remove("/nothing"), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->Remove("/dir"), StorageStatus::kFailed) << "it has something in it";
  EXPECT_EQ(fixture.storage->Remove("/"), StorageStatus::kFailed);
  EXPECT_EQ(fixture.storage->Remove("/dir/inner"), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage->Remove("/dir"), StorageStatus::kOk);
  EXPECT_FALSE(std::filesystem::exists(fixture.folder / "dir"));

  // Not a file that is open.
  const FileHandle file = fixture.MustOpen("/a", OpenMode::kRead);
  EXPECT_EQ(fixture.storage->Remove("/a"), StorageStatus::kFailed);
  fixture.storage->Close(file);
  EXPECT_EQ(fixture.storage->Remove("/a"), StorageStatus::kOk);
  EXPECT_FALSE(std::filesystem::exists(fixture.folder / "a"));
  EXPECT_EQ(fixture.storage->Remove("/a"), StorageStatus::kNotFound);
}

TEST(FolderStorage, RenamesButWillNotReplace) {
  FolderFixture fixture;
  fixture.Put("a", "first");
  fixture.Put("b", "second");
  ASSERT_EQ(fixture.storage->MakeDirectory("/dir"), StorageStatus::kOk);

  EXPECT_EQ(fixture.storage->Rename("/a", "/b"), StorageStatus::kExists);
  EXPECT_EQ(fixture.OnDisk("a"), "first");
  EXPECT_EQ(fixture.OnDisk("b"), "second");
  EXPECT_EQ(fixture.storage->Rename("/a", "/dir"), StorageStatus::kExists);
  EXPECT_EQ(fixture.storage->Rename("/nothing", "/c"), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->Rename("/a", "/no-such-dir/a"), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->Rename("/", "/c"), StorageStatus::kFailed);

  EXPECT_EQ(fixture.storage->Rename("/a", "/c"), StorageStatus::kOk);
  EXPECT_EQ(fixture.OnDisk("c"), "first");
  EXPECT_FALSE(std::filesystem::exists(fixture.folder / "a"));
  EXPECT_EQ(fixture.storage->Rename("/c", "/dir/c"), StorageStatus::kOk) << "into another directory";
  EXPECT_EQ(fixture.OnDisk("dir/c"), "first");
  EXPECT_EQ(fixture.storage->Rename("/dir", "/moved"), StorageStatus::kOk) << "a directory too";
  EXPECT_EQ(fixture.OnDisk("moved/c"), "first");

  // Not a file that is open.
  const FileHandle file = fixture.MustOpen("/b", OpenMode::kRead);
  EXPECT_EQ(fixture.storage->Rename("/b", "/d"), StorageStatus::kFailed);
  fixture.storage->Close(file);
  EXPECT_EQ(fixture.storage->Rename("/b", "/d"), StorageStatus::kOk);
}

TEST(FolderStorage, MakesDirectoriesOneLevelAtATime) {
  FolderFixture fixture;
  EXPECT_EQ(fixture.storage->MakeDirectory("/biblionaut/books"), StorageStatus::kNotFound);
  EXPECT_EQ(fixture.storage->MakeDirectory("/biblionaut"), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage->MakeDirectory("/biblionaut/books"), StorageStatus::kOk);
  EXPECT_EQ(fixture.storage->MakeDirectory("/biblionaut"), StorageStatus::kExists);
  EXPECT_EQ(fixture.storage->MakeDirectory("/"), StorageStatus::kExists);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.folder / "biblionaut" / "books"));
  fixture.Put("file", "x");
  EXPECT_EQ(fixture.storage->MakeDirectory("/file"), StorageStatus::kExists);
}

TEST(FolderStorage, RefusesPathsACardCouldNotHoldAndStaysInItsFolder) {
  FolderFixture fixture;
  // Something next to the folder that must never be reached.
  const std::filesystem::path outside =
      fixture.folder.parent_path() / (fixture.folder.filename().string() + "-outside");
  std::ofstream(outside) << "keep";

  FileHandle file = kNoFile;
  FileInfo info;
  const std::string escape = "/../" + outside.filename().string();
  for (const std::string& path : {escape, std::string("/a/../../x"), std::string("a"), std::string("/a/"),
                                  std::string("/a:b"), std::string("/a*"), std::string("//a"), std::string("")}) {
    EXPECT_EQ(fixture.storage->Open(path, OpenMode::kRead, file), StorageStatus::kBadPath) << path;
    EXPECT_EQ(fixture.storage->Open(path, OpenMode::kCreate, file), StorageStatus::kBadPath) << path;
    EXPECT_EQ(fixture.storage->Stat(path, info), StorageStatus::kBadPath) << path;
    EXPECT_EQ(fixture.storage->Remove(path), StorageStatus::kBadPath) << path;
    EXPECT_EQ(fixture.storage->MakeDirectory(path), StorageStatus::kBadPath) << path;
    EXPECT_EQ(fixture.storage->Rename(path, "/ok"), StorageStatus::kBadPath) << path;
  }
  fixture.Put("a", "x");
  EXPECT_EQ(fixture.storage->Rename("/a", escape), StorageStatus::kBadPath);

  std::ifstream kept(outside);
  const std::string contents{std::istreambuf_iterator<char>(kept), std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "keep");
  std::filesystem::remove(outside);
}

TEST(FolderStorage, CarriesABlockFileFromEndToEnd) {
  FolderFixture fixture;
  Bytes contents(10'000);
  for (std::size_t i = 0; i < contents.size(); ++i) {
    contents[i] = static_cast<std::uint8_t>(i * 7);
  }
  std::array<std::uint8_t, 4096> buffer{};
  bn::core::BlockFile file(buffer);
  ASSERT_EQ(file.Open(*fixture.storage, "/book.bundle", OpenMode::kCreate), StorageStatus::kOk);
  ASSERT_TRUE(file.Write(std::span(contents).first(3'000)));
  ASSERT_TRUE(file.Write(std::span(contents).subspan(3'000)));
  ASSERT_TRUE(file.Sync());
  file.Close();

  ASSERT_EQ(file.Open(*fixture.storage, "/book.bundle", OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(file.Size(), 10'000u);
  Bytes got(10'000);
  std::size_t at = 0;
  for (const std::size_t piece : {7u, 600u, 4096u, 1u, 5296u}) {
    ASSERT_EQ(file.Read(std::span(got).subspan(at, piece)), static_cast<std::ptrdiff_t>(piece));
    at += piece;
  }
  EXPECT_EQ(got, contents);
  Bytes tail(4);
  ASSERT_TRUE(file.ReadAt(9'996, tail));
  EXPECT_EQ(tail, Bytes(contents.end() - 4, contents.end()));
}

TEST(FolderStorage, CarriesAFileReplacementFromEndToEnd) {
  FolderFixture fixture;
  ASSERT_EQ(fixture.storage->MakeDirectory("/state"), StorageStatus::kOk);
  std::array<std::uint8_t, 512> buffer{};
  bn::core::FileReplacement replacement(*fixture.storage, buffer);

  for (const std::string& contents : {std::string("first settings"), std::string("second, longer settings")}) {
    ASSERT_EQ(replacement.Begin("/state/settings"), StorageStatus::kOk);
    ASSERT_TRUE(replacement.File().Write(BytesOf(contents)));
    ASSERT_EQ(replacement.Commit(), StorageStatus::kOk);
    EXPECT_EQ(fixture.OnDisk("state/settings"), contents);
  }
  // Nothing else is left in the directory.
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.folder / "state")) {
    names.push_back(entry.path().filename().string());
  }
  EXPECT_EQ(names, std::vector<std::string>{"settings"});

  // A replacement cut short after its contents were complete is finished
  // by recovery; one cut short before is thrown away.
  fixture.Put("state/settings.new", "third");
  fixture.Put("state/settings.tmp", "partial");
  ASSERT_EQ(bn::core::RecoverReplacement(*fixture.storage, "/state/settings"), StorageStatus::kOk);
  EXPECT_EQ(fixture.OnDisk("state/settings"), "third");
  EXPECT_FALSE(std::filesystem::exists(fixture.folder / "state" / "settings.new"));
  EXPECT_FALSE(std::filesystem::exists(fixture.folder / "state" / "settings.tmp"));
}

}  // namespace
