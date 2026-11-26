#include "epub/document_blocks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/style_stack.h"
#include "epub/xml.h"

namespace bn::epub {

namespace {

// The flags of a level. The first group is the element's own; the second
// an element that is part of a line has from the one it is inside, and so
// they are those of the nearest element around that is a block; the third
// every element has from the one it is inside.
constexpr std::uint16_t kBlock = 1U << 0U;              // It begins and ends blocks.
constexpr std::uint16_t kAwaitsItem = 1U << 1U;         // An item of a list in which no block has begun yet.
constexpr std::uint16_t kAwaitsCell = 1U << 2U;         // A table's cell in which no block has begun yet.
constexpr std::uint16_t kHasHeight = 1U << 3U;          // The book gave it a height.
constexpr std::uint16_t kBottomFromBook = 1U << 4U;     // Its bottom margin is the book's.
constexpr std::uint16_t kPadBottomFromBook = 1U << 5U;  // Its bottom padding is the book's.
constexpr std::uint16_t kBreakAfter = 1U << 6U;         // A page ends after it.
constexpr auto kAwaits = static_cast<std::uint16_t>(kAwaitsItem | kAwaitsCell);
constexpr auto kOwn = static_cast<std::uint16_t>(std::uint32_t{kBlock} | kAwaits | kHasHeight | kBottomFromBook |
                                                 kPadBottomFromBook | kBreakAfter);

constexpr std::uint16_t kCentred = 1U << 7U;  // A width, and `auto` for both side margins.
constexpr std::uint16_t kKeepWithNext = 1U << 8U;
constexpr std::uint16_t kKeepWithNextFromBook = 1U << 9U;
constexpr std::uint16_t kKeepTogether = 1U << 10U;
constexpr auto kOfTheBlock =
    static_cast<std::uint16_t>(std::uint32_t{kCentred} | kKeepWithNext | kKeepWithNextFromBook | kKeepTogether);

constexpr std::uint16_t kLeftFromBook = 1U << 11U;   // Some part of what is to its left is the book's.
constexpr std::uint16_t kRightFromBook = 1U << 12U;  // And to its right.
constexpr std::uint16_t kFloated = 1U << 13U;        // It is, or is inside, a block that was floated.

constexpr std::int16_t Sum(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int16_t>(std::clamp<std::int32_t>(a + b, -32768, 32767));
}

// Two lengths added, each amount held to what 16 bits take.
constexpr StyleLength Plus(const StyleLength& a, const StyleLength& b) noexcept {
  return {.em = Sum(a.em, b.em), .px = Sum(a.px, b.px), .percent = Sum(a.percent, b.percent)};
}

// What a length comes to with 16 pixels to the em and a page 30 em wide,
// in 320ths of a pixel: for telling which of two margins is the larger,
// and for nothing else.
constexpr std::int32_t Measure(const StyleLength& length) noexcept {
  return (std::int32_t{length.em} * 80) + (std::int32_t{length.px} * 40) + (std::int32_t{length.percent} * 24);
}

char Small(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

// Whether an element's name is this one, which is written in small
// letters, in letters of either case.
bool Is(std::string_view name, std::string_view small) noexcept {
  return name.size() == small.size() &&
         std::equal(name.begin(), name.end(), small.begin(), [](char a, char b) { return Small(a) == b; });
}

// The level of a heading's name, `h1` to `h6`; nothing for any other.
std::uint8_t HeadingLevel(std::string_view name) noexcept {
  if (name.size() != 2 || Small(name[0]) != 'h' || name[1] < '1' || name[1] > '6') {
    return 0;
  }
  return static_cast<std::uint8_t>(name[1] - '0');
}

bool IsBlank(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }

// A whole number as an attribute has one: a sign or none, and one to
// nine digits.
bool Number(std::string_view text, std::int32_t& number) noexcept {
  const bool negative = !text.empty() && text.front() == '-';
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    text.remove_prefix(1);
  }
  if (text.empty() || text.size() > 9) {
    return false;
  }
  std::int32_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = (value * 10) + (ch - '0');
  }
  number = negative ? -value : value;
  return true;
}

}  // namespace

