#include "pager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "layout/line_breaker.h"
#include "layout/paragraph_setter.h"

namespace bn::tools {

namespace {

using epub::BlockStyle;
using epub::ItemKind;
using epub::SegmentBlock;
using epub::SegmentItem;
using epub::SegmentLength;
using epub::SegmentPicture;
using epub::SegmentPlace;
using epub::SegmentStatus;

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

// What gathers the lines the setter gives.
class Lines final : public layout::TextLineSink {
 public:
  void OnLine(const layout::TextLine& line) noexcept override { lines.push_back(line); }
  std::vector<layout::TextLine> lines;
};

}  // namespace

Pager::Pager(epub::SegmentReader& reader, const PageGeometry& geometry, layout::Hyphenator* hyphenator)
    : reader_(reader), geometry_(geometry), hyphenator_(hyphenator) {}

bool Pager::Fail(const std::string& what, SegmentStatus status) {
  error_ = what + ": " + epub::SegmentStatusName(status);
  placed_ = false;
  return false;
}

bool Pager::GoTo(std::uint32_t block) {
  if (placed_ && nextBlock_ == block) {
    return true;
  }
  placed_ = false;
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
  for (std::uint32_t passed = indexed; passed < block; ++passed) {
    SegmentBlock head;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      status = reader_.ReadBlock(head);
    }
    for (SegmentItem item; status == SegmentStatus::kOk || status == SegmentStatus::kMore;) {
      status = reader_.NextItem(item);
      if (status == SegmentStatus::kOk && item.kind == ItemKind::kEnd) {
        break;
      }
    }
    if (status != SegmentStatus::kOk) {
      return Fail("a block on the way", status);
    }
  }
  placed_ = true;
  nextBlock_ = block;
  return true;
}

