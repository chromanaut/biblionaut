#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/style_stack.h"
#include "epub/xml.h"

// A document's blocks.
//
// A StyleStack (style_stack.h) says what each element of a document comes
// to. A DocumentBlocks goes through the document with one, told of each
// element as it opens and closes and of where text or a picture comes,
// and says what a bundle keeps of it: where a block of text begins and
// ends, of what kind it is and with what record, and how the text at any
// place is set. It keeps nothing of the text, and writes nothing.
//
// The rules it goes by:
//
//   - A block is made by what is in it, not by an element. One begins at
//     the first text that is more than white space, or picture, or `br`,
//     where none is open, and ends where an element that is a block opens
//     or closes. So an element with only blocks inside makes none of its
//     own; text standing straight in a `div`, an `li` or a `td` is a
//     block; text beside blocks in one element is a block before them and
//     one after; and an element with nothing in it makes none.
//   - Its kind is from the nearest thing around it that names one: a
//     heading, with its level, inside `h1` to `h6`; a list item for the
//     first block begun inside an item of a list, however deep, and a
//     table's cell for the first begun inside a `td` or a `th`; and a
//     paragraph otherwise, the later blocks of an item or a cell among
//     them. An `hr` is a block by itself, a rule.
//   - The first block begun inside an item of a list has the item's
//     marker and its number, whatever its kind. An item's number is one
//     more than that of the item before it in its list, the first being
//     the list's `start` or 1, unless its own `value` says another.
//   - `br` ends a line of the block that is open, and where none is open
//     it begins one, which is the empty line books mean by it.
//   - Of the lines of a block (how they are aligned, their size, the
//     distance between them, a first line's indent, their direction and
//     white space) it has what the nearest element around it that is a
//     block has.
//   - To each side it has the margin and the padding of every element it
//     is inside that is a block, added up.
//   - Above it, it has the largest of the margins that meet there, which
//     is CSS's own rule: the bottom margins of the elements that closed
//     since the block before and the top margins of those that opened.
//     With margins below nothing among them it is the largest above
//     nothing and the one furthest below, added. Where their units differ,
//     which is the largest is judged with 16 pixels to the em and a page
//     30 em wide, and the one that wins is given as it was written.
//     Padding is added, and keeps apart the margins to either side of it;
//     so is the height the book gave an element that has nothing in it.
//     The space is said whole with the block it stands above, and nothing
//     is said of a space below a block.
//   - "A page ends after this" is said as "a page ends before" of the
//     block that follows.
//   - A block has the first letter's style of the nearest element around
//     it that has one and that it is the first block of; and it is said
//     to begin floated if its first text stands in an element that is
//     part of a line and floated. Those are the two ways a book writes a
//     capital that is dropped.
//
// Not its to say: anything of the text itself, a link, a picture's size,
// a table's rows and columns, and what an `svg` written in the document
// is, which go with what writes a chapter down.

namespace bn::epub {

// What a block is.
enum class BlockKind : std::uint8_t {
  kParagraph,
  kHeading,
  kListItem,
  kRule,
  kTableCell,
};

inline constexpr std::size_t kBlockKinds = 5;

// "paragraph", "heading", "list-item", "rule", "table-cell".
const char* BlockKindName(BlockKind kind) noexcept;

// What is said of a block as it begins: its record, as a bundle keeps one.
// Where a thing has "from the book" beside it, that is whether a sheet or
// the document's own attributes said it, and not what the element is.
struct BlockRecord {
  BlockKind kind = BlockKind::kParagraph;
  std::uint8_t headingLevel = 0;  // 1 to 6 for a heading, and nothing otherwise.

  // Its lines.
  StyleAlign align = StyleAlign::kUnset;
  bool alignFromBook = false;
  bool rightToLeft = false;
  bool spacesAsWritten = false;
  std::uint16_t size = 1000;  // Thousandths of the reader's text size.
  bool sizeFromBook = false;
  StyleLineHeight lineHeight;
  bool lineHeightFromBook = false;
  bool hasIndent = false;
  StyleLength indent;
  bool indentFromBook = false;

