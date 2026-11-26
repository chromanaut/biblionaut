#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "epub/bundle.h"

// A chapter's segment: what a chapter of a book is, once imported, as bytes
// in the book's bundle (epub/bundle.h), with what writes one and what reads
// one.
//
// A chapter is blocks, one after another in the document's order: a
// paragraph, a heading, an item of a list, a picture standing by itself, a
// rule, a cell of a table. No block is inside another, and each says of
// itself all that setting it needs, so reading can begin at any block with
// nothing before it read.
//
//   blocks ...
//      an index page after every 256 blocks
//      an anchor page whenever one is full
//   ... more blocks ...
//   the last index page and the last anchor page, as full as they are
//   the directory
//
// Everything before the directory is records, each begun by four bytes: a
// kind, a byte the kind may use, and a length in two bytes.
//
//   A block's head     the block's kind; a heading's level; the length of
//                      its style record, which follows. Then its items.
//   An item            its kind; a byte for the kind; the length of what
//                      follows, kSegmentMostItem at the most. The last item
//                      of a block is its end, which has how many characters
//                      the block is and the checksum of the block from its
//                      first byte to the one before that checksum.
//   A page             kSegmentIndexPage or kSegmentAnchorPage; nothing; the
//                      length of the page, which follows. It lies between
//                      two blocks or between two items of one, is no part
//                      of the block or of its checksum, and whoever goes
//                      from block to block passes over it.
//
// An index page is a record of 16 bytes for each of up to 256 blocks: where
// the block begins in the segment, how many characters the chapter has
// before it, where in the source document whatever began it began, its
// kind and its bits for breaking, and two bytes of nothing. The first
// 65,536 blocks are indexed. Those after are written and are read by going
// on from the last that is.
//
// An anchor page is the ids of the document, in the order they were met:
// for each the length of its name in a byte, the name, the block it is in
// (two bytes) and how many characters into the block (four). An id on what
// makes no block is at the start of the block that follows, and one after
// the last block is at the chapter's end: the block it names is one past
// the last. There are 128 pages at the most.
//
// The directory, at the end:
//
//   2, 2, 2, 2   how many index pages, anchor pages, pictures, languages
//   4, 4         the chapter's blocks and characters
//   4            the source document's length
//   12 each      for each index page: where it is, the chapter's
//                characters before its first block, its checksum
//   12 each      for each anchor page: where it is (4), how long (2), how
//                many ids (2), its checksum
//   8 each       for each picture the chapter shows, in the order of their
//                numbers: its number (2), its width and height in pixels
//                (2, 2), whether they could be read (1), nothing (1)
//   the rest     the tags of the languages the text names, each a length
//                in a byte and its bytes
//
// The chapter's record in the bundle says where the directory is and has
// its checksum; the directory has the checksum of each page; and a block
// has its own. So everything read is checked by something checked before
// it, with no pass over the segment.
//
// Characters are what a place in a chapter is counted in: the bytes of its
// text, one for each picture, and one for a block that has neither.
//
// Numbers are little-endian. A checksum is a CRC-32 (core/checksum.h).

namespace bn::epub {

// How something asked of a segment's writer or reader ended.
enum class SegmentStatus : std::uint8_t {
  kOk = 0,
  kEnd,          // There is no more: no block after the last.
  kMore,         // Something that is not what was asked for was passed over: ask again.
  kDamaged,      // It fails a checksum, or says of itself what cannot be.
  kStoreFailed,  // Storage did not do what it was asked.
  kTooBig,       // The segment would not fit in a file.
  kMisused,      // Asked for what cannot be, or out of turn.
};

// The name of a status, for diagnostics: "ok", "end", ...
const char* SegmentStatusName(SegmentStatus status) noexcept;

inline constexpr std::size_t kSegmentBlocksInPage = 256;
inline constexpr std::size_t kSegmentIndexPages = 256;
inline constexpr std::size_t kSegmentIndexedBlocks = kSegmentBlocksInPage * kSegmentIndexPages;
inline constexpr std::size_t kSegmentAnchorPages = 128;
// The longest name of an id, and of a link's fragment; and the longest
// text that stands for a picture.
inline constexpr std::size_t kSegmentMostName = 255;
// The most pictures a chapter's directory has sizes for.
inline constexpr std::size_t kSegmentPictures = 512;
// The most languages a chapter's text can name, and the longest tag.
inline constexpr std::size_t kSegmentLanguages = 15;
inline constexpr std::size_t kSegmentMostLanguage = 23;
// The most bytes an item has after its first four.
inline constexpr std::size_t kSegmentMostItem = 4088;
// The number of a picture the book does not have.
inline constexpr std::uint16_t kSegmentNoPicture = 65535;

// The kinds of record that are pages.
inline constexpr std::uint8_t kSegmentIndexPage = 0xF0;
inline constexpr std::uint8_t kSegmentAnchorPage = 0xF1;

// The kinds of block. A reader gives a block's kind as the number the
// segment has, which may be one that is not here.
enum class BlockKind : std::uint8_t {
  kParagraph = 1,
  kHeading,  // Its level is 1 to 6.
  kListItem,
  kPicture,  // A picture standing by itself.
  kRule,
  kTableCell,
};

// "paragraph", "heading", ...; "?" for a number that is no kind.
const char* BlockKindName(std::uint8_t kind) noexcept;

// A length: three amounts that are added together. An amount in em is
// turned into the first when a block is written, so that nothing kept
// depends on the reader's font or on the page.
struct SegmentLength {
  std::int16_t em = 0;       // 64ths of the reader's text size.
  std::int16_t px = 0;       // 8ths of a CSS pixel.
  std::int16_t percent = 0;  // 64ths of a hundredth of the page's text width.

