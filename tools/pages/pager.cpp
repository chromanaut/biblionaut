#include "pager.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bn::tools {

namespace {

using epub::BlockStyle;
using epub::ItemKind;
using epub::SegmentBlock;
using epub::SegmentItem;
using epub::SegmentLength;
using epub::SegmentPlace;
using epub::SegmentStatus;
using layout::ChapterPlace;
using layout::PagerStatus;

// The reader's text size, in pixels; the height of a line where a block
// says nothing of it, in thousandths of its text size; and how high a
// picture is of which nothing is known.
constexpr std::int64_t kTextSize = 16;
constexpr std::int64_t kUsualLineHeight = 1250;
constexpr std::int64_t kUnknownPicture = 100;
// How many blocks are kept set.
constexpr std::size_t kBlocksKept = 256;

// A length in quarters of a pixel, for a block whose text is `size`
// thousandths of the reader's on a page whose text is `pageWidth` wide.
// Each of its three amounts is taken whole, what is left over dropped.
std::int64_t Quarters(const SegmentLength& length, std::int64_t size, std::int64_t pageWidth) {
  return ((length.em * size) / 1000) + (length.px / 2) + ((length.percent * pageWidth) / 6400);
}

std::int64_t Pixels(const SegmentLength& length, std::int64_t size, std::int64_t pageWidth) {
  return Quarters(length, size, pageWidth) / 4;
}

std::uint16_t Held(std::int64_t value, std::int64_t least, std::int64_t most) {
  return static_cast<std::uint16_t>(std::clamp(value, least, most));
}

// What gathers the lines the pager gives of a block.
class Gathered final : public layout::ChapterLineSink {
 public:
  void OnChapterLine(const layout::ChapterLine& line) noexcept override { lines.push_back(line); }
  std::vector<layout::ChapterLine> lines;
};

// Whether a line of a page is the line of its block it should be.
bool Same(const layout::ChapterLine& on, const ChapterLine& line) {
  return on.begin == line.begin && on.end == line.end && on.width == line.width && on.height == line.height &&
         on.gap == line.gap && on.Is(layout::kLineKept) == line.keep && on.Is(layout::kLineStarts) == line.starts &&
         on.Is(layout::kLineHyphen) == line.hyphen && on.Is(layout::kLineLast) == line.last;
}

}  // namespace

// ---- What a block's record comes to ----

void MadeUpSetting::MeasureBlock(const SegmentBlock& block, layout::BlockMeasures& measures) noexcept {
  const BlockStyle& said = block.style;
  const std::int64_t size = said.size;
  const std::int64_t pageWidth = geometry_.width;
  const std::int64_t narrowest = pageWidth / 10;
  measures.restWidth =
      Held(pageWidth - Quarters(said.marginLeft, size, pageWidth) - Quarters(said.marginRight, size, pageWidth),
           narrowest, pageWidth);
  measures.firstWidth = Held(measures.restWidth - Quarters(said.indent, size, pageWidth), narrowest, pageWidth);
  std::int64_t lineHeight = (kTextSize * size * kUsualLineHeight) / 1000000;
  if ((said.has & epub::kBlockLineHeight) != 0) {
    lineHeight = said.lineMultiple != 0 ? (kTextSize * size * said.lineMultiple) / 1000000
                                        : Pixels(said.lineHeight, size, pageWidth);
  }
  measures.lineHeight = Held(lineHeight, 1, 65535);
  measures.spaceBefore = Held(Pixels(said.spaceBefore, size, pageWidth), 0, 65535);
  measures.spaceAfter = Held(Pixels(said.spaceAfter, size, pageWidth), 0, 65535);
  measures.hyphenPenalty = 0;
}

void MadeUpSetting::MeasureRun(const SegmentBlock& /*block*/, const epub::RunStyle& run,
                               layout::RunMeasures& measures) noexcept {
  measures.style = static_cast<std::uint16_t>(((run.bits & epub::kRunBold) != 0 ? 1U : 0U) |
                                              ((run.bits & epub::kRunItalic) != 0 ? 2U : 0U));
  measures.hyphenator = hyphenator_;
}

