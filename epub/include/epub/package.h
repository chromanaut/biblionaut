#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/fixed_text.h"
#include "epub/book_path.h"
#include "epub/xml.h"
#include "epub/zip.h"

// What a book says of itself: its title and author, the documents it is
// made of and the order they are read in, and where its cover and its
// table of contents are. In an EPUB this is the "package" file, an XML
// file with the ending .opf, which META-INF/container.xml points to.

namespace bn::epub {

// How reading a book's package ended.
enum class PackageStatus : std::uint8_t {
  kOk = 0,
  kNoContainer,  // No META-INF/container.xml, or it does not name a package file.
  kNoPackage,    // The package file it names is not in the book.
  kNotPackage,   // The package file is not XML, or lists nothing to read.
  kNoMemory,     // The book has more documents than there is memory to list, or the parser ran out.
  kUnreadable,   // The ZIP file is damaged where one of these is, or storage failed.
};

// The name of a status, for diagnostics: "ok", "no container", ...
const char* PackageStatusName(PackageStatus status) noexcept;

// What kind of thing a file of the book is, going by what the package
// says of it.
enum class MediaType : std::uint8_t {
  kOther = 0,
  kXhtml,
  kCss,
  kNcx,
  kJpeg,
  kPng,
  kGif,
  kSvg,
  kFont,
};

// The kind a media type's name stands for: "application/xhtml+xml",
// "image/jpeg" and the rest, with the misspellings books use.
MediaType MediaTypeOf(std::string_view name) noexcept;
const char* MediaTypeName(MediaType type) noexcept;

// One document of the book's text, in its place in the reading order.
struct SpineItem {
  // Where the document is in the ZIP file, to give to a ZipEntryReader.
  // Only if `present`.
  ZipEntry entry;
  // The file is in the book. A package can list a file that is not.
  bool present = false;
  // It is part of the run of the book from front to back. A document that
  // is not is one set aside, such as a page of notes that is only reached
  // by a link.
  bool linear = true;
  MediaType type = MediaType::kOther;
};

// Reads a book's package and holds what it said.
//
// Reading goes in three steps. container.xml is read, to find the package
// file. The package file is parsed: the title, author and language are
// kept, and every file the manifest lists is noted, by two numbers that
// stand for its id and its path (NameHash()), not by the text of either.
// The spine, which lists ids, is matched against those. Then the ZIP
// file's directory is gone through once, and each of its entries that is
// a document of the spine, the cover or a table of contents is kept as a
// ZipEntry. So however the files are ordered in the ZIP file, and however
// many the book has, finding them costs one reading of the directory.
//
// Memory: the caller provides a block. The spine is built at its front, 28
// bytes a document, and stays there. The manifest is kept at its back, 16
// bytes a file, only while Read() runs. A book of 80 files and 50
// documents needs under 3 KB; the largest seen, 745 files and 394
// documents, 23 KB. A book that needs more than the block is refused.
//
// Since no name is kept, none is checked: two different names that came to
// one number would be taken for each other. NameHash() says how seldom
// that is. A caller that has to be sure can read an entry's name back
// with ReadZipEntryName().
//
// Used by one task.
class Package {
 public:
  // `memory` must stay alive, and be left alone, for as long as this is.
  explicit Package(std::span<std::uint8_t> memory) noexcept;

  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;
  ~Package() = default;

  // Reads the package of the book `archive` has open. `entries` is the
  // same file opened a second time, `reader` reads an entry from it, and
  // `xml` parses: all three are the caller's, to share with whatever else
  // it does with the book, and are left free to use again.
  [[nodiscard]] PackageStatus Read(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                                   XmlParser& xml) noexcept;

  PackageStatus Status() const noexcept { return status_; }

  // What the book calls itself, who it says wrote it, and its language as
  // a code ("en", "pt-BR"): the first of each that it gives, with any run
  // of spaces and line ends made one space. Empty if it gives none.
  std::string_view Title() const noexcept { return title_.View(); }
  std::string_view Author() const noexcept { return author_.View(); }
  std::string_view Language() const noexcept { return language_.View(); }

  // The path of the package file within the book.
  std::string_view PackagePath() const noexcept { return packagePath_.View(); }

