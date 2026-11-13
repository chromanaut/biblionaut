#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "core/inflate.h"

// Reading a ZIP file, which is what an EPUB is: finding the entries in it,
// and reading what each one holds.
//
// A ZIP file ends with a directory: one record for each entry, giving its
// name, its sizes, its checksum and where in the file it is. Everything
// here goes by that directory, as ZIP readers do, and takes nothing from
// the copy of those facts that sits in front of each entry's data.
//
// No list of the entries is kept in memory, however many there are: an
// entry is found by reading through the directory in the file. See Find()
// for what that costs and how to keep it small.
//
// The file is not trusted. Whatever bytes it holds, every call here ends
// with an answer, and nothing is read from outside the file or written
// outside the caller's memory.
//
// What is not read, and is reported as kUnsupported: ZIP64 (a file with
// 65,535 entries or more, or with anything of 4 GB or more), a file in
// several parts, an entry that is encrypted, and an entry compressed any
// way other than DEFLATE. The rules for EPUB forbid all of these but
// ZIP64, which they allow a book that needs it.

namespace bn::epub {

// How something asked of a ZIP file ended.
enum class ZipStatus : std::uint8_t {
  kOk = 0,
  kEnd,            // Next(): there are no more entries.
  kNotFound,       // No entry has that name.
  kNotOpen,        // Nothing has been opened to ask it of.
  kNotZip,         // The file is not a ZIP file: it has no end record.
  kDamaged,        // It is one, but it contradicts itself, or an entry's data does not inflate.
  kBadChecksum,    // An entry's bytes do not have the checksum the directory gives for them.
  kUnsupported,    // A ZIP file, or an entry, of a kind this does not read (listed above).
  kStorageFailed,  // The file could not be read.
  kNoWindow,       // A compressed entry, and no window to inflate it with.
};

// The name of a status, for diagnostics: "ok", "not found", "damaged", ...
const char* ZipStatusName(ZipStatus status) noexcept;

// The ways an entry's data can be kept that are read here.
inline constexpr std::uint16_t kZipStored = 0;    // As it is.
inline constexpr std::uint16_t kZipDeflated = 8;  // Compressed with DEFLATE.

// What the directory says of one entry. It is small and has no pointers in
// it, so it can be kept, in memory or in a file, and used later to read
// the entry without finding it again; it is good for as long as the ZIP
// file is not changed.
struct ZipEntry {
  std::uint32_t headerOffset = 0;    // Where in the file the entry starts.
  std::uint32_t compressedSize = 0;  // The bytes it takes up there.
  std::uint32_t size = 0;            // The bytes it holds once read.
  std::uint32_t crc32 = 0;           // The checksum of those bytes.
  std::uint16_t method = 0;          // kZipStored, kZipDeflated, or something that cannot be read.
  std::uint16_t flags = 0;           // The entry's "general purpose" bits, as they are in the file.
  std::uint16_t nameLength = 0;      // The bytes in its name.
};

// A ZIP file's directory.
//
// It reads through a BlockFile the caller has opened, and keeps only where
// the directory is. Used by one task at a time, and that task must not be
// part of the way through another call on the same BlockFile.
//
// Between calls the BlockFile may be used for anything, reading an entry
// included, but then each step from an entry back to the directory reads a
// block of the file again. Where finding and reading take turns, open the
// file twice and give the entries a BlockFile of their own.
class ZipArchive {
 public:
  // Where Next() has got to. A new one is at the first entry.
  struct Cursor {
    std::uint32_t offset = 0;
    std::uint32_t index = 0;
  };

  ZipArchive() noexcept = default;

  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;
  ~ZipArchive() = default;

  // Finds the directory of the ZIP file open as `file`, which must stay
  // open, and alive, for as long as this is used. Costs one or two reads
  // of storage, unless the file ends with a comment, when it may cost up
  // to nineteen.
  [[nodiscard]] ZipStatus Open(core::BlockFile& file) noexcept;
  void Close() noexcept;
  bool IsOpen() const noexcept { return file_ != nullptr; }

  // How many entries the file says it has. Directories count, where the
  // file lists them: they are entries with nothing in them whose names end
  // in '/'.
  std::uint32_t EntryCount() const noexcept { return entryCount_; }

  // Finds the entry whose name is exactly `name`, byte for byte: upper and
  // lower case are different, and so are two ways of writing the same
  // accented letter.
  //
  // The search reads the directory from the entry after the last one
  // found, round to where it started. So asking for entries in the order
  // the file has them costs almost nothing each time, which is what a
  // reader working through a book mostly does; asking in no order, or for
  // a name that is not there, costs up to a read of the whole directory
  // (about 80 bytes an entry: 20 reads of storage for a thousand entries).
  // The counters zip_finds and zip_records say how it is going.
  //
  // If two entries have the same name, which of them is found depends on
  // where the search started.
  [[nodiscard]] ZipStatus Find(std::string_view name, ZipEntry& entry) noexcept;

