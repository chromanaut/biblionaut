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
  kOk = 0,       // Nothing wrong so far; at the end, it was read.
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
//
// It is 24 bytes, which is what a ZipEntry is: an entry's numbers take 22
// of those, and a ZipEntry leaves the last two over. So the numbers are
// laid out here themselves, and what else there is to say of a document
// takes those two bytes, where beside a ZipEntry it would make the
// document 28.
struct SpineItem {
  // Where the document is in the ZIP file, to give to a ZipEntryReader.
  // Only if `present`.
  ZipEntry Entry() const noexcept {
    return {.headerOffset = headerOffset,
            .compressedSize = compressedSize,
            .size = size,
            .crc32 = crc32,
            .method = method,
            .flags = flags,
            .nameLength = nameLength};
  }
  void SetEntry(const ZipEntry& entry) noexcept {
    headerOffset = entry.headerOffset;
    compressedSize = entry.compressedSize;
    size = entry.size;
    crc32 = entry.crc32;
    method = entry.method;
    flags = entry.flags;
    nameLength = entry.nameLength;
  }

  // What a ZipEntry has, each as it is there.
  std::uint32_t headerOffset = 0;
  std::uint32_t compressedSize = 0;
  std::uint32_t size = 0;
  std::uint32_t crc32 = 0;
  std::uint16_t method = 0;
  std::uint16_t flags = 0;
  std::uint16_t nameLength = 0;
  MediaType type = MediaType::kOther;
  // The file is in the book. A package can list a file that is not.
  bool present : 1 = false;
  // It is part of the run of the book from front to back. A document that
  // is not is one set aside, such as a page of notes that is only reached
  // by a link.
  bool linear : 1 = true;
};
static_assert(sizeof(SpineItem) == sizeof(ZipEntry));

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
// Memory: the caller provides a block. The spine is built at its front, 24
// bytes a document, and the manifest at its back, 16 bytes a file. Both
// stay: the manifest is what SpineIndexOf() looks a path up in. A book of
// 80 files and 50 documents needs under 3 KB; the largest seen, 745 files
// and 394 documents, 21 KB. A book that needs more than the block is
// refused.
//
// Since no name is kept, none is checked: two different names that came to
// one number would be taken for each other. NameHash() says how seldom
// that is. A caller that has to be sure can read an entry's name back
// with ReadZipEntryName().
//
// It is done a piece at a time: Begin(), then Step() until it says there
// is no more. How long the whole takes is the book's to decide, and a book
// may be made to make it long: a package file of megabytes, a ZIP file of
// tens of thousands of entries. How long a step takes is not. A step does
// one of these and no more: looks at kEntriesPerStep entries of the ZIP
// file's directory, or as many as lie in kDirectoryBytesPerStep bytes of
// it; parses 1 KB of a file; makes kComparesPerStep comparisons among the
// manifest's files; or goes through the manifest a few times, which the
// block's size bounds. With files read 4 KB at a time, a step reads five
// blocks from storage at the most, and usually one or none.
//
// Used by one task.
class Package final : private XmlHandler {
 public:
  // What one Step() does at the most (see above).
  static constexpr std::uint32_t kEntriesPerStep = 16;
  static constexpr std::uint32_t kDirectoryBytesPerStep = 4096;
  static constexpr std::uint32_t kComparesPerStep = 16384;

  // `memory` must stay alive, and be left alone, for as long as this is.
  explicit Package(std::span<std::uint8_t> memory) noexcept;

  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;
  ~Package() = default;

  // Gets ready to read the package of the book `archive` has open,
  // dropping whatever was read before. `entries` is the same file opened
  // a second time, `reader` reads an entry from it, and `xml` parses: all
  // four are the caller's, are in use until the reading is over, and must
  // stay alive that long. Reads nothing yet.
  //
  // Returns kOk if there is anything to do.
  [[nodiscard]] PackageStatus Begin(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                                    XmlParser& xml) noexcept;

  // Does the next piece of the reading. Returns true if there is more to
  // do, false once the reading is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more: the whole reading in one
  // call, for a tool or a test, where nothing else is waiting.
  [[nodiscard]] PackageStatus Read(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                                   XmlParser& xml) noexcept;

  // How the reading ended; while it is going on, kOk for as long as
  // nothing has gone wrong. Until it is over the book's documents, cover
  // and tables of contents are not to be had: Spine() is empty, and the
  // rest say there is none.
  PackageStatus Status() const noexcept { return status_; }

