// The fuzz target for the paragraph setter (layout/paragraph_setter.h),
// which is given the text of a book a paragraph at a time.
//
// What it is given:
//
//   byte 0   bits 0 to 2:  how wide the lines may be: one of eight widths,
//                          from 1 to 30,000
//            bit 3:        the paragraph's first two lines are narrower
//            bits 4 and 5: how much text is offered in one call: all there
//                          is, or 1, 7 or 61 bytes
//            bit 6:        the setter has the least memory it will work
//                          with, and not the usual
//            bit 7:        there is a hyphenator
//   byte 1   bits 0 and 1: what a hyphen costs
//            bits 2 and 3: what text is measured in: every character 10
//                          wide; a made-up face; that face with each style
//                          wider than the one before; or nothing with any
//                          width at all
//
//   then the paragraph's text. A byte of 0 in it is not text: it asks for
//   the line to end there. Nor is a byte from 1 to 7: what follows it is in
//   the style of that number.
//
// What has to hold, whatever the bytes are:
//
//   - the paragraph is done with in a number of calls that depends only on
//     how long it is, and no call does more than MostWorkFor() says, gives
//     the breaker more than one item, asks the hyphenator about more than
//     one word, or has more than a word's worth of bytes measured; nor does
//     the breaker do more in a call than it says it will;
//   - the lines come in order, each a stretch of the text after the one
//     before, and what is on no line is spaces, line feeds, soft hyphens
//     and zero width spaces;
//   - a line begins and ends with something of a word, is as wide as its
//     text adds up to, has as many places between words as it says, and is
//     said to be too wide when it is;
//   - a line is only too wide for one character that is wider than a line,
//     with nothing else on it that has any width;
//   - the measure is never given a soft hyphen or a zero width space, nor
//     pieces that are out of order; and never a piece that begins inside a
//     character, where the text of every style is whole characters;
//   - the lines are the same when all the text is offered at once.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_support.h"
#include "hyphen_patterns.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/paragraph_setter.h"
#include "text_measure.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::LineBreaker;
using bn::layout::ParagraphSetter;
using bn::layout::ParagraphSettings;
using bn::layout::ParagraphStatus;
using bn::layout::TextLine;
using bn::test::TestMeasure;

constexpr std::array<std::uint16_t, 8> kWidths = {1, 7, 40, 200, 480, 1760, 30000, 65};
constexpr std::array<std::size_t, 4> kAtATime = {0, 1, 7, 61};
constexpr std::array<std::uint32_t, 4> kPenalties = {0, 400, 12100, 0xFFFFFFFF};

constexpr std::string_view kSoftHyphen = "\xC2\xAD";
constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";

// One thing a paragraph is made of: some text in a style, or a line end
// that is asked for and is no byte of the text.
struct Part {
  std::string text;
  std::uint16_t style = 0;
  bool endsLine = false;
};

// How a paragraph is to be set.
struct How {
  std::uint16_t width = 0;
  std::vector<std::uint16_t> first;
  std::uint32_t penalty = 0;
  std::size_t nodes = 0;
  std::size_t records = 0;
  Hyphenator* hyphenator = nullptr;
};

class Kept final : public bn::layout::TextLineSink {
 public:
  void OnLine(const TextLine& line) noexcept override { lines.push_back(line); }

  std::vector<TextLine> lines;
};

// A language for the hyphenator, made once.
Hyphenator* Language() {
  static const std::vector<std::uint8_t> kTable =
      bn::test::PlainHyphenTable({"1a", "1e", "1i", "1o", "1u", "hy3ph", ".un1", "4m1p", "n2at", "a1b", "2bb"}, 2, 2);
  static Hyphenator hyphenator;
  static const bool kOpened = hyphenator.Open(kTable) == bn::layout::HyphenStatus::kOk;
  BN_FUZZ_CHECK(kOpened, "the table the target made was refused");
  return &hyphenator;
}