  bool operator==(const SegmentLength& other) const noexcept = default;
};

// The fields a block's style record may have, as the bits of its masks.
inline constexpr std::uint16_t kBlockAlign = 1U << 0U;
inline constexpr std::uint16_t kBlockDirection = 1U << 1U;
inline constexpr std::uint16_t kBlockSize = 1U << 2U;
inline constexpr std::uint16_t kBlockLineHeight = 1U << 3U;
inline constexpr std::uint16_t kBlockIndent = 1U << 4U;
inline constexpr std::uint16_t kBlockMarginLeft = 1U << 5U;
inline constexpr std::uint16_t kBlockMarginRight = 1U << 6U;
inline constexpr std::uint16_t kBlockSpaceBefore = 1U << 7U;
inline constexpr std::uint16_t kBlockSpaceAfter = 1U << 8U;
inline constexpr std::uint16_t kBlockBreaking = 1U << 9U;
inline constexpr std::uint16_t kBlockDepths = 1U << 10U;
inline constexpr std::uint16_t kBlockMarker = 1U << 11U;
inline constexpr std::uint16_t kBlockTable = 1U << 12U;
inline constexpr std::uint16_t kBlockWidth = 1U << 13U;
inline constexpr std::uint16_t kBlockHeight = 1U << 14U;
inline constexpr std::uint16_t kBlockFields = 0x7FFFU;

// The bits of "breaking and the rest". The low byte is what a block's
// record in the index has, so that a page can be chosen without reading
// the block.
inline constexpr std::uint16_t kBreakPageBefore = 1U << 0U;
inline constexpr std::uint16_t kBreakKeepWithNext = 1U << 1U;
inline constexpr std::uint16_t kBreakKeepTogether = 1U << 2U;
inline constexpr std::uint16_t kBreakSpaceAsWritten = 1U << 3U;  // `pre`.
inline constexpr std::uint16_t kBreakFloated = 1U << 4U;
inline constexpr std::uint16_t kBreakNote = 1U << 5U;     // It is a note's text.
inline constexpr std::uint16_t kBreakCentred = 1U << 6U;  // By margins that are `auto`.

// What a block's style record says. Only the fields whose bits are in
// `has` are in the record, and the others here are as nothing said; a bit
// of `fromBook` says the field came from the book's own style sheets and
// not from what the element is.
struct BlockStyle {
  std::uint16_t has = 0;
  std::uint16_t fromBook = 0;
  std::uint8_t align = 0;      // 0 left, 1 right, 2 centre, 3 justified, 4 start, 5 end.
  std::uint8_t direction = 0;  // 0 left to right, 1 right to left.
  std::uint16_t size = 1000;   // Of the reader's text size, in thousandths.
  // The height of a line: a multiple of the text's size in thousandths,
  // or, where that is 0, a length.
  std::uint16_t lineMultiple = 0;
  SegmentLength lineHeight;
  SegmentLength indent;  // Of the first line.
  // Every enclosing element's margin and padding on that side, added up.
  SegmentLength marginLeft;
  SegmentLength marginRight;
  SegmentLength spaceBefore;
  SegmentLength spaceAfter;
  std::uint16_t breaking = 0;  // kBreakPageBefore and the rest.
  std::uint8_t quoteDepth = 0;
  std::uint8_t listDepth = 0;
  std::uint8_t marker = 0;  // What stands before an item of a list: a number that means what whoever imports says.
  std::uint32_t markerNumber = 0;
  // Which table of the chapter, the row and the column, how many of each
  // the cell spans, whether it is a header cell (1), how deep in tables.
  std::uint16_t table = 0;
  std::uint16_t row = 0;
  std::uint16_t column = 0;
  std::uint8_t rowSpan = 0;
  std::uint8_t columnSpan = 0;
  std::uint8_t tableBits = 0;
  std::uint8_t tableDepth = 0;
  // As declared, for a picture.
  SegmentLength width;
  SegmentLength height;

