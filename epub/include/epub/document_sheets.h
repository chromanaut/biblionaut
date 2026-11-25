#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/byte_source.h"
#include "epub/book_files.h"
#include "epub/book_path.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/xml.h"

// What a document asks for: the style sheets it is to be read with.
//
// A document of a book is given the rules it asks for and no others, in
// the order CSS gives them. DocumentSheets finds them: it reads the
// document's head, reads each sheet the head names into the book's table
// of rules (CssRules), once for the whole book, and gives the list of
// sheets that CssCascade::Begin() takes for that document.
//
// What a document asks for, and the order its list is in:
//
//   1. The sheets it links, in the order of the links, each after the
//      sheets it brings in with `@import`, and those after theirs. A `link`
//      element counts if its `rel` has the word `stylesheet` and not the
//      word `alternate`, its `type` is `text/css` or is not given, and it
//      has an `href`. The sheet is the file of the book that the `href`
//      names from the document's folder (ResolveBookPath()), whether the
//      book's manifest lists it or not.
//   2. The rules in its own `<style>` elements, as one sheet, which is
//      kept in the table only until the next document is begun. A `<style>`
//      counts if its `type` is `text/css` or is not given.
//
// An element's own `style` attribute comes over all of those, and is the
// cascade's to read.
//
// A sheet named more than once, by two links or by two sheets that import
// it, stands in the list once, where it was named last: its rules then
// come after everything named before that, as they would if it had been
// read again.
//
// Only the head is read. The document is parsed until its `<body>` opens,
// and the rest of it is not looked at. A document whose first element is
// not `html` has no head, and is given no sheets.
//
// What is left out, each counted by its kind (SheetLeftOut), with nothing
// left out that is not counted:
//
//   - A link or a `<style>` whose `media` is for some other kind of device:
//     the rule is `@media`'s (CssMediaApplies()). So a sheet linked for
//     `amzn-mobi` is not read, and neither is one linked for
//     `(min-device-width: 0px)`: a condition in brackets is never taken,
//     so that what is made of a book does not depend on the device that
//     made it.
//   - A link or an import that names no file of the book, or one the book
//     does not have.
//   - A link after the first kMaxLinks of a document.
//   - An import more than kMaxDepth sheets down from the link, and an
//     import of a sheet that is already on the way to it.
//   - A sheet for which there is no room: the book already has kMaxSheets
//     that are known, or the caller's memory for their names is full, or
//     the table has as many sheets as it holds (CssRules::kMaxSheets). And
//     an import after the first kMaxImports of a book.
//   - A sheet whose file could not be read to its end. The rules read from
//     it before that are kept.
//   - A `<style>` whose text does not fit in the memory it is held in
//     (see the constructor), and an `@import` inside a `<style>`, which is
//     not followed. A `<style>` that the head ends inside is not one, and
//     nothing of it is read; if it was for some other kind of device, that
//     was counted as it opened.
//   - A head that could not be read through, because the document is not
//     XML or its file failed. The sheets named before that are still
//     given. A file that fails is counted unless the parser had told of
//     the `<body>` by then; and the parser may tell of a tag some bytes
//     after reading it (a tag that was cut short where a piece ended is
//     not looked at again until as much again has come), so a file that
//     fails just after its head may be counted too, and what it is given
//     goes by how its bytes came.
//
// Where this differs from what CSS itself has:
//
//   - A `<style>` that stands before a link is still read after the
//     linked sheets. No book of 548 has one.
//   - A `<style>` in the body is not seen: the head is all that is read.
//     Whoever reads the body can count those.
//   - `<?xml-stylesheet?>` is not read. Of 548 books it is only in the SVG
//     pages of one whose spine is SVG.
//   - Text inside a comment is not a `<style>`, whatever it looks like:
//     `<!--[if gte mso 9]><style>...` is a comment, to XML and to this.
//
// The document is opened by its path, through the book's files; or its
// caller opens it and hands it over (Begin() with a source), and then
// only the sheets are opened by their paths, each once for the book.
//
// It is done a step at a time (Step()). One step opens one file of the
// book, or has the XML parser read one piece of the document
// (XmlParser::kChunkBytes), or has the reader of CSS read one piece of a
// sheet (CssReader::kChunkBytes), or does one move of working the list
// out, which looks at no more than kMaxSheets sheets; and never two of
// those. So a step reads kMostBytesPerStep bytes of the book at the most.
//
// The document and the sheets are not trusted: whatever their bytes,
// every document ends with a list, and the counts of what was left out.
//
// It is about 2 KB, and takes the memory for the sheets' names and for
// `<style>` text from its caller. Used by one task at a time.

