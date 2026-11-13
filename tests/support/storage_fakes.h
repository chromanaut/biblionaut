#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/storage.h"

namespace bn::test {

// Storage kept in memory, which records what is asked of it, can be told
// to fail, and can lose its power.
//
// Losing power is what the storage layer's promises are about, so it is
// modelled the hard way: what was written to a file since it was last
// synced is gone, as on a card that had not got round to it. A file never
// synced is left empty. Names (creating, removing, renaming) take effect at
// once and survive.
class FakeStorage final : public core::Storage {
 public:
  using Bytes = std::vector<std::uint8_t>;

  struct Entry {
    bool isDirectory = false;
    Bytes data;     // What a read sees.
    Bytes durable;  // What would be left after a loss of power.
  };

  static Entry Directory() {
    Entry entry;
    entry.isDirectory = true;
    return entry;
  }

  struct ReadCall {
    std::uint32_t offset;
    std::size_t size;
  };

  FakeStorage() { entries["/"] = Directory(); }

  // ---- For tests to set things up and look at them ----

  // Puts a file there, safely written, making the directories above it.
  void Put(const std::string& path, const Bytes& contents) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      entries[path.substr(0, slash)] = Directory();
    }
    entries[path] = Entry{.data = contents, .durable = contents};
  }
  bool Has(const std::string& path) const { return entries.contains(path); }
  const Bytes& Contents(const std::string& path) const { return entries.at(path).data; }
  int OpenCount() const {
    return static_cast<int>(std::count_if(open_.begin(), open_.end(), [](const OpenFile& f) { return f.inUse; }));
  }
  int ListedCount() const {
    return static_cast<int>(std::count_if(listed_.begin(), listed_.end(), [](const Listing& l) { return l.inUse; }));
  }

  // The power goes: every open file is forgotten, unsynced writes are
  // lost, and nothing works until PowerOn().
  void PowerOff() {
    powered = false;
    for (OpenFile& file : open_) {
      file = OpenFile{};
    }
    for (Listing& listing : listed_) {
      listing = Listing{};
    }
    for (auto& [path, entry] : entries) {
      entry.data = entry.durable;
    }
  }
  void PowerOn() {
    powered = true;
    changesUntilPowerOff = -1;
  }

  // ---- The interface ----

  [[nodiscard]] core::StorageStatus Start() noexcept override {
    started = present && powered;
    return started ? core::StorageStatus::kOk : core::StorageStatus::kAbsent;
  }

  [[nodiscard]] core::StorageStatus Open(std::string_view path, core::OpenMode mode,
                                         core::FileHandle& file) noexcept override {
    file = core::kNoFile;
    const bool create = mode == core::OpenMode::kCreate;
    if (const core::StorageStatus status = Check(path, create); status != core::StorageStatus::kOk) {
      return status;
    }
    const std::string name(path);
    const auto found = entries.find(name);
    if (found != entries.end() && found->second.isDirectory) {
      return core::StorageStatus::kNotFound;
    }
    if (!create && found == entries.end()) {
      return core::StorageStatus::kNotFound;
    }
    if (create && !HasParent(name)) {
      return core::StorageStatus::kNotFound;
    }
    if (create && IsOpen(name)) {
      return core::StorageStatus::kFailed;  // Emptying a file that is in use.
    }
    const auto place = std::find_if(open_.begin(), open_.end(), [](const OpenFile& f) { return !f.inUse; });
    if (place == open_.end() || OpenCount() + ListedCount() >= static_cast<int>(kMaxOpen)) {
      return core::StorageStatus::kTooManyOpen;
    }
    if (create) {
      // Emptying a file is itself only safe once synced: until then a loss
      // of power leaves what was there.
      entries[name].data.clear();
    }
    *place = OpenFile{.inUse = true, .writable = create, .path = name};
    file = static_cast<core::FileHandle>(place - open_.begin());
    ++opens;
    return core::StorageStatus::kOk;
  }

  void Close(core::FileHandle file) noexcept override {
    if (OpenFile* found = Find(file); found != nullptr) {
      *found = OpenFile{};
      ++closes;
    }
  }

  [[nodiscard]] core::StorageStatus Size(core::FileHandle file, std::uint32_t& size) noexcept override {
    const OpenFile* found = Find(file);
    if (found == nullptr || !powered || failSize) {
      return core::StorageStatus::kFailed;
    }
    size = static_cast<std::uint32_t>(entries.at(found->path).data.size());
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus ReadAt(core::FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                           std::size_t& read) noexcept override {
    read = 0;
    const OpenFile* found = Find(file);
    if (found == nullptr || !powered) {
      return core::StorageStatus::kFailed;
    }
    reads.push_back({.offset = offset, .size = dst.size()});
    if (failReads) {
      return core::StorageStatus::kFailed;
    }
    const Bytes& data = entries.at(found->path).data;
    if (offset < data.size()) {
      read = std::min(dst.size(), data.size() - offset);
      std::memcpy(dst.data(), data.data() + offset, read);
    }
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus WriteAt(core::FileHandle file, std::uint32_t offset,
                                            std::span<const std::uint8_t> src) noexcept override {
    const OpenFile* found = Find(file);
    if (found == nullptr || !found->writable || !Change()) {
      return core::StorageStatus::kFailed;
    }
    ++writes;
    if (failWrites != core::StorageStatus::kOk) {
      return failWrites;
    }
    Bytes& data = entries.at(found->path).data;
    if (offset > data.size()) {
      return core::StorageStatus::kFailed;  // No gaps.
    }
    if (data.size() < offset + src.size()) {
      data.resize(offset + src.size());
    }
    std::memcpy(data.data() + offset, src.data(), src.size());
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus Sync(core::FileHandle file) noexcept override {
    const OpenFile* found = Find(file);
    if (found == nullptr || !Change()) {
      return core::StorageStatus::kFailed;
    }
    ++syncs;
    if (failSyncs) {
      return core::StorageStatus::kFailed;
    }
    Entry& entry = entries.at(found->path);
    entry.durable = entry.data;
    log.push_back("sync " + found->path);
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus Stat(std::string_view path, core::FileInfo& info) noexcept override {
    if (const core::StorageStatus status = Check(path, false); status != core::StorageStatus::kOk) {
      return status;
    }
    const auto found = entries.find(std::string(path));
    if (found == entries.end()) {
      return core::StorageStatus::kNotFound;
    }
    info.isDirectory = found->second.isDirectory;
    info.size = static_cast<std::uint32_t>(found->second.data.size());
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus Remove(std::string_view path) noexcept override {
    if (const core::StorageStatus status = Check(path, true); status != core::StorageStatus::kOk) {
      return status;
    }
    const std::string name(path);
    const auto found = entries.find(name);
    if (found == entries.end()) {
      return core::StorageStatus::kNotFound;
    }
    if (failRemoves || name == "/" || IsOpen(name) || (found->second.isDirectory && HasChildren(name))) {
      return core::StorageStatus::kFailed;
    }
    entries.erase(found);
    log.push_back("remove " + name);
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus Rename(std::string_view from, std::string_view to) noexcept override {
    if (const core::StorageStatus status = Check(from, true); status != core::StorageStatus::kOk) {
      return status;
    }
    if (!core::IsStoragePath(to)) {
      return core::StorageStatus::kBadPath;
    }
    const std::string oldName(from);
    const std::string newName(to);
    const auto found = entries.find(oldName);
    if (found == entries.end() || !HasParent(newName)) {
      return core::StorageStatus::kNotFound;
    }
    if (entries.contains(newName)) {
      return core::StorageStatus::kExists;
    }
    if (failRenames || IsOpen(oldName) || found->second.isDirectory) {
      return core::StorageStatus::kFailed;  // (Renaming directories is not modelled.)
    }
    entries[newName] = std::move(found->second);
    entries.erase(oldName);
    log.push_back("rename " + oldName + " " + newName);
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus MakeDirectory(std::string_view path) noexcept override {
    if (const core::StorageStatus status = Check(path, true); status != core::StorageStatus::kOk) {
      return status;
    }
    const std::string name(path);
    if (entries.contains(name)) {
      return core::StorageStatus::kExists;
    }
    if (!HasParent(name)) {
      return core::StorageStatus::kNotFound;
    }
    entries[name] = Directory();
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus OpenDirectory(std::string_view path,
                                                  core::DirectoryHandle& directory) noexcept override {
    directory = core::kNoDirectory;
    if (const core::StorageStatus status = Check(path, false); status != core::StorageStatus::kOk) {
      return status;
    }
    const std::string name(path);
    const auto found = entries.find(name);
    if (found == entries.end() || !found->second.isDirectory) {
      return core::StorageStatus::kNotFound;
    }
    const auto place = std::find_if(listed_.begin(), listed_.end(), [](const Listing& l) { return !l.inUse; });
    if (place == listed_.end() || OpenCount() + ListedCount() >= static_cast<int>(kMaxOpen)) {
      return core::StorageStatus::kTooManyOpen;
    }
    if (failListing) {
      return core::StorageStatus::kFailed;
    }
    // What is in it now, in the order of the names.
    *place = Listing{.inUse = true, .path = name, .names = {}, .next = 0};
    const std::string prefix = name == "/" ? name : name + "/";
    for (const auto& [itemPath, entry] : entries) {
      if (itemPath.size() > prefix.size() && itemPath.starts_with(prefix) &&
          itemPath.find('/', prefix.size()) == std::string::npos) {
        place->names.push_back(itemPath.substr(prefix.size()));
      }
    }
    directory = static_cast<core::DirectoryHandle>(place - listed_.begin());
    ++directoryOpens;
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus NextInDirectory(core::DirectoryHandle directory, std::span<char> name,
                                                    std::size_t& length, core::FileInfo& info) noexcept override {
    length = 0;
    if (directory < 0 || static_cast<std::size_t>(directory) >= listed_.size() ||
        !listed_[static_cast<std::size_t>(directory)].inUse || !powered) {
      return core::StorageStatus::kFailed;
    }
    Listing& listing = listed_[static_cast<std::size_t>(directory)];
    if (failListingAfter == 0) {
      return core::StorageStatus::kFailed;
    }
    if (failListingAfter > 0) {
      --failListingAfter;
    }
    while (listing.next < listing.names.size()) {
      const std::string& itemName = listing.names[listing.next++];
      const auto found = entries.find((listing.path == "/" ? "" : listing.path) + "/" + itemName);
      if (found == entries.end() || itemName.size() > name.size()) {
        continue;  // Removed since the listing began, or too long for the caller.
      }
      std::memcpy(name.data(), itemName.data(), itemName.size());
      length = itemName.size();
      info.isDirectory = found->second.isDirectory;
      info.size = static_cast<std::uint32_t>(found->second.data.size());
      return core::StorageStatus::kOk;
    }
    return core::StorageStatus::kOk;
  }

  void CloseDirectory(core::DirectoryHandle directory) noexcept override {
    if (directory >= 0 && static_cast<std::size_t>(directory) < listed_.size() &&
        listed_[static_cast<std::size_t>(directory)].inUse) {
      listed_[static_cast<std::size_t>(directory)] = Listing{};
      ++directoryCloses;
    }
  }

  // ---- What tests set and read ----

  bool present = true;  // There is a card.
  bool started = false;
  bool powered = true;
  // After this many more changes (a write, a sync, a file created, removed
  // or renamed, a directory made), the power goes, before the next one
  // takes effect. -1: never.
  int changesUntilPowerOff = -1;
  int changes = 0;  // Changes made so far.

  bool failReads = false;
  core::StorageStatus failWrites = core::StorageStatus::kOk;  // What writes report, if not kOk.
  bool failSyncs = false;
  bool failSize = false;
  bool failRemoves = false;
  bool failRenames = false;
  bool failListing = false;   // No directory can be listed.
  int failListingAfter = -1;  // A listing gives this many more things, then fails. -1: never.

  int opens = 0;
  int closes = 0;
  int directoryOpens = 0;
  int directoryCloses = 0;
  int writes = 0;
  int syncs = 0;
  std::vector<ReadCall> reads;
  std::vector<std::string> log;  // Syncs, removes and renames that were done, in order.
  std::map<std::string, Entry> entries;

 private:
  struct OpenFile {
    bool inUse = false;
    bool writable = false;
    std::string path;
  };

  // A directory being listed: the names that were in it when the listing
  // began, and how far through them it has got.
  struct Listing {
    bool inUse = false;
    std::string path;
    std::vector<std::string> names;
    std::size_t next = 0;
  };

  // Whether a call can go ahead at all; `changing` if it changes what is
  // stored, which is when the power may go.
  core::StorageStatus Check(std::string_view path, bool changing) {
    if (!started || !powered) {
      return powered ? core::StorageStatus::kAbsent : core::StorageStatus::kFailed;
    }
    if (!core::IsStoragePath(path)) {
      return core::StorageStatus::kBadPath;
    }
    if (changing && !Change()) {
      return core::StorageStatus::kFailed;
    }
    return core::StorageStatus::kOk;
  }

  // Counts a change; false if the power went instead.
  bool Change() {
    if (!powered) {
      return false;
    }
    if (changesUntilPowerOff == 0) {
      PowerOff();
      return false;
    }
    if (changesUntilPowerOff > 0) {
      --changesUntilPowerOff;
    }
    ++changes;
    return true;
  }

  OpenFile* Find(core::FileHandle file) {
    if (file < 0 || static_cast<std::size_t>(file) >= open_.size() || !open_[static_cast<std::size_t>(file)].inUse) {
      return nullptr;
    }
    return &open_[static_cast<std::size_t>(file)];
  }
  bool IsOpen(const std::string& path) const {
    return std::any_of(open_.begin(), open_.end(), [&](const OpenFile& f) { return f.inUse && f.path == path; });
  }
  bool HasParent(const std::string& path) const {
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    const auto found = entries.find(parent);
    return found != entries.end() && found->second.isDirectory;
  }
  bool HasChildren(const std::string& path) const {
    const std::string prefix = path + "/";
    return std::any_of(entries.begin(), entries.end(),
                       [&](const auto& item) { return item.first.starts_with(prefix); });
  }

  std::vector<OpenFile> open_ = std::vector<OpenFile>(kMaxOpen);  // As many as a device allows.
  std::vector<Listing> listed_ = std::vector<Listing>(kMaxListed);
};

}  // namespace bn::test
