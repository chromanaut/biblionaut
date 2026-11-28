// The fuzz target for a chapter's segment (epub/segment.h): written from
// what the input says, and read; or the input's own bytes, read.
//
// What it is given:
//
//   byte 0   bit 0: 0, the target writes the segment; 1, the rest is one
//
//   then, for a segment the target writes:
//
//   byte 0   bit 1: once it has been read back, one byte of it is
//                   changed and it is read again
//            bits 2 to 4: how many languages the chapter names, three times
//                   over
//   2 bytes  which byte is changed, and which of its bits
//   the rest things given to the writer, each a byte that says what (the
//            low four bits) and then what that kind needs: a block begun,
//            with its kind, its style and how far on in the source it
//            began; text, of bytes that follow or made up here to a
//            length; a style; a link begun, and ended; a picture; an id;
//            a block ended; and a run of small blocks, which is how an
//            input of a few bytes comes to a chapter of several index
//            pages. What would be out of turn (text outside a block, a
//            link in a link) is not given, nor more than 3,000 blocks or
//            300,000 characters.
//
//   and for a segment that is the input's bytes:
//
//   byte 0   bit 1: the directory's checksum, in the chapter's record, is
//                   the one its bytes come to; and the record's blocks and
//                   characters are the directory's
//            bit 2: the checksum of each index page and anchor page the
//                   directory names is put right in the directory, and
//                   then the directory's own
//   2 bytes  how long the directory is: it is the segment's last bytes
//   4 bytes  what is looked for: a block, a count of characters, a place
//            in the source
//   1 byte   how long the id looked for is; then the id
//   the rest the segment
//
// What has to hold of a segment the target writes:
//
//   - every call of the writer that is in turn ends kOk, writes two pages
//     at the most (a step, one), each a whole page where the last ended;
//   - the chapter's record says what was given: its blocks, its
//     characters, what was left out; and the file is the segment;
//   - read back, every block is what was given, with its style, its items
//     (text given piece after piece as one run), its characters and the
//     characters before it; text items are of a kilobyte at the most, and
//     where every piece of a block's text was whole characters, so is
//     every text item of it;
//   - a block looked for by its number, its characters, or where it began
//     in the source is found where it was read, and an id at the block
//     and the place it was given, the first of its name;
//   - the ids, gone through page by page, are the ids given, in order;
//     and the pictures and the languages are those given;
//   - with one byte changed, everything asked either ends in a status
//     that is not kOk or gives what the chapter held.
//
// And of any bytes at all:
//
//   - opening ends in a status, a page of the directory a step;
//   - whatever is then asked ends in a status, in no more steps than the
//     format allows (two to find a block or a count of characters, twenty
//     for a place in the source, two for each anchor page and two more for
//     an id), and no call asks the file for more than a page's worth;
//   - going from block to block ends, in no more calls than the segment
//     has records; an item's text is no longer than an item may be, a
//     link's name and a picture's text no longer than a name, and a
//     style's language one the chapter has;
//   - a place that is found is of a block that begins before the
//     directory.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/segment.h"
#include "fuzz_support.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::BlockStyle;
using bn::epub::BundleChapter;
using bn::epub::ChapterState;
using bn::epub::ItemKind;
using bn::epub::LinkKind;
using bn::epub::RunStyle;
using bn::epub::SegmentAnchor;
using bn::epub::SegmentBlock;
using bn::epub::SegmentBlockKind;
using bn::epub::SegmentItem;
using bn::epub::SegmentPicture;
using bn::epub::SegmentPlace;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::epub::SegmentWriter;
using bn::epub::StyleLength;
using bn::test::FakeStorage;
using Bytes = std::vector<std::uint8_t>;

const char* const kPath = "/s";
// The most a chapter the target writes is given: an input of a few bytes
// can ask for a great deal.
constexpr std::size_t kMostBlocks = 3000;
constexpr std::uint32_t kMostCharacters = 300000;

std::uint32_t Get16(const Bytes& from, std::size_t at) {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}
std::uint32_t Get32(const Bytes& from, std::size_t at) { return Get16(from, at) | (Get16(from, at + 2) << 16U); }
void Put32(Bytes& to, std::size_t at, std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    to[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
  }
}

// An item, as it was given and as it is read back.
struct Item {
  ItemKind kind = ItemKind::kText;
  std::string text;
  RunStyle run;
  LinkKind link = LinkKind::kInside;
  std::uint16_t chapter = 0;
  std::uint16_t picture = 0;
  std::uint16_t bits = 0;
  StyleLength width;
  StyleLength height;

  bool operator==(const Item&) const = default;
};
struct Block {
  std::uint8_t kind = 0;
  std::uint8_t level = 0;
  BlockStyle style;
  std::uint32_t source = 0;
  std::uint32_t before = 0;
  std::uint32_t characters = 0;
  std::uint32_t offset = 0;  // Where it was read from.
  // Every piece of its text that was given, and every text item of it
  // that was read, was whole characters.
  bool wholeText = true;
  std::vector<Item> items;
};
struct Anchor {
  std::string name;
  std::uint32_t block = 0;
  std::uint32_t into = 0;

