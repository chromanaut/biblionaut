#include "layout/page_chooser.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using bn::layout::PageFromBottom;
using bn::layout::PageFromTop;
using bn::layout::PageHolding;
using bn::layout::PageLine;
using bn::layout::PageStatus;
using bn::layout::PageStatusName;

// ---- A run of lines, as the tests write one down ----

using Lines = std::vector<PageLine>;

// A line with nothing particular about it.
PageLine Plain(std::uint16_t height, std::uint16_t gap = 0) {
  return {.height = height, .gapBefore = gap, .keepWithPrevious = false, .startsPage = false, .tag = 0};
}
// A line kept with the one before it.
PageLine Kept(std::uint16_t height, std::uint16_t gap = 0) {
  return {.height = height, .gapBefore = gap, .keepWithPrevious = true, .startsPage = false, .tag = 0};
}
// A line that starts a page.
PageLine Starting(std::uint16_t height, std::uint16_t gap = 0) {
  return {.height = height, .gapBefore = gap, .keepWithPrevious = false, .startsPage = true, .tag = 0};
}

// Gives each line its place in the run for a tag, so that a page filled up
// says which line it begins with.
Lines Numbered(Lines lines) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    lines[i].tag = static_cast<std::uint32_t>(i);
  }
  return lines;
}

// A run of `count` lines, all `height` high, the first of which starts a
// page as the first line of a chapter does.
Lines Even(std::size_t count, std::uint16_t height) {
  Lines lines(count, Plain(height));
  lines[0].startsPage = true;
  return Numbered(lines);
}

// Where the blocks of a run begin: the lines a caller would read together.
using Blocks = std::vector<std::size_t>;

Blocks OneBlock() { return {0}; }

Blocks BlocksOf(std::size_t lines, std::size_t each) {
  Blocks blocks;
  for (std::size_t start = 0; start < lines; start += each) {
    blocks.push_back(start);
  }
  return blocks;
}

// ---- Filling a page each way ----

struct Down {
  std::size_t end = 0;  // The line after the page's last.
  std::uint32_t height = 0;
  bool tooTall = false;
  bool over = false;
};

// Fills a page down from the line `first`, offering lines until one is
// refused or there are no more, and checks that each is put where the gaps
// and heights before it say.
Down FillDown(const Lines& lines, std::size_t first, std::uint16_t pageHeight) {
  PageFromTop page;
  EXPECT_EQ(page.Begin(pageHeight), PageStatus::kOk);
  std::uint32_t bottom = 0;
  for (std::size_t i = first; i < lines.size(); ++i) {
    const PageLine& line = lines[i];
    if (!page.Add(line)) {
      break;
    }
    const std::uint32_t gap = i == first && !line.startsPage ? 0U : line.gapBefore;
    EXPECT_EQ(page.Top(), bottom + gap) << "line " << i;
    bottom = page.Top() + line.height;
    EXPECT_EQ(page.Height(), bottom) << "line " << i;
  }
  return {.end = first + page.Lines(), .height = page.Height(), .tooTall = page.TooTall(), .over = page.Over()};
}

struct Up {
  bool found = false;
  std::size_t first = 0;  // The line the page begins with.
  std::uint32_t lines = 0;
  std::uint32_t height = 0;
  bool atPageStart = false;
  bool tooTall = false;
  bool cutShort = false;
  std::size_t blocksGiven = 0;
  std::uint32_t firstBlock = 0;  // Which of the blocks given it says its first line is in.
};

// Whether the line `first` is in the block that was given after `given`
// others, the first given having been the block of that number.
bool IsInBlockGiven(const Blocks& blocks, std::size_t firstGiven, std::uint32_t given, std::size_t first) {
  if (given > firstGiven) {
    return false;
  }
  const std::size_t block = firstGiven - given;
  return blocks[block] <= first && (block + 1 == blocks.size() || first < blocks[block + 1]);
}

// Fills a page up from the line before `end`: gives the block that line is
// in, down to it, then the blocks above, until no more are wanted or there
// are none.
Up FillUp(const Lines& lines, const Blocks& blocks, std::size_t end, std::uint16_t pageHeight,
          std::size_t entryCount = 4096) {
  std::vector<PageFromBottom::Entry> entries(entryCount);
  PageFromBottom page(entries);
  EXPECT_EQ(page.Begin(pageHeight), PageStatus::kOk);
  std::size_t block = blocks.size();
  while (block > 0 && blocks[block - 1] >= end) {
    --block;
  }
  Up up;
  const std::size_t firstGiven = block == 0 ? 0 : block - 1;
  std::size_t blockEnd = end;
  while (block > 0) {
    --block;
    page.BeginBlock();
    for (std::size_t i = blocks[block]; i < blockEnd; ++i) {
      page.Add(lines[i]);
    }
    ++up.blocksGiven;
    if (page.EndBlock()) {
      break;
    }
    blockEnd = blocks[block];
  }
  up.found = page.Found();
  up.first = page.FirstTag();
  up.lines = page.Lines();
  up.height = page.Height();
  up.atPageStart = page.AtPageStart();
  up.tooTall = page.TooTall();
  up.cutShort = page.CutShort();
  up.firstBlock = page.FirstBlock();
  // Whatever page it is, it says truly which block its first line is in.
  EXPECT_TRUE(!up.found || IsInBlockGiven(blocks, firstGiven, up.firstBlock, up.first))
      << "a page up to line " << end << " says its first line, " << up.first << ", is in the block given after "
      << up.firstBlock;
  return up;
}

struct Holding {
  bool found = false;
  std::size_t first = 0;  // The line the page begins with.
  std::uint32_t lines = 0;
  std::uint32_t height = 0;
  std::size_t blocksGiven = 0;
  std::uint32_t firstBlock = 0;
};

