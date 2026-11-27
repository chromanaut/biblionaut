#include "layout/paragraph_setter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hyphen_patterns.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "text_measure.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;
using bn::layout::Line;
using bn::layout::LineBreaker;
using bn::layout::LineSink;
using bn::layout::ParagraphSetter;
using bn::layout::ParagraphSettings;
using bn::layout::ParagraphStatus;
using bn::layout::ParagraphStatusName;
using bn::layout::TextLine;
using bn::layout::TextLineSink;
using bn::test::TestMeasure;

// ---- A paragraph, as the tests write one down, and what came of it ----

// Some text in one style.
struct Run {
  std::string text;
  std::uint16_t style = 0;
};
using Runs = std::vector<Run>;

constexpr std::string_view kSoftHyphen = "\xC2\xAD";
constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";

// How a paragraph is to be set.
struct Setting {
  std::uint16_t width = 200;
  // GCC wants every member that a list of named ones leaves out to have a
  // value here, and clang-tidy calls this one redundant.
  std::vector<std::uint16_t> first = {};  // NOLINT(readability-redundant-member-init)
  std::uint32_t penalty = 0;
  std::size_t nodes = LineBreaker::kUsualNodes;
  std::size_t records = ParagraphSetter::kUsualRecords;
  // How many bytes of a run are offered in one call: 0 for all there are.
  std::size_t atATime = 0;
  Hyphenator* hyphenator = nullptr;
};

class Kept final : public TextLineSink {
 public:
  void OnLine(const TextLine& line) noexcept override { lines.push_back(line); }

  std::vector<TextLine> lines;
};

// What came of setting a paragraph.
struct Set {
  ParagraphStatus begun = ParagraphStatus::kNotStarted;
  std::string text;  // All its runs, end to end.
  std::vector<TextLine> lines;
  // The text of each line, without the marks that are on no line, and with a
  // hyphen after it where one is to be shown.
  std::vector<std::string> shown;
  std::uint32_t lastResortCuts = 0;
  std::uint32_t linesCutShort = 0;
  std::uint32_t wordsAsked = 0;
  std::uint32_t settledEarly = 0;
  std::uint32_t mostWork = 0;
  std::uint32_t breakerMostWork = 0;
  // Of all the calls made: how many there were, and the most one of them
  // gave the breaker, asked the hyphenator and had measured.
  std::size_t calls = 0;
  std::uint32_t mostItemsInACall = 0;
  std::uint32_t mostAskedInACall = 0;
  std::size_t mostMeasuredInACall = 0;
  // A call was needed that the text does not account for.
  bool tooManyCalls = false;
};

std::string WithoutMarks(std::string text) {
  for (const std::string_view mark : {kSoftHyphen, kZeroWidthSpace}) {
    for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at)) {
      text.erase(at, mark.size());
    }
  }
  return text;
}

// Keeps count of what one call after another does.
class Watch {
 public:
  Watch(const ParagraphSetter& setter, const TestMeasure& measure, Set& set) noexcept
      : setter_(setter), measure_(measure), set_(set) {}

  void Before() noexcept {
    items_ = setter_.Breaker().Items();
    asked_ = setter_.WordsAsked();
    measured_ = measure_.bytes;
  }
  void After() noexcept {
    ++set_.calls;
    set_.mostItemsInACall = std::max(set_.mostItemsInACall, setter_.Breaker().Items() - items_);
    set_.mostAskedInACall = std::max(set_.mostAskedInACall, setter_.WordsAsked() - asked_);
    set_.mostMeasuredInACall = std::max(set_.mostMeasuredInACall, measure_.bytes - measured_);
  }

 private:
  const ParagraphSetter& setter_;
  const TestMeasure& measure_;
  Set& set_;
  std::uint32_t items_ = 0;
  std::uint32_t asked_ = 0;
  std::size_t measured_ = 0;
};

Set SetRuns(const Runs& runs, TestMeasure& measure, const Setting& setup = {}) {
  Set set;
  std::vector<LineBreaker::Node> nodes(setup.nodes);
  std::vector<ParagraphSetter::Record> records(setup.records);
  ParagraphSetter setter(nodes, records);
  Kept kept;
  setter.UseHyphenator(setup.hyphenator);
  const ParagraphSettings settings{.widths = {.first = setup.first, .rest = setup.width},
                                   .hyphenPenalty = setup.penalty};
  set.begun = setter.Begin(settings, measure, kept);
  if (set.begun != ParagraphStatus::kOk) {
    return set;
  }
  for (const Run& run : runs) {
    set.text += run.text;
  }
  // No paragraph should need more calls than this: a few for every byte.
  const std::size_t callsAllowed = 64 + (16 * set.text.size());
  Watch watch(setter, measure, set);
  for (const Run& run : runs) {
    std::string_view rest = run.text;
    while (!rest.empty() && set.calls < callsAllowed) {
      const std::string_view offered = setup.atATime == 0 ? rest : rest.substr(0, setup.atATime);
      watch.Before();
      const std::size_t taken = setter.AddText(offered, run.style);
      watch.After();
      EXPECT_LE(taken, offered.size());
      rest.remove_prefix(std::min(taken, rest.size()));
    }
  }
  bool finished = false;
  while (!finished && set.calls < callsAllowed) {
    watch.Before();
    finished = setter.Finish();
    watch.After();
  }
  set.tooManyCalls = !finished;

  set.lines = kept.lines;
  for (const TextLine& line : set.lines) {
    std::string shown;
    if (line.begin <= line.end && line.end <= set.text.size()) {
      shown = WithoutMarks(set.text.substr(line.begin, line.end - line.begin));
    } else {
      shown = "(not in the text)";
    }
    set.shown.push_back(shown + (line.hyphen ? "-" : ""));
  }
  set.lastResortCuts = setter.LastResortCuts();
  set.linesCutShort = setter.LinesCutShort();
  set.wordsAsked = setter.WordsAsked();
  set.settledEarly = setter.Breaker().SettledEarly();
  set.mostWork = setter.MostWork();
  set.breakerMostWork = setter.Breaker().MostWork();
  return set;
}

Set SetText(const std::string& text, TestMeasure& measure, const Setting& setup = {}) {
  return SetRuns({{.text = text, .style = 0}}, measure, setup);
}

using Shown = std::vector<std::string>;

// A measure in which every character, space and hyphen is 10 wide: a line
// 70 wide holds seven of them.
TestMeasure Tens() { return TestMeasure(10); }

// A language in which a word may be broken between an a and a b, and
// nowhere else.
class BetweenAAndB {
 public:
  BetweenAAndB() : table_(bn::test::PlainHyphenTable({"a1b"}, 1, 1)) {
    EXPECT_EQ(hyphenator_.Open(table_), HyphenStatus::kOk);
  }
  Hyphenator* Get() noexcept { return &hyphenator_; }

 private:
  std::vector<std::uint8_t> table_;
  Hyphenator hyphenator_;
};

// ---- Words and spaces ----

TEST(ParagraphSetter, SaysWhichOfTheTextIsOnEachLine) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa bbb ccc ddd", measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"aaa bbb", "ccc ddd"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].begin, 0U);
  EXPECT_EQ(set.lines[0].end, 7U);
  EXPECT_EQ(set.lines[0].width, 70U);
  EXPECT_EQ(set.lines[0].number, 0U);
  EXPECT_FALSE(set.lines[0].last);
  EXPECT_EQ(set.lines[1].begin, 8U);
  EXPECT_EQ(set.lines[1].end, 15U);
  EXPECT_EQ(set.lines[1].number, 1U);
  EXPECT_TRUE(set.lines[1].last);
  EXPECT_FALSE(set.lines[1].hyphen);
  EXPECT_FALSE(set.lines[1].overfull);
}

