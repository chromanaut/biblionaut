#include "layout/line_breaker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bn::layout::BreakStatus;
using bn::layout::BreakStatusName;
using bn::layout::Line;
using bn::layout::LineBreaker;
using bn::layout::LineSink;
using bn::layout::LineWidths;

// ---- A paragraph, as the tests write one down ----

enum class Kind : std::uint8_t { kBox, kSpace, kBreak, kForced };

struct Item {
  Kind kind = Kind::kBox;
  std::uint16_t width = 0;
  std::uint32_t penalty = 0;
  std::string text;  // What it looks like, for the tests that read their lines back as text.
};

Item Box(std::uint16_t width) { return {.kind = Kind::kBox, .width = width, .penalty = 0, .text = {}}; }
Item Space(std::uint16_t width) { return {.kind = Kind::kSpace, .width = width, .penalty = 0, .text = " "}; }
Item Break(std::uint16_t width, std::uint32_t penalty) {
  return {.kind = Kind::kBreak, .width = width, .penalty = penalty, .text = {}};
}
Item Forced() { return {.kind = Kind::kForced, .width = 0, .penalty = 0, .text = {}}; }

using Items = std::vector<Item>;
using Widths = std::vector<std::uint16_t>;

// A paragraph from a line of text, every character one unit wide. A run of
// letters is a box and a space is a space; '~' is a place inside a word
// where a line may end, showing a hyphen one unit wide, at `penalty`; '|'
// is a forced break.
Items FromText(std::string_view text, std::uint32_t penalty = 0) {
  Items items;
  std::string word;
  const auto endWord = [&] {
    if (!word.empty()) {
      Item box = Box(static_cast<std::uint16_t>(word.size()));
      box.text = word;
      items.push_back(box);
      word.clear();
    }
  };
  for (const char ch : text) {
    if (ch == ' ') {
      endWord();
      items.push_back(Space(1));
    } else if (ch == '~') {
      endWord();
      items.push_back(Break(1, penalty));
    } else if (ch == '|') {
      endWord();
      items.push_back(Forced());
    } else {
      word += ch;
    }
  }
  endWord();
  return items;
}

class Collector final : public LineSink {
 public:
  void OnLine(const Line& line) noexcept override { lines.push_back(line); }

  std::vector<Line> lines;
};

// What came of breaking a paragraph.
struct Broken {
  BreakStatus begun = BreakStatus::kNotStarted;
  BreakStatus finished = BreakStatus::kNotStarted;
  std::vector<Line> lines;
  std::uint32_t settledEarly = 0;
  std::uint32_t overfullLines = 0;
  std::size_t mostNodes = 0;
  std::uint32_t furthestBehind = 0;  // The most items there ever were that were not yet on a line given out.
  bool pendingInOrder = true;        // Pending() never went back, and never passed the items given.
};

void Add(LineBreaker& breaker, const Item& item) {
  switch (item.kind) {
    case Kind::kBox:
      breaker.AddBox(item.width);
      break;
    case Kind::kSpace:
      breaker.AddSpace(item.width);
      break;
    case Kind::kBreak:
      breaker.AddBreak(item.width, item.penalty);
      break;
    case Kind::kForced:
      breaker.AddForcedBreak();
      break;
  }
}

Broken BreakWith(const Items& items, const Widths& first, std::uint16_t rest, std::size_t nodeCount = 16384) {
  std::vector<LineBreaker::Node> nodes(nodeCount);
  LineBreaker breaker(nodes);
  Collector collector;
  Broken broken;
  broken.begun = breaker.Begin(LineWidths{.first = first, .rest = rest}, collector);
  std::uint32_t pendingBefore = 0;
  for (const Item& item : items) {
    Add(breaker, item);
    const std::uint32_t pending = breaker.Pending();
    broken.pendingInOrder = broken.pendingInOrder && pending >= pendingBefore && pending <= breaker.Items();
    broken.furthestBehind = std::max(broken.furthestBehind, breaker.Items() - pending);
    pendingBefore = pending;
  }
  broken.finished = breaker.Finish();
  broken.pendingInOrder = broken.pendingInOrder && breaker.Pending() == breaker.Items();
  broken.lines = collector.lines;
  broken.settledEarly = breaker.SettledEarly();
  broken.overfullLines = breaker.OverfullLines();
  broken.mostNodes = breaker.MostNodes();
  return broken;
}

// The lines of a paragraph made by FromText(), as text: a hyphen where a
// line ends inside a word.
std::vector<std::string> AsText(const Items& items, const std::vector<Line>& lines) {
  std::vector<std::string> out;
  for (const Line& line : lines) {
    std::string text;
    for (std::uint32_t i = line.firstItem; i < line.endItem && i < items.size(); ++i) {
      text += items[i].text;
    }
    if (line.hyphenated) {
      text += '-';
    }
    out.push_back(text);
  }
  return out;
}

std::vector<std::string> LinesOf(std::string_view text, const Widths& first, std::uint16_t rest,
                                 std::uint32_t penalty = 0) {
  const Items items = FromText(text, penalty);
  return AsText(items, BreakWith(items, first, rest).lines);
}

using Text = std::vector<std::string>;

// ---- What any breaking of a paragraph has to be ----

std::uint16_t Allowed(const Widths& first, std::uint16_t rest, std::size_t line) {
  return line < first.size() ? first[line] : rest;
}

// The width of what is on a line, worked out from the items themselves.
std::uint32_t WidthOn(const Items& items, const Line& line) {
  std::uint32_t width = 0;
  for (std::uint32_t i = line.firstItem; i < line.endItem; ++i) {
    if (items[i].kind == Kind::kBox || items[i].kind == Kind::kSpace) {
      width += items[i].width;
    }
  }
  if (line.hyphenated) {
    width += items[line.endItem].width;
  }
  return width;
}