// Sets the paragraph, offering `atATime` bytes of text in a call (0 for all
// there are), and holds every call to what one call may do.
std::vector<TextLine> Set(const std::vector<Part>& parts, const How& how, std::size_t atATime, TestMeasure& measure,
                          std::size_t textBytes) {
  std::vector<LineBreaker::Node> nodes(how.nodes);
  std::vector<ParagraphSetter::Record> records(how.records);
  ParagraphSetter setter(nodes, records);
  Kept kept;
  setter.UseHyphenator(how.hyphenator);
  const ParagraphSettings settings{.widths = {.first = how.first, .rest = how.width}, .hyphenPenalty = how.penalty};
  BN_FUZZ_CHECK(setter.Begin(settings, measure, kept) == ParagraphStatus::kOk, "a paragraph could not be begun");

  // A few calls for every byte and every line end, and no more.
  const std::size_t callsAllowed = 64 + (16 * (textBytes + parts.size()));
  std::size_t calls = 0;
  std::uint32_t items = 0;
  std::uint32_t asked = 0;
  std::size_t measured = 0;
  const auto before = [&] {
    items = setter.Breaker().Items();
    asked = setter.WordsAsked();
    measured = measure.bytes;
  };
  const auto after = [&] {
    ++calls;
    BN_FUZZ_CHECK(calls <= callsAllowed, "a paragraph takes more calls than its length accounts for");
    BN_FUZZ_CHECK(setter.Breaker().Items() - items <= 1, "one call gave the breaker more than one item");
    BN_FUZZ_CHECK(setter.WordsAsked() - asked <= 1, "one call asked the hyphenator about more than one word");
    BN_FUZZ_CHECK(measure.bytes - measured <= ParagraphSetter::kMaxWordBytes + 3,
                  "one call had more than a word's worth of bytes measured");
  };

  for (const Part& part : parts) {
    if (part.endsLine) {
      bool ended = false;
      while (!ended) {
        before();
        ended = setter.EndLine();
        after();
      }
      continue;
    }
    std::string_view rest = part.text;
    while (!rest.empty()) {
      const std::string_view offered = atATime == 0 ? rest : rest.substr(0, atATime);
      before();
      const std::size_t taken = setter.AddText(offered, part.style);
      after();
      BN_FUZZ_CHECK(taken <= offered.size(), "more text was taken than was offered");
      rest.remove_prefix(taken);
    }
  }
  bool finished = false;
  while (!finished) {
    before();
    finished = setter.Finish();
    after();
  }

  BN_FUZZ_CHECK(setter.Text() == textBytes, "the setter's count of the text it took is not the text");
  BN_FUZZ_CHECK(setter.Lines() == kept.lines.size(), "the setter's count of its lines is not the lines it gave");
  BN_FUZZ_CHECK(setter.MostWork() <= ParagraphSetter::MostWorkFor(how.records),
                "one call did more work than a call may");
  BN_FUZZ_CHECK(setter.Breaker().MostWork() <= LineBreaker::MostWorkFor(how.nodes),
                "one call to the breaker did more work than a call may");
  return kept.lines;
}

bool IsBetweenWords(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

// `text` with the marks taken out that are on no line and have no width.
std::string WithoutMarks(std::string text) {
  for (const std::string_view mark : {kSoftHyphen, kZeroWidthSpace}) {
    for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at)) {
      text.erase(at, mark.size());
    }
  }
  return text;
}

// Whether what is on no line could rightly be on none: spaces, line feeds
// and marks.
bool IsOnNoLine(const std::string& text, std::size_t from, std::size_t end) {
  const std::string left = WithoutMarks(text.substr(from, end - from));
  return std::all_of(left.begin(), left.end(), IsBetweenWords);
}

