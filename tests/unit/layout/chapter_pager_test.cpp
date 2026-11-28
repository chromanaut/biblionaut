#include "layout/chapter_pager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/segment.h"
#include "hyphenation/american_english.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/made_up_face.h"
#include "layout/page_chooser.h"
#include "layout/paragraph_setter.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::BlockStyle;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleMaker;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::ChapterState;
using bn::epub::LinkKind;
using bn::epub::RunStyle;
using bn::epub::SegmentBlock;
using bn::epub::SegmentBlockKind;
using bn::epub::SegmentPicture;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::epub::SegmentWriter;
using bn::epub::StyleLength;
using bn::layout::BlockMeasures;
using bn::layout::BlockSetting;
using bn::layout::ChapterLine;
using bn::layout::ChapterLineSink;
using bn::layout::ChapterPage;
using bn::layout::ChapterPager;
using bn::layout::ChapterPlace;
using bn::layout::LineBreaker;
using bn::layout::MadeUpFace;
using bn::layout::PageFromBottom;
using bn::layout::PagerStatus;
using bn::layout::ParagraphSetter;
using bn::layout::ParagraphSettings;
using bn::layout::ParagraphStatus;
using bn::layout::RunMeasures;
using bn::layout::TextLine;
using bn::layout::TextLineSink;
using bn::test::FakeStorage;

const char* const kPath = "/book.bundle";

// ---- A chapter, as a test writes one down ----

// One thing in a block.
struct Piece {
  enum class What : std::uint8_t { kText, kStyle, kPicture, kLink, kLinkEnd };
  What what = What::kText;
  std::string text;
  RunStyle run;
  std::uint16_t picture = 0;
  StyleLength width;
  StyleLength height;
};

struct TestBlock {
  SegmentBlockKind kind = SegmentBlockKind::kParagraph;
  BlockStyle style;
  std::vector<Piece> pieces;
};

using Chapter = std::vector<TestBlock>;
// The size of each picture whose size the chapter's directory has.
using Sizes = std::map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>>;

Piece Words(std::string text) {
  Piece piece;
  piece.text = std::move(text);
  return piece;
}

Piece InStyle(std::uint16_t bits, std::uint8_t language = 0) {
  Piece piece;
  piece.what = Piece::What::kStyle;
  piece.run.bits = bits;
  piece.run.language = language;
  return piece;
}

Piece Picture(std::uint16_t number, std::int16_t declaredHeight = 0) {
  Piece piece;
  piece.what = Piece::What::kPicture;
  piece.picture = number;
  piece.height.px = declaredHeight;
  return piece;
}

TestBlock Paragraph(std::string text, const BlockStyle& style = {}) {
  TestBlock block;
  block.style = style;
  block.pieces.push_back(Words(std::move(text)));
  return block;
}

// A style with some of what a block may say, each in whole pixels as the
// tests' own rules read them (below).
struct Says {
  std::int16_t lineHeight = 0;
  std::int16_t indent = 0;
  std::int16_t left = 0;
  std::int16_t right = 0;
  std::int16_t before = 0;
  std::int16_t after = 0;
  std::uint16_t breaking = 0;
};

BlockStyle StyleThat(const Says& says) {
  BlockStyle style;
  const auto set = [&style](std::uint16_t field, StyleLength& length, std::int16_t value) {
    if (value != 0) {
      style.has = static_cast<std::uint16_t>(style.has | field);
      length.px = value;
    }
  };
  set(bn::epub::kBlockLineHeight, style.lineHeight, says.lineHeight);
  set(bn::epub::kBlockIndent, style.indent, says.indent);
  set(bn::epub::kBlockMarginLeft, style.marginLeft, says.left);
  set(bn::epub::kBlockMarginRight, style.marginRight, says.right);
  set(bn::epub::kBlockSpaceBefore, style.spaceBefore, says.before);
  set(bn::epub::kBlockSpaceAfter, style.spaceAfter, says.after);
  if (says.breaking != 0) {
    style.has = static_cast<std::uint16_t>(style.has | bn::epub::kBlockBreaking);
    style.breaking = says.breaking;
  }
  return style;
}

// ---- What a block comes to, by the tests' own rules ----

// A block's lengths are taken as whole pixels and nothing else of them is
// looked at; a run's style is its two lowest bits; and a run in a
// language of the chapter's has its words broken by `hyphenator`.
class Rules final : public BlockSetting {
 public:
  static std::uint16_t Pixels(const StyleLength& length) {
    return static_cast<std::uint16_t>(std::clamp<int>(length.px, 0, 30000));
  }

  void MeasureBlock(const SegmentBlock& block, BlockMeasures& measures) noexcept override {
    ++blocksAsked;
    const int rest =
        std::clamp<int>(width - Pixels(block.style.marginLeft) - Pixels(block.style.marginRight), width / 4, width);
    measures.restWidth = static_cast<std::uint16_t>(rest);
    measures.firstWidth =
        static_cast<std::uint16_t>(std::clamp<int>(rest - Pixels(block.style.indent), width / 4, width));
    measures.lineHeight = Pixels(block.style.lineHeight) != 0 ? Pixels(block.style.lineHeight) : usualLineHeight;
    measures.spaceBefore = Pixels(block.style.spaceBefore);
    measures.spaceAfter = Pixels(block.style.spaceAfter);
    measures.hyphenPenalty = hyphenPenalty;
    if (badWidths) {
      measures.restWidth = 0;
    }
  }

  void MeasureRun(const SegmentBlock& /*block*/, const RunStyle& run, RunMeasures& measures) noexcept override {
    ++runsAsked;
    measures.style = static_cast<std::uint16_t>(run.bits & 3U);
    measures.hyphenator = run.language != 0 ? hyphenator : nullptr;
  }

  std::uint16_t PictureHeight(const SegmentBlock& /*block*/, const StyleLength& /*width*/, const StyleLength& height,
                              const SegmentPicture* picture) noexcept override {
    ++picturesAsked;
    if (picture != nullptr && picture->sized) {
      return picture->height;
    }
    return Pixels(height) != 0 ? Pixels(height) : unknownPicture;
  }

  int width = 1200;  // Of a page's text, in the made-up face's quarters of a pixel.
  std::uint16_t usualLineHeight = 20;
  std::uint16_t unknownPicture = 100;
  bn::layout::Hyphenator* hyphenator = nullptr;
  std::uint32_t hyphenPenalty = 0;
  bool badWidths = false;
  int blocksAsked = 0;
  int runsAsked = 0;
  int picturesAsked = 0;
};

// What gathers lines.
class Gathered final : public TextLineSink, public ChapterLineSink {
 public:
  void OnLine(const TextLine& line) noexcept override { text.push_back(line); }
  void OnChapterLine(const ChapterLine& line) noexcept override { lines.push_back(line); }
  std::vector<TextLine> text;
  std::vector<ChapterLine> lines;
};

bool Asks(const BlockStyle& style, std::uint16_t bit) { return (style.breaking & bit) != 0; }