std::uint16_t MadeUpSetting::PictureHeight(const SegmentBlock& block, const SegmentLength& /*width*/,
                                           const SegmentLength& height, const epub::SegmentPicture* picture) noexcept {
  // As high as the picture is, made to fit the page's width; or as it is
  // declared; or as high as a picture of which nothing is known.
  const std::int64_t pageWidth = geometry_.width;
  std::int64_t lineHeight = Pixels(height, block.style.size, pageWidth);
  lineHeight = lineHeight > 0 ? lineHeight : kUnknownPicture;
  if (picture != nullptr && picture->sized && picture->width != 0 && picture->height != 0) {
    const std::int64_t widthInPixels = pageWidth / 4;
    lineHeight = picture->width > widthInPixels ? (picture->height * widthInPixels) / picture->width : picture->height;
  }
  return Held(std::min<std::int64_t>(lineHeight, geometry_.height), 1, 65535);
}

// ---- The pager, and what the tool adds to it ----

// The chooser is given an entry, and a page a record, for every line a
// page could hold, a line being a pixel high at the least: so no page is
// cut short for want of either.
Pager::Pager(epub::SegmentReader& reader, const PageGeometry& geometry, layout::Hyphenator* hyphenator)
    : reader_(reader),
      geometry_(geometry),
      setting_(geometry, hyphenator),
      nodes_(layout::LineBreaker::kUsualNodes),
      records_(layout::ParagraphSetter::kUsualRecords),
      setter_(nodes_, records_),
      entries_(std::max<std::size_t>(layout::PageFromBottom::kUsualEntries, std::size_t{geometry.height} + 1)),
      lines_(std::max<std::size_t>(layout::ChapterPager::kMinLines, std::size_t{geometry.height} + 1)),
      pager_(reader_, setter_, face_, setting_, entries_, lines_) {}

bool Pager::Fail(const std::string& what, SegmentStatus status) {
  error_ = what + ": " + epub::SegmentStatusName(status);
  return false;
}

bool Pager::Run(PagerStatus begun, const std::string& what) {
  PagerStatus status = begun;
  if (status == PagerStatus::kOk) {
    while (pager_.Step()) {
    }
    status = pager_.Status();
  }
  if (status == PagerStatus::kOk) {
    return true;
  }
  error_ = what + ": " +
           (status == PagerStatus::kReadFailed  ? epub::SegmentStatusName(pager_.ReaderStatus())
            : status == PagerStatus::kSetFailed ? layout::ParagraphStatusName(pager_.SetterStatus())
                                                : layout::PagerStatusName(status));
  return false;
}

bool Pager::Read(std::uint32_t block, SetBlock& set, std::string& characters) {
  if (block >= reader_.Blocks()) {
    error_ = "the chapter has no block " + std::to_string(block);
    return false;
  }
  // By the index; or, for a block past the last that is indexed, from that
  // one on.
  const std::uint32_t indexed = std::min(block, reader_.IndexedBlocks() - 1);
  if (const SegmentStatus status = reader_.FindBlock(indexed); status != SegmentStatus::kOk) {
    return Fail("the index", status);
  }
  const SegmentPlace place = reader_.Place();
  if (!place.found) {
    error_ = "the index has no block " + std::to_string(indexed);
    return false;
  }
  if (const SegmentStatus status = reader_.Seek(place); status != SegmentStatus::kOk) {
    return Fail("going to a block", status);
  }
  for (std::uint32_t at = indexed; at <= block; ++at) {
    SegmentBlock head;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      status = reader_.ReadBlock(head);
    }
    if (status != SegmentStatus::kOk) {
      return Fail("a block's head", status);
    }
    characters.clear();
    for (;;) {
      SegmentItem item;
      status = reader_.NextItem(item);
      if (status == SegmentStatus::kMore) {
        continue;
      }
      if (status != SegmentStatus::kOk) {
        return Fail("an item", status);
      }
      if (item.kind == ItemKind::kEnd) {
        set.characters = item.characters;
        break;
      }
      if (item.kind == ItemKind::kText) {
        characters += item.text;
      } else if (item.kind == ItemKind::kPicture) {
        characters += kPictureByte;
      }
    }
    set.kind = head.kind;
    set.level = head.level;
    set.charactersBefore = head.charactersBefore;
  }
  return true;
}