  bool operator==(const Anchor&) const = default;
};
struct Chapter {
  std::vector<Block> blocks;
  std::vector<Anchor> anchors;
  std::map<std::uint16_t, bool> pictures;  // Each one shown, and whether it was given a size.
  std::size_t languages = 0;
  std::uint32_t characters = 0;
  std::uint32_t leftOut = 0;
};

Item ItemOf(ItemKind kind, std::string text = {}) {
  Item item;
  item.kind = kind;
  item.text = std::move(text);
  return item;
}

// A segment's file, and what reads it.
struct Card {
  Card() { BN_FUZZ_CHECK(storage.Start() == StorageStatus::kOk, "the storage did not start"); }

  void Open(OpenMode mode) {
    BN_FUZZ_CHECK(file.Open(storage, kPath, mode) == StorageStatus::kOk, "the segment's file does not open");
  }

  // How many bytes were asked of storage since `mark`: a call may ask the
  // file for a page's worth, which is two of its blocks where they lie
  // across two.
  void HoldReads(std::size_t mark) const {
    std::size_t bytes = 0;
    for (std::size_t i = mark; i < storage.reads.size(); ++i) {
      bytes += storage.reads[i].size;
    }
    BN_FUZZ_CHECK(storage.reads.size() - mark <= 2 && bytes <= 8192,
                  "one call of the reader asked the file for more than a page's worth");
  }

  // Does a piece of the reader's work to its end, in `most` steps at the
  // most.
  SegmentStatus Steps(SegmentStatus begun, std::size_t most) {
    if (begun != SegmentStatus::kOk) {
      return begun;
    }
    std::size_t steps = 0;
    for (bool more = true; more; ++steps) {
      const std::size_t mark = storage.reads.size();
      more = reader.Step();
      HoldReads(mark);
      BN_FUZZ_CHECK(steps < most, "a piece of the reader's work took more steps than it may");
    }
    BN_FUZZ_CHECK(!reader.Step(), "a piece of work that was over went on");
    return reader.Status();
  }

  // Reads the next block to its end. kEnd if there is none.
  SegmentStatus ReadBlock(Block& block, std::size_t& calls) {
    block = Block{};
    SegmentBlock head;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      const std::size_t mark = storage.reads.size();
      status = reader.ReadBlock(head);
      HoldReads(mark);
      BN_FUZZ_CHECK(++calls <= mostCalls, "going from block to block does not end");
    }
    if (status != SegmentStatus::kOk) {
      return status;
    }
    BN_FUZZ_CHECK(head.kind != 0 && head.kind < bn::epub::kSegmentIndexPage && head.offset < directoryOffset,
                  "a block of no kind, or that begins in the directory");
    block.kind = head.kind;
    block.level = head.level;
    block.style = head.style;
    block.before = head.charactersBefore;
    block.offset = head.offset;
    for (;;) {
      SegmentItem item;
      const std::size_t mark = storage.reads.size();
      status = reader.NextItem(item);
      HoldReads(mark);
      BN_FUZZ_CHECK(++calls <= mostCalls, "the items of a block do not end");
      if (status == SegmentStatus::kMore) {
        continue;
      }
      if (status != SegmentStatus::kOk) {
        return status;
      }
      BN_FUZZ_CHECK(item.text.size() <= bn::epub::kSegmentMostItem, "an item longer than an item may be");
      switch (item.kind) {
        case ItemKind::kEnd:
          block.characters = item.characters;
          return SegmentStatus::kOk;
        case ItemKind::kText:
          longestText = std::max(longestText, item.text.size());
          block.wholeText = block.wholeText && bn::fuzz::IsWholeCharacters(item.text);
          if (!block.items.empty() && block.items.back().kind == ItemKind::kText) {
            block.items.back().text += item.text;
          } else {
            block.items.push_back(ItemOf(ItemKind::kText, std::string(item.text)));
          }
          break;
        case ItemKind::kStyle: {
          BN_FUZZ_CHECK(item.style.language <= reader.Languages() && item.style.familyClass <= 7,
                        "a style of a language the chapter has not, or of no class");
          Item read = ItemOf(ItemKind::kStyle);
          read.run = item.style;
          block.items.push_back(read);
          break;
        }
        case ItemKind::kLinkStart: {
          BN_FUZZ_CHECK(item.text.size() <= bn::epub::kSegmentMostName && item.link <= LinkKind::kOutside,
                        "a link's name longer than a name, or a link of no kind");
          Item read = ItemOf(ItemKind::kLinkStart, std::string(item.text));
          read.link = item.link;
          read.chapter = item.chapter;
          block.items.push_back(read);
          break;
        }
        case ItemKind::kLinkEnd:
          block.items.push_back(ItemOf(ItemKind::kLinkEnd));
          break;
        case ItemKind::kPicture: {
          BN_FUZZ_CHECK(item.text.size() <= bn::epub::kSegmentMostName, "a picture's text longer than a name");
          Item read = ItemOf(ItemKind::kPicture, std::string(item.text));
          read.picture = item.picture;
          read.bits = item.pictureBits;
          read.width = item.width;
          read.height = item.height;
          block.items.push_back(read);
          break;
        }
      }
    }
  }

  FakeStorage storage;
  alignas(4) std::array<std::uint8_t, 4096> buffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  BlockFile file{buffer};
  SegmentReader reader{page};
  std::uint32_t directoryOffset = 0;
  std::size_t mostCalls = 0;
  std::size_t longestText = 0;
};