bool Pager::ReadAndSet(std::uint32_t block, SetBlock& set) {
  // What the block before asks of this one: read from its head alone, if
  // it has not been set.
  bool keptWithBefore = false;
  std::int64_t spaceAbove = 0;
  if (block > 0) {
    if (const auto before = blocks_.find(block - 1); before != blocks_.end()) {
      keptWithBefore = before->second.keepWithNext;
      spaceAbove = before->second.spaceAfter;
    } else {
      if (!GoTo(block - 1)) {
        return false;
      }
      SegmentBlock head;
      SegmentStatus status = SegmentStatus::kMore;
      while (status == SegmentStatus::kMore) {
        status = reader_.ReadBlock(head);
      }
      if (status != SegmentStatus::kOk) {
        return Fail("the head of the block before", status);
      }
      placed_ = false;
      keptWithBefore = (head.style.breaking & epub::kBreakKeepWithNext) != 0;
      spaceAbove = std::clamp<std::int64_t>(Pixels(head.style.spaceAfter, head.style.size, geometry_.width), 0, 65535);
    }
  }

  if (!GoTo(block)) {
    return false;
  }
  placed_ = false;
  SegmentBlock head;
  SegmentStatus status = SegmentStatus::kMore;
  while (status == SegmentStatus::kMore) {
    status = reader_.ReadBlock(head);
  }
  if (status != SegmentStatus::kOk) {
    return Fail("a block's head", status);
  }
  // Its characters, and the style each run of them is in.
  std::string characters;
  std::vector<std::pair<std::size_t, std::uint16_t>> runs;  // Where each run ends, and its style.
  std::uint16_t style = 0;
  bool hasPicture = false;
  std::uint16_t picture = epub::kSegmentNoPicture;
  SegmentLength declaredHeight;
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
    if (item.kind == ItemKind::kStyle) {
      style = static_cast<std::uint16_t>(((item.style.bits & epub::kRunBold) != 0 ? 1U : 0U) |
                                         ((item.style.bits & epub::kRunItalic) != 0 ? 2U : 0U));
      continue;
    }
    if (item.kind == ItemKind::kText) {
      characters += item.text;
    } else if (item.kind == ItemKind::kPicture) {
      characters += kPictureByte;
      if (!hasPicture) {
        hasPicture = true;
        picture = item.picture;
        declaredHeight = item.height;
      }
    } else {
      continue;  // A link's start and end are nothing to a page.
    }
    if (!runs.empty() && runs.back().second == style) {
      runs.back().first = characters.size();
    } else {
      runs.emplace_back(characters.size(), style);
    }
  }
  placed_ = true;
  nextBlock_ = block + 1;

  const BlockStyle& said = head.style;
  const std::int64_t size = said.size;
  const std::int64_t pageWidth = geometry_.width;
  set.kind = head.kind;
  set.level = head.level;
  set.charactersBefore = head.charactersBefore;
  set.keepWithNext = (said.breaking & epub::kBreakKeepWithNext) != 0;
  set.spaceAfter = Held(Pixels(said.spaceAfter, size, pageWidth), 0, 65535);

  // How wide its lines are, and how high.
  const std::int64_t narrowest = pageWidth / 10;
  const std::uint16_t rest =
      Held(pageWidth - Quarters(said.marginLeft, size, pageWidth) - Quarters(said.marginRight, size, pageWidth),
           narrowest, pageWidth);
  const std::array<std::uint16_t, 1> first = {
      Held(rest - Quarters(said.indent, size, pageWidth), narrowest, pageWidth)};
  std::int64_t lineHeight = (kTextSize * size * kUsualLineHeight) / 1000000;
  if ((said.has & epub::kBlockLineHeight) != 0) {
    lineHeight = said.lineMultiple != 0 ? (kTextSize * size * said.lineMultiple) / 1000000
                                        : Pixels(said.lineHeight, size, pageWidth);
  }
  if (head.kind == static_cast<std::uint8_t>(epub::SegmentBlockKind::kPicture) && hasPicture) {
    // As high as the picture is, made to fit the page's width; or as it
    // is declared; or as high as a picture of which nothing is known.
    lineHeight = Pixels(declaredHeight, size, pageWidth);
    lineHeight = lineHeight > 0 ? lineHeight : kUnknownPicture;
    SegmentPicture sized;
    bool found = false;
    if (const SegmentStatus read = reader_.ReadPicture(picture, sized, found); read != SegmentStatus::kOk) {
      return Fail("a picture's size", read);
    }
    if (found && sized.sized && sized.width != 0 && sized.height != 0) {
      const std::int64_t widthInPixels = pageWidth / 4;
      lineHeight = sized.width > widthInPixels ? (sized.height * widthInPixels) / sized.width : sized.height;
    }
    lineHeight = std::min<std::int64_t>(lineHeight, geometry_.height);
  }
  const std::uint16_t height = Held(lineHeight, 1, 65535);
  const std::uint16_t gap =
      Held(std::clamp<std::int64_t>(Pixels(said.spaceBefore, size, pageWidth), 0, 65535) + spaceAbove, 0, 65535);

  // Its lines.
  std::vector<layout::LineBreaker::Node> nodes(layout::LineBreaker::kUsualNodes);
  std::vector<layout::ParagraphSetter::Record> records(layout::ParagraphSetter::kUsualRecords);
  layout::ParagraphSetter setter(nodes, records);
  setter.UseHyphenator(hyphenator_);
  Lines sink;
  const bool indented = (said.has & epub::kBlockIndent) != 0 && first[0] != rest;
  const layout::ParagraphSettings settings{
      .widths = {.first = indented ? std::span<const std::uint16_t>(first) : std::span<const std::uint16_t>(),
                 .rest = rest},
      .hyphenPenalty = 0};
  if (const layout::ParagraphStatus begun = setter.Begin(settings, face_, sink);
      begun != layout::ParagraphStatus::kOk) {
    error_ = std::string("setting a block: ") + layout::ParagraphStatusName(begun);
    return false;
  }
  std::size_t from = 0;
  for (const auto& [end, runStyle] : runs) {
    std::string_view text = std::string_view(characters).substr(from, end - from);
    while (!text.empty()) {
      text.remove_prefix(setter.AddText(text, runStyle));
    }
    from = end;
  }
  while (!setter.Finish()) {
  }
  ++blocksSet_;

  set.lines.clear();
  for (const layout::TextLine& line : sink.lines) {
    ChapterLine made;
    made.begin = line.begin;
    made.end = line.end;
    made.width = line.width;
    made.hyphen = line.hyphen;
    made.last = line.last;
    made.text = characters.substr(line.begin, line.end - line.begin);
    set.lines.push_back(std::move(made));
  }
  if (set.lines.empty()) {
    // A rule, or a block of nothing but spaces: one line, of nothing.
    ChapterLine none;
    none.last = true;
    set.lines.push_back(none);
  }
  const bool together = (said.breaking & epub::kBreakKeepTogether) != 0;
  for (std::size_t i = 0; i < set.lines.size(); ++i) {
    ChapterLine& line = set.lines[i];
    line.height = height;
    line.gap = i == 0 ? gap : std::uint16_t{0};
    line.keep = i == 0 ? keptWithBefore : together;
    line.starts = i == 0 && (block == 0 || (said.breaking & epub::kBreakPageBefore) != 0);
  }
  return true;
}