// Checks that the lines hold every box once, in order, that each says truly
// how wide it is and how it ends, and that each fits unless it says it does
// not.
void ExpectSound(const Items& items, const std::vector<Line>& lines, const Widths& first, std::uint16_t rest) {
  std::uint32_t at = 0;
  for (std::size_t n = 0; n < lines.size(); ++n) {
    const Line& line = lines[n];
    ASSERT_EQ(line.number, n);
    ASSERT_LE(at, line.firstItem);
    ASSERT_LE(line.firstItem, line.endItem);
    ASSERT_LE(line.endItem, items.size());
    for (std::uint32_t i = at; i < line.firstItem; ++i) {
      ASSERT_NE(items[i].kind, Kind::kBox) << "item " << i << " is on no line";
    }
    ASSERT_EQ(line.width, WidthOn(items, line)) << "line " << n;
    const std::uint16_t allowed = Allowed(first, rest, n);
    if (line.overfull) {
      EXPECT_GT(line.width, allowed) << "line " << n;
    } else {
      EXPECT_LE(line.width, allowed) << "line " << n;
    }
    if (line.last) {
      EXPECT_FALSE(line.hyphenated);
    } else {
      ASSERT_LT(line.endItem, items.size());
      const Kind kind = items[line.endItem].kind;
      EXPECT_TRUE(kind == Kind::kSpace || kind == Kind::kBreak) << "line " << n;
      EXPECT_EQ(line.hyphenated, kind == Kind::kBreak) << "line " << n;
      EXPECT_GT(line.endItem, line.firstItem) << "line " << n << " is empty";
    }
    at = line.endItem;
  }
  for (std::size_t i = at; i < items.size(); ++i) {
    ASSERT_NE(items[i].kind, Kind::kBox) << "item " << i << " is on no line";
  }
}

// What a set of lines costs, by the rule the header gives.
std::uint64_t CostOf(const Items& items, const std::vector<Line>& lines, const Widths& first, std::uint16_t rest) {
  std::uint64_t cost = 0;
  for (std::size_t n = 0; n < lines.size(); ++n) {
    const Line& line = lines[n];
    const std::uint64_t allowed = Allowed(first, rest, n);
    if (line.width > allowed) {
      const std::uint64_t over = line.width - allowed;
      cost += LineBreaker::kOverfullCost + over * over;
    } else if (!line.last) {
      const std::uint64_t slack = allowed - line.width;
      cost += slack * slack;
    }
    if (line.hyphenated) {
      cost += items[line.endItem].penalty;
    }
  }
  return cost;
}

// ---- Paragraphs made up by the tests, and other ways of breaking them ----
//
// The paragraphs are in a plain form: boxes with one space, one break or one
// forced break between them, and a box at each end. Then a line can end at
// every space and every break, has to end at every forced break, and what
// is on a line is simply the items between two of those. Every word is
// narrow enough for every line, so there is always a way that fits.

// Numbers that come out the same on every machine, which the standard
// library's distributions do not promise.
class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}

  // A number from 0 up to, and not including, `below`.
  std::uint32_t Below(std::uint32_t below) {
    state_ = (state_ * 1664525U) + 1013904223U;
    return (state_ >> 8U) % below;
  }
  std::uint32_t Between(std::uint32_t low, std::uint32_t high) { return low + Below(high - low + 1); }

 private:
  std::uint32_t state_;
};

Items MadeUp(Dice& dice, std::size_t words, std::uint32_t widestWord, bool withForcedBreaks) {
  Items items;
  for (std::size_t word = 0; word < words; ++word) {
    if (word > 0) {
      if (withForcedBreaks && dice.Below(20) == 0) {
        items.push_back(Forced());
      } else {
        items.push_back(Space(static_cast<std::uint16_t>(dice.Between(1, 3))));
      }
    }
    std::uint32_t left = dice.Between(1, widestWord);
    const std::uint32_t pieces = std::min(left, dice.Between(1, 3));
    for (std::uint32_t piece = 0; piece < pieces; ++piece) {
      if (piece > 0) {
        const std::uint32_t penalty = dice.Below(4) == 0 ? 0 : dice.Between(1, 300);
        items.push_back(Break(static_cast<std::uint16_t>(dice.Below(3)), penalty));
      }
      const std::uint32_t piecesAfter = pieces - piece - 1;
      const std::uint32_t width = piecesAfter == 0 ? left : dice.Between(1, left - piecesAfter);
      items.push_back(Box(static_cast<std::uint16_t>(width)));
      left -= width;
    }
  }
  return items;
}

Widths MadeUpWidths(Dice& dice, std::uint32_t narrowest, std::uint32_t widest) {
  Widths first(dice.Below(4));
  for (std::uint16_t& width : first) {
    width = static_cast<std::uint16_t>(dice.Between(narrowest, widest));
  }
  return first;
}

bool EndsALine(const Item& item) { return item.kind != Kind::kBox; }