  bool operator==(const BlockStyle& other) const noexcept = default;
};

// The most bytes a style record is.
inline constexpr std::size_t kBlockStyleMostBytes = 77;

// Writes a style record, and says how long it is: nothing at all for a
// style that has no field. Fields whose bits are not in `has` are not
// written, and a bit of `fromBook` for such a field is dropped.
std::size_t StoreBlockStyle(const BlockStyle& style, std::span<std::uint8_t, kBlockStyleMostBytes> out) noexcept;
// Reads one. False if `in` is not a style record from end to end: a field
// there is none of, a length that is not what its fields come to, an
// alignment or a direction there is none of.
[[nodiscard]] bool LoadBlockStyle(std::span<const std::uint8_t> in, BlockStyle& style) noexcept;

// How a run of text is set: the bits of a style item.
inline constexpr std::uint16_t kRunBold = 1U << 0U;
inline constexpr std::uint16_t kRunItalic = 1U << 1U;
inline constexpr std::uint16_t kRunUnderline = 1U << 2U;
inline constexpr std::uint16_t kRunLineThrough = 1U << 3U;
inline constexpr std::uint16_t kRunSmallCaps = 1U << 4U;
inline constexpr std::uint16_t kRunRaised = 1U << 5U;
inline constexpr std::uint16_t kRunLowered = 1U << 6U;

// A style item: how the text after it is set, until the next style item
// or the block's end. A block begins as one of these is made.
struct RunStyle {
  std::uint16_t bits = 0;      // kRunBold and the rest.
  std::uint16_t fromBook = 0;  // The same bits: for each, whether the book said so.
  // The class of the font family, 0 to 7 (CssFamilyClass has the six there
  // are), and whether the book named it.
  std::uint8_t familyClass = 0;
  bool familyFromBook = false;
  // Which of the chapter's languages, from 1; 0 for the book's own.
  std::uint8_t language = 0;
  std::uint16_t size = 1000;  // Of the block's size, in thousandths.

  bool operator==(const RunStyle& other) const noexcept = default;
};

// What a link leads to.
enum class LinkKind : std::uint8_t {
  kInside = 0,  // A place in the book.
  kNote,        // A note in the book: the link is its reference.
  kOutside,     // Somewhere that is not the book. Nothing is kept of where.
};

// A picture's bits: it stands in a line of text, and not as its own block.
inline constexpr std::uint16_t kPictureInLine = 1U << 0U;

// What the directory says of a picture the chapter shows.
struct SegmentPicture {
  std::uint16_t number = 0;
  std::uint16_t width = 0;  // In pixels.
  std::uint16_t height = 0;
  bool sized = false;  // Its size could be read.
};

// What the caller must give a SegmentWriter to work in.
inline constexpr std::size_t kSegmentWriterBytes = 19304;
// The most text one call of SegmentWriter::Text() takes.
inline constexpr std::size_t kSegmentMostText = kSegmentMostItem;

// Writes a chapter's segment at the end of a bundle's segments.
//
//   Begin(file, bundle.DataEnd())
//   for each block:
//     BeginBlock(), then Text(), Style(), LinkStart(), LinkEnd() and
//     Picture() as the block has them, then EndBlock()
//   Anchor() wherever an id is met, in a block or between two
//   BeginEndBlocks(), then Step() until it says there is no more
//   SetPictureSize() for each of Pictures() whose size could be read
//   BeginFinish(), then Step() until it says there is no more
//   bundle.Commit(chapter, writer.Chapter())
//
// The file is the bundle's, opened to be changed, and is used by nothing
// else from Begin() to the end. Nothing written is part of the bundle
// until it is committed: a segment that is given up, or cut off by the
// power going, lies past where the segments end and is written over by
// the next.
//
// Its memory is kSegmentWriterBytes the caller gives, and none of it grows
// with the chapter or with a paragraph. It writes whole pages, in order. A
// call writes two pages at the most, and a Step() one.
//
// Text is written as it is given: nothing here looks at what it says but
// to count the characters it uses. It has to be whole characters at each
// call. Text given in several calls with nothing between them is put
// together into items of about a kilobyte.
//
// Once a call has ended in anything but kOk, every later one ends the
// same way, until Begin().
//
// Used by one task.
class SegmentWriter {
 public:
  // `memory` is kSegmentWriterBytes that begin at a multiple of four, the
  // caller's, alive and left alone for as long as this is in use.
  explicit SegmentWriter(std::span<std::uint8_t> memory) noexcept;