namespace bn::epub {

// The kinds of thing that are left out, for the count. The text told with
// each is what was left out: a path in the book, or a reference as it was
// written.
enum class SheetLeftOut : std::uint8_t {
  kLinkMedia,        // A link for some other kind of device. The text is its `href`.
  kLinkNotInBook,    // A link to no file of the book, or to one it does not have. Its `href`, or the path.
  kLinkTooMany,      // A link after the first kMaxLinks. The path.
  kImportNotInBook,  // An import of no file of the book, or of one it does not have. As written, or the path.
  kImportTooDeep,    // An import more than kMaxDepth down from a link. The path.
  kImportLoop,       // An import of a sheet that is on the way to it. The path.
  kImportTooMany,    // An import after the first kMaxImports of a book. The path.
  kSheetTooMany,     // A sheet there is no room for. The path, or "style" for a document's own.
  kSheetNotRead,     // A sheet whose file could not be read to its end. The path.
  kStyleMedia,       // A `<style>` for some other kind of device. The text is its `media`.
  kStyleTooLong,     // A `<style>` whose text does not fit. No text.
  kStyleImport,      // An `@import` inside a `<style>`. As written.
  kHeadNotRead,      // A head that could not be read through. The text is why: the XML parser's status.
};
inline constexpr std::size_t kSheetLeftOutKinds = 13;

// "link for another medium", "import too deep", ...
const char* SheetLeftOutName(SheetLeftOut what) noexcept;

// What is told of the sheets as they are found. Text is only good while
// the call is running.
class SheetsListener {
 public:
  // A sheet of the book has been read into the table, to its end or as
  // far as its file could be read: its path, the number the table knows
  // it by, and the reader, which still has its counts of the sheet.
  virtual void OnSheetRead(std::string_view path, std::uint8_t sheet, const CssReader& reader) noexcept = 0;
  // Something was left out.
  virtual void OnLeftOut(SheetLeftOut what, std::string_view text) noexcept = 0;

 protected:
  ~SheetsListener() = default;  // Never destroyed through the interface.
};

// How finding a document's sheets stands.
enum class SheetsStatus : std::uint8_t {
  kOk = 0,      // Nothing wrong so far; at the end, the document has its list.
  kNotStarted,  // Begin() has not been called.
  kNoDocument,  // The book has no such document, or it could not be opened.
};

// "ok", "not started" or "no document".
const char* SheetsStatusName(SheetsStatus status) noexcept;

class DocumentSheets final : private XmlHandler, private CssHandler {
 public:
  // The most links of one document that are followed. A document of 548
  // books has 7.
  static constexpr std::size_t kMaxLinks = 8;
  // How far down from a link imports are followed: a link's imports are
  // one down. Through links that count, the books go one down.
  static constexpr std::size_t kMaxDepth = 4;
  // The most sheets of one book that are known, read or found not to be
  // there. A book has 18.
  static constexpr std::size_t kMaxSheets = CssRules::kMaxSheets;
  // The most imports that are remembered among all a book's sheets. A
  // sheet has 2.
  static constexpr std::size_t kMaxImports = 64;
  // What a caller is advised to give: for the names of a book's sheets
  // (the longest in the books is 74 bytes, and a book has 18), and for a
  // document's `<style>` text (the most in one head is 2.0 KB).
  static constexpr std::size_t kNamesBytes = 2048;
  static constexpr std::size_t kStyleBytes = 4096;
  // The most of the book that one Step() reads.
  static constexpr std::size_t kMostBytesPerStep =
      XmlParser::kChunkBytes > CssReader::kChunkBytes ? XmlParser::kChunkBytes : CssReader::kChunkBytes;

  // The book's files, the parser and the reader to read them with, and the
  // table the rules go into: all must stay alive as long as this does, and
  // be left alone while a document's sheets are being found. The table is
  // this object's to add to: nothing else may add a sheet to it, or take
  // it back, from BeginBook() on.
  //
  // `names` is for the paths of the book's sheets, and `style` for the
  // text of one document's `<style>` elements, each with a byte to end
  // it: a `<style>` that does not fit in what is left is left out whole.
  DocumentSheets(BookFiles& files, XmlParser& parser, CssReader& reader, CssRules& rules, std::span<char> names,
                 std::span<std::uint8_t> style) noexcept;