// Every line of a chapter, worked out a block at a time with the setter
// given each block's text whole, and none of the pager.
std::vector<ChapterLine> LinesByHand(const Chapter& chapter, const Sizes& sizes, Rules rules) {
  std::vector<ChapterLine> all;
  std::vector<LineBreaker::Node> nodes(LineBreaker::kUsualNodes);
  std::vector<ParagraphSetter::Record> records(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter(nodes, records);
  MadeUpFace face;
  bool keptWithBefore = false;
  std::uint32_t spaceAbove = 0;
  for (std::uint32_t number = 0; number < chapter.size(); ++number) {
    const TestBlock& block = chapter[number];
    SegmentBlock head;
    head.kind = static_cast<std::uint8_t>(block.kind);
    head.style = block.style;
    BlockMeasures measures;
    rules.MeasureBlock(head, measures);
    std::uint16_t lineHeight = measures.lineHeight;

    const std::array<std::uint16_t, 1> first = {measures.firstWidth};
    const ParagraphSettings settings{
        .widths = {.first = measures.firstWidth != measures.restWidth ? std::span<const std::uint16_t>(first)
                                                                      : std::span<const std::uint16_t>(),
                   .rest = measures.restWidth},
        .hyphenPenalty = 0};
    Gathered gathered;
    EXPECT_EQ(setter.Begin(settings, face, gathered), ParagraphStatus::kOk);
    RunMeasures run;
    rules.MeasureRun(head, RunStyle{}, run);
    setter.UseHyphenator(run.hyphenator);
    bool somethingSeen = false;
    const auto give = [&setter, &run](std::string_view text) {
      while (!text.empty()) {
        text.remove_prefix(setter.AddText(text, run.style));
      }
    };
    for (const Piece& piece : block.pieces) {
      if (piece.what == Piece::What::kStyle) {
        rules.MeasureRun(head, piece.run, run);
        setter.UseHyphenator(run.hyphenator);
      } else if (piece.what == Piece::What::kText) {
        give(piece.text);
        somethingSeen = somethingSeen || !piece.text.empty();
      } else if (piece.what == Piece::What::kPicture) {
        if (block.kind == SegmentBlockKind::kPicture && !somethingSeen) {
          SegmentPicture sized;
          const auto found = sizes.find(piece.picture);
          if (found != sizes.end()) {
            sized = {
                .number = piece.picture, .width = found->second.first, .height = found->second.second, .sized = true};
          }
          lineHeight = rules.PictureHeight(head, piece.width, piece.height, found != sizes.end() ? &sized : nullptr);
        }
        give(std::string_view(&ChapterPager::kPictureByte, 1));
        somethingSeen = true;
      }
    }
    while (!setter.Finish()) {
    }
    if (gathered.text.empty()) {
      gathered.text.push_back(TextLine{.last = true});
    }
    for (std::size_t i = 0; i < gathered.text.size(); ++i) {
      const TextLine& line = gathered.text[i];
      ChapterLine made;
      made.block = number;
      made.begin = line.begin;
      made.end = line.end;
      made.width = line.width;
      made.height = std::max<std::uint16_t>(lineHeight, 1);
      made.spaces = line.spaces;
      std::uint32_t flags = (line.hyphen ? std::uint32_t{bn::layout::kLineHyphen} : 0U) |
                            (line.last ? std::uint32_t{bn::layout::kLineLast} : 0U) |
                            (line.overfull ? std::uint32_t{bn::layout::kLineOverfull} : 0U);
      if (i == 0) {
        made.gap = static_cast<std::uint16_t>(std::min<std::uint32_t>(measures.spaceBefore + spaceAbove, 65535));
        flags |= keptWithBefore ? std::uint32_t{bn::layout::kLineKept} : 0U;
        flags |=
            number == 0 || Asks(block.style, bn::epub::kBreakPageBefore) ? std::uint32_t{bn::layout::kLineStarts} : 0U;
      } else {
        flags |= Asks(block.style, bn::epub::kBreakKeepTogether) ? std::uint32_t{bn::layout::kLineKept} : 0U;
      }
      made.flags = static_cast<std::uint8_t>(flags);
      all.push_back(made);
    }
    keptWithBefore = Asks(block.style, bn::epub::kBreakKeepWithNext);
    spaceAbove = measures.spaceAfter;
  }
  return all;
}

// ---- Pages, worked out by adding heights up over the whole chapter ----

using Lines = std::vector<ChapterLine>;

bool Starts(const ChapterLine& line) { return line.Is(bn::layout::kLineStarts); }
bool MayBegin(const ChapterLine& line) { return Starts(line) || !line.Is(bn::layout::kLineKept); }

// How high the lines from `first` up to, and not including, `end` stand on
// a page that begins with `first`.
std::uint32_t HeightOf(const Lines& lines, std::size_t first, std::size_t end) {
  std::uint32_t height = lines[first].height;
  if (Starts(lines[first])) {
    height += lines[first].gap;
  }
  for (std::size_t i = first + 1; i < end; ++i) {
    height += std::uint32_t{lines[i].gap} + lines[i].height;
  }
  return height;
}

// Where a page that begins with `first` ends.
std::size_t DownByHand(const Lines& lines, std::size_t first, std::uint16_t pageHeight) {
  std::size_t end = first + 1;
  while (end < lines.size() && !Starts(lines[end]) && HeightOf(lines, first, end + 1) <= pageHeight) {
    ++end;
  }
  if (end < lines.size() && !Starts(lines[end]) && lines[end].Is(bn::layout::kLineKept)) {
    for (std::size_t place = end - 1; place > first; --place) {
      if (!lines[place].Is(bn::layout::kLineKept)) {
        return place;
      }
    }
  }
  return end;
}

// Where a page that ends before `end` begins.
std::size_t UpByHand(const Lines& lines, std::size_t end, std::uint16_t pageHeight) {
  std::size_t highest = end - 1;
  while (highest > 0 && !Starts(lines[highest]) && HeightOf(lines, highest - 1, end) <= pageHeight) {
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
      return asked;
    }
    if (MayBegin(lines[place]) || place == 0) {
      return place;
    }
  }
}

// How many of the lines are above a place: in a block before it, or in
// its block and beginning before it.
std::size_t LinesAbove(const Lines& lines, ChapterPlace place) {
  std::size_t above = 0;
  while (above < lines.size() &&
         (lines[above].block < place.block || (lines[above].block == place.block && lines[above].begin < place.into))) {
    ++above;
  }
  return above;
}

// The line that holds a place: the last of its block that begins at or
// before it, and the block's first if none does.
std::size_t LineHolding(const Lines& lines, ChapterPlace place) {
  std::size_t holding = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].block == place.block && (holding == lines.size() || lines[i].begin <= place.into)) {
      holding = i;
    }
  }
  return holding;
}

ChapterPlace PlaceOf(const Lines& lines, std::size_t index, std::uint32_t blocks) {
  return index < lines.size() ? lines[index].Place() : ChapterPlace{.block = blocks, .into = 0};
}

// ---- Numbers that are the same every run ----

class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}
  std::uint32_t Next() {
    state_ = (state_ * 1664525U) + 1013904223U;
    return state_ >> 8U;
  }
  std::uint32_t Below(std::uint32_t limit) { return Next() % limit; }
  bool OneIn(std::uint32_t n) { return Below(n) == 0; }

 private:
  std::uint32_t state_;
};

std::string SomeWords(Dice& dice, std::size_t bytes) {
  static const std::array<std::string_view, 14> kWords = {
      "the ",   "quick ", "brown ",      "fox ", "caf\xC3\xA9 ",  "na\xC3\xAFve ", "\xE2\x80\x94 ",
      "jumps ", "over ",  "well-known ", "a ",   "immeasurably ", "it\nwas ",      "WIDE  "};
  std::string text;
  while (text.size() < bytes) {
    text += kWords.at(dice.Below(kWords.size()));
  }
  return text;
}

std::string WordsFrom(std::uint32_t seed, std::size_t bytes) {
  Dice dice(seed);
  return SomeWords(dice, bytes);
}

// A chapter of so many blocks, with everything a page is chosen by: each
// number drawn in a statement of its own.
Chapter MadeUpChapter(std::uint32_t seed, std::size_t blocks) {
  Dice dice(seed);
  Chapter chapter;
  for (std::size_t i = 0; i < blocks; ++i) {
    TestBlock block;
    Says says;
    if (dice.OneIn(3)) {
      says.lineHeight = static_cast<std::int16_t>(14 + dice.Below(30));
    }
    if (dice.OneIn(4)) {
      says.indent = static_cast<std::int16_t>(dice.Below(200));
    }
    if (dice.OneIn(5)) {
      says.left = static_cast<std::int16_t>(dice.Below(300));
    }
    if (dice.OneIn(5)) {
      says.right = static_cast<std::int16_t>(dice.Below(300));
    }
    if (dice.OneIn(3)) {
      says.before = static_cast<std::int16_t>(dice.Below(40));
    }
    if (dice.OneIn(3)) {
      says.after = static_cast<std::int16_t>(dice.Below(40));
    }
    const std::uint32_t breaks = dice.Below(24);
    says.breaking = static_cast<std::uint16_t>((breaks == 0 ? bn::epub::kBreakPageBefore : 0U) |
                                               (breaks < 4 ? bn::epub::kBreakKeepWithNext : 0U) |
                                               (breaks % 5 == 1 ? bn::epub::kBreakKeepTogether : 0U));
    block.style = StyleThat(says);
    const std::uint32_t pick = dice.Below(20);
    if (pick == 0) {
      block.kind = SegmentBlockKind::kRule;
    } else if (pick == 1) {
      block.kind = SegmentBlockKind::kPicture;
      const auto number = static_cast<std::uint16_t>(dice.Below(12));
      const auto declared = static_cast<std::int16_t>(dice.OneIn(2) ? 0 : 30 + dice.Below(900));
      block.pieces.push_back(Picture(number, declared));
    } else if (pick == 2) {
      block.pieces.push_back(Words("   "));
    } else {
      block.kind = pick == 3 ? SegmentBlockKind::kHeading : SegmentBlockKind::kParagraph;
      const std::uint32_t runs = 1 + dice.Below(4);
      for (std::uint32_t run = 0; run < runs; ++run) {
        if (dice.OneIn(2)) {
          block.pieces.push_back(InStyle(static_cast<std::uint16_t>(dice.Below(8))));
        }
        const std::uint32_t size = dice.Below(12);
        const std::size_t bytes = size == 0  ? 3000 + dice.Below(4000)
                                  : size < 3 ? 400 + dice.Below(900)
                                             : dice.Below(160);
        block.pieces.push_back(Words(SomeWords(dice, bytes)));
        if (dice.OneIn(8)) {
          block.pieces.push_back(Picture(static_cast<std::uint16_t>(dice.Below(12))));
        }
      }
    }
    chapter.push_back(std::move(block));
  }
  return chapter;
}