TEST(ParagraphSetter, MakesTheLinesEvenRatherThanFillingEachInTurn) {
  // Filled in turn, the second line would be "cc" alone with most of the
  // line empty after it.
  TestMeasure measure = Tens();
  const Set set = SetText("aaaa bbb cc dddddddd ee", measure, {.width = 100});

  EXPECT_EQ(set.shown, (Shown{"aaaa", "bbb cc", "dddddddd", "ee"}));
}

TEST(ParagraphSetter, TakesSpacesInARowAsOneSpaceAsWideAsAllOfThem) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa   bbb\tccc", measure, {.width = 200});

  ASSERT_EQ(set.lines.size(), 1U);
  EXPECT_EQ(set.shown, (Shown{"aaa   bbb\tccc"}));
  EXPECT_EQ(set.lines[0].width, 130U);
  EXPECT_EQ(set.lines[0].spaces, 2U);
}

TEST(ParagraphSetter, LeavesOutTheSpacesAtEitherEndOfAParagraph) {
  TestMeasure measure = Tens();
  const Set set = SetText("  aaa bbb   ", measure, {.width = 200});

  ASSERT_EQ(set.lines.size(), 1U);
  EXPECT_EQ(set.lines[0].begin, 2U);
  EXPECT_EQ(set.lines[0].end, 9U);
  EXPECT_EQ(set.lines[0].width, 70U);
}

TEST(ParagraphSetter, LeavesTheSpaceALineEndsAtOffBothLines) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa   bbb", measure, {.width = 40});

  EXPECT_EQ(set.shown, (Shown{"aaa", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].end, 3U);
  EXPECT_EQ(set.lines[1].begin, 6U);
  EXPECT_EQ(set.lines[0].spaces, 0U);
}

TEST(ParagraphSetter, CountsThePlacesALineCanBeStretchedAt) {
  TestMeasure measure = Tens();
  const Set set = SetText("a b c d e f g", measure, {.width = 70});

  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.shown, (Shown{"a b c d", "e f g"}));
  EXPECT_EQ(set.lines[0].spaces, 3U);
  EXPECT_EQ(set.lines[1].spaces, 2U);
}

TEST(ParagraphSetter, GivesTheFirstLinesWidthsOfTheirOwn) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa bbb ccc ddd eee", measure, {.width = 70, .first = {30, 30}});

  EXPECT_EQ(set.shown, (Shown{"aaa", "bbb", "ccc ddd", "eee"}));
}

TEST(ParagraphSetter, SetsNothingOfAParagraphWithNoWords) {
  TestMeasure measure = Tens();

  EXPECT_TRUE(SetText("", measure).lines.empty());
  EXPECT_TRUE(SetText("   \t ", measure).lines.empty());
}

// ---- Line ends the text asks for ----

TEST(ParagraphSetter, EndsALineAtALineFeed) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa bbb\nccc", measure, {.width = 200});

  EXPECT_EQ(set.shown, (Shown{"aaa bbb", "ccc"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_TRUE(set.lines[0].last);
  EXPECT_EQ(set.lines[0].end, 7U);
  EXPECT_EQ(set.lines[1].begin, 8U);
}

TEST(ParagraphSetter, LeavesTheSpacesRoundALineFeedOffBothLines) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa  \n  bbb", measure, {.width = 200});

  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].begin, 0U);
  EXPECT_EQ(set.lines[0].end, 3U);
  EXPECT_EQ(set.lines[0].width, 30U);
  EXPECT_EQ(set.lines[1].begin, 8U);
  EXPECT_EQ(set.lines[1].width, 30U);
}

TEST(ParagraphSetter, MakesAnEmptyLineOfTwoLineFeedsTogether) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaa\n\nbbb", measure, {.width = 200});

  EXPECT_EQ(set.shown, (Shown{"aaa", "", "bbb"}));
  ASSERT_EQ(set.lines.size(), 3U);
  EXPECT_EQ(set.lines[1].begin, 4U);
  EXPECT_EQ(set.lines[1].end, 4U);
  EXPECT_EQ(set.lines[1].width, 0U);
  EXPECT_TRUE(set.lines[1].last);
}

TEST(ParagraphSetter, MakesNoLineOfALineFeedThatNothingFollows) {
  TestMeasure measure = Tens();

  EXPECT_EQ(SetText("aaa\n", measure).shown, (Shown{"aaa"}));
  EXPECT_EQ(SetText("\naaa", measure).shown, (Shown{"", "aaa"}));
}

TEST(ParagraphSetter, EndsALineWhenAskedToWithoutAByteOfText) {
  TestMeasure measure = Tens();
  std::vector<LineBreaker::Node> nodes(LineBreaker::kUsualNodes);
  std::vector<ParagraphSetter::Record> records(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter(nodes, records);
  Kept kept;
  ASSERT_EQ(setter.Begin({.widths = {.first = {}, .rest = 200}, .hyphenPenalty = 0}, measure, kept),
            ParagraphStatus::kOk);

  const auto add = [&setter](std::string_view text) {
    for (int calls = 0; !text.empty() && calls < 100; ++calls) {
      text.remove_prefix(setter.AddText(text, 0));
    }
  };
  add("aaa bbb");
  int calls = 0;
  while (!setter.EndLine() && calls < 100) {
    ++calls;
  }
  add("ccc");
  while (!setter.Finish() && calls < 200) {
    ++calls;
  }

  ASSERT_EQ(kept.lines.size(), 2U);
  EXPECT_EQ(kept.lines[0].begin, 0U);
  EXPECT_EQ(kept.lines[0].end, 7U);
  EXPECT_TRUE(kept.lines[0].last);
  EXPECT_EQ(kept.lines[1].begin, 7U);
  EXPECT_EQ(kept.lines[1].end, 10U);
  EXPECT_EQ(setter.Text(), 10U);
}

// ---- Breaking words ----

TEST(ParagraphSetter, BreaksAWordWhereTheHyphenatorSaysAndShowsAHyphen) {
  TestMeasure measure = Tens();
  BetweenAAndB language;
  const Set set = SetText("xx aaabbb", measure, {.width = 70, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_TRUE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].width, 70U);  // The hyphen's width is counted.
  EXPECT_EQ(set.lines[0].end, 6U);
  EXPECT_EQ(set.lines[1].begin, 6U);
  EXPECT_EQ(set.wordsAsked, 2U);
}

TEST(ParagraphSetter, GivesALineThatEndsInsideAWordTheHyphenThatPlaceHas) {
  // A hyphen that is 10 wide and one more for each byte of the stretch
  // measured that comes before it: 13 after "aaa".
  TestMeasure measure = Tens();
  measure.hyphenWiderByPlace = 1;
  BetweenAAndB language;
  const Set set = SetText("xx aaabbb", measure, {.width = 73, .hyphenator = language.Get()});
  EXPECT_EQ(set.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].width, 73U);
  // A line one narrower has no room for it.
  EXPECT_EQ(SetText("xx aaabbb", measure, {.width = 72, .hyphenator = language.Get()}).shown, (Shown{"xx", "aaabbb"}));

  // A word that is longer before its break: 16 after "aaaaaa".
  const Set longer = SetText("xx aaaaaabbb", measure, {.width = 110, .hyphenator = language.Get()});
  EXPECT_EQ(longer.shown, (Shown{"xx aaaaaa-", "bbb"}));
  ASSERT_EQ(longer.lines.size(), 2U);
  EXPECT_EQ(longer.lines[0].width, 106U);

  // The place is counted in the stretch that was measured. A word in two
  // styles is measured as two stretches, and its break is two bytes into
  // the second: 12.
  const Set styled = SetRuns({{"xx a", 0}, {"aabbb", 1}}, measure, {.width = 73, .hyphenator = language.Get()});
  EXPECT_EQ(styled.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(styled.lines.size(), 2U);
  EXPECT_EQ(styled.lines[0].width, 72U);

  // And at a soft hyphen, what is before it was measured by itself: 13
  // after "aaa" again.
  const Set soft = SetText(
      "xx aaa\xC2\xAD"
      "bbb",
      measure, {.width = 73});
  EXPECT_EQ(soft.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(soft.lines.size(), 2U);
  EXPECT_EQ(soft.lines[0].width, 73U);

  // Each place has its own: 11 after the "a" of a word with two soft
  // hyphens, and 15 after its "bbbbb", where the line ends.
  const Set two = SetText(
      "a\xC2\xAD"
      "bbbbb\xC2\xAD"
      "cccc",
      measure, {.width = 75});
  EXPECT_EQ(two.shown, (Shown{"abbbbb-", "cccc"}));
  ASSERT_EQ(two.lines.size(), 2U);
  EXPECT_EQ(two.lines[0].width, 75U);
}

TEST(ParagraphSetter, LeavesAWordWholeWhereBreakingItIsDearerThanTheGap) {
  // Without the break the first line has 50 left on it, which costs 2,500.
  TestMeasure measure = Tens();
  BetweenAAndB language;

  EXPECT_EQ(SetText("xx aaabbb", measure, {.width = 70, .penalty = 2400, .hyphenator = language.Get()}).shown,
            (Shown{"xx aaa-", "bbb"}));
  EXPECT_EQ(SetText("xx aaabbb", measure, {.width = 70, .penalty = 2600, .hyphenator = language.Get()}).shown,
            (Shown{"xx", "aaabbb"}));
}

TEST(ParagraphSetter, BreaksNoWordWithoutAHyphenator) {
  TestMeasure measure = Tens();
  const Set set = SetText("xx aaabbb", measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"xx", "aaabbb"}));
  EXPECT_EQ(set.wordsAsked, 0U);
}