  DocumentSheets(const DocumentSheets&) = delete;
  DocumentSheets& operator=(const DocumentSheets&) = delete;
  ~DocumentSheets() = default;

  // Who is told of each sheet read and each thing left out; null for
  // nobody. Not while a document's sheets are being found.
  void SetListener(SheetsListener* listener) noexcept { listener_ = listener; }

  // A book begins: every sheet is forgotten, the table is cleared, and the
  // counts start again.
  void BeginBook() noexcept;

  // Gets ready to find the sheets of the document at `path`, a path from
  // the top of the book as BookFiles takes it. Whatever was being found is
  // dropped, and the last document's own `<style>` rules are taken out of
  // the table, so its list is no longer good. Reads nothing yet. A path
  // that is empty, or longer than kMaxBookPath, is no document's: nothing
  // is opened for it, and it ends as kNoDocument.
  void Begin(std::string_view path) noexcept;

  // The same for a document its caller has opened already: `source` gives
  // the document from its first byte, and `path` says only where its links
  // are from. Nothing is opened for the document, so it does not have to
  // be found in the book by its name, which whoever has the book's package
  // need not do: the package has where each document is. `source` is read
  // only as far as the head goes, and never after the first step that
  // opens a sheet, so it may be the one reader the book's files are opened
  // with; it must stay good until then, or until the next Begin().
  void Begin(std::string_view path, core::ByteSource& source) noexcept;

  // Does the next step. Returns true if there is more to do, false once
  // the document has its list or could not be opened: Status() says which.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  SheetsStatus Find(std::string_view path) noexcept;
  SheetsStatus Find(std::string_view path, core::ByteSource& source) noexcept;

  SheetsStatus Status() const noexcept { return status_; }
  // The document has its list.
  bool Finished() const noexcept { return phase_ == Phase::kDone && status_ == SheetsStatus::kOk; }

  // The document's list, for CssCascade::Begin(): the rules of the first
  // apply first. Good until the next Begin() or BeginBook().
  std::span<const std::uint8_t> Sheets() const noexcept { return {list_.data(), listed_}; }
  // The path of the sheet the table knows by that number; nothing for the
  // document's own, and for a number that is no sheet's.
  std::string_view PathOf(std::uint8_t sheet) const noexcept;
  // Whether the document's list ends with a sheet of its own `<style>`
  // rules.
  bool HasOwnStyle() const noexcept { return ownStyle_; }

  // How many things of each kind were left out since BeginBook().
  std::uint32_t LeftOut(SheetLeftOut what) const noexcept { return leftOut_[static_cast<std::size_t>(what)]; }
  // The sheets of the book that have been read into the table, and how
  // much of `names` is in use.
  std::size_t SheetsRead() const noexcept { return sheetsRead_; }
  std::size_t NamesBytes() const noexcept { return namesUsed_; }

 private:
  // What is known of one sheet of the book.
  enum class Known : std::uint8_t {
    kUnread,   // Named, and not yet read.
    kRead,     // Read into the table, with its imports remembered.
    kMissing,  // The book has no such file, or it could not be opened.
    kNowhere,  // Not a file of the book at all: the name is the reference as written.
  };
  struct Sheet {
    std::uint16_t nameAt = 0;
    std::uint16_t nameLength = 0;
    Known known = Known::kUnread;
    std::uint8_t number = 0;       // In the table, once read.
    std::uint8_t firstImport = 0;  // Its imports, in imports_.
    std::uint8_t imports = 0;
  };
  // A sheet on the way down from a link, and which of its imports is next.
  struct Level {
    std::uint8_t sheet = 0;
    std::uint8_t next = 0;
  };
  enum class Phase : std::uint8_t {
    kIdle,      // No document.
    kOpening,   // The document is to be opened.
    kHead,      // Its head is being read.
    kWalking,   // The list is being worked out from the links.
    kSheet,     // A sheet of the book is being read.
    kOwnStyle,  // The document's own `<style>` text is being read.
    kDone,
  };
  static constexpr std::uint8_t kNone = 0xFF;