  SegmentWriter(const SegmentWriter&) = delete;
  SegmentWriter& operator=(const SegmentWriter&) = delete;
  ~SegmentWriter() = default;

  // Begins a segment at `offset` of `file`, which is a multiple of
  // kBundleSectorBytes and not past the file's end.
  [[nodiscard]] SegmentStatus Begin(core::BlockFile& file, std::uint32_t offset) noexcept;

  // Begins a block. `level` is 1 to 6 for a heading and 0 for the others.
  // `source` is where in the document whatever began the block began; one
  // that is before the last block's is taken as the last block's.
  [[nodiscard]] SegmentStatus BeginBlock(BlockKind kind, std::uint8_t level, const BlockStyle& style,
                                         std::uint32_t source) noexcept;
  // Text of the block in hand: kSegmentMostText bytes at the most.
  [[nodiscard]] SegmentStatus Text(std::string_view text) noexcept;
  // How the text after this is set.
  [[nodiscard]] SegmentStatus Style(const RunStyle& style) noexcept;
  // The text from here to LinkEnd() is a link to `fragment` of `chapter`:
  // a name of kSegmentMostName bytes at the most, which may be none. No
  // link is begun inside another, and a block ends with none open.
  [[nodiscard]] SegmentStatus LinkStart(LinkKind kind, std::uint16_t chapter, std::string_view fragment) noexcept;
  [[nodiscard]] SegmentStatus LinkEnd() noexcept;
  // A picture: its number among the book's (kSegmentNoPicture for one the
  // book does not have), kPictureInLine or not, its width and height as
  // the document declares them, and the text that stands for it, of
  // kSegmentMostName bytes at the most.
  [[nodiscard]] SegmentStatus Picture(std::uint16_t number, std::uint16_t bits, const SegmentLength& width,
                                      const SegmentLength& height, std::string_view alt) noexcept;
  [[nodiscard]] SegmentStatus EndBlock() noexcept;

  // An id of the document: here, in the block in hand; or, between blocks,
  // at the start of the next. One that cannot be kept is left out and
  // counted: a name of more than kSegmentMostName bytes, one for which
  // there is no page left, one in a block that is not indexed. A name of
  // no bytes is no id.
  [[nodiscard]] SegmentStatus Anchor(std::string_view name) noexcept;

  // The number a style item gives the language `tag`: the same for the
  // same tag. 0, the book's own, for a tag of no bytes; and for one that
  // cannot be kept (longer than kSegmentMostLanguage, or one more than
  // kSegmentLanguages), which is counted as left out.
  [[nodiscard]] std::uint8_t Language(std::string_view tag) noexcept;

  // Counts `count` more things left out for a limit, which are the
  // caller's to know of.
  void CountLeftOut(std::uint32_t count) noexcept;

  // Ends the blocks: the last index page and the last anchor page are
  // written. Then Step().
  [[nodiscard]] SegmentStatus BeginEndBlocks() noexcept;
  // Once the blocks are ended: how many pictures the chapter shows that
  // the directory has room for, the number of each in rising order, and
  // the size of one, for those whose size could be read.
  std::size_t Pictures() const noexcept { return pictureCount_; }
  std::uint16_t PictureNumber(std::size_t index) const noexcept;
  [[nodiscard]] SegmentStatus SetPictureSize(std::size_t index, std::uint16_t width, std::uint16_t height) noexcept;
  // Writes the directory and what is left of the last page. `state` is
  // kWhole or kCutShort, and `reason` what the chapter's record is to give
  // as why; `sourceBytes` is the document's length. Then Step().
  [[nodiscard]] SegmentStatus BeginFinish(ChapterState state, std::uint8_t reason, std::uint32_t sourceBytes) noexcept;

  // Does the next piece of what was begun. Returns true if there is more
  // to do, false once it is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;
  SegmentStatus Status() const noexcept { return status_; }

