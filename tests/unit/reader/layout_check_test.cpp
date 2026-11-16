#include "reader/layout_check.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "hyphen_patterns.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/line_digest.h"
#include "layout/made_up_face.h"
#include "layout/paragraph_setter.h"
#include "reader/trace_ids.h"
#include "storage_fakes.h"
#include "stream_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::LineBreaker;
using bn::layout::ParagraphSetter;
using bn::reader::LayoutCheck;
using bn::test::FakeStorage;
using Bytes = std::vector<std::uint8_t>;
using State = LayoutCheck::State;

Bytes BytesOf(const std::string& text) { return {text.begin(), text.end()}; }

// A clock that moves on a little every time it is read, so that whatever
// is timed takes some time.
class SteppingClock final : public bn::core::Clock {
 public:
  std::uint64_t NowUs() noexcept override {
    now += step;
    return now;
  }
  std::uint64_t now = 0;
  std::uint64_t step = 50;
};

// ---- A second reading of the file, the plain way bn-lines reads one ----

struct Run {
  std::string text;
  std::uint16_t style = 0;
};

// The paragraphs of a file: what is between its empty lines, a carriage
// return at the end of a line left off.
std::vector<std::string> ParagraphsOf(const std::string& contents) {
  std::vector<std::string> paragraphs;
  std::string paragraph;
  std::size_t from = 0;
  while (from <= contents.size()) {
    std::size_t end = contents.find('\n', from);
    end = end == std::string::npos ? contents.size() : end;
    std::string line = contents.substr(from, end - from);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (!paragraph.empty()) {
        paragraphs.push_back(paragraph);
      }
      paragraph.clear();
    } else {
      paragraph += (paragraph.empty() ? "" : "\n") + line;
    }
    from = end + 1;
  }
  if (!paragraph.empty()) {
    paragraphs.push_back(paragraph);
  }
  return paragraphs;
}

std::vector<Run> RunsOf(const std::string& written) {
  std::vector<Run> runs(1);
  for (const char ch : written) {
    if (ch == '*') {
      runs.push_back({.text = "", .style = static_cast<std::uint16_t>(runs.back().style == 0 ? 1 : 0)});
    } else {
      runs.back().text += ch;
    }
  }
  return runs;
}

struct Expected {
  std::uint64_t paragraphs = 0;
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t hyphens = 0;
  std::uint32_t checksum = 0;
  std::uint64_t wordsAsked = 0;
  std::uint64_t bytesMeasured = 0;
};

class Sink final : public bn::layout::TextLineSink {
 public:
  void OnLine(const bn::layout::TextLine& line) noexcept override {
    digest.Add(line);
    hyphens += line.hyphen ? 1U : 0U;
  }
  bn::layout::LineDigest digest;
  std::uint64_t hyphens = 0;
};

// The file set the way the check sets it, the whole of each run given at
// once.
Expected SetPlainly(const std::string& contents, Hyphenator* hyphenator) {
  std::vector<LineBreaker::Node> nodes(LayoutCheck::kNodes);
  std::vector<ParagraphSetter::Record> records(LayoutCheck::kRecords);
  ParagraphSetter setter(nodes, records);
  setter.UseHyphenator(hyphenator);
  bn::layout::MadeUpFace face;
  Sink sink;
  Expected expected;
  const bn::layout::ParagraphSettings settings{.widths = {.first = {}, .rest = LayoutCheck::kWidth},
                                               .hyphenPenalty = LayoutCheck::kHyphenPenalty};
  for (const std::string& paragraph : ParagraphsOf(contents)) {
    EXPECT_EQ(setter.Begin(settings, face, sink), bn::layout::ParagraphStatus::kOk);
    for (const Run& run : RunsOf(paragraph)) {
      std::string_view rest = run.text;
      while (!rest.empty()) {
        rest.remove_prefix(setter.AddText(rest, run.style));
      }
    }
    while (!setter.Finish()) {
    }
    ++expected.paragraphs;
    expected.bytes += setter.Text();
    expected.wordsAsked += setter.WordsAsked();
  }
  expected.lines = sink.digest.Lines();
  expected.hyphens = sink.hyphens;
  expected.checksum = sink.digest.Checksum();
  expected.bytesMeasured = face.BytesMeasured();
  return expected;
}

