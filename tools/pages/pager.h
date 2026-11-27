#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "epub/segment.h"
#include "layout/hyphenator.h"
#include "layout/made_up_face.h"
#include "layout/page_chooser.h"

// A chapter of a bundle set in pages, for bn-pages.
//
// The chapter is read through the reader of its segment (epub/segment.h)
// and in the ways a device has: a block found by the index and read from
// its head, the next read on from there. Each block is set in lines by the
// paragraph setter in the made-up face (layout/made_up_face.h), and pages
// are chosen among the lines by the page chooser (layout/page_chooser.h):
// down from a line, up to a line, or about a place the chapter is entered
// at.
//
// How a block's record becomes a page's measures, which is this tool's own
// until something sets real text in a real face:
//
//   - Widths are in quarters of a pixel, as the made-up face has them, and
//     heights in pixels. The reader's text size is 16 pixels.
//   - A length is its three amounts added: so much of the block's text
//     size, so many pixels, and so much of the page's text width.
//   - A block's lines are as wide as the page's text less its margins to
//     left and right, and its first line less its indent as well; never
//     wider than the page's text, and never narrower than a tenth of it.
//   - A line is as high as the block's line height says: a multiple of its
//     text size, or a length, or one and a quarter times the size where the
//     block says nothing. A block that is a picture is one line as high as
//     the picture, made to fit the page's width and at the most its height;
//     as high as it is declared where its size is not known; and 100
//     pixels where neither is said.
//   - Above a block's first line is the block's space before and the space
//     after of the block before it.
//   - A block's first line starts a page if the block asks for a page to
//     end before it, and so does the chapter's first line. It is kept with
//     the line before if the block before asks to be kept with the next.
//     Every other line of a block that asks to be kept together is kept
//     with the line before it.
//   - A run's style, to the made-up face, is 1 if it is bold, 2 if it is
//     italic and 3 if both.
//   - A picture among the words is one character, as the format counts it.
//
// A place in a chapter is a block and how many characters into it. A line
// is known by its block and where in the block it begins.

namespace bn::tools {

// What a page is set to.
struct PageGeometry {
  std::uint16_t width = 1760;  // Of its text, in quarters of a pixel.
  std::uint16_t height = 760;  // In pixels.
};

// One line of a block.
struct ChapterLine {
  std::uint32_t begin = 0;  // The characters of the block that are on it: from here
  std::uint32_t end = 0;    // up to, and not including, here.
  std::uint32_t width = 0;
  std::uint16_t height = 0;
  std::uint16_t gap = 0;  // The space that belongs above it.
  bool keep = false;      // It is kept with the line before.
  bool starts = false;    // It starts a page.
  bool hyphen = false;    // A hyphen is shown at its end.
  bool last = false;      // It ends its paragraph, or a line the text asked to end.
  std::string text;       // Its characters, a picture being the byte kPictureByte.
};

// The byte that stands for a picture among a block's characters.
inline constexpr char kPictureByte = 0x1A;

// A line of a chapter: which block, and which of the block's lines. One
// past a chapter's last block, with line 0, is the chapter's end.
struct LineRef {
  std::uint32_t block = 0;
  std::uint32_t line = 0;

  bool operator==(const LineRef&) const = default;
  auto operator<=>(const LineRef&) const = default;
};

// A block as it was read and set.
struct SetBlock {
  std::uint8_t kind = 0;
  std::uint8_t level = 0;
  std::uint32_t characters = 0;
  std::uint32_t charactersBefore = 0;
  std::vector<ChapterLine> lines;
  // What the block after it needs of it.
  bool keepWithNext = false;
  std::uint16_t spaceAfter = 0;
};

// A page.
struct Page {
  bool found = false;      // There is such a page: false for the page above a chapter's first line.
  bool filledDown = true;  // It was filled down from its first line; false, up from its last.
  bool cutShort = false;   // Filled up, the chooser ran out of room to remember lines in.
  LineRef first;
  LineRef next;  // The line after its last.
  std::uint32_t height = 0;
  std::vector<LineRef> lines;
};

class Pager {
 public:
  // `reader` has the chapter open, and is used by nothing else while this
  // is. `hyphenator` may be null.
  Pager(epub::SegmentReader& reader, const PageGeometry& geometry, layout::Hyphenator* hyphenator);

  // Each of these says false if the chapter could not be read or set, and
  // Error() then says why.

  // A block, read and set.
  bool Block(std::uint32_t block, const SetBlock*& set);
  // The line of a block that holds the character so far into it.
  bool LineHolding(std::uint32_t block, std::uint32_t into, LineRef& line);
  // The page that begins with a line.
  bool Forward(LineRef first, Page& page);
  // The page that ends with the line above one. Where filling it up comes
  // to a line that starts a page, it is that line's page filled down, as
  // a reader going back to a chapter's start is shown its first page.
  bool Back(LineRef before, Page& page);
  // The page a chapter entered at a place is shown: it begins with the
  // line that holds the place, or a little above it where that line is
  // kept with those before.
  bool Entering(std::uint32_t block, std::uint32_t into, Page& page);

  // The line after the chapter's last.
  LineRef End() const { return {.block = reader_.Blocks(), .line = 0}; }
  const std::string& Error() const { return error_; }
  // How many blocks have been set, counting each time one was.
  std::uint64_t BlocksSet() const { return blocksSet_; }

 private:
  bool Fail(const std::string& what, epub::SegmentStatus status);
  // Goes to a block, by the index or by reading on to it.
  bool GoTo(std::uint32_t block);
  bool ReadAndSet(std::uint32_t block, SetBlock& set);
  layout::PageLine PageLineOf(const ChapterLine& line, LineRef ref);

  epub::SegmentReader& reader_;
  PageGeometry geometry_;
  layout::Hyphenator* hyphenator_;
  layout::MadeUpFace face_;
  std::string error_;
  std::uint64_t blocksSet_ = 0;
  // The blocks that have been set, and which was set least lately.
  std::map<std::uint32_t, SetBlock> blocks_;
  std::vector<std::uint32_t> order_;
  // The lines given to a chooser, by the number each was given as.
  std::vector<LineRef> tags_;
  // Which block the reader will read next, if it is known.
  bool placed_ = false;
  std::uint32_t nextBlock_ = 0;
};

}  // namespace bn::tools