// Finds where the page that is to show the line `asked` begins: gives the
// block that line is in, down to it, then the blocks above, until no more
// are wanted or there are none.
Holding FindHolding(const Lines& lines, const Blocks& blocks, std::size_t asked, std::uint16_t pageHeight) {
  PageHolding page;
  EXPECT_EQ(page.Begin(pageHeight), PageStatus::kOk);
  std::size_t block = blocks.size();
  while (block > 0 && blocks[block - 1] > asked) {
    --block;
  }
  Holding holding;
  const std::size_t firstGiven = block == 0 ? 0 : block - 1;
  std::size_t blockEnd = asked + 1;
  while (block > 0) {
    --block;
    page.BeginBlock();
    for (std::size_t i = blocks[block]; i < blockEnd; ++i) {
      page.Add(lines[i]);
    }
    ++holding.blocksGiven;
    if (page.EndBlock()) {
      break;
    }
    blockEnd = blocks[block];
  }
  holding.found = page.Found();
  holding.first = page.FirstTag();
  holding.lines = page.Lines();
  holding.height = page.Height();
  holding.firstBlock = page.FirstBlock();
  EXPECT_TRUE(!holding.found || IsInBlockGiven(blocks, firstGiven, holding.firstBlock, holding.first))
      << "the page about line " << asked << " says its first line, " << holding.first
      << ", is in the block given after " << holding.firstBlock;
  return holding;
}

// ---- The same, worked out over the whole run ----

// How high the lines from `first` up to, and not including, `end` stand on
// a page that begins with `first`.
std::uint32_t HeightOf(const Lines& lines, std::size_t first, std::size_t end) {
  std::uint32_t height = lines[first].height;
  if (lines[first].startsPage) {
    height += lines[first].gapBefore;
  }
  for (std::size_t i = first + 1; i < end; ++i) {
    height += std::uint32_t{lines[i].gapBefore} + lines[i].height;
  }
  return height;
}

bool MayBegin(const PageLine& line) { return line.startsPage || !line.keepWithPrevious; }

// Where a page that begins with `first` ends.
std::size_t DownByHand(const Lines& lines, std::size_t first, std::uint16_t pageHeight) {
  std::size_t end = first + 1;
  while (end < lines.size() && !lines[end].startsPage && HeightOf(lines, first, end + 1) <= pageHeight) {
    ++end;
  }
  // It stopped at a line that does not fit. If that line is kept with the
  // one above, the page ends at the last place it may, if there is one.
  if (end < lines.size() && !lines[end].startsPage && lines[end].keepWithPrevious) {
    for (std::size_t place = end - 1; place > first; --place) {
      if (!lines[place].keepWithPrevious) {
        return place;
      }
    }
  }
  return end;
}

// Where a page that ends before `end` begins.
std::size_t UpByHand(const Lines& lines, std::size_t end, std::uint16_t pageHeight) {
  std::size_t highest = end - 1;
  while (highest > 0 && !lines[highest].startsPage && HeightOf(lines, highest - 1, end) <= pageHeight) {
    --highest;
  }
  for (std::size_t place = highest; place < end; ++place) {
    if (MayBegin(lines[place])) {
      return place;
    }
  }
  return highest;
}

// Where the page that is to show the line `asked` begins.
std::size_t HoldingByHand(const Lines& lines, std::size_t asked, std::uint16_t pageHeight) {
  for (std::size_t place = asked;; --place) {
    if (place != asked && HeightOf(lines, place, asked + 1) > pageHeight) {
      return asked;  // Too far up for the line asked for to be on the page.
    }
    if (MayBegin(lines[place]) || place == 0) {
      return place;
    }
  }
}

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

// A run of lines of all sorts: most the height of a line of text, a few of
// no height, a few as tall as a picture; gaps before some; one in four kept
// with the line before; and now and then one that starts a page.
Lines MadeUp(Dice& dice, std::size_t count) {
  Lines lines(count);
  for (std::size_t i = 0; i < count; ++i) {
    PageLine& line = lines[i];
    const std::uint32_t kind = dice.Below(40);
    line.height =
        static_cast<std::uint16_t>(kind == 0 ? 0 : (kind == 1 ? dice.Between(200, 900) : dice.Between(8, 40)));
    line.gapBefore = static_cast<std::uint16_t>(dice.Below(3) == 0 ? dice.Between(1, 30) : 0);
    line.keepWithPrevious = dice.Below(4) == 0;
    line.startsPage = i == 0 || dice.Below(40) == 0;
  }
  return Numbered(lines);
}

// Blocks of a few lines each, and now and then one of a few hundred.
Blocks MadeUpBlocks(Dice& dice, std::size_t lines) {
  Blocks blocks;
  for (std::size_t start = 0; start < lines;) {
    blocks.push_back(start);
    start += dice.Below(10) == 0 ? dice.Between(50, 300) : dice.Between(1, 12);
  }
  return blocks;
}

// ---- Down from the first line ----

TEST(PageFromTop, TakesAsManyLinesAsFit) {
  const Lines lines = Even(100, 20);
  const Down page = FillDown(lines, 0, 110);
  EXPECT_EQ(page.end, 5U);
  EXPECT_EQ(page.height, 100U);
  EXPECT_TRUE(page.over);
  EXPECT_FALSE(page.tooTall);

  // A page exactly as high as its lines holds them all.
  EXPECT_EQ(FillDown(lines, 0, 100).end, 5U);
  EXPECT_EQ(FillDown(lines, 0, 99).end, 4U);
}

TEST(PageFromTop, BeginsTheNextPageWhereThisOneEnds) {
  const Lines lines = Even(100, 20);
  std::size_t first = 0;
  std::size_t pages = 0;
  while (first < lines.size()) {
    const Down page = FillDown(lines, first, 150);
    ASSERT_GT(page.end, first);
    first = page.end;
    ++pages;
  }
  // Seven lines a page: fourteen pages of seven, and one of two.
  EXPECT_EQ(pages, 15U);
}

TEST(PageFromTop, HoldsWhatIsLeftWhenTheLinesRunOut) {
  const Lines lines = Even(3, 20);
  const Down page = FillDown(lines, 0, 500);
  EXPECT_EQ(page.end, 3U);
  EXPECT_EQ(page.height, 60U);
  EXPECT_FALSE(page.over);
}