void ExpectSetAs(const LayoutCheck::Pass& pass, const Expected& expected, const std::string& what) {
  EXPECT_EQ(pass.paragraphs, expected.paragraphs) << what;
  EXPECT_EQ(pass.bytes, expected.bytes) << what;
  EXPECT_EQ(pass.lines, expected.lines) << what;
  EXPECT_EQ(pass.hyphens, expected.hyphens) << what;
  EXPECT_EQ(pass.checksum, expected.checksum) << what;
  EXPECT_EQ(pass.wordsAsked, expected.wordsAsked) << what;
  EXPECT_EQ(pass.bytesMeasured, expected.bytesMeasured) << what;
}

// ---- Text for it to set ----

// Paragraphs of made-up words, some long, with a word now and then in the
// second style and a line ended now and then.
std::string MadeUpText(unsigned seed, int paragraphs) {
  static const std::array<std::string, 12> kWords = {"the",     "harbour", "lamp",         "abababababab",
                                                     "evening", "was",     "ababbaababab", "lit",
                                                     "before",  "anyone",  "had",          "baobabbaobab"};
  std::string text;
  unsigned state = seed;
  const auto next = [&state](unsigned below) {
    state = (state * 1103515245U) + 12345U;
    return (state >> 16U) % below;
  };
  for (int paragraph = 0; paragraph < paragraphs; ++paragraph) {
    const unsigned words = 5 + next(200);
    for (unsigned word = 0; word < words; ++word) {
      const std::string& chosen = kWords[next(static_cast<unsigned>(kWords.size()))];
      text += next(15) == 0 ? "*" + chosen + "*" : chosen;
      text += next(40) == 0 ? "\n" : " ";
    }
    text += "\n\n";
  }
  return text;
}

// A table in which a word may be broken between an a and a b.
Bytes BetweenAAndB() { return bn::test::PlainHyphenTable({"a1b"}, 1, 1); }

struct CheckFixture {
  CheckFixture() { EXPECT_EQ(storage.Start(), bn::core::StorageStatus::kOk); }

  void PutText(const std::string& text) { storage.Put(std::string(LayoutCheck::kTextPath), BytesOf(text)); }
  void PutTable(const Bytes& table) { storage.Put(std::string(LayoutCheck::kTablePath), table); }

  // Turns until the check is no longer running. Returns how many it took.
  // NOLINTNEXTLINE(readability-make-member-function-const): it changes the check, which is only pointed to.
  int RunToTheEnd() {
    int turns = 0;
    while (check->CurrentState() == State::kRunning && turns < 200'000) {
      check->Tick();
      ++turns;
    }
    return turns;
  }

  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    for (const bn::test::StreamItem& item : bn::test::ParseStream(sink.received)) {
      if (item.isFrame && item.type == bn::core::FrameType::kText) {
        lines.push_back(item.Text());
      }
    }
    return lines;
  }
  std::string LineWith(const std::string& part) const {
    for (const std::string& line : Lines()) {
      if (line.find(part) != std::string::npos) {
        return line;
      }
    }
    return {};
  }
  std::string AllLines() const {
    std::string all;
    for (const std::string& line : Lines()) {
      all += line + "\n";
    }
    return all;
  }

  // What the trace holds under `id`, in order.
  std::vector<std::uint32_t> Recorded(bn::core::SpanId id) {
    std::array<bn::core::TraceRecord, 64> records{};
    const std::size_t count = trace.tracer.Drain(records);
    std::vector<std::uint32_t> numbers;
    for (std::size_t i = 0; i < count; ++i) {
      if (records[i].id == id) {
        numbers.push_back(records[i].arg);
      }
    }
    return numbers;
  }

  bn::test::TraceFixture<64> trace;
  bn::test::FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};
  FakeStorage storage;
  SteppingClock clock;
  // On the heap, where the application keeps it.
  std::unique_ptr<LayoutCheck> check = std::make_unique<LayoutCheck>(storage, clock, stream);
};