  // How many blocks have been ended, and the characters in them.
  std::uint32_t Blocks() const noexcept { return blocks_; }
  std::uint32_t Characters() const noexcept { return characters_; }
  // How many bytes the segment is so far.
  std::uint32_t Bytes() const noexcept { return flushed_ + static_cast<std::uint32_t>(fill_); }
  // How many things have been left out for a limit.
  std::uint32_t LeftOut() const noexcept { return leftOut_; }

  // The chapter's record, to be committed: once BeginFinish() and its
  // steps have ended kOk.
  const BundleChapter& Chapter() const noexcept { return chapter_; }

 private:
  enum class Stage : std::uint8_t {
    kIdle,
    kBlocks,      // Blocks are being given.
    kEndIndex,    // The blocks are ended: the last index page,
    kEndAnchors,  // and the last anchor page, are being written,
    kEndLast,     // and the pictures' records made ready.
    kPictures,    // The pictures' sizes are being given.
    kDirectory,   // The directory is being written,
    kLastPage,    // and what is left of the last page.
    kDone,
  };

  SegmentStatus Fail(SegmentStatus status) noexcept;
  // Adds bytes to the segment, writing a page whenever one is full; the
  // second counts them in the block's checksum as well.
  bool Append(std::span<const std::uint8_t> bytes) noexcept;
  bool AppendToBlock(std::span<const std::uint8_t> bytes) noexcept;
  bool Flush() noexcept;
  // Writes the text item that is being put together, if there is one.
  bool EmitText() noexcept;
  bool EmitItem(std::uint8_t kind, std::uint8_t byte, std::span<const std::uint8_t> bytes) noexcept;
  // Begins the record of the page being filled, notes it for the
  // directory, and gives the page, which is to be added next. Nothing, if
  // that failed.
  std::span<const std::uint8_t> BeginIndexPage() noexcept;
  std::span<const std::uint8_t> BeginAnchorPage() noexcept;
  // Adds what it can of what is waiting, as far as one page written.
  // Returns true once nothing is waiting.
  bool Drain() noexcept;
  void CountCharacters(std::string_view text) noexcept;
  void NotePicture(std::uint16_t number) noexcept;

  // The caller's memory, in its parts.
  std::span<std::uint8_t> out_;            // The page being filled.
  std::span<std::uint8_t> text_;           // The text item being put together.
  std::span<std::uint8_t> index_;          // The index page being filled; then the pictures' records.
  std::span<std::uint8_t> anchors_;        // The anchor page being filled.
  std::span<std::uint8_t> indexEntries_;   // The directory's entries for the index pages,
  std::span<std::uint8_t> anchorEntries_;  // and for the anchor pages.
  std::span<std::uint8_t> pictures_;       // The numbers of the pictures met, in rising order.
  std::span<std::uint8_t> tags_;           // The languages' tags, each with its length before it.

  core::BlockFile* file_ = nullptr;
  SegmentStatus status_ = SegmentStatus::kMisused;
  Stage stage_ = Stage::kIdle;
  std::uint32_t offset_ = 0;   // Where in the file the segment begins.
  std::uint32_t flushed_ = 0;  // How much of it has been written,
  std::size_t fill_ = 0;       // and how much more is in the page.

  // The block in hand.
  bool inBlock_ = false;
  bool linkOpen_ = false;
  std::uint8_t blockKind_ = 0;
  std::uint8_t blockBreaking_ = 0;
  std::uint8_t runClass_ = 0;
  std::uint32_t blockStart_ = 0;
  std::uint32_t blockSource_ = 0;
  std::uint32_t blockCharacters_ = 0;
  std::uint32_t blockChecksum_ = 0;
  std::size_t textFill_ = 0;

  // The chapter so far.
  std::uint32_t blocks_ = 0;
  std::uint32_t characters_ = 0;
  std::uint32_t leftOut_ = 0;
  std::uint32_t lastSource_ = 0;
  std::size_t indexPages_ = 0;    // Index pages written.
  std::size_t indexRecords_ = 0;  // Records in the one being filled.
  std::uint32_t indexFirst_ = 0;  // The chapter's characters before its first block.
  std::size_t anchorPages_ = 0;   // Anchor pages written.
  std::size_t anchorFill_ = 0;    // Bytes in the one being filled,
  std::size_t anchorIds_ = 0;     // and ids.
  std::size_t pictureCount_ = 0;
  std::size_t languages_ = 0;
  std::size_t tagsFill_ = 0;

