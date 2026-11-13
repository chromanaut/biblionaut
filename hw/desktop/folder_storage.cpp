#include "desktop/folder_storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "core/trace.h"
#include "hw/trace_ids.h"

namespace bn::hw {

namespace {

using core::StorageStatus;

// What the error the computer reported comes to.
StorageStatus FromErrno(int error) noexcept {
  switch (error) {
    case ENOENT:
    case ENOTDIR:
      return StorageStatus::kNotFound;
    case EEXIST:
      return StorageStatus::kExists;
    case ENOSPC:
    case EDQUOT:
      return StorageStatus::kNoSpace;
    case EFBIG:
      return StorageStatus::kTooBig;
    default:
      return StorageStatus::kFailed;
  }
}

}  // namespace

FolderStorage::FolderStorage(std::string folder) : folder_(std::move(folder)) {}

FolderStorage::~FolderStorage() {
  for (const OpenFile& file : open_) {
    if (file.descriptor >= 0) {
      ::close(file.descriptor);
    }
  }
  for (DIR* stream : listed_) {
    if (stream != nullptr) {
      ::closedir(stream);
    }
  }
}

StorageStatus FolderStorage::Start() noexcept {
  const std::lock_guard lock(mutex_);
  struct stat found = {};
  started_ = !folder_.empty() && ::stat(folder_.c_str(), &found) == 0 && S_ISDIR(found.st_mode);
  return started_ ? StorageStatus::kOk : StorageStatus::kAbsent;
}

StorageStatus FolderStorage::Locate(std::string_view path, std::string& hostPath) const {
  if (!started_) {
    return StorageStatus::kAbsent;
  }
  // The rule also keeps a path inside the folder: no part can be "..".
  if (!core::IsStoragePath(path)) {
    return StorageStatus::kBadPath;
  }
  hostPath = folder_;
  if (path.size() > 1) {
    hostPath.append(path);
  }
  return StorageStatus::kOk;
}

FolderStorage::OpenFile* FolderStorage::Find(core::FileHandle file) noexcept {
  if (file < 0 || static_cast<std::size_t>(file) >= open_.size()) {
    return nullptr;
  }
  OpenFile& found = open_[static_cast<std::size_t>(file)];
  return found.descriptor >= 0 ? &found : nullptr;
}

bool FolderStorage::IsOpen(std::string_view path) const {
  for (const OpenFile& file : open_) {
    if (file.descriptor >= 0 && file.path == path) {
      return true;
    }
  }
  return false;
}

std::size_t FolderStorage::OpenCount() const noexcept {
  const auto files = std::count_if(open_.begin(), open_.end(), [](const OpenFile& f) { return f.descriptor >= 0; });
  const auto directories = std::count_if(listed_.begin(), listed_.end(), [](const DIR* d) { return d != nullptr; });
  return static_cast<std::size_t>(files + directories);
}

StorageStatus FolderStorage::Open(std::string_view path, core::OpenMode mode, core::FileHandle& file) noexcept {
  file = core::kNoFile;
  const std::lock_guard lock(mutex_);
  std::string hostPath;
  if (const StorageStatus status = Locate(path, hostPath); status != StorageStatus::kOk) {
    return status;
  }
  std::size_t place = 0;
  while (place < open_.size() && open_[place].descriptor >= 0) {
    ++place;
  }
  if (place == open_.size() || OpenCount() >= kMaxOpen) {
    return StorageStatus::kTooManyOpen;
  }
  if (mode == core::OpenMode::kCreate && IsOpen(path)) {
    return StorageStatus::kFailed;  // Emptying a file that is in use.
  }

  const bool create = mode == core::OpenMode::kCreate;
  // The flags are the system's own, and signed.
  // NOLINTBEGIN(bugprone-signed-bitwise,hicpp-signed-bitwise,cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int descriptor = ::open(hostPath.c_str(), (create ? O_RDWR | O_CREAT | O_TRUNC : O_RDONLY) | O_CLOEXEC, 0644);
  // NOLINTEND(bugprone-signed-bitwise,hicpp-signed-bitwise,cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  if (descriptor < 0) {
    return FromErrno(errno);
  }
  struct stat found = {};
  if (::fstat(descriptor, &found) != 0 || !S_ISREG(found.st_mode)) {
    ::close(descriptor);
    return StorageStatus::kNotFound;  // A directory is not a file to open.
  }
  if (found.st_size > static_cast<off_t>(std::numeric_limits<std::uint32_t>::max())) {
    ::close(descriptor);
    return StorageStatus::kTooBig;
  }
  open_[place] = OpenFile{.descriptor = descriptor, .writable = create, .path = std::string(path)};
  file = static_cast<core::FileHandle>(place);
  return StorageStatus::kOk;
}