// ---- Tests ----

TEST(LayoutCheck, DoesNothingUntilItIsBegun) {
  CheckFixture fixture;
  fixture.PutText("Some text.\n");
  EXPECT_EQ(fixture.check->CurrentState(), State::kIdle);
  fixture.check->Tick();
  fixture.check->Tick();
  EXPECT_EQ(fixture.check->CurrentState(), State::kIdle);
  EXPECT_EQ(fixture.storage.opens, 0);
  EXPECT_TRUE(fixture.Lines().empty());
}

TEST(LayoutCheck, SetsTheTextOnTheCardThreeWaysAndSaysWhatItCameTo) {
  CheckFixture fixture;
  const std::string text = MadeUpText(1, 40);
  const Bytes table = BetweenAAndB();
  fixture.PutText(text);
  fixture.PutTable(table);

  fixture.check->Begin();
  EXPECT_EQ(fixture.check->CurrentState(), State::kRunning);
  EXPECT_EQ(fixture.storage.opens, 0) << "nothing happens until a turn";
  fixture.RunToTheEnd();

  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_TRUE(fixture.check->Hyphenating());
  ASSERT_EQ(fixture.check->Passes(), 3U);

  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), bn::layout::HyphenStatus::kOk);
  const Expected hyphenated = SetPlainly(text, &hyphenator);
  const Expected plain = SetPlainly(text, nullptr);
  EXPECT_GT(hyphenated.hyphens, 10U) << "the text is one that is hyphenated";
  EXPECT_NE(hyphenated.checksum, plain.checksum);

  const LayoutCheck::Pass& first = fixture.check->PassFigures(0);
  const LayoutCheck::Pass& second = fixture.check->PassFigures(1);
  const LayoutCheck::Pass& third = fixture.check->PassFigures(2);
  EXPECT_TRUE(first.hyphenated);
  EXPECT_FALSE(first.callByCall);
  EXPECT_FALSE(second.hyphenated);
  EXPECT_FALSE(second.callByCall);
  EXPECT_TRUE(third.hyphenated);
  EXPECT_TRUE(third.callByCall);
  ExpectSetAs(first, hyphenated, "hyphenated");
  ExpectSetAs(second, plain, "not hyphenated");
  ExpectSetAs(third, hyphenated, "every call timed");

  // Time went on each part, and was counted apart.
  for (const LayoutCheck::Pass* pass : {&first, &second, &third}) {
    EXPECT_GT(pass->setUs, 0U);
    EXPECT_GT(pass->cardUs, 0U);
    EXPECT_GE(pass->reads, 1U);
    EXPECT_GT(pass->longestParagraphUs, 0U);
    EXPECT_GT(pass->longestParagraphLines, 0U);
    EXPECT_GT(pass->calls, pass->paragraphs);
    EXPECT_LE(pass->mostSetterWork, ParagraphSetter::MostWorkFor(LayoutCheck::kRecords));
    EXPECT_LE(pass->mostBreakerWork, LineBreaker::MostWorkFor(LayoutCheck::kNodes));
    EXPECT_LE(pass->mostNodes, LayoutCheck::kNodes);
  }
  EXPECT_GT(first.mostHyphenatorWork, 0U);
  EXPECT_EQ(second.mostHyphenatorWork, 0U) << "nothing is left over from the hyphenated setting";
  EXPECT_EQ(first.longestCallUs, 0U) << "only the last setting times every call";
  EXPECT_GT(third.longestCallUs, 0U);

  // And it said so.
  const std::string checksum = [&] {
    std::array<char, 9> hex{};
    std::snprintf(hex.data(), hex.size(), "%08x", static_cast<unsigned>(hyphenated.checksum));
    return std::string(hex.data());
  }();
  EXPECT_EQ(fixture.LineWith("layout check: hyphenated: "),
            "layout check: hyphenated: " + std::to_string(hyphenated.paragraphs) + " paragraphs, " +
                std::to_string(hyphenated.bytes) + " bytes, " + std::to_string(hyphenated.lines) + " lines (" +
                std::to_string(hyphenated.hyphens) + " with a hyphen, 0 too wide); checksum " + checksum + ".");
  EXPECT_NE(fixture.LineWith("layout check: hyphenated, every call timed: "), "");
  EXPECT_NE(fixture.LineWith("layout check: passed."), "");
  EXPECT_NE(fixture.LineWith("layout check: the longest step: "), "");
  EXPECT_EQ(fixture.LineWith("FAILED"), "");
}

