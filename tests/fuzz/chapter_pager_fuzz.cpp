// The fuzz target for a chapter in pages (layout/chapter_pager.h): a
// chapter written from what the input says, or the input's own bytes as a
// chapter's segment, read and set in pages.
//
// What it is given:
//
//   byte 0   bit 0: 0, the target writes the chapter; 1, the rest is one
//            bits 1, 2: how many records a page's lines have: 8, 16, 64,
//                   or one for every line a page could hold
//            bit 3: the chooser that fills a page up has the fewest
//                   entries it will work with, and not one for every line
//            bits 4, 5: how many nodes the breaker has: 32, 64 or 256
//            bit 6: a line whose block says nothing of its height has
//                   none; and, for a chapter that is the input's bytes,
//                   the checksum of each page the directory names is put
//                   right first
//            bit 7: for a chapter that is the input's bytes, its record's
//                   checksum, blocks and characters are the directory's
//   byte 1   how high a page is: three times it, and one
//   byte 2   how wide a page's text is: eight times it, and 200
//   2 bytes  what the places asked about are worked out from; and, for a
//            chapter the target writes, the top bit says that one bit of
//            the chapter is changed once everything else has been asked,
//            and each block's lines asked for again
//
//   then, for a chapter the target writes, things given to the writer,
//   each a byte whose low three bits say what, and what that kind needs:
//   a block begun (its kind and what it asks of pages, and which of six
//   lengths it has, each a byte); text, of bytes that follow; text made up
//   here to a length; a style; a picture; a block ended; a run of small
//   blocks; and a link begun or ended. What would be out of turn is not
//   given, nor more than 300 blocks or 4,000 characters.
//
//   and for a chapter that is the input's bytes: 2 bytes, how long its
//   directory is, which is the segment's last bytes; and the segment.
//
// What has to hold of a chapter the target writes:
//
//   - every piece of the pager's work ends kOk;
//   - a block's lines are the lines the setter gives of the block's text
//     given whole, each with the height, the space above and the marks
//     that its block's record and the block before it come to;
//   - the page down from a line, the page up to one and the page about a
//     place are the lines that adding heights up over the whole chapter
//     chooses, each where that puts it; a page of more lines than there
//     are records ends, or begins, where they run out and says so; and
//     with the fewest entries, a page filled up is still lines of the
//     chapter in their order, ending where it was asked to;
//   - with one bit of the chapter changed, a block's lines are not given
//     (the reader says why) or are the lines they were, but for what the
//     first of them has from the block before, whose head is read without
//     the rest of it and so without its checksum;
//   - going down from the chapter's first line, each page begins where
//     the one before ended: for the first 80 pages, or as many as the
//     work an input may be allows (`kMostWork`, below), which in all but
//     the longest chapters is every line on one page.
//
// And of any bytes at all: every piece of work ends kOk, or says that the
// chapter could not be read or has no such block; and a page it gives has
// lines that follow one another down it.
//
// And of both: no step makes more than one call of the reader or of the
// setter, asks the file for more than a page's worth, or takes in more
// lines than the breaker has nodes; and no piece of work takes more steps
// than the chapter's bytes allow.

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
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/segment.h"
#include "fuzz_support.h"
#include "layout/chapter_pager.h"
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
using bn::epub::BundleChapter;
using bn::epub::ChapterState;
using bn::epub::LinkKind;
using bn::epub::RunStyle;
using bn::epub::SegmentBlock;
using bn::epub::SegmentBlockKind;
using bn::epub::SegmentLength;
using bn::epub::SegmentPicture;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::epub::SegmentWriter;
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
using Bytes = std::vector<std::uint8_t>;
using Lines = std::vector<ChapterLine>;

const char* const kPath = "/s";
// The most a chapter the target writes is given.
constexpr std::size_t kMostBlocks = 300;
constexpr std::size_t kMostCharacters = 4000;
// How much work an input may be: a byte of text measured is `kByteIs`, and
// a read of storage `kReadIs`, which is about what each takes here beside
// the other. A page that begins far down a long block has the block set
// from its start, and one filled up through blocks a pixel high finds
// hundreds of them by the index, at two reads each: so some pages take a
// hundred times what others do, and an input has to be done with in a few
// seconds where the engine has the lowest priority the machine gives, and
// for a while next to nothing of a processor. Of the pages going down, of
// those up to places and about them, and of those of any bytes, no more
// are asked once they have come to `kMostWorkOfAKind`; and at `kMostWork`
// the input is done with where it stands, part of the way through a page
// if need be.
constexpr std::uint64_t kByteIs = 3;
constexpr std::uint64_t kReadIs = 50;
constexpr std::uint64_t kMostWorkOfAKind = 8000;
constexpr std::uint64_t kMostWork = 60000;

