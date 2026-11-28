#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "epub/book_bundle.h"
#include "epub/book_files.h"
#include "epub/book_path.h"
#include "epub/bundle.h"
#include "epub/css_rules.h"
#include "epub/document_blocks.h"
#include "epub/document_sheets.h"
#include "epub/segment.h"
#include "epub/style_stack.h"
#include "epub/xml.h"
#include "epub/zip.h"

// Importing a chapter: a document of a book made into a chapter's segment
// (segment.h) at the end of the book's bundle.
//
// The document is read twice from its start: its head first, by a
// DocumentSheets, for the style sheets it asks for; then all of it, with a
// cascade over those sheets and a DocumentBlocks, which says where each
// block of text begins and ends and with what record. What they say is
// given to a SegmentWriter with the text.
//
// What becomes of the text:
//
//   - White space (a space, a tab, a line end, a return) is combined as
//     CSS has it: any run of it within a block is one space, and none is
//     kept at a block's start, at its end, or to either side of a line's
//     end. The space is written when something is written after it in its
//     line, and set as the first of the white space it stands for was.
//   - Where white space is as written (`pre`), text is kept as it is, but
//     for the white space before a block's first text: of that, only what
//     follows the last line end is kept, kMostHeld bytes of it at the
//     most, set as the text is. White space that is kept is something
//     written, for the rule above: white space beside it that is combined
//     becomes a space.
//   - `br` is a line feed in the text. One that ends a block's text ends
//     its last line and adds none, as CSS has it.
//   - A character written by a name HTML gives it (`&nbsp;`) is that
//     character (html_entities.h); a name that is none is left out and
//     counted.
//   - Nothing else is changed: a soft hyphen stays, and no character is
//     composed with another.
//
// What becomes of the rest:
//
//   - How a run of text is set is written where it changes: its size as so
//     much of its block's, and its language as the book's own where its
//     tag is the one the caller gives as the book's, in letters of either
//     case. The chapter's languages are in the order text in them is
//     first written. A bit of a run is the book's where it is set and a
//     sheet of the book set it: of a sheet saying what text is not (a
//     paragraph not bold, a heading not bold) nothing is kept, so text
//     that an element made bold and the book made plain is plain text,
//     and no more is known of it.
//   - An `id` of any element, and the `name` of an `a` that has no `id`,
//     is an anchor where the element opens: before white space that has
//     not yet become a space, or that is held; and at the start of a block
//     whose first characters may yet be a capital that is dropped.
//   - A rule (`hr`) has nothing in it: what a document writes inside one
//     is left out.
//   - A picture (`img`) is written as one the book does not have, with
//     the text that stands for it (`alt`): nothing yet says which of the
//     book's pictures it is. Nor is a link written, nor where a cell is in
//     its table.
//   - A capital that is dropped. A block whose first letter has a style of
//     its own (document_blocks.h) has that letter, and the one or two
//     quotation marks before it if they are set as it is, written as a run
//     in that style; and if the rule floats the letter the block says how
//     many characters that is (kBreakDropped). A block that begins with
//     floated text says so too, if the floated text is no more than one
//     letter with one or two quotation marks before it, all set alike, or
//     one picture whose `alt` is kMostFirst bytes at the most; a block
//     that has both is taken that way, and not by its first letter's
//     rule. A letter here is one of the Latin, Greek or Cyrillic alphabets
//     (U+0041 to U+024F, U+0370 to U+052F and U+1E00 to U+1FFF, but for
//     the signs among them), or for a first letter's style a digit as
//     well.
//
// Where a block began in the document is where the last tag before its
// first text began, an element's opening or its closing: the tag of the
// `hr`, the `br` or the `img` itself, for a block one of those begins.
//
// A document that goes wrong part way, because it is not XML from there on
// or cannot be read from the book, is a chapter cut short: what came
// before is kept, as far as the parser told of it, which looks at a
// document a piece at a time. One of which nothing could be read is
// refused.
//
// What the chapter's record counts as left out is what the writer could
// not keep, a name that is no character's, white space there was no room
// to hold, and what the StyleStack and the cascade left out for being too
// deep or one language too many.

