#include "core/file_replacement.h"

#include <cstring>

namespace bn::core {

namespace {

constexpr std::string_view kBeingWritten = ".tmp";
constexpr std::string_view kComplete = ".new";

using PathBuffer = std::array<char, Storage::kMaxPath + 1>;

// `path` with `suffix` after it, in `buffer`. The path must be short
// enough: no longer than FileReplacement::kMaxPath.
std::string_view WithSuffix(std::string_view path, std::string_view suffix, PathBuffer& buffer) noexcept {
  std::memcpy(buffer.data(), path.data(), path.size());
  std::memcpy(buffer.data() + path.size(), suffix.data(), suffix.size());
  return {buffer.data(), path.size() + suffix.size()};
}

// Removes what is at `path`; nothing being there is as good.
StorageStatus RemoveIfThere(Storage& storage, std::string_view path) noexcept {
  const StorageStatus status = storage.Remove(path);
  return status == StorageStatus::kNotFound ? StorageStatus::kOk : status;
}

}  // namespace

StorageStatus FileReplacement::Begin(std::string_view path) noexcept {
  Abandon();
  if (path.size() > kMaxPath || !IsStoragePath(path) || path.size() == 1) {
    return StorageStatus::kBadPath;
  }
  std::memcpy(path_.data(), path.data(), path.size());
  pathLength_ = path.size();

  PathBuffer buffer{};
  const StorageStatus status = file_.Open(storage_, WithSuffix(path, kBeingWritten, buffer), OpenMode::kCreate);
  begun_ = status == StorageStatus::kOk;
  return status;
}

StorageStatus FileReplacement::Commit() noexcept {
  if (!begun_) {
    return StorageStatus::kFailed;
  }
  const std::string_view path(path_.data(), pathLength_);
  PathBuffer beingWritten{};
  PathBuffer complete{};
  const std::string_view tmp = WithSuffix(path, kBeingWritten, beingWritten);
  const std::string_view next = WithSuffix(path, kComplete, complete);

  // Until the new contents carry the name that says they are complete,
  // giving up leaves the old file untouched.
  if (!file_.Sync()) {
    const StorageStatus status = file_.LastStatus();
    Abandon();
    return status;
  }
  file_.Close();
  StorageStatus status = RemoveIfThere(storage_, next);  // Left by a replacement cut short and never recovered.
  if (status == StorageStatus::kOk) {
    status = storage_.Rename(tmp, next);
  }
  if (status != StorageStatus::kOk) {
    Abandon();
    return status;
  }

  // From here the new contents win: if either step fails, or never
  // happens, RecoverReplacement() does it.
  begun_ = false;
  status = RemoveIfThere(storage_, path);
  if (status == StorageStatus::kOk) {
    status = storage_.Rename(next, path);
  }
  return status;
}

void FileReplacement::Abandon() noexcept {
  if (!begun_) {
    return;
  }
  begun_ = false;
  file_.Close();
  PathBuffer buffer{};
  // If this fails there is nothing more to do about it here: recovery
  // throws the leftover away.
  (void)storage_.Remove(WithSuffix({path_.data(), pathLength_}, kBeingWritten, buffer));
}

StorageStatus RecoverReplacement(Storage& storage, std::string_view path) noexcept {
  if (path.size() > FileReplacement::kMaxPath || !IsStoragePath(path) || path.size() == 1) {
    return StorageStatus::kBadPath;
  }
  // One name at a time, to go easy on the stack.
  PathBuffer buffer{};

  // Contents that were still being written: never finished, so not to be
  // trusted.
  StorageStatus status = RemoveIfThere(storage, WithSuffix(path, kBeingWritten, buffer));
  if (status != StorageStatus::kOk) {
    return status;
  }

  // Contents that were complete: they are the file, whether or not the old
  // one had been removed yet.
  const std::string_view next = WithSuffix(path, kComplete, buffer);
  FileInfo info;
  status = storage.Stat(next, info);
  if (status == StorageStatus::kNotFound) {
    return StorageStatus::kOk;
  }
  if (status != StorageStatus::kOk) {
    return status;
  }
  status = RemoveIfThere(storage, path);
  if (status != StorageStatus::kOk) {
    return status;
  }
  return storage.Rename(next, path);
}

}  // namespace bn::core