TEST(ParagraphSetter, BreaksAtASoftHyphenAndLeavesItOffBothLines) {
  TestMeasure measure = Tens();
  const Set set = SetText(
      "xx aaa\xC2\xAD"
      "bbb",
      measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_TRUE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].end, 6U);
  EXPECT_EQ(set.lines[1].begin, 8U);
  EXPECT_FALSE(measure.measuredAMark);
}

TEST(ParagraphSetter, GivesASoftHyphenNoWidthWhereNoLineEndsAtIt) {
  TestMeasure measure = Tens();
  const Set set = SetText(
      "aaa\xC2\xAD"
      "bbb",
      measure, {.width = 200});

  ASSERT_EQ(set.lines.size(), 1U);
  EXPECT_EQ(set.shown, (Shown{"aaabbb"}));
  EXPECT_EQ(set.lines[0].width, 60U);
  EXPECT_FALSE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].end, 8U);
  EXPECT_FALSE(measure.measuredAMark);
}

TEST(ParagraphSetter, BreaksAWordWithASoftHyphenOnlyWhereItHasOne) {
  // The hyphenator would break it between the a and the b: the book says
  // after the first a, and the book is taken at its word.
  TestMeasure measure = Tens();
  BetweenAAndB language;
  const Set set = SetText(
      "xxxx a\xC2\xAD"
      "aabbb",
      measure, {.width = 70, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"xxxx a-", "aabbb"}));
  EXPECT_EQ(set.wordsAsked, 1U);  // About "xxxx", and not about the other.
}

TEST(ParagraphSetter, EndsALineAfterAHyphenWrittenInAWordAndShowsNoOther) {
  TestMeasure measure = Tens();
  const Set set = SetText("xx aaa-bbb", measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_FALSE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].end, 7U);
  EXPECT_EQ(set.lines[0].width, 70U);
  EXPECT_EQ(set.lines[1].begin, 7U);
}

TEST(ParagraphSetter, EndsALineAfterADash) {
  TestMeasure measure = Tens();

  EXPECT_EQ(SetText("xx aaa\xE2\x80\x94"
                    "bbb",
                    measure, {.width = 70})
                .shown,
            (Shown{"xx aaa\xE2\x80\x94", "bbb"}));
  EXPECT_EQ(SetText("xx aaa\xE2\x80\x93"
                    "bbb",
                    measure, {.width = 70})
                .shown,
            (Shown{"xx aaa\xE2\x80\x93", "bbb"}));
}

TEST(ParagraphSetter, EndsNoLineAfterAHyphenThatIsNotToBeParted) {
  TestMeasure measure = Tens();

  // One the word begins with.
  EXPECT_EQ(SetText("xxxx -bbb", measure, {.width = 60}).shown, (Shown{"xxxx", "-bbb"}));
  // A minus before a figure, where one before a letter would do.
  EXPECT_EQ(SetText("xxx a-1", measure, {.width = 60}).shown, (Shown{"xxx", "a-1"}));
  EXPECT_EQ(SetText("xxx a-b", measure, {.width = 60}).shown, (Shown{"xxx a-", "b"}));
  // The hyphen that is made not to break (U+2011).
  EXPECT_EQ(SetText("xx aaa\xE2\x80\x91"
                    "bbb",
                    measure, {.width = 70})
                .shown,
            (Shown{"xx",
                   "aaa\xE2\x80\x91"
                   "bbb"}));
  // One the word ends with: the space after it is where the line ends.
  EXPECT_EQ(SetText("aaa- bbb", measure, {.width = 40}).shown, (Shown{"aaa-", "bbb"}));
}

TEST(ParagraphSetter, EndsALineAfterTheLastOfHyphensInARow) {
  TestMeasure measure = Tens();
  const Set set = SetText("xx aa--bbb", measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"xx aa--", "bbb"}));
  EXPECT_EQ(set.lastResortCuts, 0U);
}

TEST(ParagraphSetter, BreaksThePartsOfAWordWithAHyphenInIt) {
  // The hyphenator looks at each side of the hyphen as a word.
  TestMeasure measure = Tens();
  BetweenAAndB language;
  const Set set = SetText("xxxxx ab-ab", measure, {.width = 110, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"xxxxx ab-a-", "b"}));
}

TEST(ParagraphSetter, EndsALineAtAZeroWidthSpaceWhichIsOnNeitherLine) {
  TestMeasure measure = Tens();
  const Set set = SetText(
      "xx aaa\xE2\x80\x8B"
      "bbb",
      measure, {.width = 60});

  EXPECT_EQ(set.shown, (Shown{"xx aaa", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_FALSE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].end, 6U);
  EXPECT_EQ(set.lines[1].begin, 9U);
  EXPECT_FALSE(measure.measuredAMark);
}

TEST(ParagraphSetter, KeepsAWordWithANoBreakSpaceInItTogether) {
  TestMeasure measure = Tens();
  const Set set = SetText(
      "aaa\xC2\xA0"
      "bbb ccc",
      measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"aaa\xC2\xA0"
                              "bbb",
                              "ccc"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].spaces, 0U);
  EXPECT_EQ(set.lines[0].width, 70U);
}

// ---- Styles ----