// The least any breaking can cost, found by trying every one of them: each
// space and each break either ends a line or does not.
std::optional<std::uint64_t> CheapestOfEveryWay(const Items& items, const Widths& first, std::uint16_t rest) {
  std::vector<std::size_t> optional;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].kind == Kind::kSpace || items[i].kind == Kind::kBreak) {
      optional.push_back(i);
    }
  }
  std::optional<std::uint64_t> cheapest;
  for (std::uint32_t chosen = 0; chosen < (1U << optional.size()); ++chosen) {
    std::vector<bool> ends(items.size() + 1, false);
    ends[items.size()] = true;
    for (std::size_t i = 0; i < items.size(); ++i) {
      ends[i] = items[i].kind == Kind::kForced;
    }
    for (std::size_t bit = 0; bit < optional.size(); ++bit) {
      if (((chosen >> bit) & 1U) != 0) {
        ends[optional[bit]] = true;
      }
    }
    std::uint64_t cost = 0;
    std::uint64_t width = 0;
    std::size_t line = 0;
    bool fits = true;
    for (std::size_t i = 0; i <= items.size() && fits; ++i) {
      if (!ends[i]) {
        // A box, or a space or a break that no line ends at: and such a
        // break shows nothing.
        if (items[i].kind != Kind::kBreak) {
          width += items[i].width;
        }
        continue;
      }
      const bool atBreak = i < items.size() && items[i].kind == Kind::kBreak;
      const bool last = i == items.size() || items[i].kind == Kind::kForced;
      if (atBreak) {
        width += items[i].width;
      }
      const std::uint64_t allowed = Allowed(first, rest, line);
      fits = width <= allowed;
      if (fits && !last) {
        cost += (allowed - width) * (allowed - width);
      }
      if (atBreak) {
        cost += items[i].penalty;
      }
      width = 0;
      ++line;
    }
    if (fits && (!cheapest || cost < *cheapest)) {
      cheapest = cost;
    }
  }
  return cheapest;
}

// The same, for paragraphs too long to try every way: worked forwards from
// each place a line could begin to every place it could then end, keeping
// for each place and each count of lines before it the least it has cost to
// get there.
std::uint64_t CheapestWorkedForwards(const Items& items, const Widths& first, std::uint16_t rest) {
  constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
  const std::size_t counts = first.size() + 1;
  // costs[i][c]: the least to have a line begin at item i with c lines
  // before it (c stops counting once the first widths are used up).
  std::vector<std::vector<std::uint64_t>> costs(items.size() + 2, std::vector<std::uint64_t>(counts, kNever));
  costs[0][0] = 0;
  std::vector<std::uint64_t> atEnd(counts, kNever);
  for (std::size_t begin = 0; begin < items.size(); ++begin) {
    if (begin > 0 && !EndsALine(items[begin - 1])) {
      continue;  // No line begins here.
    }
    for (std::size_t count = 0; count < counts; ++count) {
      const std::uint64_t soFar = costs[begin][count];
      if (soFar == kNever) {
        continue;
      }
      const std::uint64_t allowed = Allowed(first, rest, count);
      const std::size_t next = std::min(count + 1, first.size());
      std::uint64_t width = 0;
      for (std::size_t end = begin; end <= items.size(); ++end) {
        const bool atEndOfAll = end == items.size();
        if (!atEndOfAll && !EndsALine(items[end])) {
          width += items[end].width;
          if (width > allowed) {
            break;
          }
          continue;
        }
        const bool atBreak = !atEndOfAll && items[end].kind == Kind::kBreak;
        const bool last = atEndOfAll || items[end].kind == Kind::kForced;
        const std::uint64_t shown = width + (atBreak ? items[end].width : 0);
        if (shown <= allowed) {
          const std::uint64_t cost =
              soFar + (last ? 0 : (allowed - shown) * (allowed - shown)) + (atBreak ? items[end].penalty : 0);
          std::uint64_t& into = atEndOfAll ? atEnd[next] : costs[end + 1][next];
          into = std::min(into, cost);
        }
        if (last) {
          break;
        }
        if (items[end].kind == Kind::kSpace) {
          width += items[end].width;
          if (width > allowed) {
            break;
          }
        }
      }
    }
  }
  return *std::min_element(atEnd.begin(), atEnd.end());
}

// ---- Tests ----

TEST(LineBreaker, PutsAParagraphThatFitsOnOneLine) {
  const Items items = FromText("one two three");
  const Broken broken = BreakWith(items, {}, 20);

  EXPECT_EQ(broken.begun, BreakStatus::kOk);
  EXPECT_EQ(broken.finished, BreakStatus::kOk);
  ASSERT_EQ(broken.lines.size(), 1U);
  EXPECT_EQ(broken.lines[0].firstItem, 0U);
  EXPECT_EQ(broken.lines[0].endItem, 5U);
  EXPECT_EQ(broken.lines[0].width, 13U);
  EXPECT_EQ(broken.lines[0].number, 0U);
  EXPECT_TRUE(broken.lines[0].last);
  EXPECT_FALSE(broken.lines[0].hyphenated);
  EXPECT_FALSE(broken.lines[0].overfull);
}

TEST(LineBreaker, EndsLinesAtSpacesAndLeavesThoseSpacesOut) {
  const Items items = FromText("aaaa bbbb cccc dddd");
  const Broken broken = BreakWith(items, {}, 9);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aaaa bbbb", "cccc dddd"}));
  ASSERT_EQ(broken.lines.size(), 2U);
  // The space the first line ended at is item 3: it is on neither line.
  EXPECT_EQ(broken.lines[0].endItem, 3U);
  EXPECT_EQ(broken.lines[1].firstItem, 4U);
  EXPECT_EQ(broken.lines[0].width, 9U);
  EXPECT_FALSE(broken.lines[0].last);
  EXPECT_TRUE(broken.lines[1].last);
}

TEST(LineBreaker, TakesAWordDownEarlyToEvenOutTheLines) {
  // Filling each line as full as it goes gives "aaa bb", "cc", "ddddd": a
  // gap of four after "cc". Taking "bb" down leaves gaps of three and one.
  EXPECT_EQ(LinesOf("aaa bb cc ddddd", {}, 6), (Text{"aaa", "bb cc", "ddddd"}));
}

TEST(LineBreaker, LetsTheLastLineBeShort) {
  // "aaaaaa bbb" fills a line exactly and leaves one letter for the last.
  // That costs nothing, where evening the two lines out would.
  EXPECT_EQ(LinesOf("aaaaaa bbb c", {}, 10), (Text{"aaaaaa bbb", "c"}));
}