namespace bn::epub {

// How importing a chapter stands.
enum class ImportStatus : std::uint8_t {
  kOk = 0,       // Nothing wrong so far; at the end, Chapter() is to be committed.
  kStoreFailed,  // Storage did not do what it was asked, in writing the bundle.
  kTooBig,       // The segment would not fit in a file.
  kMisused,      // Memory of the wrong size, or asked out of turn.
};

// "ok", "store failed", "too big", "misused".
const char* ImportStatusName(ImportStatus status) noexcept;

// Why a chapter was cut short or refused: what its record gives as the
// reason.
enum class ImportReason : std::uint8_t {
  kNone = 0,
  kNoFile,      // The book has no such file, or it could not be opened.
  kUnreadable,  // The file could not be read to its end: it is damaged in the book.
  kNotXml,      // It is not XML, from where the chapter ends.
  kNoMemory,    // The parser was given too little memory for it.
};

inline constexpr std::size_t kImportReasons = 5;

// "none", "no file", "unreadable", "not XML", "no memory".
const char* ImportReasonName(ImportReason reason) noexcept;

// What an importer did not take as written, beside what the writer and the
// StyleStack count.
enum class ImportLeftOut : std::uint8_t {
  kEntity,  // A character written by a name that is none of HTML's.
  kHeld,    // White space as written before a block's first text that there was no room to hold.
};

inline constexpr std::size_t kImportLeftOutKinds = 2;

// The document a chapter is imported from.
class ChapterDocument {
 public:
  // Opens the document, to be read from its first byte, and puts the path
  // of the file in the book into `path`, as ZipBookFiles::OpenAt() does:
  // `length` is how much of `path` that is. Null if it cannot be opened.
  // It is asked twice for one chapter. The source is good until it is
  // asked again, or a sheet of the book is opened.
  [[nodiscard]] virtual core::ByteSource* Open(std::span<char> path, std::size_t& length) noexcept = 0;

 protected:
  ~ChapterDocument() = default;  // Never destroyed through the interface.
};

// A document of a book that is a ZIP file, by its place in the file, as a
// bundle's table of the spine has it.
class ZipChapterDocument final : public ChapterDocument {
 public:
  ZipChapterDocument(ZipBookFiles& files, const ZipEntry& entry) noexcept : files_(files), entry_(entry) {}

  [[nodiscard]] core::ByteSource* Open(std::span<char> path, std::size_t& length) noexcept override {
    return files_.OpenAt(entry_, path, length);
  }

 private:
  ZipBookFiles& files_;
  ZipEntry entry_;
};

// The versions of importing's rules, for a bundle's header page
// (BundleBook): the first is raised by a change that can move text within
// a paragraph or change which paragraphs there are, and the second by one
// that makes importing better without moving any.
inline constexpr std::uint16_t kImportTextRules = 1;
inline constexpr std::uint16_t kImportVersion = 1;

// What becomes of a document of the spine that is not imported at all, by
// its record in the bundle's table: one whose file the book does not have
// is refused, and one that is not a document of text is said to be none.
// True if that is so of this one, and `chapter` is then its record, to be
// committed; false if it is to be imported.
[[nodiscard]] bool ChapterWithoutDocument(const BundleFile& file, BundleChapter& chapter) noexcept;

// Imports one chapter.
//
//   Begin(document, sheets, parser, file, bundle.DataEnd(), language)
//   Step() until it says there is no more
//   bundle.Commit(chapter, importer.Chapter())
//
// The sheets' finder is the book's: it keeps what it has read of the
// book's sheets from one chapter to the next, so its BeginBook() is the
// caller's to call, once for a book. The parser is the one the finder
// reads heads with.
//
// Its memory is kWorkBytes the caller gives. None of it grows with the
// chapter or with a paragraph.
//
// A Step() opens the document, or has the finder of sheets do a step, or
// has the parser read one piece of the document, and never two of those:
// so it reads kMostBytesPerStep of the book at the most. And it writes
// kMostPagesPerStep pages of the bundle at the most: a piece of the
// document is 1,024 bytes, which can begin a block with every three
// (`<hr/>a` is two); a rule and a block of one character are 235 bytes of
// the segment between them, with the longest records a block may have and
// with their records in the index; that is 40 KB, which is ten pages, and
// one more for what the step before left of a page.
//
// Used by one task.
class ChapterImporter final : private XmlHandler {
 public:
  // How many open elements are kept.
  static constexpr std::size_t kLevels = 32;
  static constexpr std::size_t kCascadeBytes = 2048;
  static constexpr std::size_t kBlocksBytes = StyleStack::kLanguagesBytes + (kLevels * DocumentBlocks::kLevelBytes);
  // What the caller must give: for the segment's writer, the cascade, the
  // blocks, and the document's path.
  static constexpr std::size_t kWorkBytes = kSegmentWriterBytes + kCascadeBytes + kBlocksBytes + kMaxBookPath;
  // The most white space as written that is held before a block's first
  // text, until that text comes.
  static constexpr std::size_t kMostHeld = 64;
  // What a block that may begin with a dropped capital waits with: its
  // first characters, or the text that stands for its picture.
  static constexpr std::size_t kMostFirst = 64;
  static constexpr std::size_t kMostBytesPerStep = DocumentSheets::kMostBytesPerStep;
  static constexpr std::size_t kMostPagesPerStep = 11;

