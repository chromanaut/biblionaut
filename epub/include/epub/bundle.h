#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/block_file.h"
#include "epub/zip.h"

// A bundle: the file a book is imported into, one for each book.
//
// This is the file as a container: what is written once when it is made,
// the record it keeps of each chapter, and how a chapter is added so that
// the power going at any moment loses no chapter added before. What a
// chapter's own bytes are is not here: to this they are a run of bytes, a
// segment, of which the record says where it is.
//
//   0          the header page                4,096 bytes
//   4,096      the book's four tables         one after another: strings,
//                                             spine, pictures, contents
//   A          slot 0                         a whole number of pages
//   A + S      slot 1                         the same size
//   A + 2S     the chapters' segments         each begins on a multiple of 512
//
// Numbers are little-endian. A checksum is a CRC-32, the one a ZIP file
// uses (core/checksum.h).
//
// The header page and the tables are written when the bundle is made and
// never again. The header page says what book the bundle is of, where
// everything else is, and each table's checksum; its last four bytes are
// the checksum of the rest of it.
//
// A slot holds a record of kBundleChapterBytes for each document of the
// book's spine, in the spine's order, with a sequence number, where the
// segments end, and a checksum of itself. There are two so that one is
// always whole. To add a chapter its segment is written where the segments
// end and synced; then the slot that is not the newer one is written, a
// copy of the newer with that chapter's record changed and the next
// sequence number, and synced. Opening takes the slot with the higher
// number that passes its checksum. A cut before the slot is whole leaves
// the other slot, which knows nothing of the segment: its bytes lie past
// the end that slot records, and the next segment is written over them.
//
// Nothing that has been committed shares a sector of 512 bytes with
// anything written after it: a card rewrites whole sectors, and a write
// that is cut short may leave any part of them. So the slots begin on
// pages, and the segments end on a sector, each filled out with zeros to
// the end of its last one before it is committed.
//
// Whatever the bytes of a file are, opening it ends in a status, and so
// does everything asked of it afterwards: a checksum says when a bundle
// is to be made again, and is not what makes reading one safe.
//
// What takes long is done a piece at a time: opening, committing and
// checking a table are begun, and then Step() is called until it says
// there is no more. A step reads one page of the file at the most, writes
// one at the most, and syncs once at the most, however many documents the
// book has.

namespace bn::epub {

// How something asked of a bundle ended.
enum class BundleStatus : std::uint8_t {
  kOk = 0,
  kNotBundle,     // The file is too short to be one, or does not begin as one does.
  kOtherVersion,  // It is a bundle of another version of the format.
  kDamaged,       // It fails a checksum, or says of itself what cannot be: to be made again.
  kStoreFailed,   // Storage did not do what it was asked.
  kTooBig,        // The book has more than the format has room for.
  // Asked for what cannot be: a record that is not there, something read
  // while a piece of work is under way, a chapter whose segment is not
  // where the segments end, tables not written in their order.
  kMisused,
};

// The name of a status, for diagnostics: "ok", "not a bundle", ...
const char* BundleStatusName(BundleStatus status) noexcept;

inline constexpr std::size_t kBundlePageBytes = 4096;
inline constexpr std::size_t kBundleSectorBytes = 512;
// The version of the format this reads and writes.
inline constexpr std::uint16_t kBundleFormat = 1;
// A file's record in the tables of the spine and of the pictures; a
// chapter's record in a slot; and what a slot has besides its records.
inline constexpr std::size_t kBundleFileBytes = 24;
inline constexpr std::size_t kBundleChapterBytes = 88;
inline constexpr std::size_t kBundleSlotOtherBytes = 24;
// The classes of font family a chapter's text is counted by.
inline constexpr std::size_t kBundleClasses = 6;
inline constexpr std::size_t kBundleMadeByBytes = 32;
// The most documents a spine, pictures a book, and entries a table of
// contents can have.
inline constexpr std::size_t kBundleMostOfEach = 65535;

// The tables, in the order they are in the file.
enum class BundleTable : std::uint8_t {
  kStrings = 0,  // What the book calls itself.
  kSpine,        // A record of kBundleFileBytes for each document of the spine.
  kPictures,     // The same for each picture.
  kContents,     // The entries of the table of contents.
};
inline constexpr std::size_t kBundleTables = 4;

// What the header page says of the book and of the bundle's making.
struct BundleBook {
  // Raised by a change to importing that can move text within a paragraph
  // or change which paragraphs there are; and by one that makes importing
  // better without moving any.
  std::uint16_t textRules = 0;
  std::uint16_t importer = 0;
  // The EPUB the bundle was made from: its size, when it was last written
  // (core::FileInfo::modified), and the checksum of its ZIP directory as
  // the bytes are in the file. A bundle is of a file for as long as all
  // three are that file's.
  std::uint32_t epubBytes = 0;
  std::uint32_t epubModified = 0;
  std::uint32_t epubDirectoryChecksum = 0;
  // The checksum of what the package said (epub/package_digest.h).
  std::uint32_t packageChecksum = 0;
  // How many documents the spine has, pictures the book has, and entries
  // its table of contents has.
  std::uint16_t documents = 0;
  std::uint16_t pictures = 0;
  std::uint16_t contents = 0;
  // What made the bundle: a version, as text, filled out with zeros.
  std::array<char, kBundleMadeByBytes> madeBy{};