TEST(LayoutCheck, ReadsTheFileAsBnLinesDoes) {
  // Each of these is set as bn-lines sets it: line ends of two bytes or
  // one, a carriage return that is not at a line's end, a line of nothing
  // but one, asterisks that change the style across a line end or are all
  // a line has, empty lines in a row, and no line end at the very end.
  const std::vector<std::string> files = {
      "one paragraph\r\nand a line ended in it\r\n\r\nand another\r\n",
      "a carriage\rreturn inside a line\n\r\nafter a line of one\r",
      "*styled\nacross* a line end\n*\n\n\n\nafter empty lines* that end styled",
      "\n\n\nnothing before this but line ends\n\n",
      "ends with a carriage return\r",
      "two\r\r\nat the end of a line\r\r",
      "",
      "*",
  };
  for (std::size_t which = 0; which < files.size(); ++which) {
    CheckFixture fixture;
    fixture.PutText(files[which]);
    fixture.check->Begin();
    fixture.RunToTheEnd();
    const std::string what = "file " + std::to_string(which);
    ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << what << "\n" << fixture.AllLines();
    ASSERT_EQ(fixture.check->Passes(), 2U) << what;
    ExpectSetAs(fixture.check->PassFigures(0), SetPlainly(files[which], nullptr), what);
  }
}

TEST(LayoutCheck, SetsParagraphsThatGoOnPastAStepAndABlock) {
  // Paragraphs longer than the 4 KB that is read at a time, with their
  // carriage returns and asterisks falling at every place a block or a
  // step can end.
  std::string text;
  for (int paragraph = 0; paragraph < 12; ++paragraph) {
    for (int line = 0; line < 60 + (paragraph * 13); ++line) {
      text += "*harbour* lamp abab evening\r\n";
    }
    text += "\r\n";
  }
  ASSERT_GT(text.size(), 6 * LayoutCheck::kChunkBytes);
  CheckFixture fixture;
  fixture.PutText(text);
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  fixture.RunToTheEnd();

  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  Hyphenator hyphenator;
  const Bytes table = BetweenAAndB();
  ASSERT_EQ(hyphenator.Open(table), bn::layout::HyphenStatus::kOk);
  ExpectSetAs(fixture.check->PassFigures(0), SetPlainly(text, &hyphenator), "hyphenated");
  ExpectSetAs(fixture.check->PassFigures(1), SetPlainly(text, nullptr), "not hyphenated");
  EXPECT_GE(fixture.check->PassFigures(0).reads, 6U);
}