TEST(ParagraphSetter, MeasuresEachPartOfAWordInItsOwnStyle) {
  // In style 1 every character is 5 wider.
  TestMeasure measure = Tens();
  measure.widerByStyle = 5;
  const Set set = SetRuns({{.text = "xx aaa", .style = 0}, {.text = "bbb cc", .style = 1}}, measure, {.width = 400});

  ASSERT_EQ(set.lines.size(), 1U);
  // xx, a space, aaa: 60. Then bbb, a space and cc at 15 each: 90.
  EXPECT_EQ(set.lines[0].width, 150U);
  EXPECT_EQ(set.lines[0].spaces, 2U);
}

TEST(ParagraphSetter, TakesAWordThatChangesStyleForOneWord) {
  // The hyphenator is asked about the whole of it, and breaks it where the
  // style changes; the hyphen is in the style of what it follows.
  TestMeasure measure = Tens();
  measure.widerByStyle = 5;
  BetweenAAndB language;
  const Set set = SetRuns({{.text = "xx aaa", .style = 0}, {.text = "bbb", .style = 1}}, measure,
                          {.width = 70, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"xx aaa-", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].width, 70U);
  EXPECT_EQ(set.lines[1].width, 45U);
  EXPECT_EQ(set.wordsAsked, 2U);
}

TEST(ParagraphSetter, EndsNoLineWhereOnlyTheStyleChanges) {
  TestMeasure measure = Tens();
  const Set set = SetRuns({{.text = "xx aaa", .style = 0}, {.text = "bbb", .style = 1}}, measure, {.width = 70});

  EXPECT_EQ(set.shown, (Shown{"xx", "aaabbb"}));
}

TEST(ParagraphSetter, TakesAWordInMoreStylesThanItHoldsAtOnceInParts) {
  // No line ends where one part gives way to the next, and the hyphenator
  // is asked about none of it.
  TestMeasure measure = Tens();
  measure.widerByStyle = 1;
  BetweenAAndB language;
  Runs runs;
  for (std::uint16_t style = 0; style < 12; ++style) {
    runs.push_back({.text = "ab", .style = style});
  }
  const Set set = SetRuns(runs, measure, {.width = 400, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"abababababababababababab"}));
  ASSERT_EQ(set.lines.size(), 1U);
  // Two characters in each of twelve styles, the first 10 wide.
  EXPECT_EQ(set.lines[0].width, 372U);
  EXPECT_EQ(set.wordsAsked, 0U);
}