TEST(LineBreaker, GivesEachOfTheFirstLinesItsOwnWidth) {
  // Two narrow lines, as beside a large first letter, and then the full
  // width.
  EXPECT_EQ(LinesOf("aa bb cc dd ee ff gg", {4, 4}, 10), (Text{"aa", "bb", "cc dd ee", "ff gg"}));
  // A first line that is shorter, as with an indent.
  EXPECT_EQ(LinesOf("aaa bbb ccc ddd eee", {5}, 7), (Text{"aaa", "bbb ccc", "ddd eee"}));
  // And one that is longer than the rest.
  EXPECT_EQ(LinesOf("aaa bbb ccc ddd eee", {11}, 7), (Text{"aaa bbb ccc", "ddd eee"}));
}

TEST(LineBreaker, EndsALineInsideAWordOnlyWhenThatIsWorthItsPenalty) {
  // Ending the first line inside the second word fills it exactly; not
  // doing so leaves a gap of six, which costs 36.
  EXPECT_EQ(LinesOf("aaaa bbbb~cccc", {}, 10, 10), (Text{"aaaa bbbb-", "cccc"}));
  EXPECT_EQ(LinesOf("aaaa bbbb~cccc", {}, 10, 100), (Text{"aaaa", "bbbbcccc"}));
}

TEST(LineBreaker, ShowsTheHyphenAndCountsItsWidth) {
  const Items items = FromText("aaaa bbbb~cccc", 10);
  const Broken broken = BreakWith(items, {}, 10);

  ASSERT_EQ(broken.lines.size(), 2U);
  EXPECT_TRUE(broken.lines[0].hyphenated);
  EXPECT_EQ(broken.lines[0].endItem, 3U);  // The break.
  EXPECT_EQ(broken.lines[0].width, 10U);   // Nine of letters and a space, and the hyphen.
  EXPECT_EQ(broken.lines[1].firstItem, 4U);
  EXPECT_FALSE(broken.lines[1].hyphenated);

  // With no room for the hyphen the line cannot end there.
  EXPECT_EQ(LinesOf("aaaa bbbb~cccc", {}, 9, 0), (Text{"aaaa", "bbbbcccc"}));
}

TEST(LineBreaker, KeepsABreakNoLineEndsAtAmongTheItemsOfItsLine) {
  const Items items = FromText("aa~bb cc~dd");
  const Broken broken = BreakWith(items, {}, 20);

  ASSERT_EQ(broken.lines.size(), 1U);
  EXPECT_EQ(broken.lines[0].firstItem, 0U);
  EXPECT_EQ(broken.lines[0].endItem, 7U);
  EXPECT_EQ(broken.lines[0].width, 9U);  // No hyphen is counted where none is shown.
  EXPECT_EQ(AsText(items, broken.lines), (Text{"aabb ccdd"}));
}

TEST(LineBreaker, EndsALineWhereItIsMadeTo) {
  const Items items = FromText("aa bb|cc dd");
  const Broken broken = BreakWith(items, {}, 20);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aa bb", "cc dd"}));
  ASSERT_EQ(broken.lines.size(), 2U);
  EXPECT_TRUE(broken.lines[0].last);  // It is not to be spread out, though the paragraph goes on.
  EXPECT_EQ(broken.lines[0].endItem, 3U);
  EXPECT_EQ(broken.lines[1].firstItem, 4U);
}

TEST(LineBreaker, DoesNotCountTheGapBeforeAForcedBreak) {
  // The line that ends at the forced break is seven short, and that costs
  // nothing: it is a last line.
  EXPECT_EQ(LinesOf("aa|bbbb cccc dddd", {}, 9), (Text{"aa", "bbbb cccc", "dddd"}));
}

TEST(LineBreaker, LeavesAnEmptyLineBetweenTwoForcedBreaks) {
  const Items items = FromText("aa||bb");
  const Broken broken = BreakWith(items, {}, 20);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aa", "", "bb"}));
  ASSERT_EQ(broken.lines.size(), 3U);
  EXPECT_EQ(broken.lines[1].firstItem, broken.lines[1].endItem);
  EXPECT_EQ(broken.lines[1].width, 0U);
  EXPECT_TRUE(broken.lines[1].last);
}

TEST(LineBreaker, GivesNoLineAfterAForcedBreakThatNothingFollows) {
  EXPECT_EQ(LinesOf("aa bb|", {}, 20), (Text{"aa bb"}));
  EXPECT_EQ(LinesOf("aa bb| ", {}, 20), (Text{"aa bb"}));
}

TEST(LineBreaker, DropsSpacesAtTheStartAfterAForcedBreakAndAtTheEnd) {
  const Items items = FromText("  aa bb | cc  ");
  const Broken broken = BreakWith(items, {}, 20);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aa bb", "cc"}));
  ASSERT_EQ(broken.lines.size(), 2U);
  EXPECT_EQ(broken.lines[0].firstItem, 2U);  // After the two spaces.
  EXPECT_EQ(broken.lines[0].endItem, 5U);    // The space before the forced break.
  EXPECT_EQ(broken.lines[0].width, 5U);
  EXPECT_EQ(broken.lines[1].firstItem, 8U);  // After the space that follows it.
  EXPECT_EQ(broken.lines[1].endItem, 9U);    // The first of the spaces at the end.
  EXPECT_EQ(broken.lines[1].width, 2U);
  EXPECT_TRUE(broken.pendingInOrder);
}