  // What the book calls itself, who it says wrote it, and its language as
  // a code ("en", "pt-BR"): the first of each that it gives, with any run
  // of spaces and line ends made one space. Empty if it gives none.
  std::string_view Title() const noexcept { return title_.View(); }
  std::string_view Author() const noexcept { return author_.View(); }
  std::string_view Language() const noexcept { return language_.View(); }
  // What the book is known by to the world, such as an ISBN or a UUID: the
  // identifier the package names as its unique one, if that has anything
  // in it, and otherwise the first that has; spaces as for the title, and
  // no more than kMaxIdentifier bytes of it. Empty if the book gives none.
  // Two books can give the same, and a book need not give one.
  static constexpr std::size_t kMaxIdentifier = 128;
  std::string_view Identifier() const noexcept {
    return uniqueIdentifier_.Empty() ? identifier_.View() : uniqueIdentifier_.View();
  }

  // The path of the package file within the book.
  std::string_view PackagePath() const noexcept { return packagePath_.View(); }

  // The documents of the book, in reading order.
  std::span<const SpineItem> Spine() const noexcept { return {spine_, Reading() ? 0 : spineCount_}; }

  // What SpineIndexOf() gives for a file that is no document of the spine.
  static constexpr std::size_t kNotInSpine = static_cast<std::size_t>(-1);
  // Which document of the spine the file at `path` is: its place in
  // Spine(). `path` is a path within the book, as ResolveBookPath() gives
  // one, which is how a link in the book to one of its documents is
  // followed. If the spine has the file more than once, its first place.
  // kNotInSpine if the spine has it nowhere, or if no book has been read.
  std::size_t SpineIndexOf(std::string_view path) const noexcept;

  // How many of the manifest's files are pictures: JPEG, PNG, GIF or SVG
  // by what the manifest says of them. A picture is known by its place
  // among them, in the manifest's order, from 0. Nothing more is kept for
  // it: the place is counted when it is asked for.
  std::uint32_t Pictures() const noexcept { return Reading() ? 0 : pictureCount_; }

  // What PictureOf() says of a file.
  static constexpr std::size_t kNotPicture = static_cast<std::size_t>(-1);
  struct Picture {
    std::size_t index = kNotPicture;  // Its place among the pictures, or kNotPicture.
    MediaType type = MediaType::kOther;
    bool isCover = false;  // It is the picture Cover() is.
  };
  // Which picture the file at `path` is: a path within the book, as
  // ResolveBookPath() gives one, or the name of an entry of the ZIP file.
  // Where the manifest has the file more than once, it is the first of
  // those that is a picture. Not a picture if the manifest does not have
  // it as one, or if no book has been read.
  Picture PictureOf(std::string_view path) const noexcept;

