#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/storage.h"

// For the desktop tools that read one file the way the firmware reads a
// file from its card.

namespace bn::tools {

// One file of the computer's, standing in for a whole card: whatever path is
// asked for, this is the file that is opened. For reading only.
class OneFile final : public core::Storage {
 public:
  explicit OneFile(std::string path) : path_(std::move(path)) {}

  OneFile(const OneFile&) = delete;
  OneFile& operator=(const OneFile&) = delete;
  ~OneFile() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  [[nodiscard]] core::StorageStatus Start() noexcept override {
    descriptor_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (descriptor_ < 0) {
      return core::StorageStatus::kNotFound;
    }
    struct stat info = {};
    if (::fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode)) {
      return core::StorageStatus::kNotFound;
    }
    if (info.st_size > static_cast<off_t>(UINT32_MAX)) {
      return core::StorageStatus::kTooBig;
    }
    size_ = static_cast<std::uint32_t>(info.st_size);
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus Open(std::string_view /*path*/, core::OpenMode mode,
                                         core::FileHandle& file) noexcept override {
    if (descriptor_ < 0 || mode != core::OpenMode::kRead) {
      return core::StorageStatus::kFailed;
    }
    file = 0;
    return core::StorageStatus::kOk;
  }
  void Close(core::FileHandle /*file*/) noexcept override {}

  [[nodiscard]] core::StorageStatus Size(core::FileHandle /*file*/, std::uint32_t& size) noexcept override {
    size = size_;
    return core::StorageStatus::kOk;
  }

  [[nodiscard]] core::StorageStatus ReadAt(core::FileHandle /*file*/, std::uint32_t offset, std::span<std::uint8_t> dst,
                                           std::size_t& read) noexcept override {
    read = 0;
    while (read < dst.size()) {
      const ssize_t got = ::pread(descriptor_, dst.data() + read, dst.size() - read,
                                  static_cast<off_t>(offset) + static_cast<off_t>(read));
      if (got < 0) {
        return core::StorageStatus::kFailed;
      }
      if (got == 0) {
        break;
      }
      read += static_cast<std::size_t>(got);
    }
    return core::StorageStatus::kOk;
  }

  // Nothing here changes anything.
  [[nodiscard]] core::StorageStatus WriteAt(core::FileHandle /*file*/, std::uint32_t /*offset*/,
                                            std::span<const std::uint8_t> /*src*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus Sync(core::FileHandle /*file*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus Stat(std::string_view /*path*/, core::FileInfo& /*info*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus Remove(std::string_view /*path*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus Rename(std::string_view /*from*/, std::string_view /*to*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus MakeDirectory(std::string_view /*path*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  // There is nothing here to list.
  [[nodiscard]] core::StorageStatus OpenDirectory(std::string_view /*path*/,
                                                  core::DirectoryHandle& /*directory*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  [[nodiscard]] core::StorageStatus NextInDirectory(core::DirectoryHandle /*directory*/, std::span<char> /*name*/,
                                                    std::size_t& /*length*/,
                                                    core::FileInfo& /*info*/) noexcept override {
    return core::StorageStatus::kFailed;
  }
  void CloseDirectory(core::DirectoryHandle /*directory*/) noexcept override {}

 private:
  std::string path_;
  int descriptor_ = -1;
  std::uint32_t size_ = 0;
};

}  // namespace bn::tools