// What is kept of an open element beside what the StyleStack keeps of it.
struct DocumentBlocks::Level {
  // For a list, the number its next item takes; for an item, its own.
  std::int32_t number = 0;
  // The margins and the padding of it and of every block it is inside,
  // to each side.
  StyleLength left;
  StyleLength right;
  // Its own, which wait for it to close; and the height the book gave it,
  // which counts if it closes with nothing in it.
  StyleLength marginBottom;
  StyleLength paddingBottom;
  StyleLength height;
  StyleLength width;  // With kCentred.
  std::uint16_t flags = 0;
  // One more than the number of the nearest element around it that is a
  // block, and of the nearest that is a list, itself if it is one; 0 for
  // none.
  std::uint8_t container = 0;
  std::uint8_t list = 0;
  std::uint8_t heading = 0;  // Its own level, if it is a heading.
  std::uint8_t quotes = 0;   // How many quotations it is inside, itself among them.
  std::uint16_t spare = 0;
};

const char* BlockKindName(BlockKind kind) noexcept {
  switch (kind) {
    case BlockKind::kParagraph:
      return "paragraph";
    case BlockKind::kHeading:
      return "heading";
    case BlockKind::kListItem:
      return "list-item";
    case BlockKind::kRule:
      return "rule";
    case BlockKind::kTableCell:
      return "table-cell";
  }
  return "?";
}

std::span<std::uint8_t> DocumentBlocks::ForStyles(std::span<std::uint8_t> memory) noexcept {
  if (memory.size() < StyleStack::kLanguagesBytes + kLevelBytes) {
    return {};
  }
  const std::size_t levels = std::min((memory.size() - StyleStack::kLanguagesBytes) / kLevelBytes, kMaxLevels);
  return memory.first(StyleStack::kLanguagesBytes + (levels * StyleStack::kLevelBytes));
}

// Its own levels come after all of the StyleStack's.
DocumentBlocks::DocumentBlocks(std::span<std::uint8_t> memory) noexcept
    : stack_(ForStyles(memory)), memory_(memory.subspan(ForStyles(memory).size()).data()), levels_(stack_.Levels()) {
  static_assert(sizeof(Level) == kOwnLevelBytes, "a level is as large as the header says");
}

void DocumentBlocks::Begin() noexcept {
  stack_.Begin();
  open_ = false;
  record_ = {};
  space_ = {};
  spaceFromBook_ = false;
  above_ = {};
  below_ = {};
  aboveMeasure_ = 0;
  belowMeasure_ = 0;
  aboveFromBook_ = false;
  belowFromBook_ = false;
  zeroFromBook_ = false;
  breakPending_ = false;
  hiddenFrom_ = kNone;
  filled_ = 0;
  letterAt_ = kNone;
  letterFloated_ = false;
  floatAt_ = kNone;
  floatOpen_ = false;
  deepBlocks_ = 0;
  work_ = 0;
}

DocumentBlocks::Level DocumentBlocks::Load(std::size_t index) const noexcept {
  Level level;
  std::memcpy(&level, memory_ + (index * kOwnLevelBytes), kOwnLevelBytes);
  return level;
}

void DocumentBlocks::Store(std::size_t index, const Level& level) noexcept {
  std::memcpy(memory_ + (index * kOwnLevelBytes), &level, kOwnLevelBytes);
}

void DocumentBlocks::Join(const StyleLength& margin, bool fromBook) noexcept {
  const std::int32_t measure = Measure(margin);
  if (measure == 0) {
    zeroFromBook_ = zeroFromBook_ || fromBook;
  } else if (measure > 0 && measure > aboveMeasure_) {
    above_ = margin;
    aboveMeasure_ = measure;
    aboveFromBook_ = fromBook;
  } else if (measure < 0 && measure < belowMeasure_) {
    below_ = margin;
    belowMeasure_ = measure;
    belowFromBook_ = fromBook;
  }
}

void DocumentBlocks::Settle() noexcept {
  if (aboveMeasure_ == 0 && belowMeasure_ == 0) {
    // Margins of nothing met here, or none: the book's if any was.
    spaceFromBook_ = spaceFromBook_ || zeroFromBook_;
  }
  if (aboveMeasure_ != 0) {
    space_ = Plus(space_, above_);
    spaceFromBook_ = spaceFromBook_ || aboveFromBook_;
  }
  if (belowMeasure_ != 0) {
    space_ = Plus(space_, below_);
    spaceFromBook_ = spaceFromBook_ || belowFromBook_;
  }
  above_ = {};
  below_ = {};
  aboveMeasure_ = 0;
  belowMeasure_ = 0;
  aboveFromBook_ = false;
  belowFromBook_ = false;
  zeroFromBook_ = false;
}