TEST(PageFromTop, CountsTheGapsBetweenItsLines) {
  const Lines lines = Numbered({Plain(20), Plain(20, 10), Plain(20, 10), Plain(20, 10)});
  // Three lines and two gaps are 80; a fourth line and its gap make 110.
  const Down page = FillDown(lines, 0, 100);
  EXPECT_EQ(page.end, 3U);
  EXPECT_EQ(page.height, 80U);
}

TEST(PageFromTop, LeavesOutTheGapAboveItsFirstLine) {
  const Lines lines = Numbered({Plain(20, 50), Plain(20), Plain(20), Plain(20)});
  const Down page = FillDown(lines, 0, 60);
  EXPECT_EQ(page.end, 3U);
  EXPECT_EQ(page.height, 60U);
}

TEST(PageFromTop, KeepsTheGapAboveALineThatStartsAPage) {
  const Lines lines = Numbered({Starting(20, 50), Plain(20), Plain(20), Plain(20)});
  // The space the book wants above a chapter's first line is on the page.
  const Down page = FillDown(lines, 0, 100);
  EXPECT_EQ(page.end, 2U);
  EXPECT_EQ(page.height, 90U);
}

TEST(PageFromTop, EndsBeforeALineThatStartsAPage) {
  const Lines lines = Numbered({Plain(20), Plain(20), Starting(20), Plain(20)});
  const Down page = FillDown(lines, 0, 500);
  EXPECT_EQ(page.end, 2U);
  EXPECT_TRUE(page.over);

  // And that line is kept with nothing: it starts a page even so.
  Lines kept = lines;
  kept[2].keepWithPrevious = true;
  EXPECT_EQ(FillDown(kept, 0, 500).end, 2U);
}

TEST(PageFromTop, DoesNotEndBetweenLinesThatAreKeptTogether) {
  // Two lines of text, a heading, and the paragraph under it, whose first
  // line is kept with the heading.
  const Lines lines = Numbered({Plain(20), Plain(20), Plain(20), Kept(20), Plain(20)});
  // The heading would fit and the line under it would not: the heading goes
  // to the next page with it.
  const Down page = FillDown(lines, 0, 70);
  EXPECT_EQ(page.end, 2U);
  EXPECT_EQ(page.height, 40U);
  EXPECT_EQ(FillDown(lines, 2, 70).end, 5U);

  // With room for both, they stay.
  EXPECT_EQ(FillDown(lines, 0, 80).end, 4U);
}

TEST(PageFromTop, GoesBackToTheLatestPlaceItMayEnd) {
  const Lines lines = Numbered({Plain(20), Plain(20), Kept(20), Plain(20), Kept(20), Kept(20), Plain(20)});
  // Line 5 does not fit, and is kept with 4, which is kept with 3. A page
  // may end above 3 and above 1; it ends above 3.
  const Down page = FillDown(lines, 0, 100);
  EXPECT_EQ(page.end, 3U);
  EXPECT_EQ(page.height, 60U);
}

TEST(PageFromTop, EndsWhereItIsFullWhenTheLinesKeptTogetherAreMoreThanAPage) {
  const Lines lines = Numbered({Plain(20), Kept(20), Kept(20), Kept(20), Kept(20)});
  const Down page = FillDown(lines, 0, 50);
  EXPECT_EQ(page.end, 2U);
  // And the next page goes on from there, though its first line was to be
  // kept with the one before.
  EXPECT_EQ(FillDown(lines, 2, 50).end, 4U);
}

TEST(PageFromTop, GivesALineTallerThanThePageAPageToItself) {
  const Lines lines = Numbered({Plain(20), Plain(700), Plain(20)});
  const Down before = FillDown(lines, 0, 500);
  EXPECT_EQ(before.end, 1U);
  EXPECT_FALSE(before.tooTall);

  const Down tall = FillDown(lines, 1, 500);
  EXPECT_EQ(tall.end, 2U);
  EXPECT_EQ(tall.height, 700U);
  EXPECT_TRUE(tall.tooTall);
}

TEST(PageFromTop, TakesNoMoreOnceALineIsRefused) {
  PageFromTop page;
  ASSERT_EQ(page.Begin(50), PageStatus::kOk);
  EXPECT_TRUE(page.Add(Plain(20)));
  EXPECT_TRUE(page.Add(Plain(20)));
  EXPECT_FALSE(page.Add(Plain(20)));
  // One that would have fitted.
  EXPECT_FALSE(page.Add(Plain(5)));
  EXPECT_EQ(page.Lines(), 2U);
  EXPECT_EQ(page.Height(), 40U);

  // Begun again, it is a new page.
  ASSERT_EQ(page.Begin(50), PageStatus::kOk);
  EXPECT_EQ(page.Lines(), 0U);
  EXPECT_FALSE(page.Over());
  EXPECT_TRUE(page.Add(Plain(5)));
}

TEST(PageFromTop, RefusesAPageOfNoHeightAndDoesNothingBeforeItIsBegun) {
  PageFromTop page;
  EXPECT_EQ(page.Status(), PageStatus::kNotStarted);
  EXPECT_FALSE(page.Add(Plain(20)));
  EXPECT_EQ(page.Lines(), 0U);

  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  EXPECT_EQ(page.Status(), PageStatus::kBadHeight);
  EXPECT_FALSE(page.Add(Plain(20)));
  EXPECT_EQ(page.Lines(), 0U);

  // A page that was there is gone when another cannot be begun.
  ASSERT_EQ(page.Begin(100), PageStatus::kOk);
  EXPECT_EQ(page.Status(), PageStatus::kOk);
  EXPECT_TRUE(page.Add(Plain(20)));
  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  EXPECT_EQ(page.Lines(), 0U);
  EXPECT_EQ(page.Height(), 0U);
}

// ---- Up from the last line ----

TEST(PageFromBottom, TakesAsManyLinesAsFitAboveItsLastLine) {
  const Lines lines = Even(100, 20);
  const Up page = FillUp(lines, OneBlock(), 60, 110);
  EXPECT_TRUE(page.found);
  EXPECT_EQ(page.first, 55U);
  EXPECT_EQ(page.lines, 5U);
  EXPECT_EQ(page.height, 100U);
  EXPECT_FALSE(page.atPageStart);
  EXPECT_FALSE(page.tooTall);
  EXPECT_FALSE(page.cutShort);

  EXPECT_EQ(FillUp(lines, OneBlock(), 60, 100).first, 55U);
  EXPECT_EQ(FillUp(lines, OneBlock(), 60, 99).first, 56U);
}