  // The space around it. The space above counts as the book's if a margin
  // that won, or a padding or a height added, was the book's; a side does
  // if any part of its sum was.
  StyleLength marginLeft;
  StyleLength marginRight;
  bool marginLeftFromBook = false;
  bool marginRightFromBook = false;
  StyleLength spaceBefore;
  bool spaceBeforeFromBook = false;

  // Where a page may end, and the rest.
  bool breakBefore = false;   // A page ends before it.
  bool keepWithNext = false;  // No page ends between it and what follows.
  bool keepWithNextFromBook = false;
  bool keepTogether = false;  // No page ends inside it.
  bool floated = false;       // It is in an element that is a block and was floated.
  // Its element has a width and `auto` for both its side margins, which is
  // how a book centres a block; and that width.
  bool centred = false;
  StyleLength width;

  // How deep in quotations (`blockquote`) and in lists it is, 255 at the
  // most.
  std::uint8_t quoteDepth = 0;
  std::uint8_t listDepth = 0;
  // It is the first block of an item of a list: what stands before it,
  // which may be nothing, and the item's number.
  bool hasMarker = false;
  StyleMarker marker = StyleMarker::kNone;
  bool markerFromBook = false;
  std::int32_t number = 0;

  // A rule set how its first letter is set, and whether that rule floats
  // the letter; and whether its first text stands in an element that is
  // part of a line and floated.
  bool hasFirstLetter = false;
  TextStyle firstLetter;
  bool firstLetterFloated = false;
  bool beginsFloated = false;
};

// What a call made of the document's blocks, in the order the things are
// written here.
struct BlockChange {
  bool ends = false;       // The block that was open ends.
  bool begins = false;     // A block begins: Record() is its record.
  bool lineEnds = false;   // A line of the open block ends (`br`).
  bool floatEnds = false;  // The floated text the open block began with ends.
  // An element that is a block opened or closed, whether or not a block
  // was open: what was met since the last block cannot be part of the
  // next.
  bool boundary = false;
  // For text and a picture: it is part of the block that is open. False
  // for what is inside something that is nothing, and for white space
  // where no block is open.
  bool shown = false;
};

// Begin() once for a document; then Open() and Close() for each element
// as its parser tells of it, with what a cascade said of it, and Text() or
// Picture() where one comes.
//
// One call does what the StyleStack's does for the element, looks at each
// byte of a text once at the most, and where a block begins looks once at
// each open element that is kept: nothing in a document or a sheet can
// make it do more than MostWorkFor() says.
class DocumentBlocks final {
 public:
  // What is kept of one open element: the StyleStack's part, and this
  // one's.
  static constexpr std::size_t kOwnLevelBytes = 48;
  static constexpr std::size_t kLevelBytes = StyleStack::kLevelBytes + kOwnLevelBytes;
  // The most open elements it keeps, however much memory it is given.
  static constexpr std::size_t kMaxLevels = 255;

  // `memory` is for the elements that are open, kLevelBytes each, after
  // StyleStack::kLanguagesBytes for the languages' tags: Levels() of them
  // fit. An element opened deeper than that is counted (by the StyleStack)
  // and takes what the deepest kept gives. If it is a block it still ends
  // the block that is open as it opens, and as it closes too if it is
  // among the first 64 that deep; and what it says as it opens still
  // counts: its top margin and padding, a page that ends before it, its
  // first letter's style. What would have waited for it to close does
  // not, nor its sides, nor what it would name a block.
  explicit DocumentBlocks(std::span<std::uint8_t> memory) noexcept;

  DocumentBlocks(const DocumentBlocks&) = delete;
  DocumentBlocks& operator=(const DocumentBlocks&) = delete;
  ~DocumentBlocks() = default;

  // A document begins: no element and no block is open.
  void Begin() noexcept;