std::uint32_t Get16(const Bytes& from, std::size_t at) { return from[at] | (std::uint32_t{from[at + 1]} << 8U); }
std::uint32_t Get32(const Bytes& from, std::size_t at) { return Get16(from, at) | (Get16(from, at + 2) << 16U); }
void Put32(Bytes& to, std::size_t at, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    to[at + i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
}

// ---- What a block comes to, by the target's own rules ----

// A block's lengths are taken as whole pixels; a run's style is its two
// lowest bits; and a picture is as high as the chapter says its file is,
// or as it is declared, or 100.
class Rules final : public BlockSetting {
 public:
  static std::uint16_t Pixels(const SegmentLength& length) {
    return static_cast<std::uint16_t>(std::clamp<int>(length.px, 0, 30000));
  }

  void MeasureBlock(const SegmentBlock& block, BlockMeasures& measures) noexcept override {
    const int rest =
        std::clamp<int>(width - Pixels(block.style.marginLeft) - Pixels(block.style.marginRight), 40, width);
    measures.restWidth = static_cast<std::uint16_t>(rest);
    measures.firstWidth = static_cast<std::uint16_t>(std::clamp<int>(rest - Pixels(block.style.indent), 40, width));
    measures.lineHeight = Pixels(block.style.lineHeight) != 0 ? Pixels(block.style.lineHeight) : usualLineHeight;
    measures.spaceBefore = Pixels(block.style.spaceBefore);
    measures.spaceAfter = Pixels(block.style.spaceAfter);
    measures.hyphenPenalty = 0;
  }

  void MeasureRun(const SegmentBlock& /*block*/, const RunStyle& run, RunMeasures& measures) noexcept override {
    measures.style = static_cast<std::uint16_t>(run.bits & 3U);
    measures.hyphenator = nullptr;
  }

  std::uint16_t PictureHeight(const SegmentBlock& /*block*/, const SegmentLength& /*width*/,
                              const SegmentLength& height, const SegmentPicture* picture) noexcept override {
    if (picture != nullptr && picture->sized) {
      return picture->height;
    }
    return Pixels(height) != 0 ? Pixels(height) : std::uint16_t{100};
  }

  int width = 1200;
  std::uint16_t usualLineHeight = 20;
};

// ---- A chapter as the target gave it to the writer ----

struct Block {
  SegmentBlockKind kind = SegmentBlockKind::kParagraph;
  BlockStyle style;
  // Its characters, a picture being the pager's byte for one, and where
  // each run of them ends with the bits of its style.
  std::string characters;
  std::vector<std::pair<std::size_t, std::uint16_t>> runs;
  // The picture that comes before any text, in a block that is a picture.
  bool pictureFirst = false;
  std::uint16_t picture = 0;
  SegmentLength pictureHeight;
};

using Sizes = std::map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>>;

class Gathered final : public TextLineSink, public ChapterLineSink {
 public:
  void OnLine(const TextLine& line) noexcept override { text.push_back(line); }
  void OnChapterLine(const ChapterLine& line) noexcept override { lines.push_back(line); }
  std::vector<TextLine> text;
  Lines lines;
};

bool Asks(const BlockStyle& style, std::uint16_t bit) { return (style.breaking & bit) != 0; }

// Every line of the chapter, a block at a time with the setter given the
// block's text whole, and none of the pager.
Lines LinesByHand(const std::vector<Block>& chapter, const Sizes& sizes, Rules& rules, std::size_t nodeCount) {
  Lines all;
  std::vector<LineBreaker::Node> nodes(nodeCount);
  std::vector<ParagraphSetter::Record> records(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter(nodes, records);
  MadeUpFace face;
  bool keptWithBefore = false;
  std::uint32_t spaceAbove = 0;
  for (std::uint32_t number = 0; number < chapter.size(); ++number) {
    const Block& block = chapter[number];
    SegmentBlock head;
    head.kind = static_cast<std::uint8_t>(block.kind);
    head.style = block.style;
    BlockMeasures measures;
    rules.MeasureBlock(head, measures);
    std::uint16_t lineHeight = measures.lineHeight;
    if (block.pictureFirst) {
      SegmentPicture sized;
      const auto found = sizes.find(block.picture);
      if (found != sizes.end()) {
        sized = {.number = block.picture, .width = found->second.first, .height = found->second.second, .sized = true};
      }
      lineHeight =
          rules.PictureHeight(head, SegmentLength{}, block.pictureHeight, found != sizes.end() ? &sized : nullptr);
    }
    const std::array<std::uint16_t, 1> first = {measures.firstWidth};
    const ParagraphSettings settings{
        .widths = {.first = measures.firstWidth != measures.restWidth ? std::span<const std::uint16_t>(first)
                                                                      : std::span<const std::uint16_t>(),
                   .rest = measures.restWidth},
        .hyphenPenalty = 0};
    Gathered gathered;
    BN_FUZZ_CHECK(setter.Begin(settings, face, gathered) == ParagraphStatus::kOk, "the setter would not begin a block");
    setter.UseHyphenator(nullptr);
    std::size_t from = 0;
    for (const auto& [end, bits] : block.runs) {
      std::string_view text = std::string_view(block.characters).substr(from, end - from);
      while (!text.empty()) {
        text.remove_prefix(setter.AddText(text, static_cast<std::uint16_t>(bits & 3U)));
      }
      from = end;
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

// ---- Pages, by adding heights up over the whole chapter ----

bool Starts(const ChapterLine& line) { return line.Is(bn::layout::kLineStarts); }
bool Kept(const ChapterLine& line) { return line.Is(bn::layout::kLineKept); }
bool MayBegin(const ChapterLine& line) { return Starts(line) || !Kept(line); }

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

// Where a page that begins with `first` ends, with records for `most`
// lines; and whether it would have held more with more of them.
std::size_t DownByHand(const Lines& lines, std::size_t first, std::uint32_t pageHeight, std::size_t most,
                       bool& cutShort) {
  cutShort = false;
  std::size_t end = first + 1;
  std::uint32_t height = HeightOf(lines, first, end);
  bool forced = false;
  while (end < lines.size() && !Starts(lines[end])) {
    const std::uint32_t with = height + lines[end].gap + lines[end].height;
    if (with > pageHeight) {
      break;
    }
    if (end - first == most) {
      forced = true;
      break;
    }
    height = with;
    ++end;
  }
  cutShort = forced;
  // It stopped at a line that does not fit, or has no record. If that line
  // is kept with the one above, the page ends at the last place it may.
  if (end < lines.size() && !Starts(lines[end]) && Kept(lines[end])) {
    for (std::size_t place = end - 1; place > first; --place) {
      if (!Kept(lines[place])) {
        return place;
      }
    }
  }
  return end;
}

std::size_t UpByHand(const Lines& lines, std::size_t end, std::uint32_t pageHeight) {
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

std::size_t HoldingByHand(const Lines& lines, std::size_t asked, std::uint32_t pageHeight) {
  for (std::size_t place = asked;; --place) {
    if (place != asked && HeightOf(lines, place, asked + 1) > pageHeight) {
      return asked;
    }
    if (MayBegin(lines[place]) || place == 0) {
      return place;
    }
  }
}

std::size_t LinesAbove(const Lines& lines, ChapterPlace place) {
  std::size_t above = 0;
  while (above < lines.size() &&
         (lines[above].block < place.block || (lines[above].block == place.block && lines[above].begin < place.into))) {
    ++above;
  }
  return above;
}

std::size_t LineHolding(const Lines& lines, ChapterPlace place) {
  std::size_t holding = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].block == place.block && (holding == lines.size() || lines[i].begin <= place.into)) {
      holding = i;
    }
  }
  return holding;
}

bool SameLine(const ChapterLine& got, const ChapterLine& want) {
  return got.block == want.block && got.begin == want.begin && got.end == want.end && got.width == want.width &&
         got.height == want.height && got.gap == want.gap && got.spaces == want.spaces && got.flags == want.flags;
}

// ---- A card with the chapter on it, and a pager over it ----

struct Choices {
  std::size_t lineRecords = 64;
  bool fewEntries = false;
  std::size_t nodes = 256;
  std::uint16_t pageHeight = 400;
};

struct Got {
  PagerStatus status = PagerStatus::kOk;
  ChapterPage page;
  Lines lines;
};

struct Desk {
  Desk(const Choices& choices, const Rules& given)
      : rules(given),
        nodes(choices.nodes),
        records(choices.lineRecords),
        entries(choices.fewEntries
                    ? PageFromBottom::kMinEntries
                    : std::max<std::size_t>(PageFromBottom::kUsualEntries, std::size_t{choices.pageHeight} + 1)),
        pageHeight(choices.pageHeight) {
    BN_FUZZ_CHECK(storage.Start() == StorageStatus::kOk, "the storage did not start");
  }

  void Open(OpenMode mode) {
    BN_FUZZ_CHECK(file.Open(storage, kPath, mode) == StorageStatus::kOk, "the chapter's file does not open");
  }

  // Does the piece of work that was begun to its end, each step held to
  // what a step may do; or as far as the input may be work, and says that
  // it gave up.
  PagerStatus Run(PagerStatus begun) {
    if (begun != PagerStatus::kOk) {
      BN_FUZZ_CHECK(!pager.Step() && pager.Status() == begun, "work that did not begin went on");
      return begun;
    }
    std::size_t steps = 0;
    for (bool more = true; more; ++steps) {
      const std::size_t mark = storage.reads.size();
      more = pager.Step();
      std::size_t bytes = 0;
      for (std::size_t i = mark; i < storage.reads.size(); ++i) {
        bytes += storage.reads[i].size;
      }
      const ChapterPager::StepWork& work = pager.LastStep();
      BN_FUZZ_CHECK(storage.reads.size() - mark <= 2 && bytes <= 8192,
                    "one step asked the file for more than a page's worth");
      BN_FUZZ_CHECK(work.readerCalls + work.setterCalls <= 1, "a step called the reader and the setter, or one twice");
      BN_FUZZ_CHECK(work.readerCalls != 0 || storage.reads.size() == mark, "a read with no call of the reader");
      BN_FUZZ_CHECK(work.lines <= nodes.size(), "a step took in more lines than the breaker has nodes");
      BN_FUZZ_CHECK(steps <= mostSteps, "a piece of the pager's work took more steps than the chapter allows");
      if (more && Work() >= kMostWork) {
        gaveUp = true;
        return PagerStatus::kOk;
      }
    }
    BN_FUZZ_CHECK(!pager.Step(), "a piece of work that was over went on");
    return pager.Status();
  }

  Got Result(PagerStatus begun) {
    Got got;
    got.status = Run(begun);
    got.page = pager.Page();
    got.lines.assign(pager.Lines().begin(), pager.Lines().end());
    return got;
  }
  // How much work has been done.
  std::uint64_t Work() const { return (kByteIs * face.BytesMeasured()) + (kReadIs * storage.reads.size()); }
  // The pages asked from here on may be so much work, and no more are
  // asked once they have been.
  void Allow(std::uint64_t work) { allowed = Work() + work; }
  bool Spent() const { return Work() >= allowed; }

  Got Down(ChapterPlace first) { return Result(pager.BeginDown(first, pageHeight)); }
  Got Up(ChapterPlace before) { return Result(pager.BeginUp(before, pageHeight)); }
  Got About(ChapterPlace place) { return Result(pager.BeginAbout(place, pageHeight)); }
  PagerStatus LinesOf(std::uint32_t block, Lines& lines) {
    Gathered gathered;
    const PagerStatus status = Run(pager.BeginLines(block, gathered));
    lines = std::move(gathered.lines);
    return status;
  }

  Rules rules;
  FakeStorage storage;
  alignas(4) std::array<std::uint8_t, 4096> buffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  BlockFile file{buffer};
  SegmentReader reader{page};
  MadeUpFace face;
  std::vector<LineBreaker::Node> nodes;
  std::vector<ParagraphSetter::Record> setterRecords =
      std::vector<ParagraphSetter::Record>(ParagraphSetter::kUsualRecords);
  ParagraphSetter setter{nodes, setterRecords};
  std::vector<ChapterLine> records;
  std::vector<PageFromBottom::Entry> entries;
  ChapterPager pager{reader, setter, face, rules, entries, records};
  std::uint16_t pageHeight;
  std::size_t mostSteps = 0;
  std::uint64_t allowed = 0;
  // A piece of work was left where it stood, the input having been as much
  // work as one may be: nothing it gave is to be looked at.
  bool gaveUp = false;
};

// What is true of any page the pager gives, whatever the chapter.
void CheckAnyPage(const Desk& desk, const Got& got) {
  if (got.status != PagerStatus::kOk) {
    return;
  }
  BN_FUZZ_CHECK(got.page.lines == got.lines.size() && got.lines.size() <= desk.records.size(),
                "a page of more lines than there are records");
  BN_FUZZ_CHECK(got.page.found == !got.lines.empty(), "a page that is there with no lines, or not there with some");
  if (!got.page.found) {
    return;
  }
  BN_FUZZ_CHECK(got.page.first == got.lines[0].Place(), "a page that does not begin with its first line");
  std::uint32_t bottom = 0;
  for (const ChapterLine& line : got.lines) {
    BN_FUZZ_CHECK(line.begin <= line.end && line.height >= 1, "a line that ends before it begins, or of no height");
    BN_FUZZ_CHECK(line.top >= bottom, "a line that begins above the bottom of the one before");
    bottom = line.top + line.height;
  }
  BN_FUZZ_CHECK(got.page.height == bottom, "a page whose height is not where its last line ends");
  BN_FUZZ_CHECK(bottom <= desk.pageHeight || got.lines.size() == 1, "lines that stand higher than the page");
}

// ---- A chapter the target writes ----

SegmentLength InPixels(std::uint8_t pixels) {
  SegmentLength length;
  length.px = pixels;
  return length;
}

std::string MadeText(std::size_t bytes, std::uint32_t seed) {
  static const std::array<std::string_view, 10> kWords = {
      "the ", "quick ", "brown ", "caf\xC3\xA9 ", "well-known ", "a ", "immeasurably ", "it\nwas ", "WIDE  ", "i "};
  std::string text;
  std::uint32_t state = seed;
  while (text.size() < bytes) {
    state = (state * 1664525U) + 1013904223U;
    text += kWords.at((state >> 16U) % kWords.size());
  }
  return text;
}

// Gives the writer what the input says, and keeps what was given.
class Giver {
 public:
  Giver(SegmentWriter& writer, std::vector<Block>& chapter) : writer_(writer), chapter_(chapter) {}

  void Give(bn::fuzz::Input& input) {
    while (!input.Empty() && chapter_.size() < kMostBlocks && characters_ < kMostCharacters) {
      const std::uint32_t what = input.Byte();
      switch (what & 7U) {
        case 0:
          Begin(input);
          break;
        case 1: {
          const std::span<const std::uint8_t> bytes = input.Take(input.Byte());
          Text(std::string(bytes.begin(), bytes.end()));
          break;
        }
        case 2: {
          const std::uint8_t length = input.Byte();
          Text(MadeText(std::size_t{length} * 16, length));
          break;
        }
        case 3:
          Style(input.Byte());
          break;
        case 4: {
          const std::uint32_t number = input.Byte();
          const std::uint8_t height = input.Byte();
          Picture(number == 255 ? bn::epub::kSegmentNoPicture : static_cast<std::uint16_t>(number & 7U), height);
          break;
        }
        case 5:
          End();
          break;
        case 6: {
          const std::uint8_t count = input.Byte();
          End();
          for (std::uint32_t i = 0; i < count && chapter_.size() < kMostBlocks; ++i) {
            BeginWith(SegmentBlockKind::kParagraph, BlockStyle{});
            Text("w" + std::to_string(i));
            End();
          }
          break;
        }
        default:
          Link();
          break;
      }
    }
    End();
  }

 private:
  void Ok(SegmentStatus status) { BN_FUZZ_CHECK(status == SegmentStatus::kOk, "the writer refused what was in turn"); }

  void Begin(bn::fuzz::Input& input) {
    const std::uint32_t kind = input.Byte();
    const std::uint32_t has = input.Byte();
    BlockStyle style;
    const auto set = [&style, &input, has](std::uint32_t bit, std::uint16_t field, SegmentLength& length) {
      if ((has & bit) != 0) {
        style.has = static_cast<std::uint16_t>(style.has | field);
        length = InPixels(input.Byte());
      }
    };
    set(1, bn::epub::kBlockLineHeight, style.lineHeight);
    set(2, bn::epub::kBlockSpaceBefore, style.spaceBefore);
    set(4, bn::epub::kBlockSpaceAfter, style.spaceAfter);
    set(8, bn::epub::kBlockIndent, style.indent);
    set(16, bn::epub::kBlockMarginLeft, style.marginLeft);
    set(32, bn::epub::kBlockMarginRight, style.marginRight);
    const auto breaking = static_cast<std::uint16_t>((kind >> 3U) & 7U);
    if (breaking != 0) {
      style.has = static_cast<std::uint16_t>(style.has | bn::epub::kBlockBreaking);
      style.breaking = breaking;
    }
    End();
    BeginWith(static_cast<SegmentBlockKind>(1 + ((kind & 7U) % 6)), style);
  }

  void BeginWith(SegmentBlockKind kind, const BlockStyle& style) {
    source_ += 10;
    Ok(writer_.BeginBlock(kind, kind == SegmentBlockKind::kHeading ? 2 : 0, style, source_));
    Block block;
    block.kind = kind;
    block.style = style;
    chapter_.push_back(std::move(block));
    inBlock_ = true;
    inLink_ = false;
    bits_ = 0;
  }

  void Add(std::string_view characters) {
    Block& block = chapter_.back();
    block.characters += characters;
    characters_ += characters.size();
    if (!block.runs.empty() && block.runs.back().second == bits_) {
      block.runs.back().first = block.characters.size();
    } else {
      block.runs.emplace_back(block.characters.size(), bits_);
    }
  }

  void Text(const std::string& text) {
    if (!inBlock_ || text.empty()) {
      return;
    }
    for (std::size_t at = 0; at < text.size(); at += 900) {
      Ok(writer_.Text(std::string_view(text).substr(at, 900)));
    }
    Add(text);
  }

  void Style(std::uint32_t bits) {
    if (!inBlock_) {
      return;
    }
    RunStyle run;
    run.bits = static_cast<std::uint16_t>(bits & 7U);
    Ok(writer_.Style(run));
    bits_ = static_cast<std::uint16_t>(bits & 3U);
  }

  void Picture(std::uint16_t number, std::uint8_t height) {
    if (!inBlock_) {
      return;
    }
    Ok(writer_.Picture(number, 0, SegmentLength{}, InPixels(height), "alt"));
    Block& block = chapter_.back();
    if (block.kind == SegmentBlockKind::kPicture && block.characters.empty()) {
      block.pictureFirst = true;
      block.picture = number;
      block.pictureHeight = InPixels(height);
    }
    Add(std::string_view(&ChapterPager::kPictureByte, 1));
  }

  void Link() {
    if (!inBlock_) {
      return;
    }
    Ok(inLink_ ? writer_.LinkEnd() : writer_.LinkStart(LinkKind::kInside, 0, "to"));
    inLink_ = !inLink_;
  }

  void End() {
    if (!inBlock_) {
      return;
    }
    if (inLink_) {
      Ok(writer_.LinkEnd());
    }
    Ok(writer_.EndBlock());
    inBlock_ = false;
  }

  SegmentWriter& writer_;
  std::vector<Block>& chapter_;
  bool inBlock_ = false;
  bool inLink_ = false;
  std::uint16_t bits_ = 0;
  std::uint32_t source_ = 0;
  std::size_t characters_ = 0;
};

// The page is the lines `first` up to `end` of the chapter's, each where
// adding the heights up puts it.
void CheckPage(const Got& got, const Lines& all, std::size_t first, std::size_t end, bool filledDown, bool cutShort,
               ChapterPlace next) {
  BN_FUZZ_CHECK(got.status == PagerStatus::kOk && got.page.found, "a page that should be there is not");
  BN_FUZZ_CHECK(got.page.filledDown == filledDown, "a page filled the other way");
  BN_FUZZ_CHECK(got.page.cutShort == cutShort, "a page that says it was cut short and was not, or does not and was");
  BN_FUZZ_CHECK(got.lines.size() == end - first, "a page of other lines than adding up chooses");
  BN_FUZZ_CHECK(got.page.first == all[first].Place() && got.page.next == next,
                "a page that begins or ends at another line than adding up chooses");
  std::uint32_t y = 0;
  for (std::size_t i = first; i < end; ++i) {
    const ChapterLine& line = got.lines[i - first];
    BN_FUZZ_CHECK(SameLine(line, all[i]), "a line of a page that is not the chapter's line");
    const std::uint32_t top = i == first ? (Starts(all[i]) ? all[i].gap : 0U) : y + all[i].gap;
    BN_FUZZ_CHECK(line.top == top, "a line that is not where adding the heights up puts it");
    y = top + all[i].height;
  }
  BN_FUZZ_CHECK(got.page.height == y, "a page's height is not what its lines add up to");
}

void Written(bn::fuzz::Input& input, const Choices& choices, const Rules& rules, std::uint32_t seed) {
  Desk desk(choices, rules);
  desk.Open(OpenMode::kCreate);
  std::vector<std::uint8_t> memory(bn::epub::kSegmentWriterBytes);
  SegmentWriter writer(memory);
  std::vector<Block> chapter;
  BN_FUZZ_CHECK(writer.Begin(desk.file, 0) == SegmentStatus::kOk, "the writer would not begin");
  Giver(writer, chapter).Give(input);
  BN_FUZZ_CHECK(writer.BeginEndBlocks() == SegmentStatus::kOk, "the blocks would not end");
  while (writer.Step()) {
  }
  // Every other picture's size is known.
  Sizes sizes;
  for (std::size_t i = 0; i < writer.Pictures(); ++i) {
    const std::uint16_t number = writer.PictureNumber(i);
    if (number % 2 == 1) {
      const std::pair<std::uint16_t, std::uint16_t> size = {static_cast<std::uint16_t>(100 + (number * 50)),
                                                            static_cast<std::uint16_t>(40 + (number * 30))};
      BN_FUZZ_CHECK(writer.SetPictureSize(i, size.first, size.second) == SegmentStatus::kOk, "a size was refused");
      sizes[number] = size;
    }
  }
  BN_FUZZ_CHECK(writer.BeginFinish(ChapterState::kWhole, 0, 0) == SegmentStatus::kOk, "the chapter would not end");
  while (writer.Step()) {
  }
  BN_FUZZ_CHECK(writer.Status() == SegmentStatus::kOk, "the writer did not end kOk");
  const BundleChapter record = writer.Chapter();
  desk.file.Close();
  desk.Open(OpenMode::kRead);
  BN_FUZZ_CHECK(desk.reader.Open(desk.file, record) == SegmentStatus::kOk, "the chapter does not open");
  desk.mostSteps = (std::size_t{record.segmentBytes} * 100) + 100000;
  const auto blocks = static_cast<std::uint32_t>(chapter.size());
  BN_FUZZ_CHECK(desk.reader.Blocks() == blocks, "the chapter has other blocks than were given");
  if (blocks == 0) {
    const Got none = desk.Down({.block = 0, .into = 0});
    BN_FUZZ_CHECK(none.status == PagerStatus::kOk && !none.page.found, "a page of a chapter with no blocks");
    BN_FUZZ_CHECK(!desk.Up({.block = 0, .into = 0}).page.found, "a page above a chapter with no blocks");
    BN_FUZZ_CHECK(desk.pager.BeginAbout({.block = 0, .into = 0}, desk.pageHeight) == PagerStatus::kNoSuchBlock,
                  "a place in a chapter with no blocks");
    return;
  }

  const Lines all = LinesByHand(chapter, sizes, desk.rules, choices.nodes);
  const std::size_t most = desk.records.size();
  const std::uint32_t pageHeight = desk.pageHeight;
  const ChapterPlace chapterEnd = desk.pager.End();
  const auto placeOf = [&all, chapterEnd](std::size_t index) {
    return index < all.size() ? all[index].Place() : chapterEnd;
  };

  // Each block's lines.
  std::size_t at = 0;
  for (std::uint32_t block = 0; block < blocks; ++block) {
    Lines lines;
    const PagerStatus status = desk.LinesOf(block, lines);
    if (desk.gaveUp) {
      return;
    }
    BN_FUZZ_CHECK(status == PagerStatus::kOk && !lines.empty(), "a block's lines were not given");
    for (const ChapterLine& line : lines) {
      BN_FUZZ_CHECK(at < all.size() && SameLine(line, all[at]) && line.top == 0,
                    "a block's line is not the line the setter gives of its text");
      ++at;
    }
  }
  BN_FUZZ_CHECK(at == all.size(), "fewer lines than the setter gives of the chapter's text");

  // Down from the chapter's first line, each page beginning where the one
  // before ended.
  desk.Allow(kMostWorkOfAKind);
  std::vector<std::size_t> firsts;
  for (std::size_t first = 0; first < all.size() && firsts.size() < 80 && (firsts.empty() || !desk.Spent());) {
    bool cutShort = false;
    const std::size_t end = DownByHand(all, first, pageHeight, most, cutShort);
    const Got got = desk.Down(all[first].Place());
    if (desk.gaveUp) {
      return;
    }
    CheckAnyPage(desk, got);
    CheckPage(got, all, first, end, true, cutShort, placeOf(end));
    firsts.push_back(first);
    first = end;
  }
  const Got atEnd = desk.Down(chapterEnd);
  if (desk.gaveUp) {
    return;
  }
  BN_FUZZ_CHECK(atEnd.status == PagerStatus::kOk && !atEnd.page.found, "a page that begins at the chapter's end");

  // Up to places, and about places, that the input's numbers choose.
  std::uint32_t state = seed;
  const auto next = [&state](std::uint32_t limit) {
    state = (state * 1664525U) + 1013904223U;
    return (state >> 12U) % limit;
  };
  desk.Allow(kMostWorkOfAKind);
  for (int trial = 0; trial < 6 && !desk.Spent(); ++trial) {
    const std::uint32_t pick = next(4);
    const std::size_t index = pick == 0 ? firsts[next(static_cast<std::uint32_t>(firsts.size()))]
                                        : next(static_cast<std::uint32_t>(all.size() + 1));
    if (next(8) == 0) {
      desk.pager.Forget();
    }
    if (next(8) == 0) {
      // Something else uses the reader between two pages.
      BN_FUZZ_CHECK(desk.reader.FindCharacters(next(1000)) == SegmentStatus::kOk, "a place was not looked for");
    }
    ChapterPlace before = placeOf(index);
    if (index < all.size() && next(3) == 0) {
      before.into += next(all[index].end - all[index].begin + 2);
    }
    const Got up = desk.Up(before);
    if (desk.gaveUp) {
      return;
    }
    CheckAnyPage(desk, up);
    const std::size_t end = LinesAbove(all, before);
    if (end == 0) {
      BN_FUZZ_CHECK(up.status == PagerStatus::kOk && !up.page.found, "a page above the chapter's first line");
    } else if (choices.fewEntries) {
      // The chooser may have run out of entries: the page is still lines
      // of the chapter in their order, and ends where it was asked to
      // unless it came to a line that starts a page.
      BN_FUZZ_CHECK(up.status == PagerStatus::kOk && up.page.found, "no page above a line that has lines above it");
      const std::size_t first = LinesAbove(all, up.page.first);
      BN_FUZZ_CHECK(first < all.size() && first + up.lines.size() <= all.size(), "a page of lines the chapter has not");
      for (std::size_t i = 0; i < up.lines.size(); ++i) {
        BN_FUZZ_CHECK(SameLine(up.lines[i], all[first + i]), "a line of a page that is not the chapter's line");
      }
      BN_FUZZ_CHECK(up.page.filledDown ? Starts(all[first]) : first + up.lines.size() == end && up.page.next == before,
                    "a page filled up that does not end where it was asked to");
    } else {
      const std::size_t first = UpByHand(all, end, pageHeight);
      if (Starts(all[first])) {
        bool cutShort = false;
        const std::size_t downEnd = DownByHand(all, first, pageHeight, most, cutShort);
        CheckPage(up, all, first, downEnd, true, cutShort, placeOf(downEnd));
      } else {
        const bool cutShort = end - first > most;
        CheckPage(up, all, cutShort ? end - most : first, end, false, cutShort, before);
      }
    }

    const ChapterLine& line = all[next(static_cast<std::uint32_t>(all.size()))];
    const ChapterPlace place{.block = line.block, .into = line.begin + next(line.end - line.begin + 3)};
    const std::size_t asked = LineHolding(all, place);
    const std::size_t first = HoldingByHand(all, asked, pageHeight);
    bool cutShort = false;
    const std::size_t downEnd = DownByHand(all, first, pageHeight, most, cutShort);
    const Got about = desk.About(place);
    if (desk.gaveUp) {
      return;
    }
    CheckAnyPage(desk, about);
    CheckPage(about, all, first, downEnd, true, cutShort, placeOf(downEnd));
  }
  BN_FUZZ_CHECK(desk.pager.BeginAbout({.block = blocks, .into = 0}, desk.pageHeight) == PagerStatus::kNoSuchBlock,
                "a place past the chapter's last block");
  if ((seed & 0x8000U) == 0 || record.directoryOffset == 0) {
    return;
  }

  // One bit changed, somewhere before the directory.
  Bytes& bytes = desk.storage.entries.at(kPath).data;
  const std::size_t changed = next(record.directoryOffset);
  bytes[changed] = static_cast<std::uint8_t>(bytes[changed] ^ (1U << next(8)));
  desk.file.Close();
  desk.Open(OpenMode::kRead);
  desk.reader.Close();
  BN_FUZZ_CHECK(desk.reader.Open(desk.file, record) == SegmentStatus::kOk, "the chapter does not open again");
  desk.pager.Forget();
  at = 0;
  const std::uint32_t fromBefore = std::uint32_t{bn::layout::kLineKept};
  for (std::uint32_t block = 0; block < blocks; ++block) {
    std::size_t count = 0;
    while (at + count < all.size() && all[at + count].block == block) {
      ++count;
    }
    Lines lines;
    const PagerStatus status = desk.LinesOf(block, lines);
    if (desk.gaveUp) {
      return;
    }
    BN_FUZZ_CHECK(status == PagerStatus::kOk ||
                      (status == PagerStatus::kReadFailed && desk.pager.ReaderStatus() != SegmentStatus::kOk),
                  "a block of a changed chapter ended in a status it should not");
    if (status == PagerStatus::kOk) {
      BN_FUZZ_CHECK(lines.size() == count, "a block of a changed chapter was read, and has other lines");
      for (std::size_t i = 0; i < count; ++i) {
        ChapterLine got = lines[i];
        ChapterLine want = all[at + i];
        if (i == 0) {
          got.gap = want.gap;
          got.flags = static_cast<std::uint8_t>(got.flags & ~fromBefore);
          want.flags = static_cast<std::uint8_t>(want.flags & ~fromBefore);
        }
        BN_FUZZ_CHECK(SameLine(got, want), "a block of a changed chapter was read, and has another line");
      }
    }
    at += count;
  }
}

// ---- Any bytes at all ----

// Puts right the checksum of each page the directory names, as far as the
// directory can be followed.
void Mend(Bytes& segment, std::size_t directory) {
  if (segment.size() < directory + 20) {
    return;
  }
  const std::size_t indexPages = Get16(segment, directory);
  const std::size_t anchorPages = Get16(segment, directory + 2);
  const std::size_t blocks = std::min<std::size_t>(Get32(segment, directory + 8), 65536);
  std::size_t at = directory + 20;
  for (std::size_t page = 0; page < indexPages + anchorPages && at + 12 <= segment.size(); ++page, at += 12) {
    const std::size_t where = Get32(segment, at);
    std::size_t length = Get16(segment, at + 4);
    if (page < indexPages) {
      length = page + 1 < indexPages ? 4096 : 16 * (blocks - std::min(blocks, 256 * page));
    }
    if (where <= directory && length <= directory - where) {
      Put32(segment, at + 8, bn::core::Crc32(std::span(segment).subspan(where, length)));
    }
  }
}

void AnyBytes(bn::fuzz::Input& input, unsigned bits, const Choices& choices, const Rules& rules, std::uint32_t seed) {
  const std::size_t directoryBytes = input.Number16();
  const std::span<const std::uint8_t> rest = input.Rest();
  Bytes segment(rest.begin(), rest.end());
  BundleChapter record;
  record.state = ChapterState::kWhole;
  record.segmentBytes = static_cast<std::uint32_t>(segment.size());
  record.directoryBytes = static_cast<std::uint32_t>(directoryBytes);
  record.directoryOffset = static_cast<std::uint32_t>(segment.size() - std::min(directoryBytes, segment.size()));
  if (directoryBytes >= 20 && directoryBytes <= segment.size()) {
    if ((bits & 0x40U) != 0) {
      Mend(segment, record.directoryOffset);
    }
    if ((bits & 0x80U) != 0) {
      record.directoryChecksum = bn::core::Crc32(std::span(segment).subspan(record.directoryOffset));
      record.blocks = Get32(segment, record.directoryOffset + 8U);
      record.characters = Get32(segment, record.directoryOffset + 12U);
    }
  }
  Desk desk(choices, rules);
  desk.storage.Put(kPath, segment);
  desk.Open(OpenMode::kRead);
  if (desk.reader.Open(desk.file, record) != SegmentStatus::kOk) {
    BN_FUZZ_CHECK(desk.pager.BeginDown({.block = 0, .into = 0}, desk.pageHeight) == PagerStatus::kMisused,
                  "work was begun on a chapter that did not open");
    return;
  }
  desk.mostSteps = (segment.size() * 100) + 100000;
  const std::uint32_t blocks = desk.reader.Blocks();
  const auto sound = [&desk](PagerStatus status) {
    BN_FUZZ_CHECK(status == PagerStatus::kOk || status == PagerStatus::kNoSuchBlock ||
                      (status == PagerStatus::kReadFailed && desk.pager.ReaderStatus() != SegmentStatus::kOk &&
                       desk.pager.ReaderStatus() != SegmentStatus::kStoreFailed),
                  "a piece of work ended in a status it should not");
  };
  std::uint32_t state = seed;
  const auto next = [&state](std::uint32_t limit) {
    state = (state * 1664525U) + 1013904223U;
    return (state >> 12U) % limit;
  };
  desk.Allow(kMostWorkOfAKind);
  for (int trial = 0; trial < 4 && !desk.Spent(); ++trial) {
    // Mostly among the first blocks, where a segment of a few bytes has
    // its blocks; now and then anywhere, and past the end.
    const std::uint32_t block = next(4) == 0 ? next(blocks + 2) : next(8);
    const ChapterPlace place{.block = block, .into = next(4) == 0 ? next(5000) : 0};
    Lines lines;
    const PagerStatus status = desk.LinesOf(block, lines);
    if (desk.gaveUp) {
      return;
    }
    sound(status);
    for (int way = 0; way < 3 && !desk.Spent(); ++way) {
      const Got got = way == 0 ? desk.Down(place) : way == 1 ? desk.Up(place) : desk.About(place);
      if (desk.gaveUp) {
        return;
      }
      sound(got.status);
      CheckAnyPage(desk, got);
    }
  }
  const Got last = desk.Up(desk.pager.End());
  if (desk.gaveUp) {
    return;
  }
  sound(last.status);
  CheckAnyPage(desk, last);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned bits = input.Byte();
  Choices choices;
  choices.pageHeight = static_cast<std::uint16_t>(1U + (input.Byte() * 3U));
  Rules rules;
  rules.width = 200 + (input.Byte() * 8);
  rules.usualLineHeight = (bits & 0x40U) != 0 && (bits & 1U) == 0 ? std::uint16_t{0} : std::uint16_t{20};
  const std::uint32_t seed = input.Number16();
  static constexpr std::array<std::size_t, 4> kLineRecords = {8, 16, 64, 0};
  choices.lineRecords = kLineRecords.at((bits >> 1U) & 3U);
  if (choices.lineRecords == 0) {
    choices.lineRecords = std::size_t{choices.pageHeight} + 1;
  }
  choices.lineRecords = std::max(choices.lineRecords, ChapterPager::kMinLines);
  choices.fewEntries = (bits & 8U) != 0;
  static constexpr std::array<std::size_t, 4> kNodes = {32, 64, 256, 256};
  choices.nodes = kNodes.at((bits >> 4U) & 3U);
  if ((bits & 1U) == 0) {
    Written(input, choices, rules, seed);
  } else {
    AnyBytes(input, bits, choices, rules, seed);
  }
  return 0;
}