// Holds the lines to what is true of the lines of any paragraph. `styles`
// has the style of each byte of `text`.
void CheckLines(const std::vector<TextLine>& lines, const std::string& text, const std::vector<std::uint16_t>& styles,
                const How& how, TestMeasure& measure) {
  std::size_t before = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const TextLine& line = lines[i];
    BN_FUZZ_CHECK(line.number == i, "the lines are not numbered as they come");
    BN_FUZZ_CHECK(line.begin <= line.end && line.end <= text.size() && line.begin >= before,
                  "a line is not a stretch of the text after the line before");
    BN_FUZZ_CHECK(IsOnNoLine(text, before, line.begin), "text that is on no line should be on one");
    before = line.end;
    const std::string_view on(text.data() + line.begin, line.end - line.begin);
    if (on.empty()) {
      BN_FUZZ_CHECK(line.width == 0 && line.spaces == 0 && !line.hyphen && line.last,
                    "an empty line is said to have something on it, or not to be a last line");
      continue;
    }
    BN_FUZZ_CHECK(!IsBetweenWords(on.front()) && !IsBetweenWords(on.back()), "a line begins or ends with a space");
    BN_FUZZ_CHECK(on.find('\n') == std::string_view::npos, "a line has a line feed on it");

    // Its width, added up from its bytes. A run of spaces is one space as
    // wide as all of them, and no wider than an item can be; a mark is
    // nothing, and does not part the spaces on either side of it.
    std::uint32_t width = 0;
    std::uint32_t spaces = 0;
    std::uint32_t run = 0;
    std::uint32_t withWidth = 0;  // How many of its characters have any.
    bool inSpaces = false;
    for (std::size_t at = line.begin; at < line.end;) {
      const std::string_view rest(text.data() + at, line.end - at);
      if (rest.starts_with(kSoftHyphen) || rest.starts_with(kZeroWidthSpace)) {
        at += rest.starts_with(kSoftHyphen) ? kSoftHyphen.size() : kZeroWidthSpace.size();
        continue;
      }
      if (IsBetweenWords(text[at])) {
        spaces += inSpaces ? 0U : 1U;
        inSpaces = true;
        run = std::min<std::uint32_t>(run + measure.SpaceWidth(styles[at]), 0xFFFF);
      } else {
        const std::uint16_t character = measure.CharacterWidth(text[at], styles[at]);
        width += run + character;
        withWidth += character != 0 ? 1U : 0U;
        run = 0;
        inSpaces = false;
      }
      ++at;
    }
    if (line.hyphen) {
      width += measure.HyphenWidth(styles[line.end - 1U]);
    }
    BN_FUZZ_CHECK(line.width == width, "a line is not as wide as its text adds up to");
    BN_FUZZ_CHECK(line.spaces == spaces, "a line has not the places between words it is said to have");
    const std::uint32_t allowed = i < how.first.size() ? how.first[i] : how.width;
    BN_FUZZ_CHECK(line.overfull == (line.width > allowed), "a line is wrong about being too wide");
    // One character can be wider than a line, and then the line is too
    // wide: with whatever has no width beside it, and nothing else.
    BN_FUZZ_CHECK(!line.overfull || (withWidth == 1 && spaces == 0),
                  "a line is too wide with more on it than one character");
  }
  BN_FUZZ_CHECK(IsOnNoLine(text, before, text.size()), "text after the last line should be on a line");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const unsigned moreChoices = input.Byte();

  How how;
  how.width = kWidths[choices & 7U];
  if ((choices & 0x08U) != 0) {
    how.first = {static_cast<std::uint16_t>(std::max(how.width / 3, 1)),
                 static_cast<std::uint16_t>(std::max(how.width / 2, 1))};
  }
  const std::size_t atATime = kAtATime[(choices >> 4U) & 3U];
  const bool least = (choices & 0x40U) != 0;
  how.nodes = least ? LineBreaker::kMinNodes : LineBreaker::kUsualNodes;
  how.records = least ? ParagraphSetter::kMinRecords : ParagraphSetter::kUsualRecords;
  how.hyphenator = (choices & 0x80U) != 0 ? Language() : nullptr;
  how.penalty = kPenalties[moreChoices & 3U];
  const unsigned face = (moreChoices >> 2U) & 3U;

  // The paragraph: its parts as they will be given, and its text whole,
  // with the style of every byte.
  std::vector<Part> parts(1);
  std::string text;
  std::vector<std::uint16_t> styles;
  bool wholeCharacters = true;
  for (const std::uint8_t byte : input.Rest()) {
    if (byte > 7) {
      parts.back().text += static_cast<char>(byte);
      text += static_cast<char>(byte);
      styles.push_back(parts.back().style);
      continue;
    }
    wholeCharacters = wholeCharacters && bn::fuzz::IsWholeCharacters(parts.back().text);
    const std::uint16_t style = byte == 0 ? parts.back().style : static_cast<std::uint16_t>(byte);
    if (byte == 0) {
      parts.push_back({.text = "", .style = style, .endsLine = true});
    }
    parts.push_back({.text = "", .style = style, .endsLine = false});
  }
  wholeCharacters = wholeCharacters && bn::fuzz::IsWholeCharacters(parts.back().text);

  const auto measureOf = [face] {
    TestMeasure measure(face == 0 ? 10 : 0);
    measure.widerByStyle = face == 2 ? 3 : 0;
    measure.nothingHasWidth = face == 3;
    return measure;
  };
  TestMeasure measure = measureOf();
  const std::vector<TextLine> lines = Set(parts, how, atATime, measure, text.size());
  BN_FUZZ_CHECK(!measure.measuredAMark, "the measure was given a mark that is on no line");
  BN_FUZZ_CHECK(!measure.badPieces, "the measure was given pieces that do not end in order, where the text does");
  BN_FUZZ_CHECK(!wholeCharacters || !measure.cutInsideACharacter,
                "the measure was given a piece that begins inside a character");
  CheckLines(lines, text, styles, how, measure);

  // And with all the text of each part offered at once, if it was not.
  if (atATime != 0) {
    TestMeasure again = measureOf();
    const std::vector<TextLine> whole = Set(parts, how, 0, again, text.size());
    BN_FUZZ_CHECK(whole.size() == lines.size(), "a paragraph has other lines when its text is offered at once");
    for (std::size_t i = 0; i < lines.size(); ++i) {
      BN_FUZZ_CHECK(whole[i].begin == lines[i].begin && whole[i].end == lines[i].end &&
                        whole[i].width == lines[i].width && whole[i].hyphen == lines[i].hyphen &&
                        whole[i].last == lines[i].last,
                    "a paragraph has other lines when its text is offered at once");
    }
  }
  return 0;
}