TEST(LayoutCheck, SetsTheTextWithoutHyphenationWhenThereIsNoTable) {
  CheckFixture fixture;
  const std::string text = MadeUpText(2, 10);
  fixture.PutText(text);
  fixture.check->Begin();
  fixture.RunToTheEnd();

  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_FALSE(fixture.check->Hyphenating());
  ASSERT_EQ(fixture.check->Passes(), 2U);
  const Expected plain = SetPlainly(text, nullptr);
  EXPECT_FALSE(fixture.check->PassFigures(0).hyphenated);
  EXPECT_FALSE(fixture.check->PassFigures(1).hyphenated);
  EXPECT_TRUE(fixture.check->PassFigures(1).callByCall);
  ExpectSetAs(fixture.check->PassFigures(0), plain, "not hyphenated");
  ExpectSetAs(fixture.check->PassFigures(1), plain, "every call timed");
  EXPECT_EQ(fixture.LineWith("layout check: no table"),
            "layout check: no table at /layout/hyphenation.bnhy (not found): the text is set without hyphenation.");
  EXPECT_NE(fixture.LineWith("layout check: not hyphenated, every call timed: "), "");
  EXPECT_EQ(fixture.LineWith("hyphenator, "), "") << "with no table, no words are hyphenated";
}

TEST(LayoutCheck, GoesWithoutHyphenationWhenTheTableIsNotOneOrIsTooLarge) {
  for (const Bytes& table : {BytesOf("not a table at all"), Bytes(LayoutCheck::kTableBytes + 1, 0)}) {
    CheckFixture fixture;
    fixture.PutText("Some text.\n");
    fixture.PutTable(table);
    fixture.check->Begin();
    fixture.RunToTheEnd();
    ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
    EXPECT_FALSE(fixture.check->Hyphenating());
    EXPECT_EQ(fixture.check->Passes(), 2U);
  }
  CheckFixture fixture;
  fixture.PutText("Some text.\n");
  fixture.PutTable(BytesOf("not a table at all"));
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.LineWith("is not a table"),
            "layout check: the file at /layout/hyphenation.bnhy is not a table (bad table): the text is set without "
            "hyphenation.");
}

TEST(LayoutCheck, FailsWithNoTextToSet) {
  CheckFixture fixture;
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  fixture.RunToTheEnd();

  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_EQ(fixture.check->Passes(), 0U);
  EXPECT_EQ(fixture.LineWith("FAILED"),
            "layout check: FAILED. no text at /layout/paragraphs.txt (not found): make it with tools/layout-text.py.");
  // What does not need the text was done all the same.
  EXPECT_NE(fixture.LineWith("layout check: breaker, 1024 nodes: "), "");
  EXPECT_NE(fixture.LineWith("layout check: setter, awkward text: "), "");
  EXPECT_NE(fixture.LineWith("layout check: the longest step: "), "");
  EXPECT_EQ(fixture.LineWith("layout check: passed."), "");
}

TEST(LayoutCheck, FailsWhenTheTextCannotBeRead) {
  CheckFixture fixture;
  fixture.PutText(MadeUpText(3, 30));
  fixture.check->Begin();
  // Up to where the text is first read, and then the card goes wrong.
  while (fixture.check->CurrentState() == State::kRunning && fixture.LineWith("layout check: text: ").empty()) {
    fixture.check->Tick();
  }
  fixture.storage.failReads = true;
  fixture.RunToTheEnd();

  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_NE(fixture.LineWith("layout check: FAILED. the text could not be read"), "") << fixture.AllLines();
}

TEST(LayoutCheck, BringsTheBreakersCallsCloseToTheirBound) {
  CheckFixture fixture;
  fixture.PutText("Some text.\n");
  fixture.check->Begin();
  fixture.RunToTheEnd();

  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const std::array<std::size_t, 3> nodes = {64, 256, LayoutCheck::kMostNodes};
  for (std::size_t which = 0; which < nodes.size(); ++which) {
    const LayoutCheck::Dearest& run = fixture.check->Breaker(which);
    EXPECT_EQ(run.bound, LineBreaker::MostWorkFor(nodes[which]));
    EXPECT_LE(run.mostWork, run.bound);
    // Every call is timed: the paragraph's items, and its end.
    EXPECT_EQ(run.calls, 2 + (6 * nodes[which]) + 1 + 1);
    EXPECT_GT(run.longestUs, 0U);
    if (nodes[which] >= LineBreaker::kUsualNodes) {
      EXPECT_GT(run.mostWork, run.bound / 8 * 7) << nodes[which];
    }
  }
  EXPECT_EQ(fixture.LineWith("layout check: breaker, 256 nodes: "),
            "layout check: breaker, 256 nodes: the dearest call " +
                std::to_string(fixture.check->Breaker(1).longestUs) + " us, " +
                std::to_string(fixture.check->Breaker(1).mostWork) + " of 37888 work; 1540 calls in " +
                std::to_string(fixture.check->Breaker(1).allUs) + " us.");
}