// The steps a piece of the reader's work may take.
constexpr std::size_t kStepsToFindBlock = 2;
constexpr std::size_t kStepsToFindSource = 20;
std::size_t StepsToFindAnchor(const SegmentReader& reader) { return (2 * reader.AnchorPages()) + 2; }

StyleLength LengthFrom(std::uint32_t seed) {
  return {.em = static_cast<std::int16_t>(seed * 7U),
          .px = static_cast<std::int16_t>(seed * 13U),
          .percent = static_cast<std::int16_t>(seed * 29U)};
}

// A style with the fields `mask` says, each with a value made from `seed`.
BlockStyle StyleFrom(std::uint32_t mask, std::uint32_t seed) {
  BlockStyle style;
  style.has = static_cast<std::uint16_t>(mask & bn::epub::kBlockFields);
  style.fromBook = static_cast<std::uint16_t>((seed * 2654435761U) >> 16U);
  const auto has = [&style](std::uint16_t field) { return (style.has & field) != 0; };
  style.align = has(bn::epub::kBlockAlign) ? static_cast<std::uint8_t>(seed % 6U) : std::uint8_t{0};
  style.direction = has(bn::epub::kBlockDirection) ? static_cast<std::uint8_t>(seed % 2U) : std::uint8_t{0};
  style.size = has(bn::epub::kBlockSize) ? static_cast<std::uint16_t>(seed * 3U) : std::uint16_t{1000};
  if (has(bn::epub::kBlockLineHeight)) {
    style.lineMultiple = static_cast<std::uint16_t>(seed * 5U);
    style.lineHeight = LengthFrom(seed + 1);
  }
  style.indent = has(bn::epub::kBlockIndent) ? LengthFrom(seed + 2) : StyleLength{};
  style.marginLeft = has(bn::epub::kBlockMarginLeft) ? LengthFrom(seed + 3) : StyleLength{};
  style.marginRight = has(bn::epub::kBlockMarginRight) ? LengthFrom(seed + 4) : StyleLength{};
  style.spaceBefore = has(bn::epub::kBlockSpaceBefore) ? LengthFrom(seed + 5) : StyleLength{};
  style.spaceAfter = has(bn::epub::kBlockSpaceAfter) ? LengthFrom(seed + 6) : StyleLength{};
  style.breaking = has(bn::epub::kBlockBreaking) ? static_cast<std::uint16_t>(seed * 11U) : std::uint16_t{0};
  if (has(bn::epub::kBlockDepths)) {
    style.quoteDepth = static_cast<std::uint8_t>(seed);
    style.listDepth = static_cast<std::uint8_t>(seed >> 3U);
  }
  if (has(bn::epub::kBlockMarker)) {
    style.marker = static_cast<std::uint8_t>(seed >> 1U);
    style.markerNumber = seed * 40503U;
  }
  if (has(bn::epub::kBlockTable)) {
    style.table = static_cast<std::uint16_t>(seed);
    style.row = static_cast<std::uint16_t>(seed * 17U);
    style.column = static_cast<std::uint16_t>(seed * 19U);
    style.rowSpan = static_cast<std::uint8_t>(seed >> 2U);
    style.columnSpan = static_cast<std::uint8_t>(seed >> 4U);
    style.tableBits = static_cast<std::uint8_t>(seed & 1U);
    style.tableDepth = static_cast<std::uint8_t>(seed >> 5U);
  }
  style.width = has(bn::epub::kBlockWidth) ? LengthFrom(seed + 7) : StyleLength{};
  style.height = has(bn::epub::kBlockHeight) ? LengthFrom(seed + 8) : StyleLength{};
  return style;
}

std::string TextOf(std::span<const std::uint8_t> bytes) { return {bytes.begin(), bytes.end()}; }

// Text of so many bytes, in characters of one to four.
std::string MadeText(std::size_t bytes, std::uint32_t seed) {
  static const std::array<std::string_view, 5> kLetters = {"a", " ", "\xC3\xA9", "\xE6\xBC\xA2", "\xF0\x9F\x98\x80"};
  std::string text;
  while (text.size() < bytes) {
    seed = (seed * 1664525U) + 1013904223U;
    const std::string_view letter = kLetters.at((seed >> 24U) % kLetters.size());
    if (text.size() + letter.size() > bytes) {
      text += 'x';
    } else {
      text += letter;
    }
  }
  return text;
}

// Gives the writer what the input says, and keeps what it was given.
class Giver {
 public:
  Giver(SegmentWriter& writer, FakeStorage& storage, Chapter& chapter)
      : writer_(writer), storage_(storage), chapter_(chapter) {}

  // Holds the call that was just made to what a call may write: so many
  // pages, each whole and where the last one ended.
  void Held(SegmentStatus status, std::size_t most) {
    BN_FUZZ_CHECK(status == SegmentStatus::kOk, "a call of the writer that was in turn did not end kOk");
    BN_FUZZ_CHECK(storage_.written.size() - pages_ <= most, "one call of the writer wrote more pages than it may");
    for (; pages_ < storage_.written.size(); ++pages_) {
      BN_FUZZ_CHECK(
          storage_.written[pages_].offset == pages_ * 4096 && (storage_.written[pages_].size == 4096 || last_),
          "a write that is not a whole page, where the last one ended");
    }
  }