void FolderStorage::Close(core::FileHandle file) noexcept {
  const std::lock_guard lock(mutex_);
  if (OpenFile* found = Find(file); found != nullptr) {
    ::close(found->descriptor);
    *found = OpenFile{};
  }
}

StorageStatus FolderStorage::Size(core::FileHandle file, std::uint32_t& size) noexcept {
  const std::lock_guard lock(mutex_);
  const OpenFile* found = Find(file);
  struct stat info = {};
  if (found == nullptr || ::fstat(found->descriptor, &info) != 0) {
    return StorageStatus::kFailed;
  }
  size = static_cast<std::uint32_t>(info.st_size);
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::ReadAt(core::FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                    std::size_t& read) noexcept {
  read = 0;
  int descriptor = -1;
  {
    const std::lock_guard lock(mutex_);
    const OpenFile* found = Find(file);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    descriptor = found->descriptor;
  }
  // The file is its one user's from here: the lock is not held while
  // reading.
  BN_TRACE_SCOPE_ARG(kSpanSdRead, static_cast<std::uint32_t>(dst.size()));
  while (read < dst.size()) {
    const ssize_t count = ::pread(descriptor, dst.data() + read, dst.size() - read,
                                  static_cast<off_t>(offset) + static_cast<off_t>(read));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return StorageStatus::kFailed;
    }
    if (count == 0) {
      break;  // The end of the file.
    }
    read += static_cast<std::size_t>(count);
  }
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::WriteAt(core::FileHandle file, std::uint32_t offset,
                                     std::span<const std::uint8_t> src) noexcept {
  int descriptor = -1;
  {
    const std::lock_guard lock(mutex_);
    const OpenFile* found = Find(file);
    if (found == nullptr || !found->writable) {
      return StorageStatus::kFailed;
    }
    descriptor = found->descriptor;
  }
  struct stat info = {};
  if (::fstat(descriptor, &info) != 0 || static_cast<off_t>(offset) > info.st_size) {
    return StorageStatus::kFailed;  // No gaps.
  }
  if (src.size() > std::numeric_limits<std::uint32_t>::max() - offset) {
    return StorageStatus::kTooBig;
  }
  BN_TRACE_SCOPE_ARG(kSpanSdWrite, static_cast<std::uint32_t>(src.size()));
  std::size_t written = 0;
  while (written < src.size()) {
    const ssize_t count = ::pwrite(descriptor, src.data() + written, src.size() - written,
                                   static_cast<off_t>(offset) + static_cast<off_t>(written));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return FromErrno(errno);
    }
    written += static_cast<std::size_t>(count);
  }
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::Sync(core::FileHandle file) noexcept {
  int descriptor = -1;
  {
    const std::lock_guard lock(mutex_);
    const OpenFile* found = Find(file);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    descriptor = found->descriptor;
  }
  BN_TRACE_SCOPE(kSpanSdSync);
  return ::fsync(descriptor) == 0 ? StorageStatus::kOk : FromErrno(errno);
}

StorageStatus FolderStorage::Stat(std::string_view path, core::FileInfo& info) noexcept {
  const std::lock_guard lock(mutex_);
  std::string hostPath;
  if (const StorageStatus status = Locate(path, hostPath); status != StorageStatus::kOk) {
    return status;
  }
  struct stat found = {};
  if (::stat(hostPath.c_str(), &found) != 0) {
    return FromErrno(errno);
  }
  info.isDirectory = S_ISDIR(found.st_mode);
  if (!info.isDirectory && found.st_size > static_cast<off_t>(std::numeric_limits<std::uint32_t>::max())) {
    return StorageStatus::kTooBig;
  }
  info.size = info.isDirectory ? 0 : static_cast<std::uint32_t>(found.st_size);
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::Remove(std::string_view path) noexcept {
  const std::lock_guard lock(mutex_);
  std::string hostPath;
  if (const StorageStatus status = Locate(path, hostPath); status != StorageStatus::kOk) {
    return status;
  }
  if (path.size() == 1 || IsOpen(path)) {
    return StorageStatus::kFailed;
  }
  struct stat found = {};
  if (::stat(hostPath.c_str(), &found) != 0) {
    return FromErrno(errno);
  }
  const int result = S_ISDIR(found.st_mode) ? ::rmdir(hostPath.c_str()) : ::unlink(hostPath.c_str());
  if (result != 0) {
    // A directory that still has something in it.
    return errno == ENOTEMPTY || errno == EEXIST ? StorageStatus::kFailed : FromErrno(errno);
  }
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::Rename(std::string_view from, std::string_view to) noexcept {
  const std::lock_guard lock(mutex_);
  std::string hostFrom;
  std::string hostTo;
  if (const StorageStatus status = Locate(from, hostFrom); status != StorageStatus::kOk) {
    return status;
  }
  if (const StorageStatus status = Locate(to, hostTo); status != StorageStatus::kOk) {
    return status;
  }
  if (from.size() == 1 || to.size() == 1 || IsOpen(from)) {
    return StorageStatus::kFailed;
  }
  struct stat found = {};
  if (::stat(hostFrom.c_str(), &found) != 0) {
    return FromErrno(errno);
  }
  // The computer would replace what is at `to`; a card would not.
  if (::stat(hostTo.c_str(), &found) == 0) {
    return StorageStatus::kExists;
  }
  return std::rename(hostFrom.c_str(), hostTo.c_str()) == 0 ? StorageStatus::kOk : FromErrno(errno);
}

StorageStatus FolderStorage::MakeDirectory(std::string_view path) noexcept {
  const std::lock_guard lock(mutex_);
  std::string hostPath;
  if (const StorageStatus status = Locate(path, hostPath); status != StorageStatus::kOk) {
    return status;
  }
  if (path.size() == 1) {
    return StorageStatus::kExists;
  }
  return ::mkdir(hostPath.c_str(), 0755) == 0 ? StorageStatus::kOk : FromErrno(errno);
}

StorageStatus FolderStorage::OpenDirectory(std::string_view path, core::DirectoryHandle& directory) noexcept {
  directory = core::kNoDirectory;
  const std::lock_guard lock(mutex_);
  std::string hostPath;
  if (const StorageStatus status = Locate(path, hostPath); status != StorageStatus::kOk) {
    return status;
  }
  std::size_t place = 0;
  while (place < listed_.size() && listed_[place] != nullptr) {
    ++place;
  }
  if (place == listed_.size() || OpenCount() >= kMaxOpen) {
    return StorageStatus::kTooManyOpen;
  }
  DIR* stream = ::opendir(hostPath.c_str());
  if (stream == nullptr) {
    return FromErrno(errno);  // Among them: it is a file.
  }
  listed_[place] = stream;
  directory = static_cast<core::DirectoryHandle>(place);
  return StorageStatus::kOk;
}

StorageStatus FolderStorage::NextInDirectory(core::DirectoryHandle directory, std::span<char> name, std::size_t& length,
                                             core::FileInfo& info) noexcept {
  length = 0;
  DIR* stream = nullptr;
  {
    const std::lock_guard lock(mutex_);
    if (directory >= 0 && static_cast<std::size_t>(directory) < listed_.size()) {
      stream = listed_[static_cast<std::size_t>(directory)];
    }
  }
  if (stream == nullptr) {
    return StorageStatus::kFailed;
  }
  // The listing is its one user's from here, like an open file.
  const int descriptor = ::dirfd(stream);
  if (descriptor < 0) {
    return StorageStatus::kFailed;
  }
  for (;;) {
    errno = 0;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): safe where each stream has one user, which this one has.
    const struct dirent* item = ::readdir(stream);
    if (item == nullptr) {
      return errno == 0 ? StorageStatus::kOk : StorageStatus::kFailed;
    }
    // This drops "." and "..", with everything else a card could not hold.
    const std::string_view itemName(static_cast<const char*>(item->d_name));
    if (itemName.size() > name.size() || !core::IsStorageName(itemName)) {
      continue;
    }
    struct stat found = {};
    if (::fstatat(descriptor, static_cast<const char*>(item->d_name), &found, 0) != 0) {
      continue;  // Gone since, or a link to nothing.
    }
    if (S_ISDIR(found.st_mode)) {
      info = {.size = 0, .isDirectory = true};
    } else if (S_ISREG(found.st_mode) &&
               found.st_size <= static_cast<off_t>(std::numeric_limits<std::uint32_t>::max())) {
      info = {.size = static_cast<std::uint32_t>(found.st_size), .isDirectory = false};
    } else {
      continue;  // Nothing Open() would open.
    }
    std::memcpy(name.data(), itemName.data(), itemName.size());
    length = itemName.size();
    return StorageStatus::kOk;
  }
}

void FolderStorage::CloseDirectory(core::DirectoryHandle directory) noexcept {
  const std::lock_guard lock(mutex_);
  if (directory >= 0 && static_cast<std::size_t>(directory) < listed_.size() &&
      listed_[static_cast<std::size_t>(directory)] != nullptr) {
    ::closedir(listed_[static_cast<std::size_t>(directory)]);
    listed_[static_cast<std::size_t>(directory)] = nullptr;
  }
}

}  // namespace bn::hw