TEST(ParagraphSetter, KeepsACharacterInTheStyleItsFirstByteCameIn) {
  // An e with an accent whose second byte comes in a run of style 3, in
  // which everything is 15 wider; then a soft hyphen and two letters in
  // style 0. The hyphen shown after the e is in the e's style, which is
  // the style of its first byte: 10 wide and not 25, so the line of 30 holds
  // it.
  TestMeasure measure = Tens();
  measure.widerByStyle = 5;
  const Runs runs = {{.text = "x\xC3", .style = 0},
                     {.text = "\xA9", .style = 3},
                     {.text = std::string(kSoftHyphen) + "yy", .style = 0}};
  const Set set = SetRuns(runs, measure, {.width = 30});

  EXPECT_EQ(set.shown, (Shown{"x\xC3\xA9-", "yy"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].width, 30U);
  EXPECT_FALSE(set.lines[0].overfull);
  EXPECT_FALSE(measure.cutInsideACharacter);

  // And the same a byte at a time.
  TestMeasure again = Tens();
  again.widerByStyle = 5;
  const Set slowly = SetRuns(runs, again, {.width = 30, .atATime = 1});
  EXPECT_EQ(slowly.shown, set.shown);
  EXPECT_FALSE(again.cutInsideACharacter);
}

TEST(ParagraphSetter, GivesAByteThatGoesOnWithNothingTheStyleItComesIn) {
  // After a space, a byte that would go on with a character begins a word
  // with no character before it to go on with: it is in the style it was
  // given in, 0, and not that of the word before, in which everything is 15
  // wider. So the hyphen shown after it is 10 wide, and the line of 85 holds
  // two letters and a space of 25 each, and it.
  TestMeasure measure = Tens();
  measure.widerByStyle = 5;
  const Runs runs = {{.text = "ab ", .style = 3}, {.text = "\xA9" + std::string(kSoftHyphen) + "yy", .style = 0}};
  const Set set = SetRuns(runs, measure, {.width = 85});

  EXPECT_EQ(set.shown, (Shown{"ab \xA9-", "yy"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_EQ(set.lines[0].width, 85U);
}

TEST(ParagraphSetter, CutsAWordInTooManyStylesOnlyWhereACharacterBegins) {
  // Seven styles of "ab", and then a soft hyphen whose first byte comes in
  // an eighth style and whose second in a ninth, where a word can hold no
  // more. The soft hyphen is all in the eighth, and the word is cut after
  // it, where the next "ab" brings the ninth: it is not cut in two, to be
  // measured as a character.
  TestMeasure measure = Tens();
  measure.widerByStyle = 1;
  Runs runs;
  for (std::uint16_t style = 0; style < 7; ++style) {
    runs.push_back({.text = "ab", .style = style});
  }
  runs.push_back({.text = "\xC2", .style = 7});
  runs.push_back({.text = "\xAD", .style = 8});
  runs.push_back({.text = "ab", .style = 9});
  const Set set = SetRuns(runs, measure, {.width = 400});

  EXPECT_EQ(set.shown, (Shown{"abababababababab"}));
  ASSERT_EQ(set.lines.size(), 1U);
  // Two characters in each of styles 0 to 6, and two in style 9.
  EXPECT_EQ(set.lines[0].width, 182U + 38U);
  EXPECT_FALSE(measure.cutInsideACharacter);
  EXPECT_FALSE(measure.measuredAMark);
  EXPECT_FALSE(measure.badPieces);
}

TEST(ParagraphSetter, KeepsABytesStyleAcrossACutOfARunTooLongForAWord) {
  // 252 letters, and then four bytes that each go on with a character and
  // begin none, given in style 1: the fourth is past what a word holds, and
  // begins the next part of the run. It is in the letters' style still, as
  // the three before it are, and so is the hyphen shown after it at the
  // soft hyphen: 10 wide and not 15, which the line has just room for.
  TestMeasure measure = Tens();
  measure.widerByStyle = 5;
  const Runs runs = {{.text = std::string(ParagraphSetter::kMaxWordBytes, 'x'), .style = 0},
                     {.text = "\x80\x80\x80\x80" + std::string(kSoftHyphen) + "yy", .style = 1}};
  const Set set = SetRuns(runs, measure, {.width = 2530});

  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_TRUE(set.lines[0].hyphen);
  EXPECT_EQ(set.lines[0].width, 2530U);
  EXPECT_EQ(set.lines[0].end, ParagraphSetter::kMaxWordBytes + 4);
  EXPECT_EQ(set.lines[1].width, 30U);
}

TEST(ParagraphSetter, EndsALineWithoutAHyphenWhereThereIsNoRoomForOne) {
  // "aaa-" is wider than the line, and "aaa" is not: sooner than have the
  // line too wide, the word is broken there with no hyphen shown.
  TestMeasure measure = Tens();
  BetweenAAndB language;
  const Set set = SetText("aaabbb", measure, {.width = 30, .hyphenator = language.Get()});

  EXPECT_EQ(set.shown, (Shown{"aaa", "bbb"}));
  ASSERT_EQ(set.lines.size(), 2U);
  EXPECT_FALSE(set.lines[0].hyphen);
  EXPECT_FALSE(set.lines[0].overfull);
  EXPECT_EQ(set.lastResortCuts, 1U);
}

// ---- Stretches too wide for a line ----

TEST(ParagraphSetter, CutsAWordThatIsWiderThanALine) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaaaaaaaaaaa", measure, {.width = 50});

  EXPECT_EQ(set.shown, (Shown{"aaaaa", "aaaaa", "aa"}));
  // A line could have ended between any two of its letters.
  EXPECT_EQ(set.lastResortCuts, 11U);
  for (const TextLine& line : set.lines) {
    EXPECT_FALSE(line.overfull);
    EXPECT_FALSE(line.hyphen);
  }
}

TEST(ParagraphSetter, FillsTheLineBeforeAWordThatHasToBeCut) {
  // The word is not kept for a line of its own: it begins where there is
  // room for some of it.
  TestMeasure measure = Tens();
  const Set set = SetText("xx aaaaaaaaaaaa", measure, {.width = 50});

  EXPECT_EQ(set.shown, (Shown{"xx aa", "aaaaa", "aaaaa"}));
}

TEST(ParagraphSetter, CutsNoWordThatALineCanHold) {
  // The only cut there could be is a last resort, and it is not one.
  TestMeasure measure = Tens();
  const Set set = SetText("xx aaaaa", measure, {.width = 50});

  EXPECT_EQ(set.shown, (Shown{"xx", "aaaaa"}));
  EXPECT_EQ(set.lastResortCuts, 0U);
}

TEST(ParagraphSetter, CutsAStretchByTheNarrowestOfTheLines) {
  TestMeasure measure = Tens();
  const Set set = SetText("aaaaaaaa", measure, {.width = 60, .first = {30}});

  for (const TextLine& line : set.lines) {
    EXPECT_FALSE(line.overfull);
  }
  ASSERT_FALSE(set.shown.empty());
  EXPECT_EQ(set.shown[0], "aaa");
}

TEST(ParagraphSetter, CutsWhereAWordOfTwoStylesIsTooWideTogether) {
  // Each half fits a line and the two do not; nothing but the change of
  // style comes between them.
  TestMeasure measure = Tens();
  const Set set = SetRuns({{.text = "aaaa", .style = 0}, {.text = "bbbb", .style = 1}}, measure, {.width = 60});

  EXPECT_EQ(set.shown, (Shown{"aaaa", "bbbb"}));
  EXPECT_EQ(set.lastResortCuts, 1U);
}

TEST(ParagraphSetter, LetsALineBeTooWideOnlyForOneCharacterThatIs) {
  TestMeasure measure = Tens();
  const Set set = SetText("ab c", measure, {.width = 5});

  EXPECT_EQ(set.shown, (Shown{"a", "b", "c"}));
  for (const TextLine& line : set.lines) {
    EXPECT_TRUE(line.overfull);
    EXPECT_EQ(line.width, 10U);
  }
}

TEST(ParagraphSetter, DoesNotPartAMarkFromTheLetterItIsSetOver) {
  // Each e has an acute accent after it as a character of its own.
  TestMeasure measure = Tens();
  const Set set = SetText(
      "e\xCC\x81"
      "e\xCC\x81"
      "e\xCC\x81",
      measure, {.width = 20});

  EXPECT_EQ(set.shown, (Shown{"e\xCC\x81"
                              "e\xCC\x81",
                              "e\xCC\x81"}));
  EXPECT_FALSE(measure.cutInsideACharacter);
}

TEST(ParagraphSetter, CutsARunOfBytesTooLongToTakeAsOneWord) {
  TestMeasure measure = Tens();
  const std::string word(600, 'a');
  const Set set = SetText("xx " + word + " yy", measure, {.width = 100});

  ASSERT_EQ(set.shown.size(), 61U);
  EXPECT_EQ(set.shown.front(), "xx aaaaaaa");
  for (std::size_t i = 1; i + 1 < set.shown.size(); ++i) {
    EXPECT_EQ(set.shown[i], std::string(10, 'a')) << i;
  }
  EXPECT_EQ(set.shown.back(), "aaa yy");
  EXPECT_FALSE(set.tooManyCalls);
}

TEST(ParagraphSetter, CutsALongRunOfBytesBetweenCharacters) {
  // Each character is two bytes, and the run is cut at 252 bytes or just
  // after: never inside one.
  TestMeasure measure = Tens();
  std::string word;
  for (int i = 0; i < 400; ++i) {
    word += "\xC3\xA9";
  }
  const Set set = SetText("a" + word, measure, {.width = 1000});

  EXPECT_FALSE(measure.cutInsideACharacter);
  EXPECT_FALSE(measure.badPieces);
  std::size_t characters = 0;
  for (const TextLine& line : set.lines) {
    EXPECT_FALSE(line.overfull);
    characters += line.width / 10;
  }
  EXPECT_EQ(characters, 401U);
}

TEST(ParagraphSetter, AsksTheHyphenatorNothingOfARunTooLongToBeAWord) {
  TestMeasure measure = Tens();
  BetweenAAndB language;
  std::string word;
  for (int i = 0; i < 200; ++i) {
    word += "ab";
  }
  const Set set = SetText(word, measure, {.width = 1000, .hyphenator = language.Get()});

  EXPECT_EQ(set.wordsAsked, 0U);
  for (const TextLine& line : set.lines) {
    EXPECT_FALSE(line.hyphen);
  }
}

// ---- However the text comes ----

TEST(ParagraphSetter, SetsAParagraphTheSameHoweverItsTextIsCutUp) {
  BetweenAAndB language;
  const std::string text =
      "  The aaabbb of well-known ab\xC2\xAD"
      "ab thi\xE2\x80\x8Bngs, 10-12 of\nthem  aabbaabbaabbaabbaabb "
      "and caf\xC3\xA9s\xE2\x80\x94"
      "all of it.  ";
  TestMeasure whole;
  const Set expected = SetText(text, whole, {.width = 400, .penalty = 500, .hyphenator = language.Get()});
  ASSERT_GT(expected.lines.size(), 3U);

  for (const std::size_t atATime : {1U, 2U, 3U, 5U, 13U, 64U}) {
    TestMeasure measure;
    const Set set =
        SetText(text, measure, {.width = 400, .penalty = 500, .atATime = atATime, .hyphenator = language.Get()});
    EXPECT_EQ(set.shown, expected.shown) << atATime << " bytes at a time";
    EXPECT_FALSE(measure.badPieces);
    EXPECT_FALSE(measure.cutInsideACharacter);
  }
}

// ---- Memory ----

TEST(ParagraphSetter, GivesOutLinesEarlyWhenItsRecordsRunOut) {
  TestMeasure measure = Tens();
  std::string text;
  for (int i = 0; i < 400; ++i) {
    text += "aaa bb ";
  }
  const Set all = SetText(text, measure, {.width = 200, .nodes = 4096, .records = 16384});
  const Set few = SetText(text, measure, {.width = 200, .nodes = 4096, .records = ParagraphSetter::kMinRecords});

  EXPECT_EQ(all.settledEarly, 0U);
  EXPECT_GT(few.settledEarly, 0U);
  EXPECT_EQ(few.linesCutShort, 0U);
  // The same words, in the same order, on lines no wider.
  std::string words;
  for (const std::string& line : few.shown) {
    words += line + " ";
  }
  EXPECT_EQ(words, text);
  for (const TextLine& line : few.lines) {
    EXPECT_LE(line.width, 200U);
  }
}

TEST(ParagraphSetter, EndsALineWhereItStandsWhenItHasMoreItemsThanThereAreRecords) {
  // Nothing has any width, so that a line is never full.
  TestMeasure measure;
  measure.nothingHasWidth = true;
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "a b ";
  }
  const Set set = SetText(text, measure, {.width = 100, .records = ParagraphSetter::kMinRecords});

  EXPECT_GT(set.linesCutShort, 0U);
  EXPECT_FALSE(set.tooManyCalls);
  std::string words;
  for (const std::string& line : set.shown) {
    words += line + " ";
  }
  EXPECT_EQ(words, text);
}

TEST(ParagraphSetter, WillNotWorkWithTooLittleMemoryOrWithWidthsTheBreakerWillNotHave) {
  TestMeasure measure = Tens();

  EXPECT_EQ(SetText("aaa", measure, {.records = ParagraphSetter::kMinRecords - 1}).begun, ParagraphStatus::kNoMemory);
  EXPECT_EQ(SetText("aaa", measure, {.nodes = LineBreaker::kMinNodes - 1}).begun, ParagraphStatus::kNoMemory);
  EXPECT_EQ(SetText("aaa", measure, {.width = 0}).begun, ParagraphStatus::kBadWidths);
  EXPECT_EQ(SetText("aaa", measure, {.records = ParagraphSetter::kMinRecords}).begun, ParagraphStatus::kOk);
}

TEST(ParagraphSetter, DoesNothingWithTextBeforeAParagraphIsBegun) {
  TestMeasure measure = Tens();
  std::vector<LineBreaker::Node> nodes(LineBreaker::kUsualNodes);
  std::vector<ParagraphSetter::Record> records(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter(nodes, records);

  EXPECT_EQ(setter.Status(), ParagraphStatus::kNotStarted);
  EXPECT_EQ(setter.AddText("aaa bbb", 0), 7U);
  EXPECT_TRUE(setter.EndLine());
  EXPECT_TRUE(setter.Finish());
  EXPECT_EQ(setter.Text(), 0U);
  EXPECT_EQ(measure.calls, 0U);
}

TEST(ParagraphSetter, SetsOneParagraphAfterAnother) {
  TestMeasure measure = Tens();
  std::vector<LineBreaker::Node> nodes(LineBreaker::kUsualNodes);
  std::vector<ParagraphSetter::Record> records(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter(nodes, records);
  Kept kept;

  for (int paragraph = 0; paragraph < 3; ++paragraph) {
    kept.lines.clear();
    ASSERT_EQ(setter.Begin({.widths = {.first = {}, .rest = 70}, .hyphenPenalty = 0}, measure, kept),
              ParagraphStatus::kOk);
    std::string_view text = "aaa bbb ccc ddd";
    for (int calls = 0; !text.empty() && calls < 100; ++calls) {
      text.remove_prefix(setter.AddText(text, 0));
    }
    for (int calls = 0; !setter.Finish() && calls < 100; ++calls) {
    }
    EXPECT_EQ(setter.Status(), ParagraphStatus::kNotStarted);
    ASSERT_EQ(kept.lines.size(), 2U);
    EXPECT_EQ(kept.lines[1].begin, 8U);
    EXPECT_EQ(kept.lines[1].number, 1U);
  }
}

TEST(ParagraphSetter, NamesEveryStatus) {
  EXPECT_STREQ(ParagraphStatusName(ParagraphStatus::kOk), "ok");
  EXPECT_STREQ(ParagraphStatusName(ParagraphStatus::kNotStarted), "not started");
  EXPECT_STREQ(ParagraphStatusName(ParagraphStatus::kNoMemory), "no memory");
  EXPECT_STREQ(ParagraphStatusName(ParagraphStatus::kBadWidths), "bad widths");
}

// ---- Paragraphs made up at random ----

// Text of the kinds of thing a paragraph can hold, made from `seed`.
std::string MadeUpText(std::uint32_t seed, std::size_t words, bool longWords) {
  std::mt19937 random(seed);
  const auto upTo = [&random](std::uint32_t most) { return static_cast<std::uint32_t>(random() % (most + 1)); };
  const std::vector<std::string> between = {" ", " ", " ", " ", "  ", "\t", "\n", " \n ", "\r\n"};
  const std::vector<std::string> inside = {"-", "-", "\xC2\xAD", "\xE2\x80\x8B", "\xE2\x80\x94", "--", "\xC2\xA0", "'"};
  const std::vector<std::string> letters = {"a", "b", "a", "b", "c", "d", "\xC3\xA9", "1", "M", "i"};
  std::string text;
  for (std::size_t word = 0; word < words; ++word) {
    const std::uint32_t length = longWords && upTo(9) == 0 ? 20 + upTo(60) : 1 + upTo(9);
    for (std::uint32_t i = 0; i < length; ++i) {
      text += upTo(11) == 0 ? inside[upTo(static_cast<std::uint32_t>(inside.size() - 1))]
                            : letters[upTo(static_cast<std::uint32_t>(letters.size() - 1))];
    }
    text += between[upTo(static_cast<std::uint32_t>(between.size() - 1))];
  }
  return text;
}

bool IsBetweenWords(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

// What is true of the lines of any paragraph, whatever its text: each
// failure is said, and the count of them returned.
int CheckLines(const Set& set, TestMeasure& measure, const Setting& setup, const std::string& what) {
  int failures = 0;
  const auto fail = [&](const std::string& why, std::size_t line) {
    ++failures;
    ADD_FAILURE() << what << ": line " << line << " " << why;
  };
  const std::string& text = set.text;
  // What is on no line: spaces and line ends between words, and marks.
  const auto onNoLine = [&](std::size_t from, std::size_t end) {
    std::string left = WithoutMarks(text.substr(from, end - from));
    return std::all_of(left.begin(), left.end(), IsBetweenWords);
  };
  std::size_t before = 0;
  for (std::size_t i = 0; i < set.lines.size(); ++i) {
    const TextLine& line = set.lines[i];
    if (line.number != i) {
      fail("is not numbered as it comes", i);
    }
    if (line.begin > line.end || line.end > text.size() || line.begin < before) {
      fail("is not a stretch of the text after the line before", i);
      return failures;
    }
    if (!onNoLine(before, line.begin)) {
      fail("leaves text before it that is on no line", i);
    }
    before = line.end;
    const std::string_view on(text.data() + line.begin, line.end - line.begin);
    if (!on.empty() && (IsBetweenWords(on.front()) || IsBetweenWords(on.back()))) {
      fail("begins or ends with a space", i);
    }
    if (on.find('\n') != std::string_view::npos) {
      fail("has a line feed on it", i);
    }
    // Its width, added up from its text: runs of spaces are one space each
    // wide, and the marks are nothing.
    const std::string shown = WithoutMarks(std::string(on));
    std::uint32_t width = 0;
    std::uint32_t spaces = 0;
    std::uint32_t characters = 0;
    for (std::size_t at = 0; at < shown.size(); ++at) {
      if (IsBetweenWords(shown[at])) {
        width += measure.SpaceWidth(0);
        spaces += at == 0 || !IsBetweenWords(shown[at - 1]) ? 1U : 0U;
      } else {
        const std::uint16_t character = measure.CharacterWidth(shown[at], 0);
        width += character;
        characters += character != 0 ? 1U : 0U;
      }
    }
    width += line.hyphen ? measure.HyphenWidth(0) : 0U;
    if (line.width != width) {
      fail("is said to be " + std::to_string(line.width) + " wide and adds up to " + std::to_string(width), i);
    }
    if (line.spaces != spaces) {
      fail("is said to have " + std::to_string(line.spaces) + " spaces and has " + std::to_string(spaces), i);
    }
    const std::uint32_t allowed = i < setup.first.size() ? setup.first[i] : setup.width;
    if (line.overfull != (line.width > allowed)) {
      fail("is wrong about being too wide", i);
    }
    // One character can be wider than a line; nothing else that has any
    // width is on the line with it.
    if (line.overfull && (characters != 1 || spaces != 0)) {
      fail("is too wide with more on it than one character", i);
    }
  }
  if (!onNoLine(before, text.size())) {
    ++failures;
    ADD_FAILURE() << what << ": text after the last line is on no line";
  }
  if (set.tooManyCalls) {
    ++failures;
    ADD_FAILURE() << what << ": took more calls than the text accounts for";
  }
  return failures;
}

TEST(ParagraphSetter, PutsAllOfAnyTextOnLinesThatAddUp) {
  BetweenAAndB language;
  int failures = 0;
  for (std::uint32_t seed = 1; seed <= 400 && failures == 0; ++seed) {
    std::mt19937 random(seed * 7919U);
    const std::string text = MadeUpText(seed, 5 + (random() % 60), seed % 3 == 0);
    Setting setup;
    setup.width = static_cast<std::uint16_t>(seed % 5 == 0 ? 30 + (random() % 60) : 150 + (random() % 900));
    if (seed % 4 == 0) {
      setup.first = {static_cast<std::uint16_t>(100 + (random() % 300)),
                     static_cast<std::uint16_t>(200 + (random() % 300))};
    }
    setup.penalty = seed % 2 == 0 ? 0 : 5000;
    setup.atATime = seed % 6 == 0 ? 0 : 1 + (random() % 40);
    setup.records = seed % 7 == 0 ? ParagraphSetter::kMinRecords : ParagraphSetter::kUsualRecords;
    setup.nodes = seed % 9 == 0 ? LineBreaker::kMinNodes : LineBreaker::kUsualNodes;
    setup.hyphenator = seed % 3 == 1 ? nullptr : language.Get();
    TestMeasure measure;
    const Set set = SetText(text, measure, setup);

    const std::string what = "seed " + std::to_string(seed);
    ASSERT_EQ(set.begun, ParagraphStatus::kOk) << what;
    failures += CheckLines(set, measure, setup, what);
    EXPECT_FALSE(measure.measuredAMark) << what;
    EXPECT_FALSE(measure.cutInsideACharacter) << what;
    EXPECT_FALSE(measure.badPieces) << what;
  }
}

// ---- A second way of making a paragraph's items ----
//
// The whole paragraph at once, with all the memory it wants: every word
// found, cut and measured before the breaker is given anything, and a
// breaker with nodes enough to hold the whole of it.

struct PlainItem {
  enum class Kind : std::uint8_t { kBox, kSpace, kHyphen, kFree, kLineEnd };
  Kind kind = Kind::kBox;
  std::uint16_t width = 0;
  std::uint32_t at = 0;   // Where in the text it begins,
  std::uint32_t end = 0;  // and, of a box, where it ends.
};

std::vector<PlainItem> PlainItemsOf(const std::string& text, TestMeasure& measure, Hyphenator* hyphenator) {
  std::vector<PlainItem> items;
  std::size_t at = 0;
  bool lineStart = true;
  const auto dashAt = [&text](std::size_t i) -> std::size_t {
    if (i < text.size() && text[i] == '-') {
      return 1;
    }
    for (const std::string_view dash : {"\xE2\x80\x90", "\xE2\x80\x92", "\xE2\x80\x93", "\xE2\x80\x94"}) {
      if (text.compare(i, dash.size(), dash) == 0) {
        return dash.size();
      }
    }
    return 0;
  };
  while (at < text.size()) {
    if (text[at] == '\n') {
      items.push_back({.kind = PlainItem::Kind::kLineEnd, .width = 0, .at = static_cast<std::uint32_t>(at), .end = 0});
      lineStart = true;
      ++at;
      continue;
    }
    if (IsBetweenWords(text[at])) {
      std::size_t end = at;
      while (end < text.size() && IsBetweenWords(text[end]) && text[end] != '\n') {
        ++end;
      }
      if (!lineStart) {
        items.push_back({.kind = PlainItem::Kind::kSpace,
                         .width = static_cast<std::uint16_t>(measure.SpaceWidth(0) * (end - at)),
                         .at = static_cast<std::uint32_t>(at),
                         .end = 0});
      }
      at = end;
      continue;
    }
    std::size_t end = at;
    while (end < text.size() && !IsBetweenWords(text[end])) {
      ++end;
    }
    const std::string word = text.substr(at, end - at);
    // Where it may be broken with a hyphen shown: where it has soft hyphens,
    // or else where the hyphenator says.
    std::vector<std::size_t> hyphens;
    if (word.find(kSoftHyphen) == std::string::npos && hyphenator != nullptr) {
      std::vector<std::uint8_t> places(Hyphenator::kMaxLetters);
      places.resize(hyphenator->Find(word, places));
      hyphens.assign(places.begin(), places.end());
    }
    std::size_t from = 0;
    bool letterSeen = false;
    const auto box = [&](std::size_t to, PlainItem::Kind then) {
      if (to > from) {
        items.push_back({.kind = PlainItem::Kind::kBox,
                         .width = static_cast<std::uint16_t>(measure.WidthOf(word.substr(from, to - from), 0)),
                         .at = static_cast<std::uint32_t>(at + from),
                         .end = static_cast<std::uint32_t>(at + to)});
        if (then != PlainItem::Kind::kBox) {
          items.push_back({.kind = then, .width = 0, .at = static_cast<std::uint32_t>(at + to), .end = 0});
        }
      }
    };
    for (std::size_t i = 0; i < word.size();) {
      if (std::find(hyphens.begin(), hyphens.end(), i) != hyphens.end()) {
        box(i, PlainItem::Kind::kHyphen);
        from = i;
      }
      if (word.compare(i, kSoftHyphen.size(), kSoftHyphen) == 0) {
        box(i, PlainItem::Kind::kHyphen);
        i += kSoftHyphen.size();
        from = i;
      } else if (word.compare(i, kZeroWidthSpace.size(), kZeroWidthSpace) == 0) {
        box(i, PlainItem::Kind::kFree);
        i += kZeroWidthSpace.size();
        from = i;
      } else if (const std::size_t dash = dashAt(at + i); dash != 0) {
        i += dash;
        const bool another = i < word.size() && dashAt(at + i) != 0;
        const bool figure = dash == 1 && i < word.size() && word[i] >= '0' && word[i] <= '9';
        if (letterSeen && i < word.size() && !another && !figure) {
          box(i, PlainItem::Kind::kFree);
          from = i;
        }
      } else {
        letterSeen = true;
        ++i;
      }
    }
    box(word.size(), PlainItem::Kind::kBox);
    lineStart = false;
    at = end;
  }
  return items;
}

class KeptItems final : public LineSink {
 public:
  void OnLine(const Line& line) noexcept override { lines.push_back(line); }

  std::vector<Line> lines;
};

// The lines of a paragraph's text by that second way.
std::vector<TextLine> PlainLinesOf(const std::string& text, TestMeasure& measure, const Setting& setup) {
  const std::vector<PlainItem> items = PlainItemsOf(text, measure, setup.hyphenator);
  std::vector<LineBreaker::Node> nodes(16384);
  LineBreaker breaker(nodes);
  KeptItems kept;
  EXPECT_EQ(breaker.Begin({.first = setup.first, .rest = setup.width}, kept), bn::layout::BreakStatus::kOk);
  for (const PlainItem& item : items) {
    switch (item.kind) {
      case PlainItem::Kind::kBox:
        breaker.AddBox(item.width);
        break;
      case PlainItem::Kind::kSpace:
        breaker.AddSpace(item.width);
        break;
      case PlainItem::Kind::kHyphen:
        breaker.AddBreak(measure.HyphenWidth(0), setup.penalty);
        break;
      case PlainItem::Kind::kFree:
        breaker.AddBreak(0, 0);
        break;
      case PlainItem::Kind::kLineEnd:
        breaker.AddForcedBreak();
        break;
    }
  }
  EXPECT_EQ(breaker.Finish(), bn::layout::BreakStatus::kOk);
  EXPECT_EQ(breaker.SettledEarly(), 0U);

  std::vector<TextLine> lines;
  for (const Line& line : kept.lines) {
    TextLine out;
    // A line begins with its first box, and ends where its last box does.
    std::uint32_t lastBox = line.firstItem;
    bool any = false;
    for (std::uint32_t i = line.firstItem; i < line.endItem && i < items.size(); ++i) {
      if (items[i].kind == PlainItem::Kind::kBox) {
        lastBox = i;
        any = true;
      }
    }
    if (any) {
      out.begin = items[line.firstItem].at;
      out.end = items[lastBox].end;
    } else {
      out.begin = line.endItem < items.size() ? items[line.endItem].at : static_cast<std::uint32_t>(text.size());
      out.end = out.begin;
    }
    out.width = line.width;
    out.number = line.number;
    out.hyphen = line.endItem < items.size() && items[line.endItem].kind == PlainItem::Kind::kHyphen;
    out.last = line.last;
    out.overfull = line.overfull;
    lines.push_back(out);
  }
  return lines;
}

TEST(ParagraphSetter, SetsAParagraphAsASecondWayOfMakingItsItemsDoes) {
  BetweenAAndB language;
  int compared = 0;
  for (std::uint32_t seed = 1; seed <= 600; ++seed) {
    std::mt19937 random(seed * 104729U);
    const std::string text = MadeUpText(seed + 5000, 5 + (random() % 50), false);
    Setting setup;
    setup.width = static_cast<std::uint16_t>(500 + (random() % 900));
    if (seed % 4 == 0) {
      setup.first = {static_cast<std::uint16_t>(450 + (random() % 300))};
    }
    setup.penalty = seed % 2 == 0 ? 0 : 3000;
    setup.atATime = seed % 5 == 0 ? 0 : 1 + (random() % 30);
    setup.nodes = 4096;
    setup.records = 8192;
    setup.hyphenator = seed % 3 == 1 ? nullptr : language.Get();
    TestMeasure measure;
    const Set set = SetText(text, measure, setup);
    const std::string what = "seed " + std::to_string(seed);
    ASSERT_EQ(set.begun, ParagraphStatus::kOk) << what;
    if (set.lastResortCuts != 0 || set.settledEarly != 0) {
      continue;  // Not what the second way does.
    }
    ++compared;

    TestMeasure plain;
    const std::vector<TextLine> expected = PlainLinesOf(text, plain, setup);
    ASSERT_EQ(set.lines.size(), expected.size()) << what;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(set.lines[i].begin, expected[i].begin) << what << ", line " << i;
      EXPECT_EQ(set.lines[i].end, expected[i].end) << what << ", line " << i;
      EXPECT_EQ(set.lines[i].width, expected[i].width) << what << ", line " << i;
      EXPECT_EQ(set.lines[i].hyphen, expected[i].hyphen) << what << ", line " << i;
      EXPECT_EQ(set.lines[i].last, expected[i].last) << what << ", line " << i;
    }
    if (::testing::Test::HasFailure()) {
      break;
    }
  }
  EXPECT_GT(compared, 500);
}

// ---- One call's work ----

TEST(ParagraphSetter, DoesNoMoreInOneCallThanItSaysItWill) {
  BetweenAAndB language;
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    std::mt19937 random(seed * 31337U);
    const std::string text = MadeUpText(seed + 9000, 20 + (random() % 80), seed % 2 == 0);
    Setting setup;
    setup.width = static_cast<std::uint16_t>(seed % 4 == 0 ? 20 + (random() % 80) : 200 + (random() % 800));
    setup.atATime = seed % 3 == 0 ? 0 : 1 + (random() % 300);
    setup.records = seed % 2 == 0 ? ParagraphSetter::kMinRecords : ParagraphSetter::kUsualRecords;
    setup.nodes = seed % 5 == 0 ? LineBreaker::kMinNodes : LineBreaker::kUsualNodes;
    setup.hyphenator = language.Get();
    TestMeasure measure;
    if (seed % 10 == 0) {
      measure.nothingHasWidth = true;
    }
    const Set set = SetText(text, measure, setup);
    const std::string what = "seed " + std::to_string(seed);

    EXPECT_LE(set.mostWork, ParagraphSetter::MostWorkFor(setup.records)) << what;
    EXPECT_LE(set.breakerMostWork, LineBreaker::MostWorkFor(setup.nodes)) << what;
    EXPECT_LE(set.mostItemsInACall, 1U) << what;
    EXPECT_LE(set.mostAskedInACall, 1U) << what;
    EXPECT_LE(set.mostMeasuredInACall, ParagraphSetter::kMaxWordBytes + 3) << what;
    EXPECT_FALSE(set.tooManyCalls) << what;
  }
}

TEST(ParagraphSetter, IsHeldToThatByTextMadeToBeAwkward) {
  // One run of bytes with no space in it, every character wider than the
  // line; soft hyphens end to end; a word in a new style at every letter;
  // and line feeds by the hundred, with the fewest records there may be.
  BetweenAAndB language;
  std::string marks;
  for (int i = 0; i < 500; ++i) {
    marks += "a\xC2\xAD";
  }
  const std::vector<std::string> texts = {std::string(5000, 'a'), marks, std::string(700, '\n'),
                                          std::string(3000, '-') + "a", "a" + std::string(3000, ' ') + "b"};
  for (std::size_t which = 0; which < texts.size(); ++which) {
    for (const std::uint16_t width : std::vector<std::uint16_t>{5, 40, 3000}) {
      TestMeasure measure = Tens();
      Setting setup;
      setup.width = width;
      setup.records = ParagraphSetter::kMinRecords;
      setup.nodes = LineBreaker::kMinNodes;
      setup.hyphenator = language.Get();
      const Set set = SetText(texts[which], measure, setup);
      const std::string what = "text " + std::to_string(which) + " at " + std::to_string(width);

      EXPECT_LE(set.mostWork, ParagraphSetter::MostWorkFor(setup.records)) << what;
      EXPECT_LE(set.breakerMostWork, LineBreaker::MostWorkFor(setup.nodes)) << what;
      EXPECT_LE(set.mostItemsInACall, 1U) << what;
      EXPECT_LE(set.mostMeasuredInACall, ParagraphSetter::kMaxWordBytes + 3) << what;
      EXPECT_FALSE(set.tooManyCalls) << what;
      EXPECT_EQ(CheckLines(set, measure, setup, what), 0);
    }
  }

  // And the styles: twelve to a word, in a word that is long besides.
  Runs runs;
  for (std::uint16_t i = 0; i < 400; ++i) {
    runs.push_back({.text = "ab", .style = static_cast<std::uint16_t>(i % 13)});
  }
  TestMeasure measure = Tens();
  const Set set = SetRuns(runs, measure, {.width = 100, .records = ParagraphSetter::kMinRecords});
  EXPECT_LE(set.mostWork, ParagraphSetter::MostWorkFor(ParagraphSetter::kMinRecords));
  EXPECT_LE(set.mostItemsInACall, 1U);
  EXPECT_FALSE(set.tooManyCalls);
  std::size_t characters = 0;
  for (const TextLine& line : set.lines) {
    EXPECT_FALSE(line.overfull);
    characters += line.end - line.begin;
  }
  EXPECT_EQ(characters, 800U);
}

}  // namespace