  // `rules` is the table the book's sheets are read into, and `work`
  // kWorkBytes that begin at a multiple of four: both the caller's, alive
  // and left alone for as long as this is in use.
  ChapterImporter(const CssRules& rules, std::span<std::uint8_t> work) noexcept;

  ChapterImporter(const ChapterImporter&) = delete;
  ChapterImporter& operator=(const ChapterImporter&) = delete;
  ~ChapterImporter() = default;

  // Gets ready to import `document` into a segment at `offset` of `file`,
  // which is where the bundle's segments end. `sheets` finds the
  // document's style sheets, into the table this was made with, and
  // `parser` reads the document. All are the caller's, in use until the
  // importing is over. `language` is the tag of the book's own language,
  // as its package gave it, which may be none: it is copied. Reads and
  // writes nothing yet.
  [[nodiscard]] ImportStatus Begin(ChapterDocument& document, DocumentSheets& sheets, XmlParser& parser,
                                   core::BlockFile& file, std::uint32_t offset, std::string_view language) noexcept;

  // Does the next piece. Returns true if there is more to do, false once
  // the importing is over: Status() says how it ended, and if that is kOk,
  // Chapter() is the chapter's record.
  [[nodiscard]] bool Step() noexcept;
  ImportStatus Status() const noexcept { return status_; }

  // The chapter's record, to be committed: whole, cut short, or refused,
  // with the reason.
  const BundleChapter& Chapter() const noexcept { return chapter_; }
  ImportReason Reason() const noexcept { return reason_; }

  // The path of the document in the book, once it has been opened.
  std::string_view Path() const noexcept { return {path_.data(), pathLength_}; }
  // What was not taken as written.
  std::uint32_t LeftOut(ImportLeftOut kind) const noexcept { return hand_.leftOut[static_cast<std::size_t>(kind)]; }
  // The blocks of the chapter so far, and what finds them: for its counts.
  const DocumentBlocks& Blocks() const noexcept { return blocks_; }
  const SegmentWriter& Writer() const noexcept { return writer_; }
  // Blocks that say they begin with a capital that is dropped.
  std::uint32_t Dropped() const noexcept { return hand_.dropped; }
  // Elements the cascade could not keep, for being too deep.
  std::uint32_t CascadeTooDeep() const noexcept { return cascade_.TooDeep(); }

 private:
  enum class Stage : std::uint8_t {
    kIdle,
    kSheets,     // The document's sheets are being found.
    kOpen,       // The document is to be opened again, to be read through.
    kDocument,   // It is being read.
    kEndBlocks,  // The blocks are ended: the writer's last pages.
    kFinish,     // The directory.
    kDone,
  };
  // A block that DocumentBlocks has begun and the writer has not: it waits
  // for its first characters, which say whether it begins with a capital
  // that is dropped.
  enum class Waiting : std::uint8_t {
    kNothing,
    kLetter,   // For the first letter, which has a style of its own.
    kFloated,  // For the end of the floated text it begins with.
  };

  // What is in hand of the chapter being imported: begun again, all of
  // it, with each chapter.
  struct Hand {
    std::array<std::uint32_t, kImportLeftOutKinds> leftOut{};
    std::uint32_t dropped = 0;