TEST(LineBreaker, TakesSpacesInARowAsOne) {
  const Items apart = FromText("aa   bb");
  const Broken broken = BreakWith(apart, {}, 4);
  EXPECT_EQ(AsText(apart, broken.lines), (Text{"aa", "bb"}));
  ASSERT_EQ(broken.lines.size(), 2U);
  EXPECT_EQ(broken.lines[0].endItem, 1U);    // The first of the three.
  EXPECT_EQ(broken.lines[1].firstItem, 4U);  // After the last of them.

  // On one line they are as wide as all three.
  const Broken together = BreakWith(apart, {}, 7);
  ASSERT_EQ(together.lines.size(), 1U);
  EXPECT_EQ(together.lines[0].width, 7U);
}

TEST(LineBreaker, PutsAStretchTooWideForAnyLineOnALineOfItsOwn) {
  const Items items = FromText("aa bbbbbbbbbbbb cc");
  const Broken broken = BreakWith(items, {}, 6);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aa", "bbbbbbbbbbbb", "cc"}));
  ASSERT_EQ(broken.lines.size(), 3U);
  EXPECT_FALSE(broken.lines[0].overfull);
  EXPECT_TRUE(broken.lines[1].overfull);
  EXPECT_EQ(broken.lines[1].width, 12U);
  EXPECT_FALSE(broken.lines[2].overfull);
  EXPECT_EQ(broken.overfullLines, 1U);
  ExpectSound(items, broken.lines, {}, 6);
}

TEST(LineBreaker, BreaksAStretchThatIsTooWideWhereItCan) {
  // The long word has two places to end a line, and each piece fits.
  EXPECT_EQ(LinesOf("aaaaa~bbbbb~ccccc", {}, 6), (Text{"aaaaa-", "bbbbb-", "ccccc"}));
  // The first piece does not fit with its hyphen, so it is too wide, and
  // goes no further than it must.
  const Items items = FromText("aaaaaaa~bbb");
  const Broken broken = BreakWith(items, {}, 6);
  EXPECT_EQ(AsText(items, broken.lines), (Text{"aaaaaaa-", "bbb"}));
  EXPECT_EQ(broken.overfullLines, 1U);
}

TEST(LineBreaker, TakesAParagraphThatIsAllOneStretchTooWide) {
  const Items items = FromText("aaaaaaaaaaaa");
  const Broken broken = BreakWith(items, {4}, 6);

  ASSERT_EQ(broken.lines.size(), 1U);
  EXPECT_TRUE(broken.lines[0].overfull);
  EXPECT_TRUE(broken.lines[0].last);
  EXPECT_EQ(broken.lines[0].width, 12U);
}

TEST(LineBreaker, IgnoresABreakWithNoBoxBeforeItOrAfterIt) {
  // At the start, before a space, after a space, twice running, and at the
  // end: none of them is a place a line ends.
  const Items items = FromText("~aa~ ~bb~~cc~");
  const Broken broken = BreakWith(items, {}, 3);

  EXPECT_EQ(AsText(items, broken.lines), (Text{"aa", "bb-", "cc"}));
  ExpectSound(items, broken.lines, {}, 3);
}

TEST(LineBreaker, GivesNoLinesForAParagraphWithNoBoxes) {
  for (const std::string_view text : {"", "   ", "~", " ~ "}) {
    const Items items = FromText(text);
    const Broken broken = BreakWith(items, {}, 10);
    EXPECT_EQ(broken.finished, BreakStatus::kOk) << '"' << text << '"';
    EXPECT_TRUE(broken.lines.empty()) << '"' << text << '"';
    EXPECT_TRUE(broken.pendingInOrder) << '"' << text << '"';
  }
}

TEST(LineBreaker, NumbersTheLinesAndSaysWhichEndTheParagraph) {
  const Items items = FromText("aa bb cc dd ee ff gg hh");
  const Broken broken = BreakWith(items, {}, 5);

  ASSERT_EQ(broken.lines.size(), 4U);
  for (std::size_t n = 0; n < broken.lines.size(); ++n) {
    EXPECT_EQ(broken.lines[n].number, n);
    EXPECT_EQ(broken.lines[n].last, n == 3);
  }
}

TEST(LineBreaker, RefusesTooFewNodesAndWidthsItCannotUse) {
  Collector collector;
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};

  const std::span<LineBreaker::Node> oneShort = std::span<LineBreaker::Node>(nodes).first(LineBreaker::kMinNodes - 1);
  LineBreaker tooFew(oneShort);
  EXPECT_EQ(tooFew.Begin(LineWidths{.first = {}, .rest = 10}, collector), BreakStatus::kNoMemory);
  EXPECT_EQ(tooFew.Status(), BreakStatus::kNoMemory);

  LineBreaker breaker(nodes);
  EXPECT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 0}, collector), BreakStatus::kBadWidths);
  EXPECT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = LineBreaker::kMaxWidth + 1}, collector),
            BreakStatus::kBadWidths);
  const std::array<std::uint16_t, 2> withNone = {10, 0};
  EXPECT_EQ(breaker.Begin(LineWidths{.first = withNone, .rest = 10}, collector), BreakStatus::kBadWidths);
  const std::array<std::uint16_t, 1> tooWide = {LineBreaker::kMaxWidth + 1};
  EXPECT_EQ(breaker.Begin(LineWidths{.first = tooWide, .rest = 10}, collector), BreakStatus::kBadWidths);
  const std::array<std::uint16_t, LineBreaker::kMaxFirstLines + 1> tooMany = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(breaker.Begin(LineWidths{.first = tooMany, .rest = 10}, collector), BreakStatus::kBadWidths);
  EXPECT_EQ(breaker.Status(), BreakStatus::kBadWidths);

  // After a refusal nothing is made of what it is given.
  breaker.AddBox(3);
  EXPECT_EQ(breaker.Items(), 0U);
  EXPECT_EQ(breaker.Finish(), BreakStatus::kNotStarted);
  EXPECT_TRUE(collector.lines.empty());

  // The widest it takes, and the most first lines, are fine.
  const std::array<std::uint16_t, LineBreaker::kMaxFirstLines> most = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(breaker.Begin(LineWidths{.first = most, .rest = LineBreaker::kMaxWidth}, collector), BreakStatus::kOk);
}