  void BeginBlock(std::uint8_t kind, std::uint8_t level, const BlockStyle& style, std::uint32_t step) {
    EndBlock();
    if (chapter_.blocks.size() >= kMostBlocks) {
      return;
    }
    source_ += step;
    Held(writer_.BeginBlock(static_cast<SegmentBlockKind>(kind), level, style, source_), 2);
    Block block;
    block.kind = kind;
    block.level = level;
    block.style = style;
    block.style.fromBook &= style.has;
    block.source = source_;
    block.before = chapter_.characters;
    chapter_.blocks.push_back(block);
    inBlock_ = true;
  }

  void Text(const std::string& text) {
    if (!inBlock_ || text.empty() || chapter_.characters + chapter_.blocks.back().characters > kMostCharacters) {
      return;
    }
    Held(writer_.Text(text), 2);
    Block& block = chapter_.blocks.back();
    block.wholeText = block.wholeText && bn::fuzz::IsWholeCharacters(text);
    if (!block.items.empty() && block.items.back().kind == ItemKind::kText) {
      block.items.back().text += text;
    } else {
      block.items.push_back(ItemOf(ItemKind::kText, text));
    }
    block.characters += static_cast<std::uint32_t>(text.size());
  }

  void Style(const RunStyle& run) {
    if (!inBlock_) {
      return;
    }
    Held(writer_.Style(run), 2);
    Item item = ItemOf(ItemKind::kStyle);
    item.run = run;
    chapter_.blocks.back().items.push_back(item);
  }

  void LinkStart(LinkKind kind, std::uint16_t chapter, const std::string& fragment) {
    if (!inBlock_ || linkOpen_) {
      return;
    }
    Held(writer_.LinkStart(kind, chapter, fragment), 2);
    Item item = ItemOf(ItemKind::kLinkStart, fragment);
    item.link = kind;
    item.chapter = chapter;
    chapter_.blocks.back().items.push_back(item);
    linkOpen_ = true;
  }

  void LinkEnd() {
    if (!inBlock_ || !linkOpen_) {
      return;
    }
    Held(writer_.LinkEnd(), 2);
    chapter_.blocks.back().items.push_back(ItemOf(ItemKind::kLinkEnd));
    linkOpen_ = false;
  }

  void Picture(std::uint16_t number, std::uint16_t bits, const std::string& alt) {
    if (!inBlock_) {
      return;
    }
    Item item = ItemOf(ItemKind::kPicture, alt);
    item.picture = number;
    item.bits = bits;
    item.width = LengthFrom(number);
    item.height = LengthFrom(bits + 1U);
    Held(writer_.Picture(number, bits, item.width, item.height, alt), 2);
    Block& block = chapter_.blocks.back();
    block.items.push_back(item);
    ++block.characters;
    if (number != bn::epub::kSegmentNoPicture) {
      if (chapter_.pictures.size() < bn::epub::kSegmentPictures || chapter_.pictures.contains(number)) {
        chapter_.pictures.emplace(number, false);
      } else {
        ++chapter_.leftOut;
      }
    }
  }

  void Anchor(const std::string& name) {
    Held(writer_.Anchor(name), 2);
    if (name.empty()) {
      return;
    }
    // (No input is long enough to fill every anchor page there may be, or
    // to have a block past the last that is indexed.)
    chapter_.anchors.push_back(
        {.name = name,
         .block = static_cast<std::uint32_t>(inBlock_ ? chapter_.blocks.size() - 1 : chapter_.blocks.size()),
         .into = inBlock_ ? chapter_.blocks.back().characters : 0});
  }

  void EndBlock() {
    if (!inBlock_) {
      return;
    }
    LinkEnd();
    Held(writer_.EndBlock(), 2);
    Block& block = chapter_.blocks.back();
    block.characters = std::max<std::uint32_t>(block.characters, 1);
    chapter_.characters += block.characters;
    inBlock_ = false;
  }

  // The pieces of work at the end, each step held to one page written.
  // The last of them is the one whose last write may be less than a page.
  void Steps(SegmentStatus begun, bool last) {
    last_ = last;
    Held(begun, 1);
    for (bool more = true; more;) {
      more = writer_.Step();
      Held(writer_.Status(), 1);
    }
  }

 private:
  SegmentWriter& writer_;
  FakeStorage& storage_;
  Chapter& chapter_;
  bool inBlock_ = false;
  bool linkOpen_ = false;
  bool last_ = false;
  std::uint32_t source_ = 0;
  std::size_t pages_ = 0;
};