    // The block in hand, as the writer has it.
    bool inBlock = false;                // The writer has a block begun.
    bool atStart = true;                 // Nothing has been written of the block's line yet.
    bool fresh = true;                   // Nothing has been written of the block yet.
    bool spaced = false;                 // White space has been met that may become a space,
    RunStyle spaceStyle;                 // set as this.
    RunStyle run;                        // How the writer was last told text is set.
    std::uint16_t size = 1000;           // The block's size, which a run's is so much of.
    std::array<char, kMostHeld> held{};  // White space as written, not yet known to be kept.
    std::size_t heldFill = 0;
    std::uint32_t heldOver = 0;  // How much more there was no room to hold.
    // One more than the number the writer gives each of the StyleStack's
    // languages; nothing for one not asked for yet.
    std::array<std::uint8_t, StyleStack::kMaxLanguages + 1> languages{};

    // Where the last tag began, and the last before the block in hand.
    std::uint32_t tag = 0;
    std::uint32_t began = 0;

    // The block that waits.
    Waiting waiting = Waiting::kNothing;
    BlockRecord record;
    std::array<char, kMostFirst> first{};  // Its first characters,
    std::size_t firstFill = 0;
    unsigned firstCount = 0;    // how many they are,
    bool firstLetter = false;   // whether the last of them is a letter,
    RunStyle firstRun;          // and how they are set;
    bool firstPicture = false;  // or a picture is its first, and that is the text that stands for it.
  };

  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override;
  void OnEndElement(std::string_view name) noexcept override;
  void OnText(std::string_view text) noexcept override;
  void OnEntity(std::string_view name) noexcept override;

  // What DocumentBlocks said of a call, done to the segment: a block
  // ended; one began.
  void Ended() noexcept;
  void Began() noexcept;
  // Text of the document.
  void Told(std::string_view text) noexcept;
  // Text that is part of a block, where white space is combined and where
  // it is as written.
  void Collapsed(std::string_view text) noexcept;
  void AsWritten(std::string_view text) noexcept;
  // White space as written, before a block's first text: held.
  void Hold(char blank) noexcept;
  // Text of the block in hand, set as `run`, with what waited to be
  // written before it; and the text alone, to the writer or to the block
  // that waits.
  void Put(const RunStyle& run, std::string_view text) noexcept;
  void Write(const RunStyle& run, std::string_view text) noexcept;
  void PutPicture(const XmlAttributes& attributes) noexcept;
  // The block in hand is a rule.
  bool InRule() const noexcept { return hand_.inBlock && hand_.record.kind == BlockKind::kRule; }
  // The block that waits is begun in the writer, with so many of its
  // first characters dropped, and what it waited with is written.
  void Settle(unsigned dropped, bool letter) noexcept;
  void SettleFloated() noexcept;
  BlockStyle StyleOf(const BlockRecord& record, unsigned dropped) const noexcept;
  // How text is set, with the StyleStack's number for its language; and
  // a style item written for it, with the writer's.
  RunStyle RunOf(const TextStyle& text) const noexcept;
  bool Emit(const RunStyle& run) noexcept;
  // The writer said how something went.
  bool Wrote(SegmentStatus status) noexcept;
  bool End(ImportStatus status) noexcept;
  bool StepSheets() noexcept;
  bool StepOpen() noexcept;
  bool StepDocument() noexcept;
  void Refuse(ImportReason reason) noexcept;

  std::span<char> path_;
  std::size_t pathLength_ = 0;
  CssCascade cascade_;
  DocumentBlocks blocks_;
  SegmentWriter writer_;

  ChapterDocument* document_ = nullptr;
  DocumentSheets* sheets_ = nullptr;
  XmlParser* parser_ = nullptr;
  core::BlockFile* file_ = nullptr;
  std::uint32_t offset_ = 0;

  Stage stage_ = Stage::kIdle;
  bool sheetsBegun_ = false;  // The document has been opened for its head.
  ImportStatus status_ = ImportStatus::kMisused;
  ImportReason reason_ = ImportReason::kNone;
  ChapterState state_ = ChapterState::kWhole;
  BundleChapter chapter_;
  std::array<char, kSegmentMostLanguage> bookLanguage_{};
  std::size_t bookLanguageLength_ = 0;
  Hand hand_;
};

}  // namespace bn::epub