TEST(LineBreaker, DoesNothingBeforeItIsBegunOrAfterItIsFinished) {
  Collector collector;
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);

  EXPECT_EQ(breaker.Status(), BreakStatus::kNotStarted);
  breaker.AddBox(3);
  breaker.AddSpace(1);
  breaker.AddBreak(1, 0);
  breaker.AddForcedBreak();
  EXPECT_EQ(breaker.Items(), 0U);
  EXPECT_EQ(breaker.Finish(), BreakStatus::kNotStarted);

  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 10}, collector), BreakStatus::kOk);
  breaker.AddBox(3);
  EXPECT_EQ(breaker.Finish(), BreakStatus::kOk);
  EXPECT_EQ(collector.lines.size(), 1U);
  EXPECT_EQ(breaker.Status(), BreakStatus::kNotStarted);

  breaker.AddBox(3);
  EXPECT_EQ(breaker.Items(), 1U);
  EXPECT_EQ(breaker.Finish(), BreakStatus::kNotStarted);
  EXPECT_EQ(collector.lines.size(), 1U);
}

TEST(LineBreaker, BreaksOneParagraphAfterAnother) {
  Collector collector;
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);

  for (int paragraph = 0; paragraph < 3; ++paragraph) {
    collector.lines.clear();
    ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 5}, collector), BreakStatus::kOk);
    const Items items = FromText("aa bb cc dd");
    for (const Item& item : items) {
      Add(breaker, item);
    }
    ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);
    EXPECT_EQ(AsText(items, collector.lines), (Text{"aa bb", "cc dd"}));
    EXPECT_EQ(breaker.Lines(), 2U);
    EXPECT_EQ(breaker.Items(), 7U);
  }
}

TEST(LineBreaker, AbandonsAParagraphWhenAnotherIsBegun) {
  Collector collector;
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);

  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 5}, collector), BreakStatus::kOk);
  breaker.AddBox(4);
  breaker.AddSpace(1);
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 5}, collector), BreakStatus::kOk);
  breaker.AddBox(2);
  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);

  ASSERT_EQ(collector.lines.size(), 1U);
  EXPECT_EQ(collector.lines[0].width, 2U);
  EXPECT_EQ(collector.lines[0].firstItem, 0U);
}

TEST(LineBreaker, NamesItsStatuses) {
  EXPECT_STREQ(BreakStatusName(BreakStatus::kOk), "ok");
  EXPECT_STREQ(BreakStatusName(BreakStatus::kNotStarted), "not started");
  EXPECT_STREQ(BreakStatusName(BreakStatus::kNoMemory), "no memory");
  EXPECT_STREQ(BreakStatusName(BreakStatus::kBadWidths), "bad widths");
}