// ---- A bundle with the chapter in it, and a pager over it ----

struct PagerFixture {
  explicit PagerFixture(std::size_t lineRecords = 64, std::size_t entryCount = PageFromBottom::kUsualEntries)
      : records(lineRecords), entries(entryCount) {
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
  }

  void MakeBundle() {
    BundleMaker maker(page);
    ASSERT_EQ(file.Open(storage, kPath, OpenMode::kCreate), StorageStatus::kOk);
    ASSERT_EQ(maker.Begin(file), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
    ASSERT_EQ(maker.WriteFile(BundleFile{}), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
    BundleBook book;
    book.documents = 1;
    ASSERT_EQ(maker.Finish(book), BundleStatus::kOk);
    while (maker.Step()) {
    }
    ASSERT_EQ(maker.Status(), BundleStatus::kOk);
    file.Close();
    ASSERT_EQ(file.Open(storage, kPath, OpenMode::kUpdate), StorageStatus::kOk);
    ASSERT_EQ(bundle.Open(file), BundleStatus::kOk);
  }

  // Writes the chapter into a new bundle, opens it to be read, and makes
  // the pager.
  void Write(const Chapter& written, const Sizes& pictureSizes = {}) {
    chapter = written;
    sizes = pictureSizes;
    MakeBundle();
    ASSERT_EQ(writer.Begin(file, bundle.DataEnd()), SegmentStatus::kOk);
    // The one language a run of these chapters may be in.
    ASSERT_EQ(writer.Language("fr"), 1);
    std::uint32_t source = 0;
    for (const TestBlock& block : written) {
      source += 100;
      ASSERT_EQ(writer.BeginBlock(block.kind, block.kind == SegmentBlockKind::kHeading ? 2 : 0, block.style, source),
                SegmentStatus::kOk);
      for (const Piece& piece : block.pieces) {
        switch (piece.what) {
          case Piece::What::kText:
            // A piece at a time, cut where a character ends and wherever
            // else that falls, as a document's text comes.
            for (std::size_t at = 0; at < piece.text.size();) {
              std::size_t end = std::min(piece.text.size(), at + 700);
              while (end < piece.text.size() && (static_cast<std::uint8_t>(piece.text[end]) & 0xC0U) == 0x80U) {
                ++end;
              }
              ASSERT_EQ(writer.Text(std::string_view(piece.text).substr(at, end - at)), SegmentStatus::kOk);
              at = end;
            }
            break;
          case Piece::What::kStyle:
            ASSERT_EQ(writer.Style(piece.run), SegmentStatus::kOk);
            break;
          case Piece::What::kPicture:
            ASSERT_EQ(writer.Picture(piece.picture, 0, piece.width, piece.height, "a picture"), SegmentStatus::kOk);
            break;
          case Piece::What::kLink:
            ASSERT_EQ(writer.LinkStart(LinkKind::kInside, 0, piece.text), SegmentStatus::kOk);
            break;
          case Piece::What::kLinkEnd:
            ASSERT_EQ(writer.LinkEnd(), SegmentStatus::kOk);
            break;
        }
      }
      ASSERT_EQ(writer.EndBlock(), SegmentStatus::kOk);
    }
    Finish();
  }

  // Ends the chapter the writer was given, commits it and opens it.
  void Finish() {
    ASSERT_EQ(writer.BeginEndBlocks(), SegmentStatus::kOk);
    while (writer.Step()) {
    }
    ASSERT_EQ(writer.Status(), SegmentStatus::kOk);
    for (std::size_t i = 0; i < writer.Pictures(); ++i) {
      if (const auto found = sizes.find(writer.PictureNumber(i)); found != sizes.end()) {
        ASSERT_EQ(writer.SetPictureSize(i, found->second.first, found->second.second), SegmentStatus::kOk);
      }
    }
    ASSERT_EQ(writer.BeginFinish(ChapterState::kWhole, 0, 0), SegmentStatus::kOk);
    while (writer.Step()) {
    }
    ASSERT_EQ(writer.Status(), SegmentStatus::kOk);
    ASSERT_EQ(bundle.Commit(0, writer.Chapter()), BundleStatus::kOk);
    BundleChapter record;
    ASSERT_EQ(bundle.ReadChapter(0, record), BundleStatus::kOk);
    ASSERT_EQ(reader.Open(file, record), SegmentStatus::kOk);
  }

  Lines ByHand() const { return LinesByHand(chapter, sizes, rules); }
  std::uint32_t Blocks() const { return static_cast<std::uint32_t>(chapter.size()); }

  // Does the work that was begun to its end, each step held to what a
  // step may do.
  PagerStatus Run() {
    std::size_t steps = 0;
    for (bool more = true; more; ++steps) {
      const std::size_t before = storage.reads.size();
      more = pager.Step();
      std::size_t bytes = 0;
      for (std::size_t i = before; i < storage.reads.size(); ++i) {
        bytes += storage.reads[i].size;
      }
      const ChapterPager::StepWork& work = pager.LastStep();
      EXPECT_LE(storage.reads.size() - before, 2U) << "more than two reads of storage in a step";
      EXPECT_LE(bytes, 8192U);
      EXPECT_LE(work.readerCalls + work.setterCalls, 1) << "a step that called both, or one twice";
      EXPECT_LE(work.lines, nodes.size());
      EXPECT_TRUE(work.readerCalls != 0 || storage.reads.size() == before) << "a read with no call of the reader";
      mostLines = std::max(mostLines, work.lines);
      if (steps > 4'000'000) {
        ADD_FAILURE() << "the work does not end";
        break;
      }
    }
    stepsTaken += steps;
    return pager.Status();
  }

  struct Got {
    PagerStatus status = PagerStatus::kOk;
    ChapterPage page;
    Lines lines;
  };

  Got Result(PagerStatus begun) {
    Got got;
    got.status = begun == PagerStatus::kOk ? Run() : begun;
    got.page = pager.Page();
    got.lines.assign(pager.Lines().begin(), pager.Lines().end());
    return got;
  }
  Got Down(ChapterPlace first) { return Result(pager.BeginDown(first, pageHeight)); }
  Got Up(ChapterPlace before) { return Result(pager.BeginUp(before, pageHeight)); }
  Got About(ChapterPlace place) { return Result(pager.BeginAbout(place, pageHeight)); }
  Lines LinesOf(std::uint32_t block) {
    Gathered gathered;
    const PagerStatus begun = pager.BeginLines(block, gathered);
    EXPECT_EQ(begun, PagerStatus::kOk);
    EXPECT_EQ(Run(), PagerStatus::kOk);
    return gathered.lines;
  }

  FakeStorage storage;
  alignas(4) std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  alignas(4) std::array<std::uint8_t, 4096> readerPage{};
  std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(bn::epub::kSegmentWriterBytes, 0xEE);
  BlockFile file{fileBuffer};
  Bundle bundle{page};
  SegmentWriter writer{memory};
  SegmentReader reader{readerPage};
  MadeUpFace face;
  Rules rules;
  std::vector<LineBreaker::Node> nodes = std::vector<LineBreaker::Node>(LineBreaker::kUsualNodes);
  std::vector<ParagraphSetter::Record> setterRecords =
      std::vector<ParagraphSetter::Record>(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter{nodes, setterRecords};
  std::vector<ChapterLine> records;
  std::vector<PageFromBottom::Entry> entries;
  ChapterPager pager{reader, setter, face, rules, entries, records};
  Chapter chapter;
  Sizes sizes;
  std::uint16_t pageHeight = 400;
  std::size_t stepsTaken = 0;
  std::uint32_t mostLines = 0;
};

// What is the same of two lines, a line on a page having a top besides.
void ExpectSameLine(const ChapterLine& got, const ChapterLine& want, const std::string& what) {
  EXPECT_EQ(got.block, want.block) << what;
  EXPECT_EQ(got.begin, want.begin) << what;
  EXPECT_EQ(got.end, want.end) << what;
  EXPECT_EQ(got.width, want.width) << what;
  EXPECT_EQ(got.height, want.height) << what;
  EXPECT_EQ(got.gap, want.gap) << what;
  EXPECT_EQ(got.spaces, want.spaces) << what;
  EXPECT_EQ(int{got.flags}, int{want.flags}) << what;
}

std::string Named(const char* what, ChapterPlace place) {
  return std::string(what) + " " + std::to_string(place.block) + "+" + std::to_string(place.into);
}

// The page is the lines `first` up to `end` of the chapter's, each where
// adding the heights up puts it.
void ExpectPage(const PagerFixture::Got& got, const Lines& all, std::size_t first, std::size_t end, bool filledDown,
                std::uint32_t blocks, const std::string& what, const ChapterPlace* next = nullptr) {
  ASSERT_EQ(got.status, PagerStatus::kOk) << what;
  ASSERT_TRUE(got.page.found) << what;
  EXPECT_EQ(got.page.filledDown, filledDown) << what;
  EXPECT_FALSE(got.page.cutShort) << what;
  EXPECT_EQ(got.page.first, PlaceOf(all, first, blocks))
      << what << ": it begins at " << got.page.first.block << "+" << got.page.first.into;
  EXPECT_EQ(got.page.next, next != nullptr ? *next : PlaceOf(all, end, blocks))
      << what << ": it ends at " << got.page.next.block << "+" << got.page.next.into;
  ASSERT_EQ(got.page.lines, end - first) << what;
  ASSERT_EQ(got.lines.size(), end - first) << what;
  EXPECT_EQ(got.page.height, HeightOf(all, first, end)) << what;
  std::uint32_t y = 0;
  for (std::size_t i = first; i < end; ++i) {
    const std::string which = what + ", line " + std::to_string(i - first);
    ExpectSameLine(got.lines[i - first], all[i], which);
    const std::uint32_t top = i == first ? (Starts(all[i]) ? all[i].gap : 0U) : y + all[i].gap;
    EXPECT_EQ(got.lines[i - first].top, top) << which;
    y = top + all[i].height;
  }
}

// The page above a place, as adding up has it: filled down from its first
// line if that line starts a page, and up to the place otherwise.
void ExpectPageAbove(const PagerFixture::Got& got, const Lines& all, ChapterPlace before, std::uint16_t pageHeight,
                     std::uint32_t blocks) {
  const std::string what = Named("the page above", before);
  const std::size_t end = LinesAbove(all, before);
  if (end == 0) {
    EXPECT_EQ(got.status, PagerStatus::kOk) << what;
    EXPECT_FALSE(got.page.found) << what;
    return;
  }
  const std::size_t first = UpByHand(all, end, pageHeight);
  if (Starts(all[first])) {
    ExpectPage(got, all, first, DownByHand(all, first, pageHeight), true, blocks, what);
  } else {
    ExpectPage(got, all, first, end, false, blocks, what, &before);
  }
}

// ---- A block's lines ----

TEST(ChapterPager, GivesEachBlocksLinesAsTheSetterSetsTheBlocksText) {
  PagerFixture f;
  f.Write(MadeUpChapter(11, 70), {{1, {300, 200}}, {4, {800, 2000}}, {7, {10, 10}}});
  const Lines all = f.ByHand();
  std::size_t at = 0;
  for (std::uint32_t block = 0; block < f.Blocks(); ++block) {
    const Lines lines = f.LinesOf(block);
    ASSERT_FALSE(lines.empty()) << block;
    for (const ChapterLine& line : lines) {
      ASSERT_LT(at, all.size());
      ExpectSameLine(line, all[at], "block " + std::to_string(block) + ", line at " + std::to_string(line.begin));
      EXPECT_EQ(line.top, 0U);
      ++at;
    }
  }
  EXPECT_EQ(at, all.size());
  // Some of everything was there to be set.
  const auto any = [&all](std::uint8_t flag) {
    return std::any_of(all.begin(), all.end(), [flag](const ChapterLine& line) { return line.Is(flag); });
  };
  EXPECT_TRUE(any(bn::layout::kLineKept));
  EXPECT_TRUE(any(bn::layout::kLineLast));
  EXPECT_GT(std::count_if(all.begin(), all.end(), Starts), 1);
  EXPECT_GT(all.size(), f.Blocks() * 3U);
}

TEST(ChapterPager, GivesABlockThatComesToNoLineOneLineWithNothingOnIt) {
  PagerFixture f;
  TestBlock rule;
  rule.kind = SegmentBlockKind::kRule;
  rule.style = StyleThat({.lineHeight = 9, .before = 7});
  f.Write({Paragraph("One."), rule, Paragraph("    "), Paragraph("Two.")});
  for (const std::uint32_t block : {1U, 2U}) {
    const Lines lines = f.LinesOf(block);
    ASSERT_EQ(lines.size(), 1U) << block;
    EXPECT_EQ(lines[0].begin, 0U);
    EXPECT_EQ(lines[0].end, 0U);
    EXPECT_EQ(lines[0].width, 0U);
    EXPECT_TRUE(lines[0].Is(bn::layout::kLineLast));
  }
  EXPECT_EQ(f.LinesOf(1)[0].height, 9);
  EXPECT_EQ(f.LinesOf(1)[0].gap, 7);
  // And such a line is on a page like any other.
  const PagerFixture::Got page = f.Down({0, 0});
  EXPECT_EQ(page.page.lines, 4U);
  EXPECT_EQ(page.page.height, 20U + 7U + 9U + 20U + 20U);
}

TEST(ChapterPager, PutsTheSpaceAfterABlockAboveTheNextAndTheBlocksOwnSpaceWithIt) {
  PagerFixture f;
  f.Write({Paragraph("One.", StyleThat({.before = 50, .after = 11})), Paragraph("Two.", StyleThat({.before = 5})),
           Paragraph("Three."), Paragraph(WordsFrom(3, 400), StyleThat({.before = 6}))});
  EXPECT_EQ(f.LinesOf(0)[0].gap, 50);
  EXPECT_EQ(f.LinesOf(1)[0].gap, 16);
  EXPECT_EQ(f.LinesOf(2)[0].gap, 0);
  // Only above a block's first line.
  const Lines long4 = f.LinesOf(3);
  ASSERT_GT(long4.size(), 2U);
  EXPECT_EQ(long4[0].gap, 6);
  EXPECT_EQ(long4[1].gap, 0);
  // The chapter's first line starts a page, and brings its space with it;
  // the others' are between them and the line before.
  const PagerFixture::Got page = f.Down({0, 0});
  ASSERT_GE(page.lines.size(), 4U);
  EXPECT_EQ(page.lines[0].top, 50U);
  EXPECT_EQ(page.lines[1].top, 50U + 20U + 16U);
  EXPECT_EQ(page.lines[2].top, 50U + 20U + 16U + 20U);
}

TEST(ChapterPager, MarksWhatIsKeptWithTheLineBeforeAndWhatStartsAPage) {
  PagerFixture f;
  const std::string longText = WordsFrom(5, 500);
  f.Write({Paragraph("A heading", StyleThat({.breaking = bn::epub::kBreakKeepWithNext})), Paragraph(longText),
           Paragraph(longText, StyleThat({.breaking = bn::epub::kBreakKeepTogether})),
           Paragraph(longText, StyleThat({.breaking = bn::epub::kBreakPageBefore})), Paragraph("Last.")});
  const auto flagsOf = [&f](std::uint32_t block) {
    std::vector<int> flags;
    for (const ChapterLine& line : f.LinesOf(block)) {
      flags.push_back(static_cast<int>(std::uint32_t{line.flags} &
                                       (std::uint32_t{bn::layout::kLineKept} | bn::layout::kLineStarts)));
    }
    return flags;
  };
  const int kept = bn::layout::kLineKept;
  const int starts = bn::layout::kLineStarts;
  EXPECT_EQ(flagsOf(0), std::vector<int>({starts}));
  const std::vector<int> under = flagsOf(1);
  ASSERT_GT(under.size(), 2U);
  EXPECT_EQ(under[0], kept);  // The line under the heading.
  EXPECT_EQ(std::count(under.begin(), under.end(), 0), static_cast<std::ptrdiff_t>(under.size() - 1));
  const std::vector<int> together = flagsOf(2);
  EXPECT_EQ(together[0], 0);
  EXPECT_EQ(std::count(together.begin(), together.end(), kept), static_cast<std::ptrdiff_t>(together.size() - 1));
  const std::vector<int> broken = flagsOf(3);
  EXPECT_EQ(broken[0], starts);
  EXPECT_EQ(std::count(broken.begin(), broken.end(), 0), static_cast<std::ptrdiff_t>(broken.size() - 1));
  EXPECT_EQ(flagsOf(4), std::vector<int>({0}));
}

TEST(ChapterPager, MakesTheLinesOfABlockThatIsAPictureAsHighAsThePicture) {
  PagerFixture f;
  TestBlock sized;
  sized.kind = SegmentBlockKind::kPicture;
  sized.pieces = {Picture(3, 55)};
  TestBlock declared;
  declared.kind = SegmentBlockKind::kPicture;
  declared.pieces = {Picture(4, 55)};
  TestBlock unknown;
  unknown.kind = SegmentBlockKind::kPicture;
  unknown.pieces = {Picture(bn::epub::kSegmentNoPicture)};
  TestBlock afterText;
  afterText.kind = SegmentBlockKind::kPicture;
  afterText.pieces = {Words("Seen first: "), Picture(3, 55)};
  TestBlock paragraph = Paragraph("Among ");
  paragraph.pieces.push_back(Picture(3, 55));
  paragraph.pieces.push_back(Words(" words."));
  TestBlock styled;
  styled.kind = SegmentBlockKind::kPicture;
  styled.pieces = {InStyle(1), Picture(3, 55), Words(" and a caption.")};
  TestBlock leading;
  leading.pieces = {Picture(3, 55), Words(" before the words of a paragraph.")};
  f.Write({sized, declared, unknown, afterText, paragraph, styled, leading}, {{3, {640, 333}}});
  EXPECT_EQ(f.LinesOf(0)[0].height, 333);  // As the chapter says the file is,
  EXPECT_EQ(f.LinesOf(1)[0].height, 55);   // or as the document declares it,
  EXPECT_EQ(f.LinesOf(2)[0].height, 100);  // or as the setting has one of which nothing is known.
  EXPECT_EQ(f.rules.picturesAsked, 3);
  // Only where the picture comes before any text, and only in a block
  // that is a picture.
  EXPECT_EQ(f.LinesOf(3)[0].height, 20);
  EXPECT_EQ(f.LinesOf(4)[0].height, 20);
  EXPECT_EQ(f.rules.picturesAsked, 3);
  // A style item before it is no text.
  EXPECT_EQ(f.LinesOf(5)[0].height, 333);
  // And a picture that a paragraph begins with is a picture among words.
  EXPECT_EQ(f.LinesOf(6)[0].height, 20);
  EXPECT_EQ(f.rules.picturesAsked, 4);
}

TEST(ChapterPager, GivesAPictureAmongWordsToTheSetterAsOneCharacter) {
  PagerFixture f;
  TestBlock block = Paragraph("ab ");
  block.pieces.push_back(Picture(1));
  block.pieces.push_back(Words(" cd"));
  TestBlock alone;
  alone.kind = SegmentBlockKind::kPicture;
  alone.pieces = {Picture(1)};
  f.Write({block, alone});
  const Lines lines = f.LinesOf(0);
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].begin, 0U);
  EXPECT_EQ(lines[0].end, 7U);  // Three bytes, the picture, three more.
  EXPECT_EQ(lines[0].spaces, 2);
  const std::uint32_t picture = MadeUpFace::CharacterWidth(ChapterPager::kPictureByte, 0);
  EXPECT_EQ(lines[0].width, (4U * 36U) + (2U * 18U) + picture);
  // And a block that is nothing but a picture is one character.
  const Lines one = f.LinesOf(1);
  ASSERT_EQ(one.size(), 1U);
  EXPECT_EQ(one[0].end, 1U);
  EXPECT_EQ(one[0].width, picture);
}

TEST(ChapterPager, AsksTheSettingWhatEachRunOfABlockComesTo) {
  PagerFixture f;
  TestBlock block = Paragraph("aa ");
  block.pieces.push_back(InStyle(1));
  block.pieces.push_back(Words("aa "));
  block.pieces.push_back(InStyle(2));
  block.pieces.push_back(Words("aa"));
  f.Write({block, Paragraph("aa")});
  f.rules.runsAsked = 0;
  const Lines lines = f.LinesOf(0);
  ASSERT_EQ(lines.size(), 1U);
  // In the made-up face everything is 4 wider in each style after the
  // first: so the line is wider than it would be all in one style, by what
  // setting the same runs by hand comes to.
  const Lines want = f.ByHand();
  EXPECT_EQ(lines[0].width, want[0].width);
  EXPECT_GT(lines[0].width, (6U * 36U) + (2U * 18U));
  // Once where the block's text begins, and once for each style item.
  EXPECT_EQ(f.rules.runsAsked, 3);
  // A block begins in the style that says nothing, whatever the last was in.
  EXPECT_EQ(f.LinesOf(1)[0].width, 72U);
}

TEST(ChapterPager, BreaksAWordWithTheHyphenatorTheSettingGivesForItsRun) {
  bn::layout::Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(bn::hyphenation::AmericanEnglish().table), bn::layout::HyphenStatus::kOk);
  // Lines that hold one long word and part of the next, in a block whose
  // second half is in a language of the chapter's: only that half's words
  // go to the hyphenator the setting has for it.
  const std::string words = "immeasurably considerable representation ";
  TestBlock block = Paragraph(words + words);
  block.pieces.push_back(InStyle(0, 1));
  block.pieces.push_back(Words(words + words));
  PagerFixture f;
  f.rules.width = 640;
  f.rules.hyphenator = &hyphenator;
  f.Write({block});
  const Lines lines = f.LinesOf(0);
  EXPECT_EQ(f.setter.WordsAsked(), 6U);
  const std::size_t half = 2 * words.size();
  int hyphensBefore = 0;
  int hyphensAfter = 0;
  for (const ChapterLine& line : lines) {
    if (line.Is(bn::layout::kLineHyphen)) {
      ++(line.end <= half ? hyphensBefore : hyphensAfter);
    }
  }
  EXPECT_EQ(hyphensBefore, 0);
  EXPECT_GT(hyphensAfter, 0);
  // At the price the setting puts on a hyphen, which is here more than any
  // gap costs, no word is broken though the hyphenator is asked.
  f.rules.hyphenPenalty = 90'000'000;
  const Lines dear = f.LinesOf(0);
  EXPECT_EQ(f.setter.WordsAsked(), 6U);
  EXPECT_TRUE(
      std::none_of(dear.begin(), dear.end(), [](const ChapterLine& line) { return line.Is(bn::layout::kLineHyphen); }));
  f.rules.hyphenPenalty = 0;
  // And with no hyphenator for it, none.
  f.rules.hyphenator = nullptr;
  const Lines plain = f.LinesOf(0);
  EXPECT_EQ(f.setter.WordsAsked(), 0U);
  EXPECT_TRUE(std::none_of(plain.begin(), plain.end(),
                           [](const ChapterLine& line) { return line.Is(bn::layout::kLineHyphen); }));
}