  bool operator==(const BundleBook& other) const noexcept = default;
};

// A file of the book, as the tables of the spine and of the pictures have
// it: where it is in the EPUB, and two bytes that mean what whoever fills
// the table says they mean.
struct BundleFile {
  ZipEntry entry;
  std::uint8_t kind = 0;
  std::uint8_t flags = 0;
};

// What has become of a document of the spine.
enum class ChapterState : std::uint8_t {
  kNotImported = 0,
  kWhole,        // Imported to its end.
  kCutShort,     // Imported as far as it could be read; what came before is kept.
  kNotDocument,  // Not a document: nothing was imported, and nothing will be.
  kRefused,      // It could not be read at all.
};

// A chapter's record.
struct BundleChapter {
  ChapterState state = ChapterState::kNotImported;
  // Why, for one cut short or refused: a number that means what whoever
  // imports says it means.
  std::uint8_t reason = 0;
  // How many things were left out of it for a limit, as far as 65,535.
  std::uint16_t leftOut = 0;
  // Where its segment is in the file and how long, and within the segment
  // where its directory is, how long, and the directory's checksum. All
  // zero for a chapter that has none.
  std::uint32_t segmentOffset = 0;
  std::uint32_t segmentBytes = 0;
  std::uint32_t directoryOffset = 0;
  std::uint32_t directoryBytes = 0;
  std::uint32_t directoryChecksum = 0;
  std::uint32_t blocks = 0;
  std::uint32_t characters = 0;
  // Its running text by class of font family.
  std::array<std::uint32_t, kBundleClasses> classCharacters{};
  // Which characters it uses: a bit for each row of 256, the lowest bit of
  // the first byte for the first row.
  std::array<std::uint8_t, 32> characterRows{};

  bool operator==(const BundleChapter& other) const noexcept = default;
};

// The records as they are in the file. Reading a chapter's record back
// says false if its bytes are no record: a state there is none of.
void StoreBundleFile(const BundleFile& file, std::span<std::uint8_t, kBundleFileBytes> out) noexcept;
BundleFile LoadBundleFile(std::span<const std::uint8_t, kBundleFileBytes> in) noexcept;
void StoreBundleChapter(const BundleChapter& chapter, std::span<std::uint8_t, kBundleChapterBytes> out) noexcept;
[[nodiscard]] bool LoadBundleChapter(std::span<const std::uint8_t, kBundleChapterBytes> in,
                                     BundleChapter& chapter) noexcept;

// How long each slot of a book of so many documents is: its records and
// what else it has, filled out to whole pages.
std::uint32_t BundleSlotBytes(std::size_t documents) noexcept;

// What a slot's records come to between them.
struct BundleTotals {
  std::uint32_t notImported = 0;  // Chapters in that state.
  std::uint64_t characters = 0;
  std::array<std::uint64_t, kBundleClasses> classCharacters{};