TEST(LineBreaker, FindsACheapestWayWhereEveryWayCanBeTried) {
  Dice dice(2026);
  for (int round = 0; round < 600; ++round) {
    const Items items = MadeUp(dice, dice.Between(1, 7), 8, round % 3 == 0);
    const Widths first = MadeUpWidths(dice, 8, 20);
    const auto rest = static_cast<std::uint16_t>(dice.Between(8, 20));

    const Broken broken = BreakWith(items, first, rest);
    ASSERT_EQ(broken.finished, BreakStatus::kOk);
    ExpectSound(items, broken.lines, first, rest);
    EXPECT_EQ(broken.overfullLines, 0U) << "round " << round;
    EXPECT_EQ(broken.settledEarly, 0U) << "round " << round;

    const std::optional<std::uint64_t> cheapest = CheapestOfEveryWay(items, first, rest);
    ASSERT_TRUE(cheapest.has_value()) << "round " << round;
    EXPECT_EQ(CostOf(items, broken.lines, first, rest), *cheapest) << "round " << round;
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(LineBreaker, AgreesWithTheOtherWayOfWorkingItOutOnShortParagraphs) {
  // So that the check used for long paragraphs is itself checked against
  // trying every way.
  Dice dice(77);
  for (int round = 0; round < 300; ++round) {
    const Items items = MadeUp(dice, dice.Between(1, 7), 8, round % 2 == 0);
    const Widths first = MadeUpWidths(dice, 8, 20);
    const auto rest = static_cast<std::uint16_t>(dice.Between(8, 20));

    const std::optional<std::uint64_t> cheapest = CheapestOfEveryWay(items, first, rest);
    ASSERT_TRUE(cheapest.has_value()) << "round " << round;
    ASSERT_EQ(CheapestWorkedForwards(items, first, rest), *cheapest) << "round " << round;
  }
}

TEST(LineBreaker, FindsACheapestWayThroughLongParagraphs) {
  Dice dice(11);
  for (int round = 0; round < 40; ++round) {
    const Items items = MadeUp(dice, dice.Between(200, 1500), 40, round % 2 == 0);
    const Widths first = MadeUpWidths(dice, 40, 300);
    const auto rest = static_cast<std::uint16_t>(dice.Between(40, 300));

    const Broken broken = BreakWith(items, first, rest);
    ASSERT_EQ(broken.finished, BreakStatus::kOk);
    ExpectSound(items, broken.lines, first, rest);
    EXPECT_EQ(broken.overfullLines, 0U) << "round " << round;
    EXPECT_EQ(broken.settledEarly, 0U) << "round " << round;
    EXPECT_TRUE(broken.pendingInOrder) << "round " << round;
    EXPECT_EQ(CostOf(items, broken.lines, first, rest), CheapestWorkedForwards(items, first, rest))
        << "round " << round;
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(LineBreaker, BreaksAParagraphThatFitsItsNodesTheCheapestWay) {
  // Paragraphs of a size that mostly fits the usual nodes. One that fits is
  // not given out before its end, and comes out the cheapest way there is.
  Dice dice(5);
  int fitted = 0;
  for (int round = 0; round < 120; ++round) {
    const Items items = MadeUp(dice, dice.Between(20, 110), 40, round % 2 == 0);
    const Widths first = MadeUpWidths(dice, 40, 300);
    const auto rest = static_cast<std::uint16_t>(dice.Between(40, 300));

    const Broken broken = BreakWith(items, first, rest, LineBreaker::kUsualNodes);
    ASSERT_EQ(broken.finished, BreakStatus::kOk);
    ExpectSound(items, broken.lines, first, rest);
    EXPECT_LE(broken.mostNodes, LineBreaker::kUsualNodes);
    const std::uint64_t cheapest = CheapestWorkedForwards(items, first, rest);
    if (broken.settledEarly == 0) {
      EXPECT_EQ(CostOf(items, broken.lines, first, rest), cheapest) << "round " << round;
      ++fitted;
    } else {
      EXPECT_GE(CostOf(items, broken.lines, first, rest), cheapest) << "round " << round;
    }
  }
  EXPECT_GT(fitted, 90);
}

TEST(LineBreaker, StaysNearTheCheapestWayWhenAParagraphOutgrowsItsNodes) {
  Dice dice(9);
  for (int round = 0; round < 30; ++round) {
    const Items items = MadeUp(dice, dice.Between(1000, 4000), 40, round % 2 == 0);
    const Widths first = MadeUpWidths(dice, 150, 300);
    const auto rest = static_cast<std::uint16_t>(dice.Between(150, 300));

    const Broken broken = BreakWith(items, first, rest, LineBreaker::kUsualNodes);
    ASSERT_EQ(broken.finished, BreakStatus::kOk);
    ExpectSound(items, broken.lines, first, rest);
    EXPECT_EQ(broken.overfullLines, 0U) << "round " << round;
    EXPECT_GT(broken.settledEarly, 0U) << "round " << round;
    EXPECT_TRUE(broken.pendingInOrder) << "round " << round;
    EXPECT_LE(broken.mostNodes, LineBreaker::kUsualNodes);
    // Lines settled before the end cannot always be the ones the whole
    // paragraph would have chosen. They are not far off.
    const std::uint64_t cheapest = CheapestWorkedForwards(items, first, rest);
    const std::uint64_t cost = CostOf(items, broken.lines, first, rest);
    EXPECT_GE(cost, cheapest) << "round " << round;
    EXPECT_LE(cost, cheapest + (cheapest / 4)) << "round " << round;
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(LineBreaker, StillGivesLinesThatFitWithTheFewestNodes) {
  Dice dice(3);
  std::uint32_t forced = 0;
  for (int round = 0; round < 60; ++round) {
    // Short words on wide lines, so that a line could begin at more places
    // than there are nodes.
    const Items items = MadeUp(dice, dice.Between(100, 600), 6, round % 2 == 0);
    const Widths first = MadeUpWidths(dice, 100, 300);
    const auto rest = static_cast<std::uint16_t>(dice.Between(100, 300));

    const Broken broken = BreakWith(items, first, rest, LineBreaker::kMinNodes);
    ASSERT_EQ(broken.finished, BreakStatus::kOk);
    ExpectSound(items, broken.lines, first, rest);
    EXPECT_EQ(broken.overfullLines, 0U) << "round " << round;
    EXPECT_TRUE(broken.pendingInOrder) << "round " << round;
    EXPECT_LE(broken.mostNodes, LineBreaker::kMinNodes);
    EXPECT_GE(CostOf(items, broken.lines, first, rest), CheapestWorkedForwards(items, first, rest));
    forced += broken.settledEarly;
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  // The test is of what happens when there are too few: see that there were.
  EXPECT_GT(forced, 0U);
}

TEST(LineBreaker, BreaksAParagraphOfAnyLengthInTheSameMemory) {
  // A paragraph of sixty thousand words, which is several times the longest
  // any book has been seen to hold, in the nodes an ordinary one gets.
  Dice dice(1);
  const Items items = MadeUp(dice, 60000, 40, false);
  const Broken broken = BreakWith(items, {150}, 200, LineBreaker::kUsualNodes);

  ASSERT_EQ(broken.finished, BreakStatus::kOk);
  ExpectSound(items, broken.lines, {150}, 200);
  EXPECT_EQ(broken.overfullLines, 0U);
  EXPECT_TRUE(broken.pendingInOrder);
  EXPECT_LE(broken.mostNodes, LineBreaker::kUsualNodes);
  // A node is a place a line could end, and here each of those is a box and
  // the space or break after it: so the lines are never further behind the
  // items than two items a node.
  EXPECT_LE(broken.furthestBehind, (2 * LineBreaker::kUsualNodes) + 2);
  const std::uint64_t cheapest = CheapestWorkedForwards(items, {150}, 200);
  const std::uint64_t cost = CostOf(items, broken.lines, {150}, 200);
  EXPECT_GE(cost, cheapest);
  EXPECT_LE(cost, cheapest + (cheapest / 4));
}

TEST(LineBreaker, KeepsItsNumbersSmallHoweverWideTheParagraphGrows) {
  // Boxes as wide as a box can usefully be, and enough of them that their
  // widths add up to more than 32 bits hold.
  std::vector<LineBreaker::Node> nodes(LineBreaker::kUsualNodes);
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = LineBreaker::kMaxWidth}, collector), BreakStatus::kOk);
  constexpr std::uint32_t kBoxes = 150000;
  for (std::uint32_t box = 0; box < kBoxes; ++box) {
    breaker.AddBox(29000);
    breaker.AddSpace(500);
  }
  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);

  // One box fits on a line and two do not.
  ASSERT_EQ(collector.lines.size(), kBoxes);
  for (const Line& line : collector.lines) {
    ASSERT_EQ(line.width, 29000U);
    ASSERT_FALSE(line.overfull);
    ASSERT_EQ(line.endItem, line.firstItem + 1);
  }
}

TEST(LineBreaker, TakesAStretchWiderThanItsNumbersCanCount) {
  // Nowhere to end a line, and widths that add up to more than 32 bits
  // hold. It is one line that is too wide, and the count stops at the most
  // there is.
  std::vector<LineBreaker::Node> nodes(LineBreaker::kMinNodes);
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 100}, collector), BreakStatus::kOk);
  constexpr std::uint32_t kBoxes = 70000;
  for (std::uint32_t box = 0; box < kBoxes; ++box) {
    breaker.AddBox(65535);
  }
  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);

  ASSERT_EQ(collector.lines.size(), 1U);
  EXPECT_TRUE(collector.lines[0].overfull);
  EXPECT_TRUE(collector.lines[0].last);
  EXPECT_EQ(collector.lines[0].firstItem, 0U);
  EXPECT_EQ(collector.lines[0].endItem, kBoxes);
  EXPECT_EQ(collector.lines[0].width, std::numeric_limits<std::uint32_t>::max());
}