TEST(PageFromBottom, TakesBlocksFromAboveUntilOneDoesNotAllFit) {
  const Lines lines = Even(100, 20);
  // Blocks of four lines, and a page that ends two lines into one of them.
  // It holds seven lines: those two, the four above, and one of the next.
  const Up page = FillUp(lines, BlocksOf(100, 4), 62, 150);
  EXPECT_EQ(page.first, 55U);
  EXPECT_EQ(page.lines, 7U);
  EXPECT_EQ(page.height, 140U);
  EXPECT_EQ(page.blocksGiven, 3U);
}

TEST(PageFromBottom, AsksForOneBlockMoreThanItUsesWhenTheLastOneFitsExactly) {
  const Lines lines = Even(100, 20);
  // Eight lines fit, which is two blocks exactly. Only the block above can
  // say that none of it fits.
  const Up page = FillUp(lines, BlocksOf(100, 4), 60, 160);
  EXPECT_EQ(page.first, 52U);
  EXPECT_EQ(page.lines, 8U);
  EXPECT_EQ(page.blocksGiven, 3U);
}

TEST(PageFromBottom, CountsTheGapsBetweenItsLinesAndLeavesOutTheOneAboveItsFirst) {
  const Lines lines = Numbered({Starting(20), Plain(20, 15), Plain(20, 15), Plain(20, 15), Plain(20, 15)});
  // Three lines and the two gaps between them are 90. The gap above the
  // first of them is not on the page.
  for (const std::size_t each : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
    const Up page = FillUp(lines, BlocksOf(5, each), 5, 100);
    EXPECT_EQ(page.first, 2U) << each;
    EXPECT_EQ(page.lines, 3U) << each;
    EXPECT_EQ(page.height, 90U) << each;
  }
}

TEST(PageFromBottom, StopsAtALineThatStartsAPage) {
  const Lines lines = Numbered({Starting(20), Plain(20), Plain(20), Starting(20, 30), Plain(20), Plain(20)});
  // There is room for more, and the line above starts a page of its own.
  for (const std::size_t each : {std::size_t{1}, std::size_t{3}, std::size_t{6}}) {
    const Up page = FillUp(lines, BlocksOf(6, each), 6, 500);
    EXPECT_EQ(page.first, 3U) << each;
    EXPECT_EQ(page.lines, 3U) << each;
    // The gap above that line is on its page.
    EXPECT_EQ(page.height, 90U) << each;
    EXPECT_TRUE(page.atPageStart) << each;
  }
}

TEST(PageFromBottom, DoesNotBeginWithALineThatStartsAPageIfItsGapDoesNotFit) {
  const Lines lines = Numbered({Starting(20, 30), Plain(20), Plain(20)});
  // The three lines are 60 high, and 90 with the gap the first brings.
  const Up tight = FillUp(lines, OneBlock(), 3, 80);
  EXPECT_EQ(tight.first, 1U);
  EXPECT_EQ(tight.lines, 2U);
  EXPECT_FALSE(tight.atPageStart);

  const Up roomy = FillUp(lines, OneBlock(), 3, 90);
  EXPECT_EQ(roomy.first, 0U);
  EXPECT_EQ(roomy.height, 90U);
  EXPECT_TRUE(roomy.atPageStart);
}

TEST(PageFromBottom, DoesNotBeginWithALineKeptWithTheOneBefore) {
  // Two lines of text, a heading, and the paragraph under it, whose first
  // line is kept with the heading.
  const Lines lines = Numbered({Starting(20), Plain(20), Plain(30), Kept(20), Plain(20), Plain(20)});
  const Blocks blocks = {0, 2, 3};
  // The paragraph fits and the heading above it does not. The page begins
  // with the paragraph's second line, and leaves its first with the heading.
  const Up page = FillUp(lines, blocks, 6, 70);
  EXPECT_EQ(page.first, 4U);
  EXPECT_EQ(page.lines, 2U);
  EXPECT_EQ(page.height, 40U);

  // The page before that ends with the heading and the line under it.
  const Up before = FillUp(lines, blocks, 4, 70);
  EXPECT_EQ(before.first, 1U);
  EXPECT_EQ(before.lines, 3U);
  EXPECT_EQ(before.height, 70U);

  // With room for the heading, it is on the page.
  EXPECT_EQ(FillUp(lines, blocks, 6, 90).first, 2U);
}

TEST(PageFromBottom, BeginsWithTheNextLineBelowThatMayBeginAPage) {
  // The lines that fit run up into a heading of three lines kept together,
  // whose first line does not fit. Neither of its other lines may begin a
  // page, nor may the line under it.
  const Lines lines = Numbered({Starting(20), Plain(30), Kept(30), Kept(30), Kept(20), Plain(20), Plain(20)});
  const Blocks blocks = {0, 1, 4};
  const Up page = FillUp(lines, blocks, 7, 130);
  EXPECT_EQ(page.first, 5U);
  EXPECT_EQ(page.lines, 2U);
  EXPECT_EQ(page.height, 40U);
}

TEST(PageFromBottom, BeginsWhereItIsFullWhenNoLineOnItMayBeginAPage) {
  const Lines lines = Numbered({Starting(20), Kept(20), Kept(20), Kept(20), Kept(20), Kept(20)});
  const Up page = FillUp(lines, OneBlock(), 6, 50);
  EXPECT_EQ(page.first, 4U);
  EXPECT_EQ(page.lines, 2U);
  EXPECT_EQ(page.height, 40U);
}