  bool operator==(const BundleTotals& other) const noexcept = default;
};

// Makes a bundle: writes the tables as they are given, then the two slots
// with every chapter not imported, then the header page, and syncs.
//
//   Begin(file, book)
//   for each table, in the order of BundleTable:
//     BeginTable(table), then Write() or WriteFile() as often as wanted
//   Finish(), then Step() until it says there is no more
//
// The file is a new, empty one the caller has opened (OpenMode::kCreate),
// and is used by nothing else until the making is over. Until then, and
// if the making is cut short at any point, the file is not a bundle:
// opening it says so, and it is made again.
//
// Its memory is a page the caller gives, which holds what has been given
// of a table and not yet written. A call to Write() writes as many pages
// as it fills; a Step() writes one, and the last one syncs.
//
// Used by one task.
class BundleMaker {
 public:
  // `page` is kBundlePageBytes that begin at a multiple of four, the
  // caller's, alive and left alone for as long as this is in use.
  explicit BundleMaker(std::span<std::uint8_t> page) noexcept;

  BundleMaker(const BundleMaker&) = delete;
  BundleMaker& operator=(const BundleMaker&) = delete;
  ~BundleMaker() = default;

  [[nodiscard]] BundleStatus Begin(core::BlockFile& file, const BundleBook& book) noexcept;

  // Goes on to `table`, which must be the one after the last begun.
  [[nodiscard]] BundleStatus BeginTable(BundleTable table) noexcept;
  // Adds bytes to the table in hand; and a file's record, which is how the
  // tables of the spine and of the pictures are filled.
  [[nodiscard]] BundleStatus Write(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] BundleStatus WriteFile(const BundleFile& file) noexcept;

  // Ends the tables. kMisused, with nothing more done, unless all four
  // were begun and the spine and the pictures have a record for each
  // document and picture the book was said to have.
  [[nodiscard]] BundleStatus Finish() noexcept;
  // Does the next piece of what is left. Returns true if there is more to
  // do, false once it is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;

  // kOk until something goes wrong, and at the end if nothing did.
  BundleStatus Status() const noexcept { return status_; }

 private:
  enum class Stage : std::uint8_t { kIdle, kTables, kSlots, kHeader, kSync, kDone };

  BundleStatus Fail(BundleStatus status) noexcept;
  // Writes the page at `offset`.
  bool WritePage(std::uint32_t offset) noexcept;
  void FillSlotPage() noexcept;
  void FillHeaderPage() noexcept;

  std::span<std::uint8_t> page_;
  core::BlockFile* file_ = nullptr;
  BundleBook book_;
  BundleStatus status_ = BundleStatus::kMisused;
  Stage stage_ = Stage::kIdle;

  // The tables: how many have been begun, where each begins and how long
  // it is so far, and its checksum.
  std::size_t tablesBegun_ = 0;
  std::array<std::uint32_t, kBundleTables> tableOffset_{};
  std::array<std::uint32_t, kBundleTables> tableBytes_{};
  std::array<std::uint32_t, kBundleTables> tableChecksum_{};
  // How much of the page is filled, and where in the file the page goes.
  std::size_t filled_ = 0;
  std::uint32_t pageOffset_ = 0;

  // The slots: where the first is, how long each is and how much of that
  // is used, which is being written, how far through it, and its checksum
  // so far.
  std::uint32_t slotOffset_ = 0;
  std::uint32_t slotBytes_ = 0;
  std::uint32_t slotUsed_ = 0;
  std::size_t slot_ = 0;
  std::uint32_t slotDone_ = 0;
  std::uint32_t slotChecksum_ = 0;
};

// A bundle that is open.
//
// The file is one the caller has opened, to be read (OpenMode::kRead) or
// to have chapters added (OpenMode::kUpdate), and is used through here and
// by whoever writes a segment, by turns. It must stay open, and alive, for
// as long as this is.
//
// Opening reads the header page and the slots and checks them. It does
// not read the tables: a table is checked against its checksum when
// Check() is asked to, and what is read from one that has not been
// checked is whatever the file holds.
//
// Its memory is a page the caller gives, in use only while a piece of
// work is under way. Nothing of the book is kept: each record asked for is
// read from the file, with one read of storage, or two where it lies
// across two blocks of the file.
//
// Used by one task.
class Bundle {
 public:
  // `page` is kBundlePageBytes that begin at a multiple of four, the
  // caller's, alive and left alone for as long as this is in use.
  explicit Bundle(std::span<std::uint8_t> page) noexcept;