  // An element opens. `style` is what a cascade said of it and
  // `firstLetter` what it said of its first letter; `said` is made what
  // the element comes to, as StyleStack::Open() makes it.
  BlockChange Open(std::string_view name, const XmlAttributes& attributes, const CssStyle& style,
                   const CssStyle& firstLetter, ElementStyle& said) noexcept;
  // The element opened last, and not yet closed, closes.
  BlockChange Close() noexcept;
  // Text comes, in any number of pieces, cut anywhere.
  BlockChange Text(std::string_view text) noexcept;
  // A picture comes, which is for the caller to know: it is as text that
  // is more than white space.
  BlockChange Picture() noexcept;

  // Whether a block is open, and its record: good until the next begins.
  bool HasBlock() const noexcept { return open_; }
  const BlockRecord& Record() const noexcept { return record_; }
  // How text standing where the document now is, is set; whether its
  // white space is as written; and whether it is inside something that is
  // nothing.
  TextStyle TextNow() const noexcept;
  bool SpacesAsWrittenNow() const noexcept;
  bool InNothing() const noexcept { return hiddenFrom_ != kNone; }

  // The StyleStack it goes through the document with: for the languages'
  // tags, and what was not taken as written.
  const StyleStack& Styles() const noexcept { return stack_; }
  std::size_t Depth() const noexcept { return stack_.Depth(); }
  std::size_t Levels() const noexcept { return levels_; }

  // The most work one call does for an element with this many attributes,
  // or a text of this many bytes, with this many open elements kept. On
  // top of what the StyleStack counts, an attribute looked at for a
  // list's first number or an item's own, a byte of text looked at, and
  // an open element looked at where a block begins are one piece of work
  // each.
  static constexpr std::uint32_t MostWorkFor(std::size_t attributes, std::size_t textBytes,
                                             std::size_t levels) noexcept {
    return static_cast<std::uint32_t>(StyleStack::MostWorkFor(attributes) + attributes + textBytes + levels);
  }
  // The work the last call did.
  std::uint32_t Work() const noexcept { return work_; }

 private:
  struct Level;

  static std::span<std::uint8_t> ForStyles(std::span<std::uint8_t> memory) noexcept;
  Level Load(std::size_t index) const noexcept;
  void Store(std::size_t index, const Level& level) noexcept;
  // A margin joins those that meet where the document now is; and a
  // padding or a height is added there, which ends their meeting.
  void Join(const StyleLength& margin, bool fromBook) noexcept;
  void Add(const StyleLength& length, bool fromBook) noexcept;
  void Settle() noexcept;
  // Something that is in a block comes: one begins if none is open, and
  // whether one did.
  bool BeginIfNone() noexcept;
  void BeginBlock(bool rule) noexcept;
  void EndBlock(BlockChange& change) noexcept;

  StyleStack stack_;
  std::uint8_t* memory_ = nullptr;  // This one's levels.
  std::size_t levels_ = 0;

  bool open_ = false;
  BlockRecord record_;
  // The space gathered since the last block began: what is settled, and
  // of the margins now meeting the largest above nothing and the one
  // furthest below it, each with what it measures.
  StyleLength space_;
  bool spaceFromBook_ = false;
  StyleLength above_;
  StyleLength below_;
  std::int32_t aboveMeasure_ = 0;
  std::int32_t belowMeasure_ = 0;
  bool aboveFromBook_ = false;
  bool belowFromBook_ = false;
  bool zeroFromBook_ = false;  // A margin of nothing among them is the book's.
  bool breakPending_ = false;  // A page is to end before the next block.

  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  // How many elements were open when one that is nothing opened.
  std::size_t hiddenFrom_ = kNone;
  // How many of the open elements, from the outermost, have something in
  // them: a block that began, or a height that was added.
  std::size_t filled_ = 0;
  // The element whose first letter's style waits for a block, and that
  // style.
  std::size_t letterAt_ = kNone;
  TextStyle letter_;
  bool letterFloated_ = false;
  // The floated element that is part of a line and opened where no block
  // was open; and whether the open block began inside it.
  std::size_t floatAt_ = kNone;
  bool floatOpen_ = false;
  // For each of the first 64 elements deeper than are kept, whether it is
  // a block.
  std::uint64_t deepBlocks_ = 0;
  std::uint32_t work_ = 0;
};

}  // namespace bn::epub