TEST(LineBreaker, GivesNothingOutBeforeTheEndUnlessItHasTo) {
  const Items items = FromText("aa bb cc dd ee ff gg hh ii jj");
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 5}, collector), BreakStatus::kOk);
  for (const Item& item : items) {
    Add(breaker, item);
  }

  EXPECT_TRUE(collector.lines.empty());
  EXPECT_EQ(breaker.Pending(), 0U);
  EXPECT_EQ(breaker.Finish(), BreakStatus::kOk);
  EXPECT_EQ(collector.lines.size(), 5U);
  EXPECT_EQ(breaker.SettledEarly(), 0U);
}

TEST(LineBreaker, SettlesOnlyAsManyLinesAsItIsAskedFor) {
  const Items items = FromText("aa bb cc dd ee ff");
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 5}, collector), BreakStatus::kOk);
  for (const Item& item : items) {
    Add(breaker, item);
  }

  // Nothing is asked for that has not been given out already.
  breaker.Settle(0);
  EXPECT_TRUE(collector.lines.empty());

  // The first item is wanted off its hands: that takes the first line.
  breaker.Settle(1);
  EXPECT_EQ(AsText(items, collector.lines), (Text{"aa bb"}));
  EXPECT_EQ(breaker.Pending(), 4U);
  EXPECT_EQ(breaker.SettledEarly(), 1U);
  breaker.Settle(4);
  EXPECT_EQ(collector.lines.size(), 1U);

  // Everything is wanted. The second line can go; the third is still being
  // added to, and stays.
  breaker.Settle(1000);
  EXPECT_EQ(AsText(items, collector.lines), (Text{"aa bb", "cc dd"}));
  EXPECT_EQ(breaker.Pending(), 8U);
  breaker.Settle(1000);
  EXPECT_EQ(collector.lines.size(), 2U);

  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);
  EXPECT_EQ(AsText(items, collector.lines), (Text{"aa bb", "cc dd", "ee ff"}));
  EXPECT_EQ(breaker.SettledEarly(), 2U);
  ExpectSound(items, collector.lines, {}, 5);
}

TEST(LineBreaker, SettlesNothingWhileAllThatIsLeftWouldGoOnOneLine) {
  const Items items = FromText("aa bb cc");
  std::array<LineBreaker::Node, LineBreaker::kMinNodes> nodes{};
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = {}, .rest = 20}, collector), BreakStatus::kOk);
  for (const Item& item : items) {
    Add(breaker, item);
  }

  breaker.Settle(1000);
  EXPECT_TRUE(collector.lines.empty());
  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);
  EXPECT_EQ(AsText(items, collector.lines), (Text{"aa bb cc"}));
  EXPECT_EQ(breaker.SettledEarly(), 0U);
}

TEST(LineBreaker, LetsACallerHoldOnlySoManyItems) {
  // The breaker has nodes to spare, so the only lines given out early are
  // the ones the caller asks for, which it does whenever it is holding more
  // items than it wants to. There are no forced breaks to make it easy.
  Dice dice(21);
  const Items items = MadeUp(dice, 20000, 40, false);
  const Widths first = {150};
  constexpr std::uint16_t kRest = 200;
  constexpr std::uint32_t kHeld = 300;

  std::vector<LineBreaker::Node> nodes(16384);
  LineBreaker breaker(nodes);
  Collector collector;
  ASSERT_EQ(breaker.Begin(LineWidths{.first = first, .rest = kRest}, collector), BreakStatus::kOk);
  std::uint32_t furthestBehind = 0;
  for (const Item& item : items) {
    Add(breaker, item);
    if (breaker.Items() - breaker.Pending() > kHeld) {
      breaker.Settle(breaker.Items() - (kHeld / 2));
    }
    furthestBehind = std::max(furthestBehind, breaker.Items() - breaker.Pending());
  }
  ASSERT_EQ(breaker.Finish(), BreakStatus::kOk);

  ExpectSound(items, collector.lines, first, kRest);
  EXPECT_EQ(breaker.OverfullLines(), 0U);
  EXPECT_GT(breaker.SettledEarly(), 0U);
  // It is never holding more than it wanted to, once it has asked.
  EXPECT_LE(furthestBehind, kHeld);
  const std::uint64_t cheapest = CheapestWorkedForwards(items, first, kRest);
  const std::uint64_t cost = CostOf(items, collector.lines, first, kRest);
  EXPECT_GE(cost, cheapest);
  EXPECT_LE(cost, cheapest + (cheapest / 4));
}

}  // namespace