  Bundle(const Bundle&) = delete;
  Bundle& operator=(const Bundle&) = delete;
  ~Bundle() = default;

  // Begins opening the bundle that `file` is; then Step() until it says
  // there is no more. Whatever was open is forgotten.
  [[nodiscard]] BundleStatus BeginOpen(core::BlockFile& file) noexcept;
  // The same, to its end, in one call: for a tool or a test.
  [[nodiscard]] BundleStatus Open(core::BlockFile& file) noexcept;
  // Forgets the file. It is the caller's to close.
  void Close() noexcept;
  // A bundle is open: it was opened, to the end, and nothing asked of it
  // since has failed in a way that leaves it unknown what the file holds.
  bool IsOpen() const noexcept { return open_; }

  // Does the next piece of whatever was begun. Returns true if there is
  // more to do, false once it is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;
  // How the last piece of work ended; kOk while one is going on and
  // nothing has gone wrong.
  BundleStatus Status() const noexcept { return status_; }

  // What follows is of an open bundle, and not to be asked while a piece
  // of work is under way (kMisused).

  const BundleBook& Book() const noexcept { return book_; }
  // Whether the bundle is of the EPUB that is this long, was last written
  // then, and whose ZIP directory has this checksum.
  bool IsOf(std::uint32_t epubBytes, std::uint32_t epubModified, std::uint32_t epubDirectoryChecksum) const noexcept;

  // Where a table is in the file, how long it is, and its checksum.
  struct Place {
    std::uint32_t offset = 0;
    std::uint32_t bytes = 0;
    std::uint32_t checksum = 0;
  };
  Place Table(BundleTable table) const noexcept;

  // Where the segments begin, and where they end: which is where the next
  // is to be written, and is a multiple of kBundleSectorBytes.
  std::uint32_t DataBegin() const noexcept { return dataBegin_; }
  std::uint32_t DataEnd() const noexcept { return dataEnd_; }
  // The number of the slot that is in force: one more with every chapter
  // committed.
  std::uint32_t Sequence() const noexcept { return sequence_; }
  // What the chapters' records come to between them.
  const BundleTotals& Totals() const noexcept { return totals_; }

  // The record of a document of the spine, of a picture, and of a chapter.
  [[nodiscard]] BundleStatus ReadSpine(std::size_t index, BundleFile& file) noexcept;
  [[nodiscard]] BundleStatus ReadPicture(std::size_t index, BundleFile& file) noexcept;
  [[nodiscard]] BundleStatus ReadChapter(std::size_t index, BundleChapter& chapter) noexcept;
  // dst.size() bytes of a table, from `at` bytes into it.
  [[nodiscard]] BundleStatus ReadTable(BundleTable table, std::uint32_t at, std::span<std::uint8_t> dst) noexcept;

  // Begins checking a table against its checksum; then Step(). It ends
  // kOk, or kDamaged if the table is not what was written.
  [[nodiscard]] BundleStatus BeginCheck(BundleTable table) noexcept;
  [[nodiscard]] BundleStatus Check(BundleTable table) noexcept;