TEST(PageFromBottom, GivesALineTallerThanThePageAPageToItself) {
  const Lines lines = Numbered({Starting(20), Plain(20), Plain(700), Plain(20)});
  const Up tall = FillUp(lines, OneBlock(), 3, 500);
  EXPECT_TRUE(tall.found);
  EXPECT_EQ(tall.first, 2U);
  EXPECT_EQ(tall.lines, 1U);
  EXPECT_EQ(tall.height, 700U);
  EXPECT_TRUE(tall.tooTall);

  // The page below it holds only the line below it.
  const Up below = FillUp(lines, OneBlock(), 4, 500);
  EXPECT_EQ(below.first, 3U);
  EXPECT_EQ(below.lines, 1U);
  EXPECT_FALSE(below.tooTall);
}

TEST(PageFromBottom, PassesOverABlockWithNoLines) {
  std::array<PageFromBottom::Entry, PageFromBottom::kUsualEntries> entries{};
  PageFromBottom page(entries);
  ASSERT_EQ(page.Begin(100), PageStatus::kOk);
  page.BeginBlock();
  EXPECT_FALSE(page.EndBlock());
  EXPECT_FALSE(page.Found());

  page.BeginBlock();
  page.Add(Plain(20));
  page.Add(Plain(20));
  EXPECT_FALSE(page.EndBlock());
  page.BeginBlock();
  EXPECT_FALSE(page.EndBlock());
  page.BeginBlock();
  page.Add(Plain(50));
  page.Add(Plain(50));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_EQ(page.Lines(), 3U);
  EXPECT_EQ(page.Height(), 90U);
}

TEST(PageFromBottom, SaysWhichOfTheBlocksGivenItsFirstLineIsIn) {
  const Lines lines = Even(100, 20);
  // Blocks of four lines, and a page that ends two lines into one of them:
  // with room for one line it begins in that block, and with room for
  // seven in the block given second after it.
  EXPECT_EQ(FillUp(lines, BlocksOf(100, 4), 62, 30).firstBlock, 0U);
  const Up page = FillUp(lines, BlocksOf(100, 4), 62, 150);
  EXPECT_EQ(page.first, 55U);
  EXPECT_EQ(page.firstBlock, 2U);

  // A block with no lines is counted like any other.
  std::array<PageFromBottom::Entry, PageFromBottom::kUsualEntries> entries{};
  PageFromBottom counted(entries);
  ASSERT_EQ(counted.Begin(100), PageStatus::kOk);
  counted.BeginBlock();
  EXPECT_FALSE(counted.EndBlock());
  counted.BeginBlock();
  counted.Add(Plain(20));
  EXPECT_FALSE(counted.EndBlock());
  EXPECT_EQ(counted.FirstBlock(), 1U);
  counted.BeginBlock();
  EXPECT_FALSE(counted.EndBlock());
  counted.BeginBlock();
  counted.Add(Kept(20));
  counted.Add(Plain(20));
  EXPECT_FALSE(counted.EndBlock());
  // Its highest line may not begin a page: the one below it does.
  EXPECT_EQ(counted.Lines(), 2U);
  EXPECT_EQ(counted.FirstBlock(), 3U);
  // And beginning again, the count begins again.
  ASSERT_EQ(counted.Begin(100), PageStatus::kOk);
  counted.BeginBlock();
  counted.Add(Plain(20));
  EXPECT_FALSE(counted.EndBlock());
  EXPECT_EQ(counted.FirstBlock(), 0U);
}

TEST(PageFromBottom, IsWhatHasBeenFoundWhenThereAreNoMoreBlocks) {
  // The lines run out above before the page is full, as at the start of a
  // book.
  const Lines lines = Numbered({Plain(20), Plain(20), Plain(20)});
  const Up page = FillUp(lines, BlocksOf(3, 1), 3, 500);
  EXPECT_TRUE(page.found);
  EXPECT_EQ(page.first, 0U);
  EXPECT_EQ(page.lines, 3U);
  EXPECT_EQ(page.height, 60U);
}

TEST(PageFromBottom, TakesNoMoreOnceItsFirstLineIsFound) {
  std::array<PageFromBottom::Entry, PageFromBottom::kUsualEntries> entries{};
  PageFromBottom page(entries);
  ASSERT_EQ(page.Begin(50), PageStatus::kOk);
  page.BeginBlock();
  for (std::uint32_t tag = 0; tag < 5; ++tag) {
    PageLine line = Plain(20);
    line.tag = tag;
    page.Add(line);
  }
  ASSERT_TRUE(page.EndBlock());
  EXPECT_EQ(page.FirstTag(), 3U);

  page.BeginBlock();
  page.Add(Plain(1));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_EQ(page.FirstTag(), 3U);
  EXPECT_EQ(page.Lines(), 2U);
}

TEST(PageFromBottom, RemembersOnlyTheEndOfALongBlock) {
  // A paragraph of twenty thousand lines, in the entries an ordinary page
  // gets.
  const Lines lines = Even(20000, 20);
  const Up page = FillUp(lines, OneBlock(), 20000, 800, PageFromBottom::kUsualEntries);
  EXPECT_EQ(page.first, 19960U);
  EXPECT_EQ(page.lines, 40U);
  EXPECT_EQ(page.height, 800U);
  EXPECT_FALSE(page.cutShort);
}

TEST(PageFromBottom, GivesAShorterPageWhenItsEntriesRunOut) {
  const Lines lines = Even(1000, 1);
  // A hundred lines would fit, and there are entries for eight.
  const Up page = FillUp(lines, OneBlock(), 1000, 100, PageFromBottom::kMinEntries);
  EXPECT_EQ(page.first, 992U);
  EXPECT_EQ(page.lines, 8U);
  EXPECT_EQ(page.height, 8U);
  EXPECT_TRUE(page.cutShort);

  // With blocks no longer than its entries it needs no more than it has.
  const Up inBlocks = FillUp(lines, BlocksOf(1000, 8), 1000, 100, PageFromBottom::kMinEntries);
  EXPECT_EQ(inBlocks.first, 900U);
  EXPECT_EQ(inBlocks.lines, 100U);
  EXPECT_FALSE(inBlocks.cutShort);
}