  // What is waiting to be added to the segment, a piece at a time; and,
  // for the directory, which part of it comes next.
  std::span<const std::uint8_t> waiting_;
  bool waitingInDirectory_ = false;
  std::size_t directoryPart_ = 0;
  std::uint32_t directoryOffset_ = 0;
  std::uint32_t directoryChecksum_ = 0;

  BundleChapter chapter_;
};

// The kinds of item.
enum class ItemKind : std::uint8_t {
  kText = 1,
  kStyle,
  kLinkStart,
  kLinkEnd,
  kPicture,
  kEnd,
};

// An item of a block, as a reader gives it. What is not of the item's
// kind is as nothing said.
struct SegmentItem {
  ItemKind kind = ItemKind::kEnd;
  // kText: the text. kLinkStart: the fragment's name. kPicture: the text
  // that stands for it. It lies in the reader's page, and is gone at the
  // next thing asked of the reader. It is the bytes the segment has, which
  // nothing here has looked at.
  std::string_view text;
  RunStyle style;                     // kStyle.
  LinkKind link = LinkKind::kInside;  // kLinkStart: what it leads to,
  std::uint16_t chapter = 0;          // and the chapter.
  std::uint16_t picture = 0;          // kPicture: its number, its bits,
  std::uint16_t pictureBits = 0;      // and its size as declared.
  SegmentLength width;
  SegmentLength height;
  std::uint32_t characters = 0;  // kEnd: how many characters the block is.
};

// A block's head, as a reader gives it.
struct SegmentBlock {
  std::uint8_t kind = 0;  // A BlockKind, or a number that is none.
  std::uint8_t level = 0;
  BlockStyle style;
  // Which block of the chapter it is, where it begins in the segment, and
  // how many characters the chapter has before it.
  std::uint32_t index = 0;
  std::uint32_t offset = 0;
  std::uint32_t charactersBefore = 0;
};

// A place in a chapter that was looked for.
struct SegmentPlace {
  bool found = false;
  // The place is the chapter's end, after its last block: there is no
  // block to read, and only `block` and `charactersBefore` say anything.
  bool atEnd = false;
  std::uint32_t block = 0;             // Which block,
  std::uint32_t offset = 0;            // where it begins in the segment,
  std::uint32_t charactersBefore = 0;  // the chapter's characters before it,
  std::uint32_t source = 0;            // and where in the document it began.
  std::uint8_t kind = 0;               // What the index says of it: its kind,
  std::uint8_t breaking = 0;           // and the low byte of its bits for breaking.
  // How many characters into the block the place is: for an id, and for a
  // count of characters. It may be past the block's end, where the place
  // is in a block after the last that is indexed.
  std::uint32_t into = 0;
};

// An id of the document, as an anchor page has it.
struct SegmentAnchor {
  std::string_view name;
  // The block it is in, which is one past the last for an id after every
  // block; and how many characters into the block.
  std::uint32_t block = 0;
  std::uint32_t into = 0;
};

// Reads a chapter's segment.
//
//   BeginOpen(file, record), then Step() until it says there is no more
//   to find a place: BeginFindBlock(), BeginFindCharacters(),
//     BeginFindSource() or BeginFindAnchor(), then Step(); then Place()
//   to read: Seek(place) or SeekStart(); then, for each block,
//     ReadBlock() until it is not kMore, and NextItem() until the item
//     is the block's end, asking again whenever it says kMore
//
// The file is the bundle's, open; the record is the chapter's, which must
// be of a chapter that was imported. Whatever the bytes of the segment
// are, everything asked ends in a status.
//
// A block's items are given as they are read, before its checksum can be
// known: the item that is the block's end is given only if the block is
// what was written, and otherwise the reading ends kDamaged there.
//
// Its memory is a page the caller gives. A Step(), a ReadBlock() and a
// NextItem() each ask the file for one page's worth of bytes at the most,
// which is two reads of storage where they lie across two of the file's
// blocks.
//
// Used by one task.
class SegmentReader {
 public:
  // `page` is kBundlePageBytes that begin at a multiple of four, the
  // caller's, alive and left alone for as long as this is in use.
  explicit SegmentReader(std::span<std::uint8_t> page) noexcept;

  SegmentReader(const SegmentReader&) = delete;
  SegmentReader& operator=(const SegmentReader&) = delete;
  ~SegmentReader() = default;

