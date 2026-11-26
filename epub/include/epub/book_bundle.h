#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/block_file.h"
#include "epub/bundle.h"
#include "epub/package.h"
#include "epub/toc.h"
#include "epub/xml.h"
#include "epub/zip.h"

// Making a book's bundle from its EPUB: what is written once, before any
// chapter is imported. The bundle's four tables are filled from the book,
// and its header page says which EPUB it is of.
//
//   The strings   The title, the first author, the language and the
//                 identifier, as the package gave them, in that order:
//                 each a length in two bytes, and then its bytes.
//   The spine     A record for each document of the spine, in its order:
//                 where the file is in the EPUB, its kind (the package's
//                 MediaType, as a number), and bits: kBundleFileInRun,
//                 kBundleFilePresent.
//   The pictures  A record for each of the manifest's files that is a
//                 picture, in the manifest's order, so that a picture is
//                 known by the place Package::PictureOf() gives it: where
//                 the file is, its kind where the file is there, and bits:
//                 kBundleFilePresent, kBundleFileCover.
//   The contents  The entries of the table of contents the book is read
//                 by, one after another: how far in the entry is (one
//                 byte; 255 for more), the length of its fragment (one
//                 byte), the document it points into (two bytes, its place
//                 in the spine, or kBundleNoChapter), the length of its
//                 label (two bytes), the fragment, and the label.
//
// Every chapter is left not imported.
//
// The EPUB gives its files in an order of its own, and the pictures'
// table is in the manifest's. Looking each picture up by its name would
// read the EPUB's directory through once apiece, and writing each record
// where it falls would be a write of part of a sector apiece. So the
// directory is gone through once for every kPicturesPerPass pictures, a
// page of their records being put together in memory and then written:
// once, for a book of up to 170 pictures.
//
// A book with no table of contents has no contents, and so has one whose
// table cannot be read at all; one whose table goes wrong part way has
// the entries that came before, and one with more than kBundleMostOfEach
// has that many. The bundle is made all the same, and `amiss` in its
// header page says that its contents are not the whole of the book's
// (kBundleContentsNotWhole).
//
// It is done a piece at a time: Begin(), then Step() until it says there
// is no more. A step reads no more of the EPUB than a step of the package
// reader or of the table of contents reader does (five blocks of 4 KB),
// writes two pages of the bundle at the most, and syncs once at the most.
// How many steps there are is the book's to decide.

namespace bn::epub {

// The bits of a file's record in a bundle made here.
inline constexpr std::uint8_t kBundleFileInRun = 1;    // Of the spine: it is in the run of the book.
inline constexpr std::uint8_t kBundleFilePresent = 2;  // The file is in the EPUB, and the record says where.
inline constexpr std::uint8_t kBundleFileCover = 4;    // Of the pictures: it is the book's cover.

// The bits of `amiss` in the header page of a bundle made here.
inline constexpr std::uint16_t kBundleContentsNotWhole =
    1;  // The table of contents was cut short, or could not be read.
inline constexpr std::uint16_t kBundlePackageCutShort = 2;  // The package file went wrong after its spine.

// What an entry of the contents has for its document when it points into
// none.
inline constexpr std::uint16_t kBundleNoChapter = 0xFFFF;

// How making a book's bundle ended.
enum class BookBundleStatus : std::uint8_t {
  kOk = 0,
  kNoBook,       // The package has not been read, or could not be.
  kUnreadable,   // The EPUB could not be read where its directory is, or storage failed in reading it.
  kStoreFailed,  // Storage did not do what it was asked, in writing the bundle.
  kTooBig,       // The book has more documents or pictures than a bundle has room for.
  kMisused,      // Memory of the wrong size, or a file for the bundle that is not new and empty.
};

// The name of a status, for diagnostics: "ok", "no book", ...
const char* BookBundleStatusName(BookBundleStatus status) noexcept;

// Makes a book's bundle.
//
// Its memory is two pages the caller gives, each kBundlePageBytes and
// beginning at a multiple of four: one for the bundle's maker, and one in
// which the pictures' records are gathered and the EPUB's directory is
// read to be summed. Besides those it is 1.4 KB in a desktop build, most
// of it the reader of the table of contents, and less on the device,
// where a pointer is half the size.
//
// Used by one task.
class BookBundler final : private TocHandler {
 public:
  // What one Step() does at the most when it goes through the EPUB's
  // directory, as the package reader's are; and how many records it
  // writes when it writes the spine's or a pass's pictures.
  static constexpr std::uint32_t kEntriesPerStep = 16;
  static constexpr std::uint32_t kDirectoryBytesPerStep = 4096;
  static constexpr std::uint32_t kComparesPerStep = 16384;
  static constexpr std::size_t kPicturesPerPass = kBundlePageBytes / kBundleFileBytes;