  // Begins committing a chapter; then Step(). The file must have been
  // opened to be changed.
  //
  // A record that has a segment (segmentBytes is not 0) must say the
  // segment begins at DataEnd(), where the caller has by now written all
  // of it, and must have its directory inside it. The segment is filled
  // out to the end of its last sector here. A record with none (a chapter
  // that is no document, or was refused) leaves the segments as they are.
  //
  // A chapter may be committed again: its record is replaced, and its old
  // segment is left where it is, unused.
  //
  // If it ends in anything but kOk the bundle is no longer open: what the
  // file holds is whatever opening it again finds, which is every chapter
  // committed before and perhaps this one.
  [[nodiscard]] BundleStatus BeginCommit(std::size_t chapter, const BundleChapter& record) noexcept;
  [[nodiscard]] BundleStatus Commit(std::size_t chapter, const BundleChapter& record) noexcept;

 private:
  enum class Job : std::uint8_t { kNone, kOpening, kChecking, kCommitting };
  enum class Stage : std::uint8_t {
    kHeader,       // Opening: the header page,
    kHeads,        // the start of each slot,
    kSlot,         // and a slot from end to end.
    kFill,         // Committing: the segment filled out to its sector,
    kSyncSegment,  // and synced;
    kCopy,         // the slot copied, changed;
    kSyncSlot,     // and synced.
    kTable,        // Checking: a table from end to end.
  };

  // What the start of a slot says.
  struct Head {
    bool plausible = false;
    std::uint32_t sequence = 0;
    std::uint32_t dataEnd = 0;
  };

  // The work in hand is over, and this is how it went.
  bool End(BundleStatus status) noexcept;
  bool ReadPage(std::uint32_t offset, std::size_t bytes) noexcept;
  std::uint32_t SlotOffset(std::size_t slot) const noexcept;
  BundleStatus ReadRecord(std::uint32_t offset, std::span<std::uint8_t> record) noexcept;
  bool Idle() const noexcept { return open_ && job_ == Job::kNone; }

  // The pieces of opening.
  void OpenHeader() noexcept;
  void OpenHead() noexcept;
  void OpenSlot() noexcept;
  // Goes on to check the slot that looks the newer of those not yet
  // found wanting. False if there is none left.
  bool ChooseSlot() noexcept;
  // Takes some of a slot's records as they go by, to add them up and to
  // see that each is one. False if one is not.
  bool Count(std::span<const std::uint8_t> bytes) noexcept;
  bool CountRecord() noexcept;
  // The pieces of committing.
  void Fill() noexcept;
  void Copy() noexcept;
  // The piece of checking a table.
  void CheckSome() noexcept;

  std::span<std::uint8_t> page_;
  core::BlockFile* file_ = nullptr;
  bool open_ = false;
  BundleStatus status_ = BundleStatus::kMisused;
  Job job_ = Job::kNone;
  Stage stage_ = Stage::kHeader;

  // What the header page said.
  BundleBook book_;
  std::array<Place, kBundleTables> tables_{};
  std::uint32_t slotOffset_ = 0;
  std::uint32_t slotBytes_ = 0;
  std::uint32_t slotUsed_ = 0;
  std::uint32_t dataBegin_ = 0;

  // The slot in force.
  std::size_t current_ = 0;
  std::uint32_t sequence_ = 0;
  std::uint32_t dataEnd_ = 0;
  BundleTotals totals_;

  // The work in hand: how far through a slot or a table it is, and the
  // checksum of what has gone by.
  std::uint32_t done_ = 0;
  std::uint32_t checksum_ = 0;
  // Opening: what each slot's start said, which have been found wanting,
  // which is being looked at, and what its records come to so far, with
  // the bytes of a record that lies across two pages.
  std::array<Head, 2> heads_{};
  std::array<bool, 2> wanting_{};
  std::size_t slot_ = 0;
  BundleTotals counted_;
  std::array<std::uint8_t, kBundleChapterBytes> record_{};
  std::size_t recordFilled_ = 0;
  std::array<std::uint8_t, 4> stored_{};  // The checksum the slot carries.
  // Checking: which table.
  std::size_t table_ = 0;
  // Committing: which chapter, its record as it was and as it is to be,
  // and where the segments will end.
  std::size_t chapter_ = 0;
  BundleChapter before_;
  BundleChapter after_;
  std::array<std::uint8_t, kBundleChapterBytes> changed_{};
  std::uint32_t newDataEnd_ = 0;
};

}  // namespace bn::epub