  // Begins opening the segment `record` says a chapter has in `file`: its
  // directory is read and checked. Then Step().
  [[nodiscard]] SegmentStatus BeginOpen(core::BlockFile& file, const BundleChapter& record) noexcept;
  // The same, to its end, in one call: for a tool or a test.
  [[nodiscard]] SegmentStatus Open(core::BlockFile& file, const BundleChapter& record) noexcept;
  void Close() noexcept;
  bool IsOpen() const noexcept { return open_; }

  // Does the next piece of whatever was begun. Returns true if there is
  // more to do, false once it is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;
  SegmentStatus Status() const noexcept { return status_; }

  // What follows is of an open segment, and not to be asked while a piece
  // of work is under way.

  std::uint32_t Blocks() const noexcept { return blocks_; }
  std::uint32_t Characters() const noexcept { return characters_; }
  std::uint32_t SourceBytes() const noexcept { return sourceBytes_; }
  // How many of the blocks are indexed: the first so many.
  std::uint32_t IndexedBlocks() const noexcept;
  std::size_t IndexPages() const noexcept { return indexPages_; }
  std::size_t AnchorPages() const noexcept { return anchorPages_; }
  std::size_t Pictures() const noexcept { return pictures_; }
  std::size_t Languages() const noexcept { return languages_; }
  // The tag of a language, by the number a style item gives it, from 1.
  // Nothing for a number the chapter has no language of.
  std::string_view Language(std::size_t number) const noexcept;

  // What the directory says of the picture `number`. It uses the page:
  // what the last item gave is gone. `found` is false if the chapter's
  // directory has no such picture.
  [[nodiscard]] SegmentStatus ReadPicture(std::uint16_t number, SegmentPicture& picture, bool& found) noexcept;
  // The same for the picture at a place among the directory's, in their
  // order: for whoever goes through them all.
  [[nodiscard]] SegmentStatus ReadPictureAt(std::size_t index, SegmentPicture& picture) noexcept;

  // Begins looking for a place; then Step(), and Place().
  //
  // The block of that number: one of the indexed ones.
  [[nodiscard]] SegmentStatus BeginFindBlock(std::uint32_t block) noexcept;
  // The block the chapter's character of that number is in; for a number
  // at or past the chapter's characters, the chapter's end.
  [[nodiscard]] SegmentStatus BeginFindCharacters(std::uint32_t characters) noexcept;
  // The last indexed block that began at or before that place in the
  // source document; the first block, if none did.
  [[nodiscard]] SegmentStatus BeginFindSource(std::uint32_t source) noexcept;
  // The id of that name: the first, where the document has it more than
  // once. `name` must stay as it is until the looking is over.
  [[nodiscard]] SegmentStatus BeginFindAnchor(std::string_view name) noexcept;
  // The same, each to its end in one call: for a tool or a test.
  [[nodiscard]] SegmentStatus FindBlock(std::uint32_t block) noexcept;
  [[nodiscard]] SegmentStatus FindCharacters(std::uint32_t characters) noexcept;
  [[nodiscard]] SegmentStatus FindSource(std::uint32_t source) noexcept;
  [[nodiscard]] SegmentStatus FindAnchor(std::string_view name) noexcept;
  // What the last looking found. `found` is false if there was no such
  // place: a chapter of no blocks, an id the chapter does not have.
  const SegmentPlace& Place() const noexcept { return place_; }

  // Begins reading the anchor page of that number, of AnchorPages(); then
  // Step(). Once that has ended kOk, NextAnchor() gives the page's ids one
  // after another, in the order the document has them, and says false
  // when there are no more. They lie in the page, and are gone at the
  // next thing asked of the reader that is not NextAnchor().
  [[nodiscard]] SegmentStatus BeginReadAnchors(std::size_t page) noexcept;
  [[nodiscard]] bool NextAnchor(SegmentAnchor& anchor) noexcept;

  // Goes to the block at a place that was found and is not the chapter's
  // end; and to the chapter's first block.
  [[nodiscard]] SegmentStatus Seek(const SegmentPlace& place) noexcept;
  [[nodiscard]] SegmentStatus SeekStart() noexcept;
  // Reads the head of the next block. kMore if a page was passed over and
  // the block is still to come; kEnd if there is no block after the last
  // read.
  [[nodiscard]] SegmentStatus ReadBlock(SegmentBlock& block) noexcept;
  // Reads the next item of the block whose head was read. kMore if what
  // was there was passed over: a page, or an item of a kind this does not
  // know, which is counted. After the item that is the block's end, the
  // next block's head is to be read.
  [[nodiscard]] SegmentStatus NextItem(SegmentItem& item) noexcept;
  // How many items of kinds this does not know have been passed over
  // since the segment was opened.
  std::uint32_t UnknownItems() const noexcept { return unknownItems_; }