  // The cover picture; the table of contents as EPUB 3 has it, a document
  // of its own (the "nav"); and as EPUB 2 has it (the "NCX"). Each is null
  // if the package names none, or names a file that is not in the book.
  const ZipEntry* Cover() const noexcept { return Present(cover_) ? &cover_.entry : nullptr; }
  MediaType CoverType() const noexcept { return cover_.type; }
  const ZipEntry* Nav() const noexcept { return Present(nav_) ? &nav_.entry : nullptr; }
  const ZipEntry* Ncx() const noexcept { return Present(ncx_) ? &ncx_.entry : nullptr; }

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
    // The first document of the spine that is this file, or kNoSpineIndex.
    std::uint16_t spineIndex;
  };
  static constexpr std::uint16_t kNoSpineIndex = 0xFFFF;

  // A file found for one of the book's particular purposes.
  struct Found {
    ZipEntry entry;
    bool present = false;
    MediaType type = MediaType::kOther;
    std::uint32_t item = 0xFFFFFFFFU;  // Which item of the manifest it is: kNoItem until it is known.
  };

  // Which piece of the reading comes next.
  enum class Stage : std::uint8_t {
    kIdle,  // None: nothing has begun, or it is over.
    kFindContainer,
    kReadContainer,
    kFindPackage,
    kReadPackage,
    kResolveSpine,
    kResolveRest,
    kFindFiles,
  };
  // The parts of a package file, and what text in its metadata is being
  // kept.
  enum class Section : std::uint8_t { kOther, kMetadata, kManifest, kSpine };
  enum class Capture : std::uint8_t { kNone, kTitle, kAuthor, kLanguage, kIdentifier, kUniqueIdentifier };
  // How a look through some of the directory for a name went.
  enum class Looked : std::uint8_t { kFound, kMore, kMissing, kUnreadable };

  static constexpr std::uint32_t kNoItem = 0xFFFFFFFFU;

  bool Reading() const noexcept { return stage_ != Stage::kIdle; }
  bool Present(const Found& found) const noexcept { return !Reading() && found.present; }

  // What is listened for in container.xml and in the package file, as
  // each is parsed.
  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override;
  void OnEndElement(std::string_view name) noexcept override;
  void OnText(std::string_view text) noexcept override;
  void OnEntity(std::string_view name) noexcept override;
  void StartInContainer(std::string_view local, const XmlAttributes& attributes) noexcept;
  void StartInMetadata(std::string_view local, const XmlAttributes& attributes) noexcept;
  void ManifestItem(const XmlAttributes& attributes) noexcept;
  void SpineReference(const XmlAttributes& attributes) noexcept;
  void OutOfMemory() noexcept;

  void Clear() noexcept;
  // The reading is over, and this is how it went.
  PackageStatus End(PackageStatus status) noexcept;
  // Goes on to another stage; and puts the part of the reading that has
  // ended, if one has, into the trace.
  void MoveTo(Stage stage) noexcept;
  void EndPart() noexcept;
  // Adds a file to the manifest, or a document to the spine. False if
  // there is no room.
  bool AddItem(std::uint64_t idHash, std::uint64_t pathHash, MediaType type, std::uint8_t flags) noexcept;
  bool AddSpine(std::uint64_t idHash, bool linear) noexcept;
  // The item with this id, or kNoItem.
  std::uint32_t ItemWithId(std::uint64_t idHash) const noexcept;
  Item& ItemAt(std::uint32_t index) noexcept { return itemsEnd_[-static_cast<std::ptrdiff_t>(index) - 1]; }
  const Item& ItemAt(std::uint32_t index) const noexcept { return itemsEnd_[-static_cast<std::ptrdiff_t>(index) - 1]; }

  // The next entry of the ZIP file's directory, from where the last was;
  // `gone` gains how far through the directory that went.
  ZipStatus NextEntry(ZipEntry& entry, std::span<char> name, std::uint32_t& gone) noexcept;
  // Looks through the next piece of the directory for the entry called
  // `name`, going round from its end to its start, once.
  Looked LookFor(std::string_view name, ZipEntry& entry) noexcept;
  // Opens `entry` and gets the parser ready to read it. False, with the
  // reading ended, if it cannot.
  bool BeginParsing(const ZipEntry& entry) noexcept;

  // The pieces, one to a stage.
  void FindContainer() noexcept;
  void ReadContainer() noexcept;
  void FindPackage() noexcept;
  void ReadPackageFile() noexcept;
  // Matches the spine to the items of the manifest, some documents at a
  // time; then the cover and the tables of contents.
  void ResolveSpine() noexcept;
  void ResolveRest() noexcept;
  // Goes through some more of the ZIP file's directory for the files that
  // are wanted.
  void FindFiles() noexcept;

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

  // What the reading uses, from Begin() until it is over, and where it
  // has got to.
  ZipArchive* archive_ = nullptr;
  core::BlockFile* entries_ = nullptr;
  ZipEntryReader* reader_ = nullptr;
  XmlParser* xml_ = nullptr;
  Stage stage_ = Stage::kIdle;
  ZipArchive::Cursor cursor_;       // Where in the ZIP file's directory it is,
  std::uint32_t looked_ = 0;        // and how many entries the search in hand has looked at.
  bool triedDecoded_ = false;       // The package file has been looked for with the %XX of its name turned into bytes.
  std::size_t resolved_ = 0;        // Documents of the spine matched to the manifest so far,
  std::size_t kept_ = 0;            // and how many of them it had.
  std::uint32_t pictureCount_ = 0;  // How many of the manifest's files are pictures.
  std::uint32_t wanted_ = 0;        // Files still to be found in the directory.
  std::uint32_t began_ = 0;         // When the reading began, and when the part of it in hand did:
  std::uint32_t partBegan_ = 0;     // for the trace.

  // Where in the file being parsed the parser is. Depths count elements,
  // the root's being 1.
  int depth_ = 0;
  Section section_ = Section::kOther;
  Capture capture_ = Capture::kNone;
  int captureDepth_ = 0;
  std::string_view base_;      // The directory the package file is in.
  bool foundPackage_ = false;  // container.xml has named a file that says it is a package.

  core::FixedText<256> title_;
  core::FixedText<128> author_;
  core::FixedText<16> language_;
  // The first identifier that had anything in it, and the one the package
  // names as its unique one, by the id the package gives for it.
  core::FixedText<kMaxIdentifier> identifier_;
  core::FixedText<kMaxIdentifier> uniqueIdentifier_;
  std::uint64_t uniqueId_ = 0;
  bool hasUniqueId_ = false;
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