void DocumentBlocks::Add(const StyleLength& length, bool fromBook) noexcept {
  Settle();
  space_ = Plus(space_, length);
  spaceFromBook_ = spaceFromBook_ || fromBook;
}

void DocumentBlocks::EndBlock(BlockChange& change) noexcept {
  change.ends = true;
  open_ = false;
  floatOpen_ = false;
}

void DocumentBlocks::BeginBlock(bool rule) noexcept {
  record_ = {};
  const std::size_t depth = stack_.Depth();
  const std::size_t kept = std::min(depth, levels_);
  const Level inner = kept == 0 ? Level{} : Load(kept - 1);

  if (rule) {
    record_.kind = BlockKind::kRule;
  } else {
    // Its kind is from the nearest element that names one; and every item
    // and cell it is inside has now had its first block.
    bool named = false;
    for (std::size_t i = kept; i-- > 0;) {
      ++work_;
      Level level = Load(i);
      if (!named && level.heading != 0) {
        record_.kind = BlockKind::kHeading;
        record_.headingLevel = level.heading;
        named = true;
      }
      if ((level.flags & kAwaitsItem) != 0) {
        if (!named) {
          record_.kind = BlockKind::kListItem;
          named = true;
        }
        if (!record_.hasMarker) {
          ElementStyle item;
          stack_.Passes(i, item);
          record_.hasMarker = true;
          record_.marker = item.marker;
          record_.markerFromBook = item.markerFromBook;
          record_.number = level.number;
        }
      }
      if ((level.flags & kAwaitsCell) != 0 && !named) {
        record_.kind = BlockKind::kTableCell;
        named = true;
      }
      if ((level.flags & kAwaits) != 0) {
        level.flags = static_cast<std::uint16_t>(level.flags & static_cast<std::uint16_t>(~kAwaits));
        Store(i, level);
      }
    }
  }

  // Its lines are as the nearest block around it has them.
  ElementStyle lines;
  if (inner.container != 0) {
    stack_.Passes(inner.container - 1U, lines);
  }
  record_.align = lines.align;
  record_.alignFromBook = lines.alignFromBook;
  record_.rightToLeft = lines.rightToLeft;
  record_.spacesAsWritten = lines.spacesAsWritten;
  record_.size = lines.text.size;
  record_.sizeFromBook = lines.text.sizeFromBook;
  record_.lineHeight = lines.lineHeight;
  record_.lineHeightFromBook = lines.lineHeightFromBook;
  record_.hasIndent = lines.hasIndent;
  record_.indent = lines.indent;
  record_.indentFromBook = lines.indentFromBook;

  record_.marginLeft = inner.left;
  record_.marginRight = inner.right;
  record_.marginLeftFromBook = (inner.flags & kLeftFromBook) != 0;
  record_.marginRightFromBook = (inner.flags & kRightFromBook) != 0;
  Settle();
  record_.spaceBefore = space_;
  record_.spaceBeforeFromBook = spaceFromBook_;
  space_ = {};
  spaceFromBook_ = false;

  record_.breakBefore = breakPending_;
  breakPending_ = false;
  record_.keepWithNext = (inner.flags & kKeepWithNext) != 0;
  record_.keepWithNextFromBook = (inner.flags & kKeepWithNextFromBook) != 0;
  record_.keepTogether = (inner.flags & kKeepTogether) != 0;
  record_.floated = (inner.flags & kFloated) != 0;
  record_.centred = (inner.flags & kCentred) != 0;
  record_.width = record_.centred ? inner.width : StyleLength{};
  record_.quoteDepth = inner.quotes;
  record_.listDepth = depth == 0 ? std::uint8_t{0} : stack_.Lists(depth - 1);

  if (!rule) {
    if (letterAt_ != kNone) {
      record_.hasFirstLetter = true;
      record_.firstLetter = letter_;
      record_.firstLetterFloated = letterFloated_;
      letterAt_ = kNone;
    }
    if (floatAt_ != kNone) {
      record_.beginsFloated = true;
      floatOpen_ = true;
    }
  }
  filled_ = depth;
  open_ = true;
}