// Writes the chapter the input describes. Returns its record.
BundleChapter WriteChapter(bn::fuzz::Input& input, unsigned languages, Card& card, Chapter& chapter) {
  std::vector<std::uint8_t> memory(bn::epub::kSegmentWriterBytes, 0xEE);
  SegmentWriter writer(memory);
  card.Open(OpenMode::kCreate);
  BN_FUZZ_CHECK(writer.Begin(card.file, 0) == SegmentStatus::kOk, "a segment could not be begun");
  for (unsigned i = 0; i < languages; ++i) {
    // More than the chapter has room for are left out; one told twice is
    // the same one.
    const std::string tag = "l" + std::to_string(i % 17U);
    const std::uint8_t number = writer.Language(tag);
    const bool kept = i % 17U < bn::epub::kSegmentLanguages;
    BN_FUZZ_CHECK(number == (kept ? (i % 17U) + 1U : 0U), "a language was given a number that is not its own");
    chapter.leftOut += kept ? 0U : 1U;
  }
  chapter.languages = std::min<std::size_t>(languages, bn::epub::kSegmentLanguages);
  Giver giver(writer, card.storage, chapter);
  std::uint32_t seed = 1;
  for (;;) {
    const std::span<const std::uint8_t> what = input.Take(1);
    if (what.empty()) {
      break;
    }
    seed = (seed * 1664525U) + what[0];
    switch (what[0] & 0x0FU) {
      case 0:
      case 1: {
        const std::uint8_t kind = static_cast<std::uint8_t>(1U + (what[0] >> 4U) % 6U);
        const std::uint32_t mask = input.Number16();
        giver.BeginBlock(kind, kind == 2 ? static_cast<std::uint8_t>(1U + seed % 6U) : std::uint8_t{0},
                         StyleFrom(mask, seed), input.Byte());
        break;
      }
      case 2:
        giver.Text(TextOf(input.Take(input.Byte())));
        break;
      case 3:
        giver.Text(MadeText(input.Number16() % (bn::epub::kSegmentMostText + 1), seed));
        break;
      case 4: {
        RunStyle run;
        run.bits = static_cast<std::uint16_t>(input.Number16());
        run.fromBook = static_cast<std::uint16_t>(seed);
        run.familyClass = static_cast<std::uint8_t>((what[0] >> 4U) % 8U);
        run.familyFromBook = (seed & 1U) != 0;
        run.language = static_cast<std::uint8_t>(input.Byte() % (chapter.languages + 1));
        run.size = static_cast<std::uint16_t>(seed >> 8U);
        giver.Style(run);
        break;
      }
      case 5: {
        const auto chapterNumber = static_cast<std::uint16_t>(input.Number16());
        giver.LinkStart(static_cast<LinkKind>((what[0] >> 4U) % 3U), chapterNumber, TextOf(input.Take(input.Byte())));
        break;
      }
      case 6:
        giver.LinkEnd();
        break;
      case 7: {
        const auto number = static_cast<std::uint16_t>(input.Number16());
        giver.Picture(number, static_cast<std::uint16_t>(what[0] >> 4U), TextOf(input.Take(input.Byte())));
        break;
      }
      case 8:
      case 9:
        giver.Anchor(TextOf(input.Take(input.Byte())));
        break;
      case 10:
        giver.EndBlock();
        break;
      case 11: {
        // A run of small blocks, each with a word and some with an id.
        const std::uint32_t count = input.Number16() % 700U;
        for (std::uint32_t i = 0; i < count; ++i) {
          giver.BeginBlock(1, 0, BlockStyle{}, i % 3U);
          giver.Text(std::to_string(i));
          if (i % 50U == 7U) {
            giver.Anchor("run-" + std::to_string(i));
          }
        }
        break;
      }
      default:
        break;
    }
  }
  giver.EndBlock();
  giver.Steps(writer.BeginEndBlocks(), false);
  BN_FUZZ_CHECK(writer.Pictures() == chapter.pictures.size(), "not as many pictures as were shown");
  std::size_t place = 0;
  for (auto& [number, sized] : chapter.pictures) {
    BN_FUZZ_CHECK(writer.PictureNumber(place) == number, "the pictures are not in the order of their numbers");
    sized = number % 2U == 0;
    if (sized) {
      BN_FUZZ_CHECK(writer.SetPictureSize(place, static_cast<std::uint16_t>(number + 1U),
                                          static_cast<std::uint16_t>(number + 2U)) == SegmentStatus::kOk,
                    "a picture's size was not taken");
    }
    ++place;
  }
  giver.Steps(writer.BeginFinish(ChapterState::kWhole, 0, 123456), true);
  const BundleChapter record = writer.Chapter();
  card.file.Close();
  BN_FUZZ_CHECK(record.blocks == chapter.blocks.size() && record.characters == chapter.characters,
                "the record does not say how many blocks and characters were given");
  BN_FUZZ_CHECK(record.leftOut == std::min<std::uint32_t>(chapter.leftOut, 65535),
                "the record's count of what was left out");
  BN_FUZZ_CHECK(record.segmentOffset == 0 && record.segmentBytes == card.storage.Contents(kPath).size() &&
                    std::uint64_t{record.directoryOffset} + record.directoryBytes == record.segmentBytes,
                "the record does not say where the segment and its directory are");
  return record;
}