TEST(LayoutCheck, TimesTheSetterOnAwkwardTextAndTheHyphenatorOnLongWords) {
  CheckFixture fixture;
  fixture.PutText("Some text.\n");
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  fixture.RunToTheEnd();

  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const LayoutCheck::Dearest& setter = fixture.check->Setter();
  EXPECT_EQ(setter.bound, ParagraphSetter::MostWorkFor(LayoutCheck::kRecords));
  EXPECT_GT(setter.mostWork, 0U);
  EXPECT_LE(setter.mostWork, setter.bound);
  EXPECT_LE(setter.breakerWork, LineBreaker::MostWorkFor(LayoutCheck::kNodes));
  EXPECT_GT(setter.calls, 10'000U);
  EXPECT_GT(setter.longestUs, 0U);

  const LayoutCheck::Dearest& words = fixture.check->Words();
  EXPECT_EQ(words.calls, 8U * 64U);
  EXPECT_GT(words.mostWork, 0U);
  EXPECT_LE(words.mostWork, words.bound);
  EXPECT_NE(fixture.LineWith("layout check: hyphenator, 8 long words: "), "");
  EXPECT_NE(fixture.LineWith("layout check: the made-up face alone, 65536 bytes in "), "");
  EXPECT_GT(fixture.check->FaceUs(), 0U);
}

TEST(LayoutCheck, StopsATurnOnceItHasWorkedASlice) {
  CheckFixture fixture;
  fixture.PutText(MadeUpText(4, 20));
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  int turns = 0;
  std::uint64_t longestTurnUs = 0;
  while (fixture.check->CurrentState() == State::kRunning && turns < 200'000) {
    const std::uint64_t before = fixture.clock.now;
    fixture.check->Tick();
    longestTurnUs = std::max(longestTurnUs, fixture.clock.now - before);
    ++turns;
  }
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_GT(turns, 10);
  // A turn goes past its slice by no more than the step it was in, and the
  // clock's reading either side of that.
  EXPECT_LE(longestTurnUs, LayoutCheck::kSliceUs + fixture.check->LongestStepUs() + (3 * fixture.clock.step));
}

TEST(LayoutCheck, ComesToTheSameAgainAndCannotBeBegunWhileItRuns) {
  CheckFixture fixture;
  fixture.PutText(MadeUpText(5, 15));
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  for (int turn = 0; turn < 50; ++turn) {
    fixture.check->Tick();
  }
  fixture.check->Begin();  // Does nothing while it runs.
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const std::uint32_t first = fixture.check->PassFigures(0).checksum;
  const std::uint32_t second = fixture.check->PassFigures(1).checksum;
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanLayoutCheck), std::vector<std::uint32_t>{1});

  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_EQ(fixture.check->PassFigures(0).checksum, first);
  EXPECT_EQ(fixture.check->PassFigures(1).checksum, second);
}

TEST(LayoutCheck, SaysEachThingInALineThatIsWhole) {
  CheckFixture fixture;
  fixture.PutText(MadeUpText(6, 10));
  fixture.PutTable(BetweenAAndB());
  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  for (const std::string& line : fixture.Lines()) {
    EXPECT_LT(line.size(), bn::core::kMaxTextLength - 24) << "room for larger figures: " << line;
    EXPECT_EQ(line.back(), '.') << line;
  }
}

}  // namespace