  // Both are the caller's, alive and left alone for as long as this is in
  // use.
  BookBundler(std::span<std::uint8_t> page, std::span<std::uint8_t> records) noexcept;

  BookBundler(const BookBundler&) = delete;
  BookBundler& operator=(const BookBundler&) = delete;
  ~BookBundler() = default;

  // Gets ready to make the bundle of the book whose package `package` has
  // read. `archive` is the book's ZIP file, open; `entries` the same file
  // open for reading entries, `reader` reads them and `xml` parses, as the
  // package reader was given them; `bundle` is a new, empty file, opened
  // to be made (OpenMode::kCreate). All are the caller's, in use until the
  // making is over, and must stay alive that long.
  //
  // `about` says what the book cannot: the versions of the importer's
  // rules, when the EPUB was last written, and what is making the bundle.
  // The rest of it is filled in here.
  //
  // Returns kOk if there is anything to do. Nothing is written yet.
  [[nodiscard]] BookBundleStatus Begin(const Package& package, ZipArchive& archive, core::BlockFile& entries,
                                       ZipEntryReader& reader, XmlParser& xml, core::BlockFile& bundle,
                                       const BundleBook& about) noexcept;

  // Does the next piece. Returns true if there is more to do, false once
  // the making is over: Status() says how it ended. If it did not end
  // well, the file is to be made again: it is no bundle, unless all that
  // failed was the last sync, and then it is one the card may not have.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  [[nodiscard]] BookBundleStatus Make(const Package& package, ZipArchive& archive, core::BlockFile& entries,
                                      ZipEntryReader& reader, XmlParser& xml, core::BlockFile& bundle,
                                      const BundleBook& about) noexcept;

  BookBundleStatus Status() const noexcept { return status_; }

  // What the bundle's header page says of the book, once it is made.
  const BundleBook& Book() const noexcept { return book_; }
  // How many of the pictures have their file in the EPUB, and how many
  // times the EPUB's directory was gone through for them.
  std::uint32_t PicturesPresent() const noexcept { return picturesPresent_; }
  std::uint32_t Passes() const noexcept { return passes_; }

 private:
  enum class Stage : std::uint8_t {
    kIdle,
    kStrings,
    kSpine,
    kPictures,   // Going through the directory for some of the pictures,
    kDirectory,  // and through its bytes, for their checksum.
    kContents,
    kFinish,
  };

  [[nodiscard]] bool OnEntry(const TocEntry& entry) noexcept override;

  // The making is over, and this is how it went.
  bool End(BookBundleStatus status) noexcept;
  // What the maker said, as a status of this. True if it said ok.
  bool Wrote(BundleStatus status) noexcept;
  void WriteStrings() noexcept;
  void WriteSpine() noexcept;
  // Begins a pass over the directory for the next of the pictures, or
  // goes on to what follows them.
  void BeginPass() noexcept;
  void GoThroughDirectory() noexcept;
  void SumDirectory() noexcept;
  void BeginContents() noexcept;
  void ReadContents() noexcept;

  std::span<std::uint8_t> records_;
  BundleMaker maker_;
  BookBundleStatus status_ = BookBundleStatus::kMisused;
  Stage stage_ = Stage::kIdle;

  const Package* package_ = nullptr;
  ZipArchive* archive_ = nullptr;
  core::BlockFile* entries_ = nullptr;
  ZipEntryReader* reader_ = nullptr;
  XmlParser* xml_ = nullptr;
  BundleBook book_;

  // How many of the spine's records are written; which picture the pass
  // in hand begins with, how many it is for, and where in the directory
  // it has got to.
  std::size_t done_ = 0;
  std::size_t passFirst_ = 0;
  std::size_t passCount_ = 0;
  ZipArchive::Cursor cursor_;
  std::uint32_t picturesPresent_ = 0;
  std::uint32_t passes_ = 0;

  ZipDirectorySum sum_;
  TocReader toc_;
  bool tocBegun_ = false;
  bool entriesWereSound_ = true;  // Storage had not failed on the EPUB when the contents were begun.
  bool writeFailed_ = false;
  std::uint32_t contents_ = 0;
};

}  // namespace bn::epub