  // What the XML parser tells of the head.
  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override;
  void OnEndElement(std::string_view name) noexcept override;
  void OnText(std::string_view text) noexcept override;
  void OnEntity(std::string_view name) noexcept override;
  // What the reader of CSS tells of a sheet: passed on to the table, but
  // for the imports.
  void OnRuleBegin() noexcept override;
  void OnSelector(const CssSelector& selector) noexcept override;
  void OnDeclaration(const CssDeclaration& declaration) noexcept override;
  void OnRuleEnd() noexcept override;
  void OnImport(std::string_view url) noexcept override;
  void OnDropped(CssDropped dropped, std::string_view text) noexcept override;

  void Link(const XmlAttributes& attributes) noexcept;
  void StepHead() noexcept;
  void StepWalk() noexcept;
  void StepSheet() noexcept;
  void StepOwnStyle() noexcept;
  // Opens the sheet to be read and begins it in the table; or finds it
  // missing, or with no room in the table.
  void OpenSheet(std::uint8_t sheet) noexcept;
  // Leaves a sheet half read as if it had not been begun.
  void DropSheetInHand() noexcept;
  // The next of the document's `<style>` texts, to be read: false when
  // there are no more.
  bool NextOwnStyle() noexcept;
  void Finish() noexcept;

  // The sheet of that name, known already or made known now; kNone if
  // there is no room for another.
  std::uint8_t Know(std::string_view name, Known as) noexcept;
  std::string_view NameOf(const Sheet& sheet) const noexcept;
  // Puts a sheet at the end of the list, and takes it out of where it was.
  void List(std::uint8_t number) noexcept;
  void Leave(SheetLeftOut what, std::string_view text) noexcept;

  BookFiles& files_;
  XmlParser& parser_;
  CssReader& reader_;
  CssRules& rules_;
  std::span<char> names_;
  std::span<std::uint8_t> style_;
  SheetsListener* listener_ = nullptr;

  SheetsStatus status_ = SheetsStatus::kNotStarted;
  Phase phase_ = Phase::kIdle;
  std::uint32_t began_ = 0;  // Trace time the document was begun.

  // The book's sheets.
  std::array<Sheet, kMaxSheets> sheets_{};
  std::size_t known_ = 0;
  std::size_t sheetsRead_ = 0;
  std::size_t namesUsed_ = 0;
  std::array<std::uint8_t, kMaxImports> imports_{};
  std::size_t imported_ = 0;
  std::array<std::uint32_t, kSheetLeftOutKinds> leftOut_{};

  // The document in hand: its folder, what its head named, and its list.
  std::array<char, kMaxBookPath> document_{};
  std::size_t documentLength_ = 0;
  // The document as its caller opened it, until its reading is begun; null
  // if it is this object's to open.
  core::ByteSource* given_ = nullptr;
  std::array<char, kMaxBookPath> path_{};  // A reference being worked out.
  bool rootSeen_ = false;
  std::array<std::uint8_t, kMaxLinks> links_{};
  std::size_t linked_ = 0;
  std::size_t linkAt_ = 0;
  std::array<std::uint8_t, CssRules::kMaxSheets> list_{};
  std::size_t listed_ = 0;

  // The way down from the link in hand, and a sheet waiting to be gone
  // into: kNone for none.
  std::array<Level, kMaxDepth + 1> way_{};
  std::size_t depth_ = 0;
  std::uint8_t pending_ = kNone;
  bool pendingIsImport_ = false;

  // The sheet being read, and where the table stood before it.
  std::uint8_t reading_ = kNone;
  CssRules::Place sheetPlace_{};

  // The document's own `<style>` text: how much of style_ holds it, the
  // element the parser is in (how deep in it, whether its text is taken,
  // where that began, whether it fits), and the text being read.
  std::size_t styleUsed_ = 0;
  std::size_t styleDepth_ = 0;
  bool styleTaken_ = false;
  std::size_t styleBegan_ = 0;
  bool styleFits_ = true;
  std::size_t styleAt_ = 0;
  bool styleReading_ = false;
  core::MemoryByteSource styleSource_{{}};
  bool ownStyle_ = false;       // The list ends with the document's own sheet,
  bool ownStyleKept_ = false;   // which is in the table from ownStylePlace_ on.
  bool ownStyleBegun_ = false;  // Its sheet has been begun in the table.
  std::uint8_t ownStyleNumber_ = 0;
  CssRules::Place ownStylePlace_{};
};

}  // namespace bn::epub