bool DocumentBlocks::BeginIfNone() noexcept {
  if (open_) {
    return false;
  }
  BeginBlock(false);
  return true;
}

BlockChange DocumentBlocks::Open(std::string_view name, const XmlAttributes& attributes, const CssStyle& style,
                                 const CssStyle& firstLetter, ElementStyle& said) noexcept {
  BlockChange change;
  const std::size_t index = stack_.Depth();
  stack_.Open(name, attributes, style, firstLetter, said);
  work_ = stack_.Work();
  if (said.display == StyleDisplay::kNone) {
    if (hiddenFrom_ == kNone) {
      hiddenFrom_ = index;
    }
    return change;
  }

  // What it has from the element it is inside, the deepest kept if that
  // one is not.
  const std::size_t outsideKept = std::min(index, levels_);
  Level level = outsideKept == 0 ? Level{} : Load(outsideKept - 1);
  level.flags = static_cast<std::uint16_t>(level.flags & static_cast<std::uint16_t>(~kOwn));
  level.number = 0;
  level.marginBottom = {};
  level.paddingBottom = {};
  level.height = {};
  level.heading = HeadingLevel(name);
  const bool kept = index < levels_;

  if (Is(name, "br")) {
    // A line's end, whatever the book says a `br` is to a page.
    if (kept) {
      Store(index, level);
    }
    change.begins = BeginIfNone();
    change.lineEnds = true;
    return change;
  }

  const bool rule = Is(name, "hr");
  const bool block = said.display != StyleDisplay::kInline || rule;
  if (block) {
    if (open_) {
      EndBlock(change);
    }
    if (said.breakBefore) {
      breakPending_ = true;
    }
    Join(said.margin[kStyleTop], (said.marginFromBook & (1U << kStyleTop)) != 0);
    if (!said.padding[kStyleTop].IsZero()) {
      Add(said.padding[kStyleTop], (said.paddingFromBook & (1U << kStyleTop)) != 0);
    }

    level.container = static_cast<std::uint8_t>(index + 1);
    level.left = Plus(Plus(level.left, said.margin[kStyleLeft]), said.padding[kStyleLeft]);
    level.right = Plus(Plus(level.right, said.margin[kStyleRight]), said.padding[kStyleRight]);
    const std::uint32_t fromBook = std::uint32_t{said.marginFromBook} | said.paddingFromBook;
    // What it is now is its own, and no longer that of the block around it.
    std::uint32_t flags = (std::uint32_t{level.flags} & ~std::uint32_t{kOfTheBlock}) | kBlock;
    flags |= (fromBook & (1U << kStyleLeft)) != 0 ? kLeftFromBook : 0U;
    flags |= (fromBook & (1U << kStyleRight)) != 0 ? kRightFromBook : 0U;
    level.marginBottom = said.margin[kStyleBottom];
    level.paddingBottom = said.padding[kStyleBottom];
    flags |= (said.marginFromBook & (1U << kStyleBottom)) != 0 ? kBottomFromBook : 0U;
    flags |= (said.paddingFromBook & (1U << kStyleBottom)) != 0 ? kPadBottomFromBook : 0U;
    if (said.hasHeight) {
      flags |= kHasHeight;
      level.height = said.height;
    }
    level.width = {};
    if (said.hasWidth && said.marginLeftAuto && said.marginRightAuto) {
      flags |= kCentred;
      level.width = said.width;
    }
    flags |= said.keepWithNext ? kKeepWithNext : 0U;
    flags |= said.keepWithNextFromBook ? kKeepWithNextFromBook : 0U;
    flags |= said.keepTogether ? kKeepTogether : 0U;
    flags |= said.floated != StyleFloat::kNone ? kFloated : 0U;
    flags |= said.breakAfter ? kBreakAfter : 0U;
    flags |= Is(name, "td") || Is(name, "th") ? kAwaitsCell : 0U;
    level.flags = static_cast<std::uint16_t>(flags);
    if (Is(name, "blockquote") && level.quotes != 255) {
      ++level.quotes;
    }
    if (said.hasFirstLetter) {
      letterAt_ = index;
      letter_ = said.firstLetter;
      letterFloated_ = said.firstLetterFloated;
    }
  } else if (said.floated != StyleFloat::kNone && !open_ && floatAt_ == kNone) {
    floatAt_ = index;
  }

  if (kept) {
    // A list counts its items from its `start`, and an item has the next
    // number of its list, or the one its own `value` says.
    const bool list = stack_.Lists(index) != (index == 0 ? 0 : stack_.Lists(index - 1));
    const bool item = said.display == StyleDisplay::kListItem;
    std::int32_t asked = 0;
    bool has = false;
    if (list || item) {
      const std::string_view wanted = list ? "start" : "value";
      for (std::size_t i = 0; i < attributes.Count() && !has; ++i) {
        ++work_;
        has = attributes.Name(i) == wanted && Number(attributes.Value(i), asked);
      }
    }
    if (list) {
      level.list = static_cast<std::uint8_t>(index + 1);
      level.number = has ? asked : 1;
    } else if (item) {
      level.flags = static_cast<std::uint16_t>(level.flags | kAwaitsItem);
      level.number = 1;
      if (level.list != 0) {
        Level around = Load(level.list - 1U);
        level.number = has ? asked : around.number;
        around.number = level.number == std::numeric_limits<std::int32_t>::max() ? level.number : level.number + 1;
        Store(level.list - 1U, around);
      } else if (has) {
        level.number = asked;
      }
    }
    Store(index, level);
  } else if (block && index - levels_ < 64) {
    deepBlocks_ |= std::uint64_t{1} << (index - levels_);
  }

  if (rule) {
    BeginBlock(true);
    change.begins = true;
  }
  return change;
}