bool Pager::Block(std::uint32_t block, const SetBlock*& set) {
  auto found = blocks_.find(block);
  if (found == blocks_.end()) {
    SetBlock made;
    std::string characters;
    if (!Read(block, made, characters)) {
      return false;
    }
    Gathered gathered;
    if (!Run(pager_.BeginLines(block, gathered), "setting a block")) {
      return false;
    }
    for (const layout::ChapterLine& line : gathered.lines) {
      if (line.end < line.begin || line.end > characters.size()) {
        error_ = "block " + std::to_string(block) + " has a line of characters it has not";
        return false;
      }
      ChapterLine one;
      one.begin = line.begin;
      one.end = line.end;
      one.width = line.width;
      one.height = line.height;
      one.gap = line.gap;
      one.keep = line.Is(layout::kLineKept);
      one.starts = line.Is(layout::kLineStarts);
      one.hyphen = line.Is(layout::kLineHyphen);
      one.last = line.Is(layout::kLineLast);
      one.text = characters.substr(line.begin, line.end - line.begin);
      made.lines.push_back(std::move(one));
    }
    if (order_.size() == kBlocksKept) {
      blocks_.erase(order_.front());
      order_.erase(order_.begin());
    }
    order_.push_back(block);
    found = blocks_.emplace(block, std::move(made)).first;
  }
  set = &found->second;
  return true;
}

bool Pager::LineHolding(std::uint32_t block, std::uint32_t into, LineRef& line) {
  const SetBlock* set = nullptr;
  if (!Block(block, set)) {
    return false;
  }
  // The last of its lines that begins at or before the place.
  std::uint32_t holding = 0;
  for (std::uint32_t i = 0; i < set->lines.size(); ++i) {
    if (set->lines[i].begin <= into) {
      holding = i;
    }
  }
  line = {.block = block, .line = holding};
  return true;
}

bool Pager::PlaceOf(LineRef line, ChapterPlace& place) {
  // One past a block's last line is the first of the block after it.
  for (;;) {
    if (line.block >= reader_.Blocks()) {
      place = pager_.End();
      return true;
    }
    const SetBlock* set = nullptr;
    if (!Block(line.block, set)) {
      return false;
    }
    if (line.line < set->lines.size()) {
      place = {.block = line.block, .into = set->lines[line.line].begin};
      return true;
    }
    line = {.block = line.block + 1, .line = 0};
  }
}

bool Pager::RefOf(ChapterPlace place, LineRef& line) {
  if (place.block >= reader_.Blocks()) {
    line = End();
    return true;
  }
  const SetBlock* set = nullptr;
  if (!Block(place.block, set)) {
    return false;
  }
  for (std::uint32_t i = 0; i < set->lines.size(); ++i) {
    if (set->lines[i].begin == place.into) {
      line = {.block = place.block, .line = i};
      return true;
    }
  }
  error_ = "a page has a line at " + std::to_string(place.block) + "+" + std::to_string(place.into) +
           ", where no line of that block begins";
  return false;
}

bool Pager::Take(Page& page) {
  // What the pager has is gone once it is asked for a block's lines, so it
  // is taken first.
  const layout::ChapterPage made = pager_.Page();
  const std::vector<layout::ChapterLine> lines(pager_.Lines().begin(), pager_.Lines().end());
  page.found = made.found;
  page.filledDown = made.filledDown;
  page.cutShort = made.cutShort;
  page.height = made.height;
  page.lines.clear();
  if (!made.found) {
    return true;
  }
  if (!RefOf(made.first, page.first) || !RefOf(made.next, page.next)) {
    return false;
  }
  for (const layout::ChapterLine& on : lines) {
    LineRef line;
    const SetBlock* set = nullptr;
    if (!RefOf(on.Place(), line) || !Block(line.block, set)) {
      return false;
    }
    if (!Same(on, set->lines[line.line])) {
      error_ = "the line at " + std::to_string(on.block) + "+" + std::to_string(on.begin) +
               " is not on a page what it is in its block";
      return false;
    }
    page.lines.push_back(line);
  }
  return true;
}

bool Pager::Forward(LineRef first, Page& page) {
  page = Page{};
  page.first = first;
  page.next = End();
  ChapterPlace place;
  return PlaceOf(first, place) && Run(pager_.BeginDown(place, geometry_.height), "a page") && Take(page);
}

bool Pager::Back(LineRef before, Page& page) {
  page = Page{};
  page.filledDown = false;
  page.next = before;
  ChapterPlace place;
  return PlaceOf(before, place) && Run(pager_.BeginUp(place, geometry_.height), "a page") && Take(page);
}

bool Pager::Entering(std::uint32_t block, std::uint32_t into, Page& page) {
  page = Page{};
  if (block >= reader_.Blocks()) {
    error_ = "the chapter has no block " + std::to_string(block);
    return false;
  }
  return Run(pager_.BeginAbout({.block = block, .into = into}, geometry_.height), "a page") && Take(page);
}

}  // namespace bn::tools
