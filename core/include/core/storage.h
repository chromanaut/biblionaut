#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Files, as the rest of the firmware sees them: the SD card on a device, a
// folder on the desktop.
//
// This is the bare interface a hardware port implements, one call for each
// thing the card is asked to do. Code that reads or writes a file goes
// through BlockFile (core/block_file.h), which adds a buffer, a position
// and the counters; code that must not leave a file half written goes
// through FileReplacement (core/file_replacement.h).

namespace bn::core {

// How something asked of storage ended.
enum class StorageStatus : std::uint8_t {
  kOk = 0,
  kAbsent,       // There is no storage: no card, or Start() has not succeeded.
  kNotFound,     // No file or directory of that name, or it is the other kind.
  kExists,       // Something of that name is already there.
  kBadPath,      // Not a path storage accepts: see IsStoragePath().
  kTooManyOpen,  // No room for another open file.
  kTooBig,       // The file is, or would become, 4 GB or more.
  kNoSpace,      // The storage is full.
  kFailed,       // The card or the file system reported an error, or the request made no sense.
};

// The name of a status, for diagnostics: "ok", "absent", "not found", ...
const char* StorageStatusName(StorageStatus status) noexcept;

enum class OpenMode : std::uint8_t {
  kRead,    // An existing file, for reading only.
  kCreate,  // A new, empty file, for writing and reading back. A file already of that name is emptied.
};

// An open file. Only meaningful to the Storage that gave it out.
using FileHandle = std::int32_t;
inline constexpr FileHandle kNoFile = -1;

// A directory that is being listed. Only meaningful to the Storage that
// gave it out.
using DirectoryHandle = std::int32_t;
inline constexpr DirectoryHandle kNoDirectory = -1;

struct FileInfo {
  std::uint32_t size = 0;  // Bytes; 0 for a directory.
  bool isDirectory = false;
};

// Whether `path` is one storage accepts. The rule is the same on every
// port, and is what an SD card's file system can hold, so that a name
// which works on the desktop works on a device:
//
//   - it starts with '/', and parts are separated by single '/' ("/" alone
//     is the top directory);
//   - it is at most Storage::kMaxPath bytes;
//   - no part is empty, ends in '.' or ' ' (which rules out "." and ".."),
//     or contains a control character or any of \ : * ? " < > |
//
// Upper and lower case are different names here, but an SD card treats
// them as the same: do not rely on either.
bool IsStoragePath(std::string_view path) noexcept;

// Whether `name` could be one part of such a path: the name of a file or a
// directory, without the path to it.
bool IsStorageName(std::string_view name) noexcept;

// One place files are kept. Implemented by each hardware port.
//
// Calls may come from more than one task at once, except that any one open
// file is used by one task at a time.
class Storage {
 public:
  // The longest path accepted, in bytes.
  static constexpr std::size_t kMaxPath = 255;

  // Gets the storage ready: on a device, finds the card and its file
  // system. Call before anything else, and again to try once more after it
  // has returned kAbsent. May take some hundreds of milliseconds.
  [[nodiscard]] virtual StorageStatus Start() noexcept = 0;

  // How many files can be open at once, counting each directory that is
  // being listed as one.
  static constexpr std::size_t kMaxOpen = 4;
  // How many of those can be directories.
  static constexpr std::size_t kMaxListed = 2;

  // Opens a file. On success `file` is its handle, to be given back to
  // Close(). Only kMaxOpen files can be open at once (kTooManyOpen).
  [[nodiscard]] virtual StorageStatus Open(std::string_view path, OpenMode mode, FileHandle& file) noexcept = 0;

  // Closes a file. What was written to it is not safely on the card until
  // Sync() has said so: closing is not a substitute.
  virtual void Close(FileHandle file) noexcept = 0;

  [[nodiscard]] virtual StorageStatus Size(FileHandle file, std::uint32_t& size) noexcept = 0;

  // Reads up to dst.size() bytes starting `offset` bytes into the file.
  // `read` says how many were read: fewer than asked for only where the
  // file ends, and 0 at or past its end.
  [[nodiscard]] virtual StorageStatus ReadAt(FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                             std::size_t& read) noexcept = 0;

  // Writes all of `src` starting `offset` bytes into a file opened with
  // kCreate, over what is there and on past the end. `offset` may be at
  // most the file's size: there are no gaps.
  [[nodiscard]] virtual StorageStatus WriteAt(FileHandle file, std::uint32_t offset,
                                              std::span<const std::uint8_t> src) noexcept = 0;

  // Returns once everything written to the file is safely on the card.
  [[nodiscard]] virtual StorageStatus Sync(FileHandle file) noexcept = 0;

  // Says what is at `path`: kNotFound if nothing is.
  [[nodiscard]] virtual StorageStatus Stat(std::string_view path, FileInfo& info) noexcept = 0;

  // Removes a file, or a directory with nothing in it. Not a file that is
  // open (kFailed).
  [[nodiscard]] virtual StorageStatus Remove(std::string_view path) noexcept = 0;

  // Gives a file or directory another name, which may be in another
  // directory. Nothing may already have that name (kExists): an SD card
  // cannot replace one file by another in a single step, and
  // FileReplacement exists to do it safely in several. Not a file that is
  // open (kFailed).
  [[nodiscard]] virtual StorageStatus Rename(std::string_view from, std::string_view to) noexcept = 0;

  // Makes a directory. The one it is made in must exist (kNotFound).
  [[nodiscard]] virtual StorageStatus MakeDirectory(std::string_view path) noexcept = 0;

  // Starts listing the directory at `path`. On success `directory` is its
  // handle, to be given back to CloseDirectory(). kNotFound if there is no
  // such directory. kTooManyOpen if kMaxListed are being listed already,
  // or if with the open files that would make more than kMaxOpen.
  [[nodiscard]] virtual StorageStatus OpenDirectory(std::string_view path, DirectoryHandle& directory) noexcept = 0;

  // Gives the next thing in the directory: its name, without the path to
  // it, in the first `length` bytes of `name`, and what it is in `info`.
  // `length` is 0 once everything has been given. Things come in no order
  // that can be relied on.
  //
  // Left out: anything with a name that could not be part of a path here
  // (IsStorageName()) or that is longer than `name`, and anything that is
  // neither a file that could be opened nor a directory. Give `name`
  // kMaxPath bytes to be sure of missing nothing for want of room.
  //
  // What is made or removed in a directory while it is being listed may
  // or may not show.
  [[nodiscard]] virtual StorageStatus NextInDirectory(DirectoryHandle directory, std::span<char> name,
                                                      std::size_t& length, FileInfo& info) noexcept = 0;

  // Ends a listing.
  virtual void CloseDirectory(DirectoryHandle directory) noexcept = 0;

 protected:
  ~Storage() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
