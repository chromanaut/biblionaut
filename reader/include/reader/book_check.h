#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/clock.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "core/trace_stream.h"
#include "epub/package.h"
#include "epub/toc.h"
#include "epub/toc_digest.h"
#include "epub/xml.h"
#include "epub/xml_digest.h"
#include "epub/zip.h"
#include "reader/device_check.h"

namespace bn::reader {

// Reads every book on the card from end to end, to find out on a device
// whether the ZIP reader, the inflater, the XML parser and the package
// reader work there and how fast they are. It writes nothing to the card.
//
//   1. times the checksum by itself, over 256 KB held in memory;
//   2. lists /books and each folder in it, one level down and no
//      further; or the top directory and its folders, if there is no
//      /books or no book there;
//   3. takes each file found whose name ends in ".epub" for a book, and
//      first opens it as a reader would: finds its directory, reads its
//      package (epub/package.h), which says what the book is called and
//      where its documents are, and then reads its table of contents
//      (epub/toc.h). Then closes it again;
//   4. opens it afresh, reads every entry to its end, which checks the
//      entry's size and checksum, and looks each entry up by its name;
//   5. parses every entry with an XML file's name as it is read, counting
//      what the parser finds and taking a checksum of it;
//   6. says what each book came to, and all of them together, in lines of
//      text on the diagnostic stream.
//
// Step 3 is what `bn-epub info` and `bn-epub toc` do to a file on the
// desktop, step 4 what `bn-zip check` does, read for read, and step 5 what
// `bn-xml book` does, so that each can be compared: a book should come to
// the same package and the same table of contents (by their checksums,
// epub/package_digest.h and epub/toc_digest.h) in the same number of
// reads, the same entries, the same bytes and the same number of reads
// again, and its XML to the same events.
//
// Time is counted four ways: inside storage, which on a device is the
// card; reading out compressed entries with storage's share taken off,
// which is the processor inflating and checksumming; parsing XML, with the
// reading and inflating of it taken off; and all of it. Reading the
// package and reading the table of contents are each counted by
// themselves, in time and in reads, and are in none of those. The package
// is read in one go, however long it takes, as it would be for a reader.
//
// Otherwise it works for about 20 ms each Tick() and then lets the
// application carry on, so a run takes longer by the clock than the times
// it reports, which count its own work only.
//
// Used by one task. It is about 109 KB, most of that the inflater's
// window, the XML parser's memory and the package reader's: too big for a
// stack.
class BookCheck final : public DeviceCheck {
 public:
  enum class State : std::uint8_t {
    kIdle,  // Never run.
    kRunning,
    kPassed,  // Every book was read, every entry of it sound, and there was at least one.
    kFailed,
  };

  // What reading one book came to, or all of them together.
  struct Figures {
    std::uint32_t entries = 0;
    std::uint32_t failedEntries = 0;  // Could not be read, or were not what the directory says.
    std::uint64_t bytesOut = 0;       // What the entries hold,
    std::uint64_t bytesIn = 0;        // and what they take up in the file.
    std::uint64_t inflatedOut = 0;    // The same two for the compressed entries alone:
    std::uint64_t inflatedIn = 0;     // what the inflater gave out, and what it was given.
    std::uint32_t reads = 0;          // Reads of storage,
    std::uint64_t readBytes = 0;      // and what they brought.
    std::uint64_t storageUs = 0;      // Time spent inside storage.
    std::uint64_t inflateUs = 0;      // Time reading out compressed entries, less storage's share.
    std::uint64_t workUs = 0;         // All the time spent.

    // The entries with an XML file's name, which are parsed as they are
    // read.
    std::uint32_t xmlDocuments = 0;
    std::uint32_t xmlNotXml = 0;      // Of them, those the parser stopped in: not XML, or not in an encoding it knows.
    std::uint32_t xmlRefused = 0;     // And those that needed more memory than the parser has.
    std::uint64_t xmlBytes = 0;       // What the parser was given.
    std::uint32_t xmlEvents = 0;      // What it found there (epub/xml_digest.h),
    std::uint64_t xmlEventBytes = 0;  // and how many bytes of that the check checksummed.
    // Time parsing, with the reading and inflating taken off. The check's
    // own counting and checksumming of what was found is in it.
    std::uint64_t xmlUs = 0;
    // The most of its memory the parser used for one document, and how
    // many documents used more than 8, 12 and 16 KB of it.
    std::uint32_t xmlMostMemory = 0;
    std::uint32_t xmlOver8K = 0;
    std::uint32_t xmlOver12K = 0;
    std::uint32_t xmlOver16K = 0;

    // Opening the book and reading its package. None of this is in the
    // figures above.
    std::uint32_t packagesRead = 0;
    std::uint32_t packagesNotRead = 0;    // The file could be opened, and its package could not be read.
    std::uint32_t packageReads = 0;       // Reads of storage, finding the directory included,
    std::uint64_t packageReadBytes = 0;   // and what they brought.
    std::uint64_t packageStorageUs = 0;   // Time spent inside storage,
    std::uint64_t packageUs = 0;          // and all the time it took.
    std::uint64_t packageLongestUs = 0;   // The longest one book took,
    std::uint32_t packageMostMemory = 0;  // and the most of the reader's memory one used.

