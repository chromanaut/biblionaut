#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "core/storage.h"

// The desktop's stand-in for the SD card.

namespace bn::hw {

// Storage kept in a folder of the computer's own file system: the folder
// is the card, and "/books/a" is the file books/a inside it.
//
// It is made to behave as a card does where that is cheap to do, so that
// what works here works on a device: the same paths are refused, only as
// many files can be open at once, a file that is open cannot be removed
// or renamed, and a rename will not replace a file. Where it cannot be
// the same, it is more forgiving: upper and lower case are different names
// here unless the computer's file system says otherwise.
class FolderStorage final : public core::Storage {
 public:
  // As many as a device allows.
  static constexpr std::size_t kMaxOpenFiles = 4;

  // `folder` must exist by the time Start() is called; until it does, this
  // is a device with no card in it.
  explicit FolderStorage(std::string folder);

  FolderStorage(const FolderStorage&) = delete;
  FolderStorage& operator=(const FolderStorage&) = delete;
  ~FolderStorage();

  [[nodiscard]] core::StorageStatus Start() noexcept override;
  [[nodiscard]] core::StorageStatus Open(std::string_view path, core::OpenMode mode,
                                         core::FileHandle& file) noexcept override;
  void Close(core::FileHandle file) noexcept override;
  [[nodiscard]] core::StorageStatus Size(core::FileHandle file, std::uint32_t& size) noexcept override;
  [[nodiscard]] core::StorageStatus ReadAt(core::FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                           std::size_t& read) noexcept override;
  [[nodiscard]] core::StorageStatus WriteAt(core::FileHandle file, std::uint32_t offset,
                                            std::span<const std::uint8_t> src) noexcept override;
  [[nodiscard]] core::StorageStatus Sync(core::FileHandle file) noexcept override;
  [[nodiscard]] core::StorageStatus Stat(std::string_view path, core::FileInfo& info) noexcept override;
  [[nodiscard]] core::StorageStatus Remove(std::string_view path) noexcept override;
  [[nodiscard]] core::StorageStatus Rename(std::string_view from, std::string_view to) noexcept override;
  [[nodiscard]] core::StorageStatus MakeDirectory(std::string_view path) noexcept override;

 private:
  struct OpenFile {
    int descriptor = -1;  // -1: this place is free.
    bool writable = false;
    std::string path;  // As it was opened, to refuse removing or renaming it.
  };

  // Checks that storage is started and `path` acceptable, and gives the
  // path in the computer's file system.
  core::StorageStatus Locate(std::string_view path, std::string& hostPath) const;
  // The open file `file` names, or null. The lock must be held.
  OpenFile* Find(core::FileHandle file) noexcept;
  bool IsOpen(std::string_view path) const;

  const std::string folder_;
  mutable std::mutex mutex_;  // Guards everything below.
  bool started_ = false;
  std::array<OpenFile, kMaxOpenFiles> open_;
};

}  // namespace bn::hw
