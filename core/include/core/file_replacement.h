#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/storage.h"

namespace bn::core {

// Writes a file so that it is never found half written.
//
// Whatever happens on the way (the battery dies, the card is pulled out),
// whoever next reads the file finds either what was there before, whole, or
// the new contents, whole. Settings, indexes and anything else that is
// written over an earlier version go through this.
//
// An SD card cannot put one file in the place of another in a single step,
// so it is done in several, using two names beside the file's own:
//
//   1. the new contents are written to "<path>.tmp" and synced;
//   2. that is renamed "<path>.new", a name that means "complete";
//   3. the old file, if there is one, is removed;
//   4. "<path>.new" is renamed to the path.
//
// If this is cut short, what is left says how far it got, and
// RecoverReplacement() finishes or undoes it: a ".tmp" is thrown away, a
// ".new" takes the file's place. So a file written this way is read by
// calling RecoverReplacement() and then opening it.
//
//   FileReplacement replacement(storage, buffer);
//   if (replacement.Begin("/biblionaut/state/settings") == StorageStatus::kOk &&
//       replacement.File().Write(bytes) &&
//       replacement.Commit() == StorageStatus::kOk) { ... }
//
// Used by one task at a time, and only one replacement of a given file at
// a time.
class FileReplacement {
 public:
  // The longest path that can be replaced: the names used on the way are
  // four bytes longer.
  static constexpr std::size_t kMaxPath = Storage::kMaxPath - 4;

  // `buffer` is for the file being written: see BlockFile.
  FileReplacement(Storage& storage, std::span<std::uint8_t> buffer) noexcept : storage_(storage), file_(buffer) {}

  FileReplacement(const FileReplacement&) = delete;
  FileReplacement& operator=(const FileReplacement&) = delete;
  ~FileReplacement() { Abandon(); }

  // Starts on new contents for `path`, abandoning any it had already
  // started on. The directory the file is in must exist.
  [[nodiscard]] StorageStatus Begin(std::string_view path) noexcept;

  // Where the new contents are written, between Begin() and Commit().
  BlockFile& File() noexcept { return file_; }

  // Puts the new contents in the file's place. On kOk they are there and
  // safe. On anything else the file is either as it was or, if the failure
  // came late, waiting for RecoverReplacement() to finish the job; either
  // way it is not half written.
  [[nodiscard]] StorageStatus Commit() noexcept;

  // Gives up on the new contents, leaving the file as it was. Does nothing
  // if none were begun or they have been committed.
  void Abandon() noexcept;

 private:
  Storage& storage_;
  BlockFile file_;
  std::array<char, Storage::kMaxPath + 1> path_{};
  std::size_t pathLength_ = 0;
  bool begun_ = false;
};

// Finishes or undoes a replacement of `path` that was cut short, if one
// was. Call before reading a file that is written with FileReplacement.
// kOk means the file is now as whole as it can be made, which includes not
// being there at all.
[[nodiscard]] StorageStatus RecoverReplacement(Storage& storage, std::string_view path) noexcept;

}  // namespace bn::core
