#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "epub/segment.h"
#include "layout/chapter_pager.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/made_up_face.h"
#include "layout/page_chooser.h"
#include "layout/paragraph_setter.h"

// A chapter of a bundle set in pages, for bn-pages.
//
// The pages are the firmware's own: layout::ChapterPager reads the chapter
// through the reader of its segment, sets each block with the paragraph
// setter and chooses pages with the page chooser. What is here is the rest
// of what a tool needs: what a block's record comes to on a page, which is
// the pager's to ask and this tool's to answer until something sets real
// text in a real face; the text of each line, which the pager does not
// give; and a line known by its number in its block, as a tool that prints
// lines would have it.
//
// How a block's record becomes a page's measures (MadeUpSetting):
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
//     block says nothing. In a block that is a picture, where the picture
//     comes before any text, a line is as high as the picture, made to fit
//     the page's width and at the most its height; as high as it is
//     declared where its size is not known; and 100 pixels where neither is
//     said.
//   - The space above a block and the space after it are the lengths the
//     block gives, and never less than nothing.
//   - A run's style, to the made-up face, is 1 if it is bold, 2 if it is
//     italic and 3 if both; and its words are broken by the one table the
//     tool was given, whatever its language.
//
// A place in a chapter is a block and how many characters into it. A line
// is known here by its block and which of the block's lines it is.

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
inline constexpr char kPictureByte = layout::ChapterPager::kPictureByte;

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
};

// A page.
struct Page {
  bool found = false;      // There is such a page: false for the page above a chapter's first line.
  bool filledDown = true;  // It was filled down from its first line; false, up from its last.
  bool cutShort = false;   // It holds fewer lines than it might, for want of room to remember them in.
  LineRef first;
  LineRef next;  // The line after its last.
  std::uint32_t height = 0;
  std::vector<LineRef> lines;
};

// What a block's record comes to, by this tool's rules.
class MadeUpSetting final : public layout::BlockSetting {
 public:
  MadeUpSetting(const PageGeometry& geometry, layout::Hyphenator* hyphenator)
      : geometry_(geometry), hyphenator_(hyphenator) {}

  void MeasureBlock(const epub::SegmentBlock& block, layout::BlockMeasures& measures) noexcept override;
  void MeasureRun(const epub::SegmentBlock& block, const epub::RunStyle& run,
                  layout::RunMeasures& measures) noexcept override;
  std::uint16_t PictureHeight(const epub::SegmentBlock& block, const epub::StyleLength& width,
                              const epub::StyleLength& height, const epub::SegmentPicture* picture) noexcept override;

 private:
  PageGeometry geometry_;
  layout::Hyphenator* hyphenator_;
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
  // How many blocks the pager has given the setter.
  std::uint64_t BlocksSet() const { return pager_.BlocksSet(); }

 private:
  bool Fail(const std::string& what, epub::SegmentStatus status);
  // Does the work the pager was begun on, to its end.
  bool Run(layout::PagerStatus begun, const std::string& what);
  // Reads a block's head and its characters.
  bool Read(std::uint32_t block, SetBlock& set, std::string& characters);
  // Where a line begins, as the pager knows a place; and the line that
  // begins at a place.
  bool PlaceOf(LineRef line, layout::ChapterPlace& place);
  bool RefOf(layout::ChapterPlace place, LineRef& line);
  // The page the pager has made, with each of its lines found among its
  // block's.
  bool Take(Page& page);

  epub::SegmentReader& reader_;
  PageGeometry geometry_;
  MadeUpSetting setting_;
  layout::MadeUpFace face_;
  std::vector<layout::LineBreaker::Node> nodes_;
  std::vector<layout::ParagraphSetter::Record> records_;
  layout::ParagraphSetter setter_;
  std::vector<layout::PageFromBottom::Entry> entries_;
  std::vector<layout::ChapterLine> lines_;
  layout::ChapterPager pager_;
  std::string error_;
  // The blocks that have been set, and which was set least lately.
  std::map<std::uint32_t, SetBlock> blocks_;
  std::vector<std::uint32_t> order_;
};

}  // namespace bn::tools