bool Pager::Block(std::uint32_t block, const SetBlock*& set) {
  auto found = blocks_.find(block);
  if (found == blocks_.end()) {
    SetBlock made;
    if (!ReadAndSet(block, made)) {
      return false;
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

layout::PageLine Pager::PageLineOf(const ChapterLine& line, LineRef ref) {
  tags_.push_back(ref);
  return {.height = line.height,
          .gapBefore = line.gap,
          .keepWithPrevious = line.keep,
          .startsPage = line.starts,
          .tag = static_cast<std::uint32_t>(tags_.size() - 1)};
}

bool Pager::Forward(LineRef first, Page& page) {
  page = Page{};
  page.first = first;
  tags_.clear();
  layout::PageFromTop top;
  if (const layout::PageStatus status = top.Begin(geometry_.height); status != layout::PageStatus::kOk) {
    error_ = std::string("a page: ") + layout::PageStatusName(status);
    return false;
  }
  // Lines are offered until one is refused or the chapter ends.
  std::vector<LineRef> offered;
  bool refused = false;
  for (LineRef at = first; at.block < reader_.Blocks() && !refused;) {
    const SetBlock* set = nullptr;
    if (!Block(at.block, set)) {
      return false;
    }
    if (at.line >= set->lines.size()) {
      at = {.block = at.block + 1, .line = 0};
      continue;
    }
    offered.push_back(at);
    refused = !top.Add(PageLineOf(set->lines[at.line], at));
    ++at.line;
  }
  const std::size_t lines = top.Lines();
  page.found = lines != 0;
  page.height = top.Height();
  page.lines.assign(offered.begin(), offered.begin() + static_cast<std::ptrdiff_t>(lines));
  page.next = lines < offered.size() ? offered[lines] : End();
  return true;
}

bool Pager::Back(LineRef before, Page& page) {
  page = Page{};
  page.filledDown = false;
  page.next = before;
  tags_.clear();
  // An entry for every line a page could hold, a line being a pixel high
  // at the least: so the chooser never runs out of them.
  std::vector<layout::PageFromBottom::Entry> entries(
      std::max<std::size_t>(layout::PageFromBottom::kUsualEntries, std::size_t{geometry_.height} + 1));
  layout::PageFromBottom bottom(entries);
  if (const layout::PageStatus status = bottom.Begin(geometry_.height); status != layout::PageStatus::kOk) {
    error_ = std::string("a page: ") + layout::PageStatusName(status);
    return false;
  }
  // The block the line is in, down to the line above it; then the block
  // above, and so on up, until no more are wanted or there are none.
  std::uint32_t block = before.block;
  std::uint32_t count = before.line;
  bool given = false;
  for (;;) {
    if (count == 0) {
      if (block == 0) {
        break;
      }
      --block;
      const SetBlock* above = nullptr;
      if (!Block(block, above)) {
        return false;
      }
      count = static_cast<std::uint32_t>(above->lines.size());
    }
    const SetBlock* set = nullptr;
    if (!Block(block, set)) {
      return false;
    }
    bottom.BeginBlock();
    for (std::uint32_t i = 0; i < count; ++i) {
      bottom.Add(PageLineOf(set->lines[i], {.block = block, .line = i}));
    }
    given = true;
    if (bottom.EndBlock()) {
      break;
    }
    count = 0;
  }
  if (!given || !bottom.Found()) {
    return true;  // Nothing is above the chapter's first line.
  }
  const LineRef first = tags_.at(bottom.FirstTag());
  if (bottom.AtPageStart()) {
    // It came to a line that starts a page: that line's page, filled down.
    return Forward(first, page);
  }
  page.found = true;
  page.first = first;
  page.cutShort = bottom.CutShort();
  page.height = bottom.Height();
  for (LineRef at = first; at != before;) {
    const SetBlock* set = nullptr;
    if (!Block(at.block, set)) {
      return false;
    }
    if (at.line >= set->lines.size()) {
      at = {.block = at.block + 1, .line = 0};
      continue;
    }
    page.lines.push_back(at);
    ++at.line;
  }
  return true;
}

bool Pager::Entering(std::uint32_t block, std::uint32_t into, Page& page) {
  LineRef asked;
  if (!LineHolding(block, into, asked)) {
    return false;
  }
  tags_.clear();
  layout::PageHolding holding;
  if (const layout::PageStatus status = holding.Begin(geometry_.height); status != layout::PageStatus::kOk) {
    error_ = std::string("a page: ") + layout::PageStatusName(status);
    return false;
  }
  // The block the line is in, down to the line; then the blocks above.
  std::uint32_t at = asked.block;
  std::uint32_t count = asked.line + 1;
  for (;;) {
    const SetBlock* set = nullptr;
    if (!Block(at, set)) {
      return false;
    }
    count = count == 0 ? static_cast<std::uint32_t>(set->lines.size()) : count;
    holding.BeginBlock();
    for (std::uint32_t i = 0; i < count; ++i) {
      holding.Add(PageLineOf(set->lines[i], {.block = at, .line = i}));
    }
    if (holding.EndBlock() || at == 0) {
      break;
    }
    --at;
    count = 0;
  }
  return Forward(holding.Found() ? tags_.at(holding.FirstTag()) : asked, page);
}

}  // namespace bn::tools