// Reads everything of a segment and sets it beside what was given. True
// if it is all there; false if something asked ended in a status that is
// not kOk. What is given and is something else stops the program.
bool ReadsAsWritten(Card& card, const BundleChapter& record, const Chapter& chapter, bool changed) {
  card.Open(OpenMode::kRead);
  card.directoryOffset = record.directoryOffset;
  card.mostCalls = (record.segmentBytes / 4) + 16;
  SegmentReader& reader = card.reader;
  if (card.Steps(reader.BeginOpen(card.file, record), 4) != SegmentStatus::kOk) {
    return false;
  }
  BN_FUZZ_CHECK(reader.Blocks() == chapter.blocks.size() && reader.Characters() == chapter.characters &&
                    reader.SourceBytes() == 123456 && reader.Languages() == chapter.languages,
                "the directory was taken and says something else");
  for (std::size_t i = 1; i <= chapter.languages; ++i) {
    BN_FUZZ_CHECK(reader.Language(i) == "l" + std::to_string(i - 1), "a language's tag is not the one given");
  }
  if (reader.SeekStart() != SegmentStatus::kOk) {
    return false;
  }
  std::size_t calls = 0;
  std::vector<std::uint32_t> offsets;
  for (const Block& given : chapter.blocks) {
    Block read;
    if (card.ReadBlock(read, calls) != SegmentStatus::kOk) {
      return false;
    }
    BN_FUZZ_CHECK(read.kind == given.kind && read.level == given.level && read.style == given.style &&
                      read.before == given.before && read.characters == given.characters && read.items == given.items,
                  "a block was read, and is not what was given");
    BN_FUZZ_CHECK(changed || !given.wholeText || read.wholeText,
                  "text that was whole characters was cut part way through one");
    offsets.push_back(read.offset);
  }
  Block none;
  if (card.ReadBlock(none, calls) != SegmentStatus::kEnd) {
    return false;
  }
  BN_FUZZ_CHECK(changed || card.longestText <= 1020, "the writer made a text item of more than a kilobyte");

  // Places: some blocks by their numbers, their characters and where
  // they began; the chapter's end; and a block there is none of.
  const std::size_t blocks = chapter.blocks.size();
  for (std::size_t block = 0; block < blocks; block += 1 + (blocks / 7)) {
    const Block& given = chapter.blocks[block];
    const auto expectPlace = [&](std::size_t wanted, const char* what) {
      const SegmentPlace& place = reader.Place();
      const Block& at = chapter.blocks[wanted];
      BN_FUZZ_CHECK(
          place.found && !place.atEnd && place.block == wanted && place.offset == offsets[wanted] &&
              place.charactersBefore == at.before && place.source == at.source && place.kind == at.kind &&
              place.breaking == ((at.style.has & bn::epub::kBlockBreaking) != 0 ? at.style.breaking & 0xFFU : 0U),
          what);
    };
    if (card.Steps(reader.BeginFindBlock(static_cast<std::uint32_t>(block)), kStepsToFindBlock) != SegmentStatus::kOk) {
      return false;
    }
    expectPlace(block, "a block looked for by its number was found somewhere else");
    const std::uint32_t into = given.characters / 2;
    if (card.Steps(reader.BeginFindCharacters(given.before + into), kStepsToFindBlock) != SegmentStatus::kOk) {
      return false;
    }
    expectPlace(block, "a block looked for by its characters was found somewhere else");
    BN_FUZZ_CHECK(reader.Place().into == into, "not as far into the block as the characters asked for");
    std::size_t last = block;
    while (last + 1 < blocks && chapter.blocks[last + 1].source == given.source) {
      ++last;
    }
    if (card.Steps(reader.BeginFindSource(given.source), kStepsToFindSource) != SegmentStatus::kOk) {
      return false;
    }
    expectPlace(last, "the last block that began at a place in the source was not the one found");
  }
  // And the first block of each index page, which the search for a place
  // in the source turns on.
  for (std::size_t block = 256; block < blocks; block += 256) {
    std::size_t last = block;
    while (last + 1 < blocks && chapter.blocks[last + 1].source == chapter.blocks[block].source) {
      ++last;
    }
    if (card.Steps(reader.BeginFindSource(chapter.blocks[block].source), kStepsToFindSource) != SegmentStatus::kOk) {
      return false;
    }
    BN_FUZZ_CHECK(reader.Place().found && reader.Place().block == last,
                  "the last block that began where a page's first did was not the one found");
  }
  if (card.Steps(reader.BeginFindCharacters(chapter.characters), kStepsToFindBlock) != SegmentStatus::kOk ||
      card.Steps(reader.BeginFindBlock(static_cast<std::uint32_t>(blocks)), kStepsToFindBlock) != SegmentStatus::kOk) {
    return false;
  }
  BN_FUZZ_CHECK(!reader.Place().found, "a block the chapter does not have was found");

  // The ids: each page gone through, and a few looked for by name.
  std::size_t met = 0;
  for (std::size_t page = 0; page < reader.AnchorPages(); ++page) {
    if (card.Steps(reader.BeginReadAnchors(page), 2) != SegmentStatus::kOk) {
      return false;
    }
    SegmentAnchor anchor;
    while (reader.NextAnchor(anchor)) {
      BN_FUZZ_CHECK(met < chapter.anchors.size() && anchor.name == chapter.anchors[met].name &&
                        anchor.block == chapter.anchors[met].block && anchor.into == chapter.anchors[met].into,
                    "an id of a page is not the next that was given");
      ++met;
    }
  }
  BN_FUZZ_CHECK(met == chapter.anchors.size(), "not as many ids in the pages as were given");
  for (std::size_t i = 0; i < chapter.anchors.size(); i += 1 + (chapter.anchors.size() / 5)) {
    const std::string& name = chapter.anchors[i].name;
    if (card.Steps(reader.BeginFindAnchor(name), StepsToFindAnchor(reader)) != SegmentStatus::kOk) {
      return false;
    }
    // The first of that name.
    const auto first = std::find_if(chapter.anchors.begin(), chapter.anchors.end(),
                                    [&name](const Anchor& anchor) { return anchor.name == name; });
    const SegmentPlace& place = reader.Place();
    BN_FUZZ_CHECK(place.found && place.block == first->block && (place.atEnd || place.into == first->into) &&
                      place.atEnd == (first->block == blocks),
                  "an id was found at another place than the first of its name");
  }

  // The pictures.
  std::size_t place = 0;
  BN_FUZZ_CHECK(reader.Pictures() == chapter.pictures.size(), "the directory was taken and has other pictures");
  for (const auto& [number, sized] : chapter.pictures) {
    SegmentPicture picture;
    bool found = false;
    const std::size_t mark = card.storage.reads.size();
    if (reader.ReadPicture(number, picture, found) != SegmentStatus::kOk) {
      return false;
    }
    card.HoldReads(mark);
    SegmentPicture byPlace;
    if (reader.ReadPictureAt(place, byPlace) != SegmentStatus::kOk) {
      return false;
    }
    BN_FUZZ_CHECK(found && picture.number == number && byPlace.number == number && picture.sized == sized &&
                      picture.width == (sized ? static_cast<std::uint16_t>(number + 1U) : 0U) &&
                      picture.height == (sized ? static_cast<std::uint16_t>(number + 2U) : 0U),
                  "a picture's size is not the one given");
    ++place;
  }
  card.file.Close();
  return true;
}