    // Reading the book's table of contents, which follows with the book
    // still open. None of this is in the figures above either.
    std::uint32_t tocsRead = 0;
    std::uint32_t tocsNav = 0;       // Of those read, how many were a nav and not an NCX.
    std::uint32_t tocsNone = 0;      // The book has none.
    std::uint32_t tocsNotRead = 0;   // It has one, which could not be read.
    std::uint32_t tocEntries = 0;    // The entries of those read.
    std::uint32_t tocReads = 0;      // Reads of storage,
    std::uint64_t tocReadBytes = 0;  // and what they brought.
    std::uint64_t tocStorageUs = 0;  // Time spent inside storage,
    std::uint64_t tocUs = 0;         // and all the time it took.
    std::uint64_t tocLongestUs = 0;  // The longest one book took.
  };

  // How much of an entry is read out at a time.
  static constexpr std::size_t kChunkBytes = 4096;
  // How long one Tick() works for before it lets go.
  static constexpr std::uint64_t kSliceUs = 20'000;
  // How much is checksummed to time the checksum.
  static constexpr std::size_t kChecksumBytes = 64 * kChunkBytes;
  // The memory the XML parser is given: more than any document should
  // want, so that what a document uses is what it wants.
  static constexpr std::size_t kXmlMemoryBytes = 32768;
  // The memory the package reader is given for a book's files and
  // documents: 16 bytes a file and 28 a document.
  static constexpr std::size_t kPackageMemoryBytes = 24576;

  BookCheck(core::Storage& storage, core::Clock& clock, core::TraceStream& stream) noexcept;

  BookCheck(const BookCheck&) = delete;
  BookCheck& operator=(const BookCheck&) = delete;
  ~BookCheck() override;

  // Starts a run from the beginning, unless one is under way.
  void Begin() noexcept override;

  // Does the next pieces of a run, if one is under way.
  void Tick() noexcept override;

  State CurrentState() const noexcept { return state_; }

  // Of the last run, or of the one under way so far: how many books there
  // were, how many of them failed, and what they all came to.
  std::uint32_t Books() const noexcept { return books_; }
  std::uint32_t FailedBooks() const noexcept { return failedBooks_; }
  const Figures& Totals() const noexcept { return totals_; }
  // How long checksumming kChecksumBytes took.
  std::uint64_t ChecksumUs() const noexcept { return checksumUs_; }
  // The checksum of everything the parser found in the XML of the book
  // last finished, its documents taken one after another.
  std::uint32_t XmlChecksum() const noexcept { return xmlChecksum_; }
  // The checksum of what the package of the book last begun was found to
  // hold (epub/package_digest.h); 0 if it could not be read.
  std::uint32_t PackageChecksum() const noexcept { return packageChecksum_; }
  // And of the entries of its table of contents (epub/toc_digest.h); 0 if
  // it has none, or none that could be read.
  std::uint32_t TocChecksum() const noexcept { return tocChecksum_; }

 private:
  // Storage that passes everything on to another, counting the reads and
  // the time spent inside.
  class Meter final : public core::Storage {
   public:
    Meter(core::Storage& storage, core::Clock& clock) noexcept : storage_(storage), clock_(clock) {}

    struct Counts {
      std::uint32_t reads = 0;
      std::uint64_t readBytes = 0;
      std::uint64_t timeUs = 0;
    };
    const Counts& Now() const noexcept { return counts_; }

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
    [[nodiscard]] core::StorageStatus OpenDirectory(std::string_view path,
                                                    core::DirectoryHandle& directory) noexcept override;
    [[nodiscard]] core::StorageStatus NextInDirectory(core::DirectoryHandle directory, std::span<char> name,
                                                      std::size_t& length, core::FileInfo& info) noexcept override;
    void CloseDirectory(core::DirectoryHandle directory) noexcept override;

   private:
    core::Storage& storage_;
    core::Clock& clock_;
    Counts counts_;
  };

  // Gives the XML parser the entry being read. It draws on the entry a
  // chunk at a time, exactly as reading the entry out does, and hands the
  // parser as much of the chunk as it asks for.
  class Feeder final : public core::ByteSource {
   public:
    explicit Feeder(BookCheck& owner) noexcept : owner_(owner) {}

    // Gets ready for another entry.
    void Start() noexcept;
    [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

    // The entry has been read to its end, or has failed.
    bool Ended() const noexcept { return ended_; }
    // Time spent reading the entry, inflating included, over all entries.
    std::uint64_t SourceUs() const noexcept { return sourceUs_; }
    // Bytes handed to the parser since Start().
    std::uint64_t Given() const noexcept { return given_; }

   private:
    BookCheck& owner_;
    std::span<const std::uint8_t> pending_;  // Of the chunk, what the parser has not had yet.
    bool ended_ = false;
    bool failed_ = false;
    std::uint64_t sourceUs_ = 0;
    std::uint64_t given_ = 0;
  };