TEST(PageFromBottom, KeepsCountThroughABlockWhoseHeightsAddUpToMoreThan32BitsHold) {
  // Lines as tall as a line can be, 65,537 of them: together exactly the
  // most that 32 bits hold. The lines of text after them carry the count
  // past that.
  Lines lines(65537, Plain(65535));
  lines.resize(lines.size() + 12, Plain(10));
  lines = Numbered(lines);
  const Up page = FillUp(lines, OneBlock(), lines.size(), 100, PageFromBottom::kUsualEntries);
  EXPECT_EQ(page.first, 65537U + 2U);
  EXPECT_EQ(page.lines, 10U);
  EXPECT_EQ(page.height, 100U);
}

TEST(PageFromBottom, RefusesTooFewEntriesAndAPageOfNoHeight) {
  std::array<PageFromBottom::Entry, PageFromBottom::kMinEntries> entries{};
  const std::span<PageFromBottom::Entry> oneShort = std::span(entries).first(PageFromBottom::kMinEntries - 1);
  PageFromBottom tooFew(oneShort);
  EXPECT_EQ(tooFew.Begin(100), PageStatus::kNoMemory);

  PageFromBottom page(entries);
  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  // Neither does anything with what it is then given, and neither asks for
  // more.
  for (PageFromBottom* each : {&tooFew, &page}) {
    each->BeginBlock();
    each->Add(Plain(20));
    EXPECT_TRUE(each->EndBlock());
    EXPECT_FALSE(each->Found());
    EXPECT_EQ(each->Lines(), 0U);
  }
  EXPECT_EQ(page.Begin(100), PageStatus::kOk);
}

TEST(PageFromBottom, DoesNothingBeforeItIsBegun) {
  std::array<PageFromBottom::Entry, PageFromBottom::kMinEntries> entries{};
  PageFromBottom page(entries);
  EXPECT_EQ(page.Status(), PageStatus::kNotStarted);
  page.BeginBlock();
  page.Add(Plain(20));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_FALSE(page.Found());

  // A page that was there is gone when another cannot be begun.
  ASSERT_EQ(page.Begin(100), PageStatus::kOk);
  EXPECT_EQ(page.Status(), PageStatus::kOk);
  page.BeginBlock();
  page.Add(Plain(20));
  EXPECT_FALSE(page.EndBlock());
  EXPECT_TRUE(page.Found());
  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  EXPECT_EQ(page.Status(), PageStatus::kBadHeight);
  EXPECT_FALSE(page.Found());
  EXPECT_EQ(page.Lines(), 0U);
}

// ---- Round a line in the middle ----

TEST(PageHolding, BeginsWithTheLineAskedForIfAPageMayBeginThere) {
  const Lines lines = Even(50, 20);
  const Holding page = FindHolding(lines, BlocksOf(50, 5), 23, 300);
  EXPECT_TRUE(page.found);
  EXPECT_EQ(page.first, 23U);
  EXPECT_EQ(page.lines, 1U);
  EXPECT_EQ(page.height, 20U);
  EXPECT_EQ(page.blocksGiven, 1U);
}

TEST(PageHolding, GoesUpToTheNearestLineAPageMayBeginWith) {
  // A paragraph whose first three lines are kept together, as they are
  // beside a large first letter.
  const Lines lines = Numbered({Starting(20), Plain(20), Kept(20), Kept(20), Plain(20), Plain(20)});
  const Blocks blocks = {0, 1};
  const Holding third = FindHolding(lines, blocks, 3, 300);
  EXPECT_EQ(third.first, 1U);
  EXPECT_EQ(third.lines, 3U);
  EXPECT_EQ(third.height, 60U);
  EXPECT_EQ(third.blocksGiven, 1U);

  EXPECT_EQ(FindHolding(lines, blocks, 2, 300).first, 1U);
  EXPECT_EQ(FindHolding(lines, blocks, 1, 300).first, 1U);
  // Below those lines a page may begin anywhere.
  EXPECT_EQ(FindHolding(lines, blocks, 4, 300).first, 4U);
}

TEST(PageHolding, GoesUpIntoTheBlockAbove) {
  // A heading, and the paragraph under it, whose first line is kept with
  // the heading.
  const Lines lines = Numbered({Starting(20), Plain(20), Plain(30), Kept(20), Plain(20)});
  const Blocks blocks = {0, 2, 3};
  const Holding page = FindHolding(lines, blocks, 3, 300);
  EXPECT_EQ(page.first, 2U);
  EXPECT_EQ(page.lines, 2U);
  EXPECT_EQ(page.height, 50U);
  EXPECT_EQ(page.blocksGiven, 2U);
}

TEST(PageHolding, BeginsWithTheLineAskedForWhenTheLinesKeptAboveItAreMoreThanAPage) {
  Lines lines(31, Kept(20));
  lines[0] = Starting(20);
  lines = Numbered(lines);
  // The only line a page may begin with is twenty lines up, and five fit.
  for (const std::size_t each : {std::size_t{1}, std::size_t{4}, std::size_t{31}}) {
    const Holding page = FindHolding(lines, BlocksOf(31, each), 20, 100);
    EXPECT_EQ(page.first, 20U) << each;
    EXPECT_EQ(page.lines, 1U) << each;
    EXPECT_EQ(page.height, 20U) << each;
  }
  // With a page high enough for all of them, it begins up there.
  EXPECT_EQ(FindHolding(lines, BlocksOf(31, 4), 20, 420).first, 0U);
}

TEST(PageHolding, SaysWhichOfTheBlocksGivenItsFirstLineIsIn) {
  // A paragraph whose first line is kept with the heading above it: the
  // page about that line begins in the block given second.
  const Lines heading = Numbered({Starting(20), Plain(20), Plain(30), Kept(20), Plain(20)});
  const Holding above = FindHolding(heading, {0, 2, 3}, 3, 300);
  EXPECT_EQ(above.first, 2U);
  EXPECT_EQ(above.firstBlock, 1U);
  EXPECT_EQ(FindHolding(heading, {0, 2, 3}, 4, 300).firstBlock, 0U);

  // Where the lines kept above the one asked for are more than a page, it
  // begins with the line asked for, though blocks above were given.
  Lines lines(31, Kept(20));
  lines[0] = Starting(20);
  lines = Numbered(lines);
  const Holding own = FindHolding(lines, BlocksOf(31, 4), 22, 100);
  EXPECT_EQ(own.first, 22U);
  EXPECT_GT(own.blocksGiven, 1U);
  EXPECT_EQ(own.firstBlock, 0U);
  // And with room for them all, in the last block given.
  const Holding all = FindHolding(lines, BlocksOf(31, 4), 22, 700);
  EXPECT_EQ(all.first, 0U);
  EXPECT_EQ(all.firstBlock, 5U);
}