  // Goes through the entries in the directory's order: gives the entry at
  // `cursor` and moves the cursor on, or returns kEnd. As much of the
  // entry's name as fits is put in `name` (entry.nameLength says how long
  // it really is); nothing ends it, so use the length.
  //
  // An entry that cannot be read because its sizes need ZIP64 is reported
  // as kUnsupported, with the cursor moved past it.
  [[nodiscard]] ZipStatus Next(Cursor& cursor, ZipEntry& entry, std::span<char> name) noexcept;

 private:
  struct Record;

  ZipStatus ReadRecord(std::uint32_t offset, Record& record) noexcept;
  // Whether the name of the record at `offset` is `name`: kOk or kNotFound.
  ZipStatus NameIs(std::uint32_t offset, std::string_view name) noexcept;

  core::BlockFile* file_ = nullptr;
  std::uint32_t directoryOffset_ = 0;  // Where the directory starts in the file,
  std::uint32_t directoryEnd_ = 0;     // and where it ends.
  std::uint32_t entryCount_ = 0;
  // Where the next search starts: the entry after the last one found.
  std::uint32_t searchOffset_ = 0;
  std::uint32_t searchIndex_ = 0;
};

// Reads the name of an entry, as the entry's own header in the file has
// it, into `name`: the way back from a ZipEntry, which does not carry its
// name, to the path of the file it is. `length` is how long the name is;
// if that is more than `name` has room for, only the start of it was
// read.
[[nodiscard]] ZipStatus ReadZipEntryName(core::BlockFile& file, const ZipEntry& entry, std::span<char> name,
                                         std::size_t& length) noexcept;

// The bytes of one entry, as they were before they went into the ZIP file.
//
// It is a ByteSource: the bytes come out in pieces of whatever size is
// asked for, read from the file and inflated as they are wanted. It never
// gives out more than the directory says the entry holds, and when it has
// given out all of them it checks that the data ended there and has the
// checksum the directory gives. So an entry has only been checked once it
// has been read to its end.
//
// It is about 2.7 KB, for the inflater inside it, and needs the inflater's
// window from its caller (core::Inflater::kWindowBytes): keep neither on a
// stack. One reader can read any number of entries, one after another.
//
// Used by one task at a time. The BlockFile may be used for other things
// between calls, at the cost of reading again what its buffer held.
class ZipEntryReader final : public core::ByteSource {
 public:
  // `window` may be empty if only entries that are not compressed will be
  // read; a compressed one is then refused with kNoWindow.
  explicit ZipEntryReader(std::span<std::uint8_t> window) noexcept;

  ZipEntryReader(const ZipEntryReader&) = delete;
  ZipEntryReader& operator=(const ZipEntryReader&) = delete;
  ~ZipEntryReader() = default;

  // Starts on `entry` of the ZIP file open as `file`, which must stay open
  // while the entry is read. It need not be the BlockFile the entry was
  // found through, only one open on the same file. Costs one read of
  // storage, to see where the entry's data begins.
  [[nodiscard]] ZipStatus Open(core::BlockFile& file, const ZipEntry& entry) noexcept;

  // The next bytes of the entry. Fills `dst` unless the entry ends first.
  // Returns how many bytes were read, 0 at the end of an entry that proved
  // sound, and kError if it did not or could not be read (Status() says
  // which). Bytes that came before a fault are returned first: the error
  // comes from the call after.
  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

  // kOk while all is well, and what went wrong once something has.
  ZipStatus Status() const noexcept { return status_; }

  // Whether the whole entry has been read and found sound.
  bool Finished() const noexcept { return finished_ && status_ == ZipStatus::kOk; }

  // Bytes of the entry not yet read.
  std::uint32_t Remaining() const noexcept { return remaining_; }

 private:
  // The entry's bytes as they are in the file: a source that ends where
  // they do.
  class Raw final : public core::ByteSource {
   public:
    void Start(core::BlockFile& file, std::uint32_t offset, std::uint32_t length) noexcept;
    [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

   private:
    core::BlockFile* file_ = nullptr;
    std::uint32_t position_ = 0;
    std::uint32_t left_ = 0;
  };

  void Finish() noexcept;
  std::ptrdiff_t Fail(ZipStatus status) noexcept;

  Raw raw_;
  core::Inflater inflater_;
  bool hasWindow_;
  bool deflated_ = false;
  bool finished_ = false;
  ZipStatus status_ = ZipStatus::kNotOpen;
  std::uint32_t remaining_ = 0;
  std::uint32_t crc_ = 0;
  std::uint32_t expectedCrc_ = 0;
};

}  // namespace bn::epub