void WrittenAndRead(bn::fuzz::Input& input, unsigned choices) {
  const std::uint32_t change = input.Number16();
  Card card;
  Chapter chapter;
  const BundleChapter record = WriteChapter(input, ((choices >> 2U) & 7U) * 3U, card, chapter);
  BN_FUZZ_CHECK(ReadsAsWritten(card, record, chapter, false), "a segment that was written does not read back");
  if ((choices & 2U) != 0) {
    Bytes bytes = card.storage.Contents(kPath);
    bytes[(change >> 3U) % bytes.size()] ^= static_cast<std::uint8_t>(1U << (change & 7U));
    Card again;
    again.storage.Put(kPath, bytes);
    static_cast<void>(ReadsAsWritten(again, record, chapter, true));
  }
}

// Puts right the checksums a directory carries of its pages, and says
// what the directory's own is: by where the format has them.
void Mend(Bytes& segment, std::size_t directory, bool pages) {
  if (!pages || segment.size() < directory + 20) {
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

void AnyBytes(bn::fuzz::Input& input, unsigned choices) {
  const std::size_t directoryBytes = input.Number16();
  const std::uint32_t block = input.Number16();
  const std::uint32_t place = input.Number16();
  const std::span<const std::uint8_t> named = input.Take(input.Byte());
  const std::string name(named.begin(), named.end());
  const std::span<const std::uint8_t> rest = input.Rest();
  Bytes segment(rest.begin(), rest.end());
  BundleChapter record;
  record.state = ChapterState::kWhole;
  record.segmentBytes = static_cast<std::uint32_t>(segment.size());
  record.directoryBytes = static_cast<std::uint32_t>(directoryBytes);
  record.directoryOffset = static_cast<std::uint32_t>(segment.size() - std::min(directoryBytes, segment.size()));
  record.blocks = block;
  record.characters = place;
  if (directoryBytes >= 20 && directoryBytes <= segment.size()) {
    Mend(segment, record.directoryOffset, (choices & 4U) != 0);
    if ((choices & 2U) != 0) {
      record.directoryChecksum = bn::core::Crc32(std::span(segment).subspan(record.directoryOffset));
      record.blocks = Get32(segment, record.directoryOffset + 8U);
      record.characters = Get32(segment, record.directoryOffset + 12U);
    }
  }
  Card card;
  card.storage.Put(kPath, segment);
  card.Open(OpenMode::kRead);
  card.directoryOffset = record.directoryOffset;
  card.mostCalls = (segment.size() / 4) + 16;
  SegmentReader& reader = card.reader;
  const SegmentStatus opened = card.Steps(reader.BeginOpen(card.file, record), 4);
  BN_FUZZ_CHECK(opened == SegmentStatus::kOk || opened == SegmentStatus::kDamaged,
                "opening ended in a status it should not");
  BN_FUZZ_CHECK(reader.IsOpen() == (opened == SegmentStatus::kOk), "a segment is open that did not open, or not");
  if (opened != SegmentStatus::kOk) {
    SegmentBlock head;
    BN_FUZZ_CHECK(reader.ReadBlock(head) == SegmentStatus::kMisused && reader.SeekStart() == SegmentStatus::kMisused,
                  "something was read of a segment that did not open");
    return;
  }
  BN_FUZZ_CHECK(reader.Blocks() == record.blocks && reader.Characters() == record.characters &&
                    reader.IndexPages() <= bn::epub::kSegmentIndexPages &&
                    reader.AnchorPages() <= bn::epub::kSegmentAnchorPages &&
                    reader.Pictures() <= bn::epub::kSegmentPictures &&
                    reader.Languages() <= bn::epub::kSegmentLanguages &&
                    reader.IndexedBlocks() == std::min<std::uint32_t>(reader.Blocks(), 65536),
                "a directory was taken that says what cannot be");
  for (std::size_t i = 0; i <= reader.Languages() + 1; ++i) {
    const std::string_view tag = reader.Language(i);
    BN_FUZZ_CHECK(tag.empty() == (i == 0 || i > reader.Languages()) && tag.size() <= bn::epub::kSegmentMostLanguage,
                  "a language's tag is not one a tag may be");
  }
  const auto sound = [](SegmentStatus status) {
    return status == SegmentStatus::kOk || status == SegmentStatus::kDamaged;
  };
  const auto placed = [&reader, &record] {
    const SegmentPlace& found = reader.Place();
    return !found.found || found.atEnd || (found.offset < record.directoryOffset && found.block < reader.Blocks());
  };

  // Every block from the first, to the end or to where it is damaged.
  BN_FUZZ_CHECK(reader.SeekStart() == SegmentStatus::kOk, "the start of an open segment could not be gone to");
  std::size_t calls = 0;
  std::uint32_t read = 0;
  SegmentStatus status = SegmentStatus::kOk;
  while (status == SegmentStatus::kOk) {
    Block got;
    status = card.ReadBlock(got, calls);
    read += status == SegmentStatus::kOk ? 1U : 0U;
  }
  BN_FUZZ_CHECK(status == SegmentStatus::kEnd || status == SegmentStatus::kDamaged,
                "going through the blocks ended in a status it should not");
  BN_FUZZ_CHECK(status != SegmentStatus::kEnd || read == reader.Blocks(),
                "the blocks ended with fewer read than the directory says there are");

  // Places looked for; and the block at one that was found, read.
  for (const std::uint32_t asked : {block, block % (reader.IndexedBlocks() + 1U), 0U}) {
    status = card.Steps(reader.BeginFindBlock(asked), kStepsToFindBlock);
    BN_FUZZ_CHECK(sound(status) && (status != SegmentStatus::kOk || placed()), "a block was found where none can be");
    BN_FUZZ_CHECK(status != SegmentStatus::kOk || reader.Place().found == (asked < reader.IndexedBlocks()),
                  "a block was found that is not indexed, or one that is was not");
    if (status == SegmentStatus::kOk && reader.Place().found) {
      BN_FUZZ_CHECK(reader.Seek(reader.Place()) == SegmentStatus::kOk, "a place that was found could not be gone to");
      Block got;
      BN_FUZZ_CHECK(card.ReadBlock(got, calls) != SegmentStatus::kMisused, "a block at a found place");
    }
  }
  for (const std::uint32_t asked : {place, place % (reader.Characters() + 1U), 0U}) {
    status = card.Steps(reader.BeginFindCharacters(asked), kStepsToFindBlock);
    BN_FUZZ_CHECK(sound(status) && (status != SegmentStatus::kOk || (reader.Place().found && placed())),
                  "a count of characters came to no place, or to one that cannot be");
    status = card.Steps(reader.BeginFindSource(asked), kStepsToFindSource);
    BN_FUZZ_CHECK(sound(status) && (status != SegmentStatus::kOk || placed()), "a place in the source");
  }
  status = card.Steps(reader.BeginFindAnchor(name), StepsToFindAnchor(reader));
  BN_FUZZ_CHECK(sound(status) && (status != SegmentStatus::kOk || placed()), "an id was found where none can be");
  for (std::size_t page = 0; page < reader.AnchorPages(); ++page) {
    status = card.Steps(reader.BeginReadAnchors(page), 2);
    BN_FUZZ_CHECK(sound(status), "an anchor page ended in a status it should not");
    SegmentAnchor anchor;
    std::size_t ids = 0;
    while (status == SegmentStatus::kOk && reader.NextAnchor(anchor)) {
      BN_FUZZ_CHECK(!anchor.name.empty() && anchor.name.size() <= bn::epub::kSegmentMostName &&
                        anchor.block <= reader.Blocks() && ++ids <= 4096 / 8,
                    "an id that is none: no name, or in a block there is not");
    }
  }
  BN_FUZZ_CHECK(reader.BeginReadAnchors(reader.AnchorPages()) == SegmentStatus::kMisused, "a page there is none of");
  for (std::size_t i = 0; i < reader.Pictures(); i += 1 + (reader.Pictures() / 4)) {
    SegmentPicture picture;
    BN_FUZZ_CHECK(reader.ReadPictureAt(i, picture) == SegmentStatus::kOk, "a picture's entry could not be read");
    bool found = false;
    const std::size_t mark = card.storage.reads.size();
    BN_FUZZ_CHECK(reader.ReadPicture(picture.number, picture, found) == SegmentStatus::kOk,
                  "a picture could not be looked for");
    card.HoldReads(mark);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  if ((choices & 1U) == 0) {
    WrittenAndRead(input, choices);
  } else {
    AnyBytes(input, choices);
  }
  return 0;
}