TEST(PageHolding, CountsTheGapALineThatStartsAPageBrings) {
  const Lines lines = Numbered({Starting(20, 70), Kept(20), Kept(20)});
  // The three lines are 60 high, and 130 with the gap above the first.
  EXPECT_EQ(FindHolding(lines, OneBlock(), 2, 120).first, 2U);
  const Holding page = FindHolding(lines, OneBlock(), 2, 130);
  EXPECT_EQ(page.first, 0U);
  EXPECT_EQ(page.lines, 3U);
  EXPECT_EQ(page.height, 130U);
}

TEST(PageHolding, BeginsWithALineTallerThanThePageIfThatIsTheLineAskedFor) {
  const Lines lines = Numbered({Starting(20), Plain(700), Kept(700)});
  const Holding tall = FindHolding(lines, OneBlock(), 1, 500);
  EXPECT_EQ(tall.first, 1U);
  EXPECT_EQ(tall.height, 700U);
  // One kept with the line above it, which cannot be on a page with it.
  const Holding kept = FindHolding(lines, OneBlock(), 2, 500);
  EXPECT_EQ(kept.first, 2U);
  EXPECT_EQ(kept.lines, 1U);
}

TEST(PageHolding, BeginsWithTheHighestLineGivenWhenTheLinesRunOut) {
  // No line here may begin a page, and nothing is above them.
  const Lines lines = Numbered({Kept(20), Kept(20), Kept(20)});
  const Holding page = FindHolding(lines, BlocksOf(3, 1), 2, 500);
  EXPECT_TRUE(page.found);
  EXPECT_EQ(page.first, 0U);
  EXPECT_EQ(page.lines, 3U);
  EXPECT_EQ(page.height, 60U);
}

TEST(PageHolding, IsNotMisledByHeightsThatAddUpToMoreThan32BitsHold) {
  // Above the line asked for, lines kept together whose heights add up to
  // a little more than 32 bits hold, so that a count that went round would
  // come to 40.
  Lines lines(65539, Kept(65535));
  lines[0] = Starting(20);
  lines[65538] = Kept(41);
  lines.push_back(Kept(20));
  lines = Numbered(lines);
  const std::size_t asked = lines.size() - 1;
  for (const std::size_t each : {std::size_t{70000}, std::size_t{16}}) {
    const Holding page = FindHolding(lines, BlocksOf(lines.size(), each), asked, 800);
    EXPECT_EQ(page.first, asked) << each;
    EXPECT_EQ(page.lines, 1U) << each;
  }
}

TEST(PageHolding, RefusesAPageOfNoHeightAndDoesNothingBeforeItIsBegun) {
  PageHolding page;
  EXPECT_EQ(page.Status(), PageStatus::kNotStarted);
  page.BeginBlock();
  page.Add(Plain(20));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_FALSE(page.Found());

  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  EXPECT_EQ(page.Status(), PageStatus::kBadHeight);
  page.BeginBlock();
  page.Add(Plain(20));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_FALSE(page.Found());

  ASSERT_EQ(page.Begin(100), PageStatus::kOk);
  page.BeginBlock();
  EXPECT_FALSE(page.EndBlock());
  EXPECT_FALSE(page.Found());
  page.BeginBlock();
  page.Add(Plain(20));
  EXPECT_TRUE(page.EndBlock());
  EXPECT_TRUE(page.Found());

  // A page that was there is gone when another cannot be begun.
  EXPECT_EQ(page.Begin(0), PageStatus::kBadHeight);
  EXPECT_FALSE(page.Found());
}

TEST(PageStatus, HasANameForEachStatus) {
  EXPECT_EQ(std::string(PageStatusName(PageStatus::kOk)), "ok");
  EXPECT_EQ(std::string(PageStatusName(PageStatus::kNotStarted)), "not started");
  EXPECT_EQ(std::string(PageStatusName(PageStatus::kNoMemory)), "no memory");
  EXPECT_EQ(std::string(PageStatusName(PageStatus::kBadHeight)), "bad height");
}

// ---- The two together ----

TEST(Pages, GoBackAndForwardThroughAChapterEnteredInTheMiddle) {
  // Thirty lines to a page, and a chapter entered at its hundredth line.
  const Lines lines = Even(400, 10);
  const Blocks blocks = BlocksOf(400, 7);
  constexpr std::uint16_t kPage = 300;

  EXPECT_EQ(FillDown(lines, 100, kPage).end, 130U);
  // Back, and back again: each page ends where the one left began.
  EXPECT_EQ(FillUp(lines, blocks, 100, kPage).first, 70U);
  EXPECT_EQ(FillUp(lines, blocks, 70, kPage).first, 40U);
  EXPECT_EQ(FillUp(lines, blocks, 40, kPage).first, 10U);
  // Ten lines are left above, and the chapter starts there. The page that
  // ends at them is not a full one, and says where it stopped, so that the
  // chapter's first page can be shown in its place.
  const Up start = FillUp(lines, blocks, 10, kPage);
  EXPECT_EQ(start.first, 0U);
  EXPECT_EQ(start.lines, 10U);
  EXPECT_TRUE(start.atPageStart);
  EXPECT_EQ(FillDown(lines, 0, kPage).end, 30U);
  // Forward from there the pages are the ones counted from the start.
  EXPECT_EQ(FillDown(lines, 30, kPage).end, 60U);
  EXPECT_EQ(FillDown(lines, 60, kPage).end, 90U);
}