TEST(ChapterPager, MarksALineThatIsWiderThanItsBlocksLinesMayBe) {
  PagerFixture f;
  f.rules.width = 160;  // Lines of 40 at the narrowest: a capital M is 54.
  f.Write({Paragraph("i M i", StyleThat({.left = 100, .right = 100})), Paragraph("i l i")});
  const Lines wide = f.LinesOf(0);
  ASSERT_EQ(wide.size(), 3U);
  EXPECT_FALSE(wide[0].Is(bn::layout::kLineOverfull));
  EXPECT_TRUE(wide[1].Is(bn::layout::kLineOverfull));
  EXPECT_EQ(wide[1].width, 54U);
  EXPECT_FALSE(wide[2].Is(bn::layout::kLineOverfull));
  for (const ChapterLine& line : f.LinesOf(1)) {
    EXPECT_FALSE(line.Is(bn::layout::kLineOverfull));
  }
}

// ---- Pages ----

struct PagesCase {
  std::uint32_t seed;
  std::size_t blocks;
  std::uint16_t pageHeight;
  int width;
};

class ChapterPagerPages : public testing::TestWithParam<PagesCase> {};

TEST_P(ChapterPagerPages, AreTheOnesAddingTheLinesHeightsUpChooses) {
  const PagesCase& given = GetParam();
  PagerFixture f;
  f.rules.width = given.width;
  f.pageHeight = given.pageHeight;
  f.Write(MadeUpChapter(given.seed, given.blocks), {{1, {300, 200}}, {4, {800, 2000}}, {7, {10, 10}}});
  const Lines all = f.ByHand();
  const std::uint32_t blocks = f.Blocks();
  Dice dice(given.seed * 7);

  // Down from the chapter's first line, a page after another: every line
  // is on one page.
  std::vector<std::size_t> firsts;
  for (std::size_t first = 0; first < all.size();) {
    const std::size_t end = DownByHand(all, first, given.pageHeight);
    const ChapterPlace place = all[first].Place();
    ExpectPage(f.Down(place), all, first, end, true, blocks, Named("the page from", place));
    if (testing::Test::HasFailure()) {
      return;
    }
    firsts.push_back(first);
    first = end;
  }
  ASSERT_GT(firsts.size(), 4U);
  // And none begins at the chapter's end.
  const PagerFixture::Got atEnd = f.Down(f.pager.End());
  EXPECT_EQ(atEnd.status, PagerStatus::kOk);
  EXPECT_FALSE(atEnd.page.found);
  EXPECT_EQ(atEnd.page.next, f.pager.End());

  // The page above each of those, above the chapter's end, and above
  // lines that begin no page.
  for (std::size_t i = 0; i <= firsts.size(); ++i) {
    const ChapterPlace before = PlaceOf(all, i < firsts.size() ? firsts[i] : all.size(), blocks);
    ExpectPageAbove(f.Up(before), all, before, given.pageHeight, blocks);
    if (testing::Test::HasFailure()) {
      return;
    }
  }
  for (int i = 0; i < 60; ++i) {
    // Where a line begins, inside one, and past its block's end.
    const ChapterLine& line = all[dice.Below(static_cast<std::uint32_t>(all.size()))];
    const std::uint32_t further = dice.Below(3) != 0 ? 0 : dice.Below(line.end - line.begin + 2);
    const std::uint32_t past = dice.OneIn(10) ? 100000 : 0;
    const ChapterPlace before{.block = line.block, .into = line.begin + further + past};
    ExpectPageAbove(f.Up(before), all, before, given.pageHeight, blocks);
    if (testing::Test::HasFailure()) {
      return;
    }
  }

  // The page a place is entered at: a line's first character, one in the
  // middle of it, and one past its block's end.
  for (int i = 0; i < 120; ++i) {
    const ChapterLine& line = all[dice.Below(static_cast<std::uint32_t>(all.size()))];
    const std::uint32_t further = dice.Below(3) == 0 ? 0 : dice.Below(line.end - line.begin + 1);
    const std::uint32_t past = dice.OneIn(15) ? 100000 : 0;
    const ChapterPlace place{.block = line.block, .into = line.begin + further + past};
    const std::size_t asked = LineHolding(all, place);
    const std::size_t first = HoldingByHand(all, asked, given.pageHeight);
    const std::size_t end = DownByHand(all, first, given.pageHeight);
    const PagerFixture::Got got = f.About(place);
    ExpectPage(got, all, first, end, true, blocks, Named("the page about", place));
    if (testing::Test::HasFailure()) {
      return;
    }
    // And the page down from the same place begins with its line.
    ExpectPage(f.Down(place), all, asked, DownByHand(all, asked, given.pageHeight), true, blocks,
               Named("the page from the line holding", place));
    if (testing::Test::HasFailure()) {
      return;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(MadeUp, ChapterPagerPages,
                         testing::Values(PagesCase{.seed = 1, .blocks = 60, .pageHeight = 400, .width = 1200},
                                         PagesCase{.seed = 2, .blocks = 90, .pageHeight = 760, .width = 1760},
                                         PagesCase{.seed = 3, .blocks = 30, .pageHeight = 130, .width = 700},
                                         PagesCase{.seed = 4, .blocks = 14, .pageHeight = 33, .width = 1000}));

TEST(ChapterPager, BeginsAPageWithTheLineThatHoldsAPlace) {
  PagerFixture f;
  const std::string text = WordsFrom(9, 900);
  f.Write({Paragraph("First."), Paragraph("   " + text), Paragraph("Last.")});
  const Lines lines = f.LinesOf(1);
  ASSERT_GT(lines.size(), 4U);
  ASSERT_GT(lines[0].begin, 0U);  // The spaces it begins with are on no line.
  // A character in the middle of a line, and the space after its last.
  EXPECT_EQ(f.Down({1, lines[2].begin + 3}).page.first, lines[2].Place());
  EXPECT_EQ(f.Down({1, lines[3].begin - 1}).page.first, lines[2].Place());
  EXPECT_EQ(f.Down({1, lines[3].begin}).page.first, lines[3].Place());
  // Before the block's first line begins, it is the first; and past the
  // block's end, the last.
  EXPECT_EQ(f.Down({1, 0}).page.first, lines[0].Place());
  EXPECT_EQ(f.Down({1, 1'000'000}).page.first, lines.back().Place());
  EXPECT_EQ(f.About({1, 1'000'000}).page.first, lines.back().Place());
  // The page above a place ends with the last line that begins before it:
  // on a page two lines high, the two above.
  f.pageHeight = 40;
  const PagerFixture::Got above = f.Up({1, lines[3].begin});
  ASSERT_EQ(above.lines.size(), 2U);
  EXPECT_EQ(above.lines[0].begin, lines[1].begin);
  EXPECT_EQ(above.lines[1].begin, lines[2].begin);
  EXPECT_EQ(above.page.next, (ChapterPlace{.block = 1, .into = lines[3].begin}));
  // A place inside a line is below where the line begins: the line is on
  // the page above it.
  const PagerFixture::Got inside = f.Up({1, lines[3].begin + 1});
  ASSERT_EQ(inside.lines.size(), 2U);
  EXPECT_EQ(inside.lines[1].begin, lines[3].begin);
  EXPECT_EQ(inside.page.next, (ChapterPlace{.block = 1, .into = lines[3].begin + 1}));
  // A place past a block's end has every line of the block above it, and
  // none of the block after.
  const PagerFixture::Got past = f.Up({1, 1'000'000});
  ASSERT_EQ(past.lines.size(), 2U);
  EXPECT_EQ(past.lines[1].begin, lines.back().begin);
  EXPECT_EQ(past.lines[1].block, 1U);
  EXPECT_EQ(past.page.next, (ChapterPlace{.block = 1, .into = 1'000'000}));
  // And above a block's first line is the block before.
  const PagerFixture::Got before = f.Up({2, 0});
  ASSERT_EQ(before.lines.size(), 2U);
  EXPECT_EQ(before.lines[1].begin, lines.back().begin);
  EXPECT_EQ(before.lines[1].block, 1U);
}

TEST(ChapterPager, SaysThereIsNoPageAboveAChaptersFirstLineAndGivesItsLastPageFromItsEnd) {
  PagerFixture f;
  f.pageHeight = 60;
  f.Write({Paragraph("One."), Paragraph("Two."), Paragraph("Three."), Paragraph("Four."), Paragraph("Five.")});
  const PagerFixture::Got none = f.Up({0, 0});
  EXPECT_EQ(none.status, PagerStatus::kOk);
  EXPECT_FALSE(none.page.found);
  EXPECT_FALSE(none.page.filledDown);
  EXPECT_EQ(none.page.lines, 0U);
  // From the chapter's end, and from any place past it.
  for (const ChapterPlace end : {f.pager.End(), ChapterPlace{.block = 99, .into = 7}}) {
    const PagerFixture::Got last = f.Up(end);
    ASSERT_TRUE(last.page.found);
    EXPECT_FALSE(last.page.filledDown);
    EXPECT_EQ(last.page.lines, 3U);
    EXPECT_EQ(last.page.first, (ChapterPlace{.block = 2, .into = 0}));
    EXPECT_EQ(last.page.next, end);
  }
  // Going up comes to the chapter's first line, which starts a page: that
  // page, filled down.
  const PagerFixture::Got first = f.Up({2, 0});
  ASSERT_TRUE(first.page.found);
  EXPECT_TRUE(first.page.filledDown);
  EXPECT_EQ(first.page.first, (ChapterPlace{.block = 0, .into = 0}));
  EXPECT_EQ(first.page.next, (ChapterPlace{.block = 3, .into = 0}));
}

TEST(ChapterPager, EndsAPageWhereItsRecordsRunOutAndGoesOnFromThere) {
  PagerFixture f(8);
  f.pageHeight = 400;
  const std::string text = WordsFrom(21, 2500);
  f.Write({Paragraph(text), Paragraph(text, StyleThat({.breaking = bn::epub::kBreakKeepTogether})), Paragraph(text)});
  const Lines all = f.ByHand();
  ASSERT_GT(all.size(), 40U);
  // Twenty lines would fit, and there are records for eight: every line
  // is still on one page, eight at a time.
  std::size_t at = 0;
  while (at < all.size()) {
    const PagerFixture::Got got = f.Down(all[at].Place());
    ASSERT_EQ(got.status, PagerStatus::kOk);
    ASSERT_TRUE(got.page.found);
    ASSERT_LE(got.page.lines, 8U);
    for (std::uint32_t i = 0; i < got.page.lines; ++i) {
      ExpectSameLine(got.lines[i], all[at + i], "line " + std::to_string(at + i));
    }
    const std::size_t end = at + got.page.lines;
    EXPECT_EQ(got.page.next, PlaceOf(all, end, 3));
    EXPECT_EQ(got.page.cutShort, end < all.size()) << at;
    // It ends as a full page would: not between lines kept together, where
    // a place above them will do.
    if (end < all.size() && all[end].Is(bn::layout::kLineKept) && got.page.lines < 8) {
      EXPECT_FALSE(all[end - 1].Is(bn::layout::kLineKept) && end - 1 > at);
    }
    at = end;
  }

  // A page that is full with its eighth line was not cut short: there was
  // no room for a ninth, whatever records there were.
  f.pageHeight = 160;
  const PagerFixture::Got full = f.Down(all[0].Place());
  ASSERT_EQ(full.page.lines, 8U);
  EXPECT_FALSE(full.page.cutShort);
  EXPECT_EQ(full.page.next, all[8].Place());
  f.pageHeight = 400;

  // Filled up, a page begins with the first line there is a record for.
  const ChapterPlace before = all[30].Place();
  const PagerFixture::Got up = f.Up(before);
  ASSERT_EQ(up.status, PagerStatus::kOk);
  ASSERT_TRUE(up.page.found);
  EXPECT_TRUE(up.page.cutShort);
  EXPECT_FALSE(up.page.filledDown);
  ASSERT_EQ(up.page.lines, 8U);
  EXPECT_EQ(up.page.next, before);
  EXPECT_EQ(up.page.first, all[22].Place());
  EXPECT_EQ(up.lines[0].top, 0U);
  EXPECT_EQ(up.lines[7].top, 7U * 20U);
  EXPECT_EQ(up.page.height, 160U);
}

TEST(ChapterPager, FillsAPageUpWithNoMoreEntriesThanTheChooserWillWorkWith) {
  PagerFixture f(64, PageFromBottom::kMinEntries);
  f.pageHeight = 300;
  f.Write({Paragraph(WordsFrom(4, 3000))});
  const Lines all = f.ByHand();
  const PagerFixture::Got up = f.Up(all[25].Place());
  ASSERT_EQ(up.status, PagerStatus::kOk);
  EXPECT_TRUE(up.page.cutShort);
  EXPECT_EQ(up.page.lines, PageFromBottom::kMinEntries);
  EXPECT_EQ(up.page.next, all[25].Place());
}

// ---- How it reads ----

TEST(ChapterPager, RemembersWhatABlockGivesTheOneAfterIt) {
  PagerFixture f;
  f.pageHeight = 760;
  Chapter chapter;
  for (int i = 0; i < 60; ++i) {
    chapter.push_back(Paragraph("A line of a few words.", StyleThat({.after = 5})));
  }
  f.Write(chapter);
  // Going down, each block's head is read once, for the block itself.
  const PagerFixture::Got page = f.Down({10, 0});
  ASSERT_EQ(page.page.lines, 30U);     // 20 high, with 5 above each but the first.
  EXPECT_EQ(f.pager.HeadsRead(), 1U);  // Block 9's, for what it gives block 10.
  EXPECT_EQ(f.pager.BlocksFound(), 2U);
  EXPECT_EQ(f.pager.BlocksSet(), 31U);  // The 30, and the one that did not fit.
  // The next page begins where that one stopped, and nothing is read again
  // for what the block before gives.
  (void)f.Down(page.page.next);
  EXPECT_EQ(f.pager.HeadsRead(), 1U);
  EXPECT_EQ(f.pager.BlocksFound(), 3U);
  // That page went on to the chapter's end, and the last four blocks are
  // remembered: a page from the last but one reads no head for it. Told to
  // forget, it reads the head of the block before.
  (void)f.Down({58, 0});
  EXPECT_EQ(f.pager.HeadsRead(), 1U);
  f.pager.Forget();
  (void)f.Down({58, 0});
  EXPECT_EQ(f.pager.HeadsRead(), 2U);
  // The chapter's first block has none before it.
  f.pager.Forget();
  (void)f.Down({0, 0});
  EXPECT_EQ(f.pager.HeadsRead(), 2U);
}

TEST(ChapterPager, SetsNoBlockBelowThePageItFillsUp) {
  PagerFixture f;
  f.pageHeight = 45;  // Two lines of 20, with 5 between them.
  Chapter chapter;
  for (int i = 0; i < 20; ++i) {
    chapter.push_back(Paragraph("A line of a few words.", StyleThat({.after = 5})));
  }
  f.Write(chapter);
  const PagerFixture::Got above = f.Up({10, 0});
  ASSERT_EQ(above.page.lines, 2U);
  EXPECT_EQ(above.page.first, (ChapterPlace{.block = 8, .into = 0}));
  // Going up: blocks 9 and 8, and 7, which did not fit. Then 8 and 9 again
  // for the page's lines, and not block 10, which the page ends above.
  EXPECT_EQ(f.pager.BlocksSet(), 5U);
}

TEST(ChapterPager, SetsOnlyAsMuchOfABlockAsAPageNeeds) {
  PagerFixture f;
  f.pageHeight = 100;
  f.Write({Paragraph(WordsFrom(8, 60000))});
  const std::uint64_t before = f.face.BytesMeasured();
  const PagerFixture::Got page = f.Down({0, 0});
  ASSERT_EQ(page.page.lines, 5U);
  // Five lines of some thirty characters, and what the breaker held
  // beyond them: nothing like the block's 60,000.
  EXPECT_LT(f.face.BytesMeasured() - before, 8000U);
}

TEST(ChapterPager, ComesToABlockPastTheIndexByReadingOnFromTheLastTheIndexHas) {
  PagerFixture f;
  f.pageHeight = 100;
  f.MakeBundle();
  ASSERT_TRUE(f.file.Seek(f.bundle.DataEnd()));
  ASSERT_TRUE(f.file.Write(FakeStorage::Bytes(3'000'000, 0xDD)));
  ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
  const std::uint32_t blocks = 65536 + 40;
  for (std::uint32_t i = 0; i < blocks; ++i) {
    ASSERT_EQ(f.writer.BeginBlock(SegmentBlockKind::kParagraph, 0, {}, i * 10), SegmentStatus::kOk);
    ASSERT_EQ(f.writer.Text(std::to_string(i)), SegmentStatus::kOk);
    ASSERT_EQ(f.writer.EndBlock(), SegmentStatus::kOk);
  }
  f.Finish();
  ASSERT_EQ(f.reader.Blocks(), blocks);
  ASSERT_EQ(f.reader.IndexedBlocks(), 65536U);

  // Each block is one line, 20 high: five to a page.
  const auto begins = [](const PagerFixture::Got& got) {
    std::vector<std::uint32_t> at;
    for (const ChapterLine& line : got.lines) {
      at.push_back(line.block);
    }
    return at;
  };
  const PagerFixture::Got down = f.Down({65550, 0});
  ASSERT_EQ(down.status, PagerStatus::kOk);
  EXPECT_EQ(begins(down), std::vector<std::uint32_t>({65550, 65551, 65552, 65553, 65554}));
  EXPECT_EQ(down.lines[2].end, 5U);  // "65552"
  // Block 65549 was read through for its head and then on into 65550; the
  // 14 before it only passed.
  EXPECT_EQ(f.pager.BlocksPassed(), 15U);
  EXPECT_EQ(f.pager.BlocksFound(), 1U);
  const PagerFixture::Got up = f.Up(f.pager.End());
  ASSERT_EQ(up.status, PagerStatus::kOk);
  EXPECT_EQ(begins(up), std::vector<std::uint32_t>({65571, 65572, 65573, 65574, 65575}));
  const PagerFixture::Got about = f.About({65540, 2});
  ASSERT_EQ(about.status, PagerStatus::kOk);
  EXPECT_EQ(begins(about), std::vector<std::uint32_t>({65540, 65541, 65542, 65543, 65544}));
  // And across the last block the index has.
  EXPECT_EQ(begins(f.Up({65538, 0})), std::vector<std::uint32_t>({65533, 65534, 65535, 65536, 65537}));
}

TEST(ChapterPager, LeavesTheReaderToBeUsedBetweenOnePageAndTheNext) {
  PagerFixture f;
  f.Write(MadeUpChapter(31, 30));
  const Lines all = f.ByHand();
  const std::size_t second = DownByHand(all, 0, f.pageHeight);
  ExpectPage(f.Down({0, 0}), all, 0, second, true, f.Blocks(), "the first page");
  // Something else reads a block half way, and looks a place up.
  ASSERT_EQ(f.reader.FindBlock(17), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.Seek(f.reader.Place()), SegmentStatus::kOk);
  SegmentBlock head;
  ASSERT_EQ(f.reader.ReadBlock(head), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.FindCharacters(40), SegmentStatus::kOk);
  const std::size_t third = DownByHand(all, second, f.pageHeight);
  ExpectPage(f.Down(all[second].Place()), all, second, third, true, f.Blocks(), "the second page");

  // And where the pager had read a block to its end, and would have found
  // the reader standing at the next had nothing else used it.
  (void)f.LinesOf(5);
  ASSERT_EQ(f.reader.FindBlock(17), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.Seek(f.reader.Place()), SegmentStatus::kOk);
  const Lines six = f.LinesOf(6);
  const std::size_t at = LinesAbove(all, {.block = 6, .into = 0});
  ASSERT_FALSE(six.empty());
  for (std::size_t i = 0; i < six.size(); ++i) {
    ExpectSameLine(six[i], all[at + i], "block 6, line " + std::to_string(i));
  }
}

// ---- When things go wrong ----

TEST(ChapterPager, EndsInAStatusWhereverStorageFails) {
  PagerFixture f;
  f.pageHeight = 200;
  f.Write(MadeUpChapter(41, 24));
  const Lines all = f.ByHand();
  const ChapterPlace middle = all[all.size() / 2].Place();
  // How many reads each piece of work makes; then the same with storage
  // failing from each of them on.
  const auto work = [&f, middle](int which) {
    f.pager.Forget();
    return which == 0 ? f.Down(middle) : which == 1 ? f.Up(middle) : f.About(middle);
  };
  for (int which = 0; which < 3; ++which) {
    // The file's own buffer may hold what is wanted: it is emptied by
    // reading from somewhere else first.
    std::size_t before = f.storage.reads.size();
    const PagerFixture::Got whole = work(which);
    ASSERT_EQ(whole.status, PagerStatus::kOk);
    const std::size_t reads = f.storage.reads.size() - before;
    ASSERT_GT(reads, 2U) << which;
    for (std::size_t failAt = 0; failAt < reads; ++failAt) {
      f.pager.Forget();
      const PagerStatus begun = which == 0   ? f.pager.BeginDown(middle, f.pageHeight)
                                : which == 1 ? f.pager.BeginUp(middle, f.pageHeight)
                                             : f.pager.BeginAbout(middle, f.pageHeight);
      ASSERT_EQ(begun, PagerStatus::kOk);
      before = f.storage.reads.size();
      PagerStatus status = PagerStatus::kOk;
      for (bool more = true; more;) {
        f.storage.failReads = f.storage.reads.size() - before >= failAt;
        more = f.pager.Step();
        status = f.pager.Status();
      }
      f.storage.failReads = false;
      if (status == PagerStatus::kOk) {
        continue;  // What it wanted from there on was in the file's buffer.
      }
      EXPECT_EQ(status, PagerStatus::kReadFailed) << which << " at read " << failAt;
      EXPECT_EQ(f.pager.ReaderStatus(), SegmentStatus::kStoreFailed);
      // And it works again once storage does.
      const PagerFixture::Got again = work(which);
      EXPECT_EQ(again.status, PagerStatus::kOk);
      EXPECT_EQ(again.page.first, whole.page.first);
      EXPECT_EQ(again.page.lines, whole.page.lines);
    }
  }
}

TEST(ChapterPager, EndsInAStatusWhateverAChaptersBytesAre) {
  PagerFixture f;
  f.pageHeight = 150;
  f.Write(MadeUpChapter(51, 12));
  BundleChapter record;
  ASSERT_EQ(f.bundle.ReadChapter(0, record), BundleStatus::kOk);
  FakeStorage::Bytes& bytes = f.storage.entries.at(kPath).data;
  const FakeStorage::Bytes sound = bytes;
  Dice dice(77);
  int found = 0;
  for (int trial = 0; trial < 60; ++trial) {
    bytes = sound;
    // A bit changed somewhere before the directory, which is left as it
    // was so that the chapter still opens.
    const std::size_t at = record.segmentOffset + dice.Below(record.directoryOffset);
    const auto change = static_cast<std::uint8_t>(1U << dice.Below(8));
    bytes[at] = static_cast<std::uint8_t>(bytes[at] ^ change);
    f.file.Close();
    ASSERT_EQ(f.file.Open(f.storage, kPath, OpenMode::kUpdate), StorageStatus::kOk);
    ASSERT_EQ(f.reader.Open(f.file, record), SegmentStatus::kOk);
    f.pager.Forget();
    const std::uint32_t block = dice.Below(12);
    const ChapterPlace place{.block = block, .into = dice.Below(200)};
    const std::uint32_t which = dice.Below(3);
    const PagerFixture::Got got = which == 0 ? f.Down(place) : which == 1 ? f.Up(place) : f.About(place);
    if (got.status != PagerStatus::kOk) {
      EXPECT_EQ(got.status, PagerStatus::kReadFailed);
      EXPECT_NE(f.pager.ReaderStatus(), SegmentStatus::kOk);
    }
    // A page may not read the block the bit is in, or not to its end. With
    // every block read whole, a bit changed in a block is found.
    bool failed = got.status != PagerStatus::kOk;
    for (std::uint32_t each = 0; each < 12; ++each) {
      Gathered gathered;
      const PagerStatus begun = f.pager.BeginLines(each, gathered);
      const PagerStatus status = begun == PagerStatus::kOk ? f.Run() : begun;
      EXPECT_TRUE(status == PagerStatus::kOk || status == PagerStatus::kReadFailed);
      failed = failed || status != PagerStatus::kOk;
    }
    found += failed ? 1 : 0;
  }
  EXPECT_EQ(found, 60);
}

TEST(ChapterPager, RefusesWhatCannotBe) {
  PagerFixture f;
  f.pageHeight = 100;
  f.Write({Paragraph(WordsFrom(2, 2000)), Paragraph("Two.")});
  Gathered sink;
  // A page of no height, and a block the chapter has not.
  EXPECT_EQ(f.pager.BeginDown({0, 0}, 0), PagerStatus::kBadHeight);
  EXPECT_EQ(f.pager.BeginUp({1, 0}, 0), PagerStatus::kBadHeight);
  EXPECT_EQ(f.pager.BeginAbout({0, 0}, 0), PagerStatus::kBadHeight);
  EXPECT_EQ(f.pager.BeginAbout({2, 0}, 100), PagerStatus::kNoSuchBlock);
  EXPECT_EQ(f.pager.BeginLines(2, sink), PagerStatus::kNoSuchBlock);
  EXPECT_EQ(f.pager.Status(), PagerStatus::kNoSuchBlock);
  EXPECT_FALSE(f.pager.Step());
  EXPECT_EQ(f.pager.Status(), PagerStatus::kNoSuchBlock);

  // Another piece of work while one is under way: refused, and the one
  // under way goes on as it was.
  ASSERT_EQ(f.pager.BeginDown({0, 0}, 100), PagerStatus::kOk);
  ASSERT_TRUE(f.pager.Step());
  ASSERT_TRUE(f.pager.Step());
  EXPECT_EQ(f.pager.BeginUp({1, 0}, 100), PagerStatus::kMisused);
  EXPECT_EQ(f.pager.BeginLines(0, sink), PagerStatus::kMisused);
  EXPECT_EQ(f.Run(), PagerStatus::kOk);
  EXPECT_EQ(f.pager.Page().lines, 5U);
  EXPECT_TRUE(sink.lines.empty());

  // Lines no setter will set.
  f.rules.badWidths = true;
  ASSERT_EQ(f.pager.BeginDown({0, 0}, 100), PagerStatus::kOk);
  EXPECT_EQ(f.Run(), PagerStatus::kSetFailed);
  EXPECT_EQ(f.pager.SetterStatus(), ParagraphStatus::kBadWidths);
  f.rules.badWidths = false;
  EXPECT_EQ(f.Down({0, 0}).page.lines, 5U);

  // A reader with no chapter open.
  f.reader.Close();
  EXPECT_EQ(f.pager.BeginDown({0, 0}, 100), PagerStatus::kMisused);
  EXPECT_EQ(f.pager.BeginLines(0, sink), PagerStatus::kMisused);
}

TEST(ChapterPager, RefusesTooFewRecordsAndTooFewEntries) {
  PagerFixture few(ChapterPager::kMinLines - 1);
  few.Write({Paragraph("One.")});
  Gathered sink;
  EXPECT_EQ(few.pager.BeginDown({0, 0}, 100), PagerStatus::kNoMemory);
  EXPECT_EQ(few.pager.BeginUp({0, 0}, 100), PagerStatus::kNoMemory);
  EXPECT_EQ(few.pager.BeginAbout({0, 0}, 100), PagerStatus::kNoMemory);
  EXPECT_EQ(few.pager.BeginLines(0, sink), PagerStatus::kNoMemory);
  EXPECT_FALSE(few.pager.Step());

  // Entries are only wanted to fill a page up.
  PagerFixture fewEntries(ChapterPager::kMinLines, PageFromBottom::kMinEntries - 1);
  fewEntries.Write({Paragraph("One."), Paragraph("Two.")});
  EXPECT_EQ(fewEntries.pager.BeginUp({1, 0}, 100), PagerStatus::kNoMemory);
  EXPECT_EQ(fewEntries.Down({0, 0}).page.lines, 2U);
  EXPECT_EQ(fewEntries.About({1, 0}).page.lines, 1U);
}

TEST(ChapterPager, GivesALineNoHeightTheHeightOfOnePixel) {
  PagerFixture f(16);
  f.rules.usualLineHeight = 0;
  f.pageHeight = 10;
  Chapter chapter;
  for (int i = 0; i < 40; ++i) {
    chapter.push_back(Paragraph("x"));
  }
  f.Write(chapter);
  const PagerFixture::Got page = f.Down({0, 0});
  EXPECT_EQ(page.page.lines, 10U);
  EXPECT_EQ(page.page.height, 10U);
  EXPECT_EQ(page.lines[9].height, 1);
  EXPECT_FALSE(page.page.cutShort);
}

TEST(PagerStatus, HasANameForEachStatus) {
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kOk), "ok");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kMisused), "misused");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kNoMemory), "no memory");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kBadHeight), "bad height");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kNoSuchBlock), "no such block");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kReadFailed), "read failed");
  EXPECT_STREQ(bn::layout::PagerStatusName(PagerStatus::kSetFailed), "set failed");
  EXPECT_STREQ(bn::layout::PagerStatusName(static_cast<PagerStatus>(200)), "unknown");
}

}  // namespace