 private:
  enum class Job : std::uint8_t {
    kNone,
    kOpening,
    kFindBlock,
    kFindCharacters,
    kFindSource,
    kFindAnchor,
    kReadAnchors,
  };
  enum class Stage : std::uint8_t {
    kDirectory,  // Opening: the directory, a page at a time.
    kEntries,    // Looking: the directory's entries for the index pages,
    kEntry,      // the entry of one page,
    kPage,       // and the page.
  };
  // An entry of the directory for an index page or an anchor page.
  struct Entry {
    std::uint32_t offset = 0;
    std::uint32_t first = 0;  // An index page: the characters before its first block.
    std::uint16_t bytes = 0;  // An anchor page: how long,
    std::uint16_t ids = 0;    // and how many ids.
    std::uint32_t checksum = 0;
  };

  SegmentStatus Fail(SegmentStatus status) noexcept;
  SegmentStatus Done(SegmentStatus status) noexcept;
  bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> dst) noexcept;
  SegmentStatus BeginFind(Job job) noexcept;
  void StepOpen() noexcept;
  void StepFind() noexcept;
  bool CheckDirectory() noexcept;
  // The entry of the index page or the anchor page `number`.
  bool LoadEntry(bool anchors, std::size_t number, Entry& entry) noexcept;
  // Reads the index page `number`, checked, into the page; and says how
  // many records it has.
  bool LoadIndexPage(const Entry& entry, std::size_t number, std::size_t& records) noexcept;
  bool PlaceFromRecord(std::size_t number, std::size_t record) noexcept;
  void PlaceAtEnd() noexcept;
  void SearchAnchorPage() noexcept;
  std::size_t RecordsInPage(std::size_t number) const noexcept;

  std::span<std::uint8_t> page_;
  core::BlockFile* file_ = nullptr;
  bool open_ = false;
  SegmentStatus status_ = SegmentStatus::kMisused;
  Job job_ = Job::kNone;
  Stage stage_ = Stage::kDirectory;

  // The chapter's record, and what its directory says.
  std::uint32_t segmentOffset_ = 0;
  std::uint32_t directoryOffset_ = 0;  // In the segment.
  std::uint32_t directoryBytes_ = 0;
  std::uint32_t directoryChecksum_ = 0;
  std::uint32_t recordBlocks_ = 0;
  std::uint32_t recordCharacters_ = 0;
  std::uint32_t blocks_ = 0;
  std::uint32_t characters_ = 0;
  std::uint32_t sourceBytes_ = 0;
  std::size_t indexPages_ = 0;
  std::size_t anchorPages_ = 0;
  std::size_t pictures_ = 0;
  std::size_t languages_ = 0;
  std::array<std::uint8_t, kSegmentLanguages*(kSegmentMostLanguage + 1)> tags_{};
  std::size_t tagsBytes_ = 0;

  // The piece of work under way.
  std::uint32_t done_ = 0;      // Opening: how much of the directory has been read,
  std::uint32_t checksum_ = 0;  // and its checksum so far.
  std::uint32_t target_ = 0;    // Looking: what for,
  std::string_view name_;       // or the id's name;
  std::size_t low_ = 0;         // the pages still to be looked among,
  std::size_t high_ = 0;
  std::size_t number_ = 0;  // the page in hand,
  Entry entry_;             // and its entry.
  std::uint32_t into_ = 0;
  bool resolving_ = false;  // An id was found, and its block is being looked up.
  // An anchor page that was read to be gone through: how long it is, and
  // how far through it NextAnchor() has got. No bytes, once the page has
  // been used for anything else.
  std::size_t anchorsBytes_ = 0;
  std::size_t anchorsAt_ = 0;
  SegmentPlace place_;

  // A few of the directory's entries, as last read: which table, from
  // which entry, how many.
  std::array<std::uint8_t, 96> entries_{};
  bool entriesAnchors_ = false;
  std::size_t entriesFirst_ = 0;
  std::size_t entriesCount_ = 0;

  // Reading blocks: where the next record is, which block it is of, and
  // the block in hand.
  std::uint32_t position_ = 0;
  std::uint32_t blockIndex_ = 0;
  std::uint32_t charactersBefore_ = 0;
  bool inBlock_ = false;
  std::uint32_t blockChecksum_ = 0;
  std::uint32_t blockCharacters_ = 0;
  std::uint32_t unknownItems_ = 0;
};

}  // namespace bn::epub