TEST(Pages, AgreeWithAddingUpTheWholeRun) {
  Dice dice(5);
  std::uint32_t tooTall = 0;
  std::uint32_t atPageStart = 0;
  std::uint32_t above = 0;     // Pages that began above the line asked for,
  std::uint32_t tooFarUp = 0;  // and ones that would have had to begin too far above it.
  for (int round = 0; round < 300; ++round) {
    const Lines lines = MadeUp(dice, dice.Between(1, 600));
    const Blocks blocks = MadeUpBlocks(dice, lines.size());
    const auto pageHeight = static_cast<std::uint16_t>(dice.Between(60, 800));

    for (int place = 0; place < 40; ++place) {
      const std::size_t first = dice.Below(static_cast<std::uint32_t>(lines.size()));
      const Down down = FillDown(lines, first, pageHeight);
      ASSERT_EQ(down.end, DownByHand(lines, first, pageHeight)) << "round " << round << ", from " << first;
      ASSERT_EQ(down.height, HeightOf(lines, first, down.end)) << "round " << round << ", from " << first;
      ASSERT_EQ(down.tooTall, HeightOf(lines, first, first + 1) > pageHeight);

      const std::size_t end = dice.Between(1, static_cast<std::uint32_t>(lines.size()));
      const Up up = FillUp(lines, blocks, end, pageHeight);
      const std::size_t byHand = UpByHand(lines, end, pageHeight);
      ASSERT_TRUE(up.found);
      ASSERT_EQ(up.first, byHand) << "round " << round << ", to " << end;
      ASSERT_EQ(up.lines, end - byHand) << "round " << round << ", to " << end;
      ASSERT_EQ(up.height, HeightOf(lines, byHand, end)) << "round " << round << ", to " << end;
      ASSERT_EQ(up.atPageStart, lines[byHand].startsPage) << "round " << round << ", to " << end;
      ASSERT_EQ(up.tooTall, HeightOf(lines, end - 1, end) > pageHeight) << "round " << round << ", to " << end;
      ASSERT_FALSE(up.cutShort);
      tooTall += up.tooTall ? 1U : 0U;
      atPageStart += up.atPageStart ? 1U : 0U;

      const std::size_t asked = dice.Below(static_cast<std::uint32_t>(lines.size()));
      const Holding holding = FindHolding(lines, blocks, asked, pageHeight);
      const std::size_t begins = HoldingByHand(lines, asked, pageHeight);
      ASSERT_TRUE(holding.found);
      ASSERT_EQ(holding.first, begins) << "round " << round << ", at " << asked;
      ASSERT_EQ(holding.lines, asked - begins + 1) << "round " << round << ", at " << asked;
      ASSERT_EQ(holding.height, HeightOf(lines, begins, asked + 1)) << "round " << round << ", at " << asked;
      // The page that begins there shows the line asked for.
      ASSERT_GT(FillDown(lines, begins, pageHeight).end, asked) << "round " << round << ", at " << asked;
      above += begins < asked ? 1U : 0U;
      tooFarUp += begins == asked && !MayBegin(lines[asked]) ? 1U : 0U;
    }
  }
  // See that the runs had some of everything.
  EXPECT_GT(tooTall, 0U);
  EXPECT_GT(atPageStart, 0U);
  EXPECT_GT(above, 0U);
  EXPECT_GT(tooFarUp, 0U);
}

TEST(Pages, APageFilledUpHoldsThePageThatWasFilledDownToTheSamePlace) {
  Dice dice(9);
  std::uint32_t pages = 0;
  for (int round = 0; round < 200; ++round) {
    const Lines lines = MadeUp(dice, dice.Between(50, 600));
    const Blocks blocks = MadeUpBlocks(dice, lines.size());
    const auto pageHeight = static_cast<std::uint16_t>(dice.Between(60, 800));

    // Every page of the run, filled down from its start.
    for (std::size_t first = 0; first < lines.size();) {
      const Down down = FillDown(lines, first, pageHeight);
      ASSERT_GT(down.end, first);
      const Up up = FillUp(lines, blocks, down.end, pageHeight);
      // Its lines fit above that end, so the page filled up has them all;
      // unless this page began where none should, for want of anywhere
      // better, and then the one filled up may begin lower.
      if (MayBegin(lines[first])) {
        ASSERT_LE(up.first, first) << "round " << round << ", page from " << first;
      }
      // And a page filled down from where that one begins reaches at least
      // as far, if this page ended where a page may.
      if (down.end == lines.size() || MayBegin(lines[down.end])) {
        ASSERT_GE(FillDown(lines, up.first, pageHeight).end, down.end) << "round " << round << ", page from " << first;
      }
      first = down.end;
      ++pages;
    }
  }
  EXPECT_GT(pages, 1000U);
}

TEST(Pages, FilledUpWithFewEntriesStillFitAndStillBeginWhereTheyMay) {
  Dice dice(13);
  std::uint32_t cutShort = 0;
  for (int round = 0; round < 200; ++round) {
    const Lines lines = MadeUp(dice, dice.Between(50, 600));
    const Blocks blocks = MadeUpBlocks(dice, lines.size());
    const auto pageHeight = static_cast<std::uint16_t>(dice.Between(200, 800));

    for (int place = 0; place < 20; ++place) {
      const std::size_t end = dice.Between(1, static_cast<std::uint32_t>(lines.size()));
      const Up up = FillUp(lines, blocks, end, pageHeight, PageFromBottom::kMinEntries);
      const std::size_t byHand = UpByHand(lines, end, pageHeight);
      ASSERT_TRUE(up.found);
      // Never higher than the lines that fit go, and with all the entries it
      // could want, just there.
      ASSERT_GE(up.first, byHand) << "round " << round << ", to " << end;
      ASSERT_LT(up.first, end);
      ASSERT_EQ(up.lines, end - up.first);
      ASSERT_EQ(up.height, HeightOf(lines, up.first, end));
      if (!up.cutShort) {
        ASSERT_EQ(up.first, byHand) << "round " << round << ", to " << end;
      }
      cutShort += up.cutShort ? 1U : 0U;
    }
  }
  EXPECT_GT(cutShort, 0U);
}

}  // namespace