  enum class Step : std::uint8_t {
    kNone,
    kChecksum,
    kOpenDirectory,
    kOpenFolder,
    kNextBook,
    kReadPackage,
    kReadToc,
    kOpenBook,
    kNextEntry,
    kParseEntry,
    kReadEntry,
    kFindEntry,
    kEndBook,
  };

  void DoStep() noexcept;
  void TimeChecksum() noexcept;
  void OpenDirectory() noexcept;
  void OpenFolder() noexcept;
  void NextBook() noexcept;
  void ReadPackage() noexcept;
  void ReadToc() noexcept;
  void OpenBook() noexcept;
  void NextEntry() noexcept;
  void ParseEntry() noexcept;
  void ReadEntry() noexcept;
  void FindEntry() noexcept;
  void EndBook() noexcept;

  // Reads the next chunk of the entry, counting it. Returns what the
  // entry's reader returned: how many bytes, 0 at the end, less on failure.
  std::ptrdiff_t PullChunk() noexcept;
  // The entry has been read as far as it goes: notes how that ended and
  // moves on.
  void EndEntryRead() noexcept;

  // Looks in the top directory next, if that is not where it has just
  // looked; otherwise ends the run.
  void LookInTopDirectoryOrFinish() noexcept;
  // Notes that an entry is not sound: its number, counted from 1, and how
  // it failed.
  void EntryFailed(std::uint32_t number, epub::ZipStatus status) noexcept;
  // Ends a book that could not be read at all.
  void BookFailed(std::string_view why) noexcept;
  void CloseBook() noexcept;
  void CloseListings() noexcept;
  void MoveTo(Step step) noexcept;
  void Finish(bool passed) noexcept;
  std::string_view Path() const noexcept { return {path_.data(), pathLength_}; }

  core::Clock& clock_;
  core::TraceStream& stream_;
  Meter meter_;

  State state_ = State::kIdle;
  Step step_ = Step::kNone;
  std::size_t count_ = 0;  // Pieces done within the step.
  std::uint32_t beganTrace_ = 0;
  std::uint32_t bookBeganTrace_ = 0;
  std::uint32_t checksum_ = 0;
  std::uint64_t checksumUs_ = 0;

  // The directories being listed: the one books are looked for in, and
  // one folder in it at a time. With the book open twice, that is as much
  // as storage lets be open at once.
  std::array<core::DirectoryHandle, 2> listings_{core::kNoDirectory, core::kNoDirectory};
  std::size_t depth_ = 0;  // Which of the two is being gone through.
  bool inTopDirectory_ = false;
  std::uint32_t foldersPassedOver_ = 0;  // Folders inside a folder, which are not looked in.
  // The path of the book being read is its directory's, a '/', and its
  // name. Of that, what comes before a name found in each of the two.
  std::array<std::size_t, 2> prefixLengths_{};
  std::size_t pathLength_ = 0;
  std::uint32_t booksHere_ = 0;
  std::uint32_t bookSize_ = 0;

  std::uint32_t books_ = 0;
  std::uint32_t failedBooks_ = 0;
  Figures totals_;
  Figures book_;
  // What finding the book's directory cost, out of book_.
  std::uint32_t openReads_ = 0;
  std::uint64_t openUs_ = 0;
  // The first of its entries that failed, counted from 1, and how.
  std::uint32_t firstFailedEntry_ = 0;
  epub::ZipStatus firstFailure_ = epub::ZipStatus::kOk;

  epub::ZipArchive archive_;
  epub::ZipArchive::Cursor cursor_;
  epub::ZipEntry entry_;

  std::array<char, core::Storage::kMaxPath> path_{};
  std::array<char, 256> entryName_{};
  // Aligned, and a multiple of four long, so that a device's card driver
  // takes them as they are.
  alignas(4) std::array<std::uint8_t, 4096> directoryBuffer_{};
  alignas(4) std::array<std::uint8_t, 4096> entriesBuffer_{};
  alignas(4) std::array<std::uint8_t, kChunkBytes> chunk_{};
  alignas(4) std::array<std::uint8_t, core::Inflater::kWindowBytes> window_{};
  // The book is open twice, once for its directory and once for its
  // entries, as epub/zip.h advises.
  core::BlockFile directoryFile_;
  core::BlockFile entriesFile_;
  epub::ZipEntryReader entryReader_;

  // For the XML: the parser with its memory, what feeds it, and what
  // takes account of everything it finds in a book.
  alignas(8) std::array<std::uint8_t, kXmlMemoryBytes> xmlMemory_{};
  epub::XmlParser xml_;
  Feeder feeder_;
  epub::XmlEventDigest digest_;
  std::uint32_t xmlChecksum_ = 0;

  // For the package: the reader with its memory.
  alignas(8) std::array<std::uint8_t, kPackageMemoryBytes> packageMemory_{};
  epub::Package package_;
  std::uint32_t packageChecksum_ = 0;

  // For the table of contents: what reads it, and what takes account of
  // its entries.
  epub::TocReader toc_;
  epub::TocDigest tocDigest_;
  std::uint32_t tocChecksum_ = 0;
};

}  // namespace bn::reader