  // The documents of the book, in reading order.
  std::span<const SpineItem> Spine() const noexcept { return {spine_, spineCount_}; }

  // The cover picture; the table of contents as EPUB 3 has it, a document
  // of its own (the "nav"); and as EPUB 2 has it (the "NCX"). Each is null
  // if the package names none, or names a file that is not in the book.
  const ZipEntry* Cover() const noexcept { return cover_.present ? &cover_.entry : nullptr; }
  MediaType CoverType() const noexcept { return cover_.type; }
  const ZipEntry* Nav() const noexcept { return nav_.present ? &nav_.entry : nullptr; }
  const ZipEntry* Ncx() const noexcept { return ncx_.present ? &ncx_.entry : nullptr; }

  // How many files the manifest lists, and how many documents the spine
  // named that the manifest does not have (which are left out of Spine()).
  std::uint32_t ManifestItems() const noexcept { return itemCount_; }
  std::uint32_t UnknownSpineItems() const noexcept { return unknownSpineItems_; }
  // The package file stopped being XML part way, after its spine: what
  // came before the mistake was kept.
  bool CutShort() const noexcept { return cutShort_; }
  // The most of the block that was in use at once, in bytes.
  std::size_t MemoryUsed() const noexcept { return memoryUsed_; }

 private:
  // A file the manifest lists.
  struct Item {
    std::uint32_t idLow;
    std::uint32_t pathLow;
    std::uint16_t idHigh;
    std::uint16_t pathHigh;
    MediaType type;
    std::uint8_t flags;
    std::uint16_t spare;
  };

  // A file found for one of the book's particular purposes.
  struct Found {
    ZipEntry entry;
    bool present = false;
    MediaType type = MediaType::kOther;
    std::uint32_t item = kNoItem;  // Which item of the manifest it is.
  };

  // What listens to the package file as it is parsed (package.cpp). It is
  // final, which the check cannot see from here.
  class PackageHandler;  // NOLINT(cppcoreguidelines-virtual-class-destructor)

  static constexpr std::uint32_t kNoItem = 0xFFFFFFFFU;

  void Clear() noexcept;
  PackageStatus Fail(PackageStatus status) noexcept;
  // What Read() does, without the record of it in the trace.
  PackageStatus ReadParts(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                          XmlParser& xml) noexcept;
  // Adds a file to the manifest, or a document to the spine. False if
  // there is no room.
  bool AddItem(std::uint64_t idHash, std::uint64_t pathHash, MediaType type, std::uint8_t flags) noexcept;
  bool AddSpine(std::uint64_t idHash, bool linear) noexcept;
  // The item with this id, or kNoItem.
  std::uint32_t ItemWithId(std::uint64_t idHash) const noexcept;
  Item& ItemAt(std::uint32_t index) noexcept { return itemsEnd_[-static_cast<std::ptrdiff_t>(index) - 1]; }
  const Item& ItemAt(std::uint32_t index) const noexcept { return itemsEnd_[-static_cast<std::ptrdiff_t>(index) - 1]; }
  // Matches the spine, the cover and the tables of contents to the items
  // of the manifest.
  void Resolve() noexcept;
  // Goes through the ZIP file's directory for the files that are wanted.
  PackageStatus FindFiles(ZipArchive& archive) noexcept;

  // The block: the spine from its front, the manifest from its back.
  SpineItem* spine_ = nullptr;
  Item* itemsEnd_ = nullptr;
  std::size_t capacityBytes_ = 0;
  std::size_t spineCount_ = 0;
  std::uint32_t itemCount_ = 0;
  std::size_t memoryUsed_ = 0;

  PackageStatus status_ = PackageStatus::kNoContainer;
  bool cutShort_ = false;
  bool outOfMemory_ = false;
  std::uint32_t unknownSpineItems_ = 0;

  core::FixedText<256> title_;
  core::FixedText<128> author_;
  core::FixedText<16> language_;
  core::FixedText<kMaxBookPath> packagePath_;

  Found cover_;
  Found nav_;
  Found ncx_;
  // What the package said of them by id, until the ids are matched.
  std::uint64_t coverId_ = 0;
  std::uint64_t ncxId_ = 0;
  bool hasCoverId_ = false;
  bool hasNcxId_ = false;
};

}  // namespace bn::epub
