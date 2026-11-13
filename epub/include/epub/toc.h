#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/block_file.h"
#include "core/fixed_text.h"
#include "epub/book_path.h"
#include "epub/package.h"
#include "epub/xml.h"
#include "epub/zip.h"

// A book's table of contents: its parts by name, each with the place in
// the book where the part begins. An EPUB has it in one of two forms, or
// in both. EPUB 3 has a page of XHTML with a list of links in it (the
// "nav"). EPUB 2 has an XML file made for the purpose (the "NCX"). Package
// finds the two files; this reads them.

namespace bn::epub {

// How reading a table of contents has gone.
enum class TocStatus : std::uint8_t {
  kOk = 0,      // Nothing wrong so far; at the end, it was read (see CutShort()).
  kNotStarted,  // Begin() has not been called.
  kNoToc,       // The book has no table of contents of the kind asked for.
  kNotToc,      // The file is not one: it is not XML, or has no list of entries in it.
  kNoMemory,    // The XML parser needs more memory than it was given.
  kUnreadable,  // The ZIP file is damaged where the file is, or storage failed.
  kStopped,     // The handler asked for no more.
};

// The name of a status, for diagnostics: "ok", "no table of contents", ...
const char* TocStatusName(TocStatus status) noexcept;

// Which of a book's tables of contents.
enum class TocKind : std::uint8_t {
  kBest = 0,  // To ask with: the nav if the book has one, and the NCX if not.
  kNav,
  kNcx,
};

// "best", "nav" or "ncx".
const char* TocKindName(TocKind kind) noexcept;

// One entry of a table of contents. Its text is only good while the
// handler that was given it is running.
struct TocEntry {
  // What the entry is called, with every run of spaces and line ends in it
  // made one space. A label longer than TocReader::kMaxLabel bytes is cut
  // at a character, and `labelCut` says so.
  std::string_view label;
  bool labelCut = false;
  // How far in the entry is: 0 for an entry of the list itself, 1 for one
  // under such an entry, and so on.
  std::uint16_t depth = 0;
  // The document the entry points into: its place in Package::Spine().
  // Package::kNotInSpine for an entry that points at nothing, which is a
  // heading for the entries under it, or at something that is not a
  // document of the spine.
  std::size_t document = Package::kNotInSpine;
  // The place within the document: the id of an element there, which is
  // what the link has after its '#'. Empty for the document's start, and
  // for an id longer than TocReader::kMaxFragment bytes.
  std::string_view fragment;
};

// What is told of a table of contents as it is read.
class TocHandler {
 public:
  // Called for each entry, in the order the table of contents has them:
  // an entry before the entries under it. Return false to be told of no
  // more.
  [[nodiscard]] virtual bool OnEntry(const TocEntry& entry) noexcept = 0;

 protected:
  ~TocHandler() = default;  // Never destroyed through the interface.
};

// Reads a table of contents, giving each entry to a handler as it is
// found. Nothing of it is kept, so one of any length costs the same
// memory: this object, which is about 1 KB, and what the XML parser needs.
//
// A book with both kinds is read from its nav unless the NCX is asked for.
// The two often differ, and the nav is the one a book made as EPUB 3 is
// meant to be read by.
//
// What is taken for an entry. In an NCX: each navPoint inside the navMap,
// by the text of its navLabel and the src of its content, nested as the
// navPoints are. In a nav: within the nav element whose type is "toc",
// each link (a) in a list (ol), and each span that stands where a link
// would as a heading with no link, nested as the lists are. Page lists,
// landmarks and anything else in either file are passed over.
//
// A file that stops being XML part way gives the entries that came before
// the mistake, and CutShort() says that it did.
//
// It can be used for one table of contents after another. Used by one
// task.
class TocReader final : private XmlHandler {
 public:
  // The longest label and the longest fragment that are kept whole. No
  // book has been seen with a label of 170 bytes or a fragment of 40.
  static constexpr std::size_t kMaxLabel = 256;
  static constexpr std::size_t kMaxFragment = 128;

  TocReader() noexcept = default;

  TocReader(const TocReader&) = delete;
  TocReader& operator=(const TocReader&) = delete;
  ~TocReader() = default;

  // Gets ready to read a table of contents of the book whose package
  // `package` has read, and to tell `handler` of it. `entries` is the book
  // open for reading entries, `reader` reads the file from it, and `xml`
  // parses: all are the caller's, are in use until the reading is over,
  // and must stay alive that long, as `package` and `handler` must.
  //
  // Returns kOk if there is one to read. Nothing has been told to the
  // handler yet.
  [[nodiscard]] TocStatus Begin(const Package& package, core::BlockFile& entries, ZipEntryReader& reader,
                                XmlParser& xml, TocHandler& handler, TocKind kind = TocKind::kBest) noexcept;

  // Reads the next piece of the file and tells the handler of the entries
  // in it. Returns true if there is more to do, false once the reading is
  // over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  [[nodiscard]] TocStatus Read(const Package& package, core::BlockFile& entries, ZipEntryReader& reader, XmlParser& xml,
                               TocHandler& handler, TocKind kind = TocKind::kBest) noexcept;

  TocStatus Status() const noexcept { return status_; }
  // Which kind is being read, or was: kNav or kNcx. kBest if none was
  // found to read.
  TocKind Kind() const noexcept { return kind_; }
  // How many entries the handler has been given.
  std::uint32_t Entries() const noexcept { return entries_; }
  // The file stopped being XML part way, after at least one entry.
  bool CutShort() const noexcept { return cutShort_; }

 private:
  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override;
  void OnEndElement(std::string_view name) noexcept override;
  void OnText(std::string_view text) noexcept override;
  void OnEntity(std::string_view name) noexcept override;

  void StartInNcx(std::string_view local, const XmlAttributes& attributes) noexcept;
  void StartInNav(std::string_view local, const XmlAttributes& attributes) noexcept;
  // Starts putting an entry together, at this depth of the table.
  void BeginEntry(int depth) noexcept;
  // Works out what a link points at, for the entry being put together.
  void SetTarget(std::string_view reference) noexcept;
  // Gives the handler the entry that has been put together.
  void Emit() noexcept;
  TocStatus End(TocStatus status) noexcept;

  const Package* package_ = nullptr;
  XmlParser* xml_ = nullptr;
  TocHandler* handler_ = nullptr;

  TocStatus status_ = TocStatus::kNotStarted;
  TocKind kind_ = TocKind::kBest;
  bool running_ = false;
  bool cutShort_ = false;
  bool stopped_ = false;
  std::uint32_t entries_ = 0;

  // The path of the file being read, which its links are worked out from.
  core::FixedText<kMaxBookPath> ownPath_;

  // Where in the file the parser is. Depths count elements, the root's
  // being 1, and 0 stands for "not inside one".
  int depth_ = 0;
  int tableDepth_ = 0;  // Of the navMap, or of the nav that is the table of contents.
  bool foundTable_ = false;
  int level_ = 0;         // How many navPoints, or lists, are open inside it.
  int captureDepth_ = 0;  // Of the element whose text is being taken as a label.
  // For a nav: whether the item of the list that is open at each level has
  // given an entry yet, a bit a level.
  std::uint32_t itemHasEntry_ = 0;

  // The entry being put together.
  bool pending_ = false;
  bool hasLabel_ = false;
  bool hasTarget_ = false;
  std::uint16_t pendingDepth_ = 0;
  std::size_t document_ = Package::kNotInSpine;
  core::FixedText<kMaxLabel> label_;
  core::FixedText<kMaxFragment> fragment_;
};

}  // namespace bn::epub