BlockChange DocumentBlocks::Close() noexcept {
  BlockChange change;
  work_ = 0;
  const std::size_t depth = stack_.Depth();
  if (depth == 0) {
    return change;
  }
  const std::size_t index = depth - 1;
  if (hiddenFrom_ != kNone) {
    stack_.Close();
    if (index == hiddenFrom_) {
      hiddenFrom_ = kNone;
    }
    return change;
  }

  if (index < levels_) {
    const Level level = Load(index);
    if ((level.flags & kBlock) != 0) {
      if (open_) {
        EndBlock(change);
      }
      if (index >= filled_ && (level.flags & kHasHeight) != 0) {
        // Nothing is in it, and the book gave it a height: a space.
        Add(level.height, true);
        filled_ = index;
      }
      if (!level.paddingBottom.IsZero()) {
        Add(level.paddingBottom, (level.flags & kPadBottomFromBook) != 0);
      }
      Join(level.marginBottom, (level.flags & kBottomFromBook) != 0);
      if ((level.flags & kBreakAfter) != 0) {
        breakPending_ = true;
      }
    }
  } else if (index - levels_ < 64 && ((deepBlocks_ >> (index - levels_)) & 1U) != 0) {
    deepBlocks_ &= ~(std::uint64_t{1} << (index - levels_));
    if (open_) {
      EndBlock(change);
    }
  }

  if (floatAt_ == index) {
    change.floatEnds = floatOpen_;
    floatAt_ = kNone;
    floatOpen_ = false;
  }
  if (letterAt_ == index) {
    letterAt_ = kNone;
  }
  stack_.Close();
  filled_ = std::min(filled_, index);
  return change;
}

BlockChange DocumentBlocks::Text(std::string_view text) noexcept {
  BlockChange change;
  work_ = 0;
  if (hiddenFrom_ != kNone) {
    return change;
  }
  bool blank = true;
  for (std::size_t i = 0; i < text.size() && blank; ++i) {
    ++work_;
    blank = IsBlank(text[i]);
  }
  if (blank) {
    // White space begins nothing; in a block it is part of it.
    change.shown = open_;
    return change;
  }
  change.begins = BeginIfNone();
  change.shown = true;
  return change;
}

BlockChange DocumentBlocks::Picture() noexcept {
  BlockChange change;
  work_ = 0;
  if (hiddenFrom_ == kNone) {
    change.begins = BeginIfNone();
    change.shown = true;
  }
  return change;
}

TextStyle DocumentBlocks::TextNow() const noexcept {
  ElementStyle said;
  if (stack_.Depth() != 0) {
    stack_.Passes(stack_.Depth() - 1, said);
  }
  return said.text;
}

}  // namespace bn::epub
