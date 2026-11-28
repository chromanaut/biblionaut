#include "epub/segment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/bundle.h"
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
using bn::epub::ItemKind;
using bn::epub::LinkKind;
using bn::epub::RunStyle;
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
using Bytes = FakeStorage::Bytes;

const char* const kPath = "/book.bundle";

// Numbers out of a file's bytes, by the places the format gives them, and
// nothing of the code that is being tested.
std::uint32_t U16(const Bytes& bytes, std::size_t at) { return bytes.at(at) | (std::uint32_t{bytes.at(at + 1)} << 8U); }
std::uint32_t U32(const Bytes& bytes, std::size_t at) { return U16(bytes, at) | (U16(bytes, at + 2) << 16U); }
std::uint32_t Crc(const Bytes& bytes, std::size_t from, std::size_t to) {
  return bn::core::Crc32(std::span(bytes).subspan(from, to - from));
}
std::string Text(const Bytes& bytes, std::size_t from, std::size_t count) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(from + count)};
}

// Numbers that are the same every run.
class Random {
 public:
  explicit Random(std::uint32_t seed) : state_(seed) {}
  std::uint32_t Next() {
    state_ = (state_ * 1664525U) + 1013904223U;
    return state_ >> 8U;
  }
  std::uint32_t Below(std::uint32_t limit) { return Next() % limit; }
  bool OneIn(std::uint32_t n) { return Below(n) == 0; }

 private:
  std::uint32_t state_;
};

// Text of about so many bytes, in whole characters of one to four bytes.
std::string Words(Random& random, std::size_t bytes) {
  static const std::array<std::string_view, 12> kPieces = {"the ",
                                                           "quick ",
                                                           "brown ",
                                                           "fox ",
                                                           "caf\xC3\xA9 ",
                                                           "na\xC3\xAFve ",
                                                           "\xE2\x80\x94 ",
                                                           "\xE6\xBC\xA2\xE5\xAD\x97 ",
                                                           "\xF0\x9F\x98\x80 ",
                                                           "jumps ",
                                                           "\xCE\xB1\xCE\xB2\xCE\xB3 ",
                                                           "over "};
  std::string text;
  while (text.size() < bytes) {
    text += kPieces.at(random.Below(kPieces.size()));
  }
  return text;
}

// One thing given to a writer.
struct Op {
  enum class What : std::uint8_t { kBegin, kText, kStyle, kLinkStart, kLinkEnd, kPicture, kEnd, kAnchor };
  What what = What::kText;
  SegmentBlockKind kind = SegmentBlockKind::kParagraph;
  std::uint8_t level = 0;
  BlockStyle style;
  std::uint32_t source = 0;
  std::string text;  // The text; a link's fragment; a picture's alt; an id's name.
  RunStyle run;
  LinkKind link = LinkKind::kInside;
  std::uint16_t chapter = 0;
  std::uint16_t picture = 0;
  std::uint16_t bits = 0;
  StyleLength width;
  StyleLength height;
};

Op Begin(SegmentBlockKind kind, std::uint32_t source, const BlockStyle& style = {}, std::uint8_t level = 0) {
  Op op;
  op.what = Op::What::kBegin;
  op.kind = kind;
  op.level = kind == SegmentBlockKind::kHeading && level == 0 ? std::uint8_t{2} : level;
  op.style = style;
  op.source = source;
  return op;
}
Op TextOp(std::string text) {
  Op op;
  op.what = Op::What::kText;
  op.text = std::move(text);
  return op;
}
Op StyleOp(const RunStyle& run) {
  Op op;
  op.what = Op::What::kStyle;
  op.run = run;
  return op;
}
Op LinkOp(LinkKind kind, std::uint16_t chapter, std::string fragment) {
  Op op;
  op.what = Op::What::kLinkStart;
  op.link = kind;
  op.chapter = chapter;
  op.text = std::move(fragment);
  return op;
}
Op Simple(Op::What what) {
  Op op;
  op.what = what;
  return op;
}
Op PictureOp(std::uint16_t number, std::uint16_t bits, StyleLength width, StyleLength height, std::string alt) {
  Op op;
  op.what = Op::What::kPicture;
  op.picture = number;
  op.bits = bits;
  op.width = width;
  op.height = height;
  op.text = std::move(alt);
  return op;
}
Op AnchorOp(std::string name) {
  Op op;
  op.what = Op::What::kAnchor;
  op.text = std::move(name);
  return op;
}

// What a chapter is to hold: worked out from what was given to the
// writer, with none of the code that is being tested.
struct WantItem {
  ItemKind kind = ItemKind::kText;
  std::string text;
  RunStyle run;
  LinkKind link = LinkKind::kInside;
  std::uint16_t chapter = 0;
  std::uint16_t picture = 0;
  std::uint16_t bits = 0;
  StyleLength width;
  StyleLength height;

  bool operator==(const WantItem&) const = default;
};
struct WantBlock {
  std::uint8_t kind = 0;
  std::uint8_t level = 0;
  BlockStyle style;
  std::uint32_t source = 0;
  std::uint32_t charactersBefore = 0;
  std::uint32_t characters = 0;
  std::vector<WantItem> items;  // Text that was given piece after piece is one item here.

  bool operator==(const WantBlock&) const = default;
};
struct WantAnchor {
  std::string name;
  std::uint32_t block = 0;
  std::uint32_t into = 0;

  bool operator==(const WantAnchor&) const = default;
};
struct Want {
  std::vector<WantBlock> blocks;
  std::vector<WantAnchor> anchors;
  std::set<std::uint16_t> pictures;
  std::uint32_t characters = 0;
};

WantItem ItemOf(ItemKind kind, std::string text = {}) {
  WantItem item;
  item.kind = kind;
  item.text = std::move(text);
  return item;
}

// Whether text ends where a character does, and does not begin inside one.
bool IsWholeCharacters(std::string_view text) {
  if (text.empty() || (static_cast<std::uint8_t>(text.front()) & 0xC0U) == 0x80U) {
    return false;
  }
  std::size_t lead = text.size() - 1;
  while (lead > 0 && text.size() - lead < 4 && (static_cast<std::uint8_t>(text[lead]) & 0xC0U) == 0x80U) {
    --lead;
  }
  const auto first = static_cast<std::uint8_t>(text[lead]);
  const std::size_t length = first < 0x80 ? 1 : first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 0;
  return length == text.size() - lead;
}

Want Expected(const std::vector<Op>& ops) {
  Want want;
  bool inBlock = false;
  std::uint32_t lastSource = 0;
  for (const Op& op : ops) {
    switch (op.what) {
      case Op::What::kBegin: {
        WantBlock block;
        block.kind = static_cast<std::uint8_t>(op.kind);
        block.level = op.level;
        block.style = op.style;
        block.style.fromBook &= block.style.has;
        block.source = std::max(op.source, lastSource);
        lastSource = block.source;
        block.charactersBefore = want.characters;
        want.blocks.push_back(block);
        inBlock = true;
        break;
      }
      case Op::What::kText: {
        WantBlock& block = want.blocks.back();
        if (!block.items.empty() && block.items.back().kind == ItemKind::kText) {
          block.items.back().text += op.text;
        } else if (!op.text.empty()) {
          block.items.push_back(ItemOf(ItemKind::kText, op.text));
        }
        block.characters += static_cast<std::uint32_t>(op.text.size());
        break;
      }
      case Op::What::kStyle: {
        WantItem item = ItemOf(ItemKind::kStyle);
        item.run = op.run;
        want.blocks.back().items.push_back(item);
        break;
      }
      case Op::What::kLinkStart: {
        WantItem item = ItemOf(ItemKind::kLinkStart, op.text);
        item.link = op.link;
        item.chapter = op.chapter;
        want.blocks.back().items.push_back(item);
        break;
      }
      case Op::What::kLinkEnd:
        want.blocks.back().items.push_back(ItemOf(ItemKind::kLinkEnd));
        break;
      case Op::What::kPicture: {
        WantItem item = ItemOf(ItemKind::kPicture, op.text);
        item.picture = op.picture;
        item.bits = op.bits;
        item.width = op.width;
        item.height = op.height;
        want.blocks.back().items.push_back(item);
        ++want.blocks.back().characters;
        if (op.picture != bn::epub::kSegmentNoPicture) {
          want.pictures.insert(op.picture);
        }
        break;
      }
      case Op::What::kEnd: {
        WantBlock& block = want.blocks.back();
        block.characters = std::max<std::uint32_t>(block.characters, 1);
        want.characters += block.characters;
        inBlock = false;
        break;
      }
      case Op::What::kAnchor:
        if (!op.text.empty()) {
          want.anchors.push_back(
              {.name = op.text,
               .block = static_cast<std::uint32_t>(inBlock ? want.blocks.size() - 1 : want.blocks.size()),
               .into = inBlock ? want.blocks.back().characters : 0});
        }
        break;
    }
  }
  return want;
}

StyleLength Length(Random& random) {
  return {.em = static_cast<std::int16_t>(static_cast<int>(random.Below(400)) - 100),
          .px = static_cast<std::int16_t>(random.Below(300)),
          .percent = static_cast<std::int16_t>(random.Below(6400))};
}

// A style with some of the fields a block may have, each with a value of
// its own.
BlockStyle SomeStyle(Random& random) {
  BlockStyle style;
  if (random.OneIn(3)) {
    return style;
  }
  style.has = static_cast<std::uint16_t>(random.Next() & random.Next() & bn::epub::kBlockFields);
  style.fromBook = static_cast<std::uint16_t>(random.Next() & style.has);
  const auto has = [&style](std::uint16_t field) { return (style.has & field) != 0; };
  style.align = has(bn::epub::kBlockAlign) ? static_cast<std::uint8_t>(random.Below(6)) : std::uint8_t{0};
  style.direction = has(bn::epub::kBlockDirection) ? static_cast<std::uint8_t>(random.Below(2)) : std::uint8_t{0};
  style.size = has(bn::epub::kBlockSize) ? static_cast<std::uint16_t>(500 + random.Below(2000)) : std::uint16_t{1000};
  if (has(bn::epub::kBlockLineHeight)) {
    style.lineMultiple = static_cast<std::uint16_t>(random.Below(3) * 600);
    style.lineHeight = Length(random);
  }
  style.indent = has(bn::epub::kBlockIndent) ? Length(random) : StyleLength{};
  style.marginLeft = has(bn::epub::kBlockMarginLeft) ? Length(random) : StyleLength{};
  style.marginRight = has(bn::epub::kBlockMarginRight) ? Length(random) : StyleLength{};
  style.spaceBefore = has(bn::epub::kBlockSpaceBefore) ? Length(random) : StyleLength{};
  style.spaceAfter = has(bn::epub::kBlockSpaceAfter) ? Length(random) : StyleLength{};
  style.breaking = has(bn::epub::kBlockBreaking) ? static_cast<std::uint16_t>(random.Below(512)) : std::uint16_t{0};
  if (has(bn::epub::kBlockDepths)) {
    style.quoteDepth = static_cast<std::uint8_t>(random.Below(4));
    style.listDepth = static_cast<std::uint8_t>(random.Below(5));
  }
  if (has(bn::epub::kBlockMarker)) {
    style.marker = static_cast<std::uint8_t>(random.Below(10));
    style.markerNumber = random.Next();
  }
  if (has(bn::epub::kBlockTable)) {
    style.table = static_cast<std::uint16_t>(random.Below(9));
    style.row = static_cast<std::uint16_t>(random.Below(700));
    style.column = static_cast<std::uint16_t>(random.Below(30));
    style.rowSpan = static_cast<std::uint8_t>(1 + random.Below(3));
    style.columnSpan = static_cast<std::uint8_t>(1 + random.Below(4));
    style.tableBits = static_cast<std::uint8_t>(random.Below(2));
    style.tableDepth = static_cast<std::uint8_t>(1 + random.Below(3));
  }
  style.width = has(bn::epub::kBlockWidth) ? Length(random) : StyleLength{};
  style.height = has(bn::epub::kBlockHeight) ? Length(random) : StyleLength{};
  return style;
}

// Adds text to a block, given a piece at a time as a document's is.
void AddText(std::vector<Op>& ops, Random& random, std::size_t bytes) {
  const std::string text = Words(random, bytes);
  for (std::size_t at = 0; at < text.size();) {
    std::size_t end = std::min(text.size(), at + 1 + random.Below(1500));
    while (end < text.size() && (static_cast<std::uint8_t>(text[end]) & 0xC0U) == 0x80U) {
      ++end;
    }
    ops.push_back(TextOp(text.substr(at, end - at)));
    at = end;
  }
}

// A chapter of so many blocks, of every kind and with everything a block
// may have, the same for the same seed whatever builds it: each number is
// drawn in a statement of its own, since the order in which the arguments
// of one call are worked out is the compiler's to choose. `languages` is how many languages
// its style items may name. With `longText`, a block in every few has
// pages of text; without, none has more than a few sentences.
std::vector<Op> MakeChapter(std::uint32_t seed, std::size_t blocks, std::uint8_t languages = 0, bool longText = true) {
  Random random(seed);
  std::vector<Op> ops;
  std::uint32_t source = 40;
  std::uint32_t ids = 0;
  const auto anchor = [&ops, &ids, &random] {
    std::string name = "id-";
    name.append(std::to_string(ids++));
    if (random.OneIn(4)) {
      name.append(random.Below(200), 'x');
    }
    ops.push_back(AnchorOp(name));
  };
  for (std::size_t i = 0; i < blocks; ++i) {
    const std::uint32_t before = random.Below(8);
    for (std::uint32_t n = 6; n <= before; ++n) {
      anchor();  // One or two ids on something that makes no block.
    }
    const std::uint32_t pick = random.Below(20);
    const SegmentBlockKind kind = pick < 12   ? SegmentBlockKind::kParagraph
                                  : pick < 14 ? SegmentBlockKind::kHeading
                                  : pick < 16 ? SegmentBlockKind::kListItem
                                  : pick < 17 ? SegmentBlockKind::kPicture
                                  : pick < 18 ? SegmentBlockKind::kRule
                                              : SegmentBlockKind::kTableCell;
    source += random.Below(900);
    const BlockStyle blockStyle = SomeStyle(random);
    const std::uint8_t level =
        kind == SegmentBlockKind::kHeading ? static_cast<std::uint8_t>(1 + random.Below(6)) : std::uint8_t{0};
    ops.push_back(Begin(kind, source, blockStyle, level));
    if (kind == SegmentBlockKind::kPicture) {
      const auto number = static_cast<std::uint16_t>(random.Below(40));
      const StyleLength width = Length(random);
      const StyleLength height = Length(random);
      const std::uint32_t altBytes = random.OneIn(2) ? random.Below(200) : 0;
      ops.push_back(PictureOp(number, 0, width, height, altBytes == 0 ? std::string() : Words(random, altBytes)));
    } else if (kind != SegmentBlockKind::kRule) {
      const std::uint32_t runs = 1 + random.Below(5);
      for (std::uint32_t run = 0; run < runs; ++run) {
        if (random.OneIn(2)) {
          RunStyle style;
          style.bits = static_cast<std::uint16_t>(random.Below(128));
          style.fromBook = static_cast<std::uint16_t>(random.Below(128));
          style.familyClass = static_cast<std::uint8_t>(random.Below(6));
          style.familyFromBook = random.OneIn(2);
          style.language = languages == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(random.Below(languages + 1U));
          style.size = static_cast<std::uint16_t>(600 + random.Below(900));
          ops.push_back(StyleOp(style));
        }
        const bool link = random.OneIn(5);
        if (link) {
          const auto linkKind = static_cast<LinkKind>(random.Below(3));
          const auto chapter = static_cast<std::uint16_t>(random.Below(60));
          const bool named = !random.OneIn(3);
          ops.push_back(LinkOp(linkKind, chapter, named ? "to-" + std::to_string(random.Below(900)) : std::string()));
        }
        // Mostly a sentence or two; now and then pages of it.
        const std::uint32_t size = longText ? random.Below(30) : 9;
        AddText(ops, random,
                size == 0  ? 9000 + random.Below(9000)
                : size < 3 ? 1500 + random.Below(3000)
                           : random.Below(300));
        if (random.OneIn(6)) {
          anchor();
        }
        if (random.OneIn(9)) {
          const std::uint16_t number =
              random.OneIn(8) ? bn::epub::kSegmentNoPicture : static_cast<std::uint16_t>(random.Below(40));
          const StyleLength width = Length(random);
          const StyleLength height = Length(random);
          ops.push_back(PictureOp(number, bn::epub::kPictureInLine, width, height, "a picture"));
        }
        if (link) {
          ops.push_back(Simple(Op::What::kLinkEnd));
        }
      }
    }
    ops.push_back(Simple(Op::What::kEnd));
  }
  if (random.OneIn(2)) {
    anchor();  // An id after the last block.
  }
  return ops;
}

// A chapter of so many blocks of a few words each, with ids of long names
// among them: every kind of record in few bytes.
std::vector<Op> MakeSmallChapter(std::uint32_t seed, std::size_t blocks) {
  Random random(seed);
  std::vector<Op> ops;
  for (std::size_t i = 0; i < blocks; ++i) {
    const SegmentBlockKind kind = random.OneIn(9) ? SegmentBlockKind::kListItem : SegmentBlockKind::kParagraph;
    const BlockStyle blockStyle = random.OneIn(4) ? SomeStyle(random) : BlockStyle{};
    ops.push_back(Begin(kind, static_cast<std::uint32_t>(i * 30), blockStyle));
    ops.push_back(TextOp(Words(random, 1 + random.Below(12))));
    if (random.OneIn(6)) {
      RunStyle run;
      run.bits = static_cast<std::uint16_t>(random.Below(128));
      run.familyClass = static_cast<std::uint8_t>(random.Below(6));
      run.language = static_cast<std::uint8_t>(random.Below(3));
      ops.push_back(StyleOp(run));
      ops.push_back(LinkOp(LinkKind::kInside, 3, "f"));
      ops.push_back(TextOp(Words(random, 1 + random.Below(12))));
      ops.push_back(Simple(Op::What::kLinkEnd));
    }
    if (random.OneIn(5)) {
      std::string name = "id-";
      name.append(std::to_string(i)).append(100, 'q');
      ops.push_back(AnchorOp(name));
    }
    if (random.OneIn(12)) {
      const auto number = static_cast<std::uint16_t>(random.Below(9));
      const StyleLength width = Length(random);
      const StyleLength height = Length(random);
      ops.push_back(PictureOp(number, bn::epub::kPictureInLine, width, height, "alt"));
    }
    ops.push_back(Simple(Op::What::kEnd));
  }
  return ops;
}

// A segment as its bytes say it is, read here by the places the format
// gives things and with none of the code that is being tested.
struct Walked {
  std::vector<WantBlock> blocks;
  std::vector<std::uint32_t> blockOffsets;
  std::vector<std::size_t> textItems;  // How long each text item is, in the order they come.
  std::vector<WantAnchor> anchors;
  std::vector<std::array<std::uint32_t, 5>> index;     // Offset, characters, source, kind, breaking.
  std::vector<std::array<std::uint32_t, 4>> pictures;  // Number, width, height, sized.
  std::vector<std::string> languages;
  std::uint32_t blocksSaid = 0;
  std::uint32_t charactersSaid = 0;
  std::uint32_t sourceBytes = 0;
  std::size_t indexPages = 0;
  std::size_t anchorPages = 0;
  std::vector<std::string> wrong;  // Everything that is not as the format has it.
};

StyleLength LengthIn(const Bytes& bytes, std::size_t at) {
  return {.em = static_cast<std::int16_t>(U16(bytes, at)),
          .px = static_cast<std::int16_t>(U16(bytes, at + 2)),
          .percent = static_cast<std::int16_t>(U16(bytes, at + 4))};
}

Walked Walk(const Bytes& file, const BundleChapter& record) {
  Walked walked;
  const auto wrong = [&walked](const std::string& what) { walked.wrong.push_back(what); };
  const Bytes bytes(file.begin() + record.segmentOffset, file.begin() + record.segmentOffset + record.segmentBytes);
  const std::size_t directory = record.directoryOffset;
  if (directory + record.directoryBytes != bytes.size()) {
    wrong("the directory does not end where the segment does");
  }
  if (Crc(bytes, directory, bytes.size()) != record.directoryChecksum) {
    wrong("the directory's checksum");
  }
  walked.indexPages = U16(bytes, directory);
  walked.anchorPages = U16(bytes, directory + 2);
  const std::size_t pictures = U16(bytes, directory + 4);
  const std::size_t languages = U16(bytes, directory + 6);
  walked.blocksSaid = U32(bytes, directory + 8);
  walked.charactersSaid = U32(bytes, directory + 12);
  walked.sourceBytes = U32(bytes, directory + 16);
  std::size_t at = directory + 20;
  std::set<std::size_t> pageOffsets;  // Where the directory says pages are.
  const std::size_t indexed = std::min<std::size_t>(walked.blocksSaid, 65536);
  for (std::size_t page = 0; page < walked.indexPages; ++page, at += 12) {
    const std::size_t where = U32(bytes, at);
    const std::size_t records = std::min<std::size_t>(256, indexed - (page * 256));
    pageOffsets.insert(where);
    if (Crc(bytes, where, where + (records * 16)) != U32(bytes, at + 8)) {
      wrong("an index page's checksum");
    }
    for (std::size_t i = 0; i < records; ++i) {
      const std::size_t entry = where + (i * 16);
      walked.index.push_back({U32(bytes, entry), U32(bytes, entry + 4), U32(bytes, entry + 8), bytes.at(entry + 12),
                              bytes.at(entry + 13)});
      if (U16(bytes, entry + 14) != 0) {
        wrong("an index record's spare bytes");
      }
    }
    if (U32(bytes, at + 4) != walked.index.at(page * 256).at(1)) {
      wrong("the characters before an index page's first block");
    }
  }
  for (std::size_t page = 0; page < walked.anchorPages; ++page, at += 12) {
    const std::size_t where = U32(bytes, at);
    const std::size_t length = U16(bytes, at + 4);
    pageOffsets.insert(where);
    if (Crc(bytes, where, where + length) != U32(bytes, at + 8)) {
      wrong("an anchor page's checksum");
    }
    std::size_t ids = 0;
    for (std::size_t in = where; in < where + length; ++ids) {
      const std::size_t name = bytes.at(in);
      walked.anchors.push_back(
          {.name = Text(bytes, in + 1, name), .block = U16(bytes, in + 1 + name), .into = U32(bytes, in + 3 + name)});
      in += name + 7;
      if (in > where + length) {
        wrong("an id runs out of its page");
      }
    }
    if (ids != U16(bytes, at + 6) || length > 4096 || length == 0) {
      wrong("an anchor page's count or length");
    }
  }
  for (std::size_t i = 0; i < pictures; ++i, at += 8) {
    walked.pictures.push_back({U16(bytes, at), U16(bytes, at + 2), U16(bytes, at + 4), bytes.at(at + 6)});
    if (bytes.at(at + 7) != 0) {
      wrong("a picture's spare byte");
    }
  }
  for (std::size_t i = 0; i < languages; ++i) {
    walked.languages.push_back(Text(bytes, at + 1, bytes.at(at)));
    at += 1 + bytes.at(at);
  }
  if (at != bytes.size()) {
    wrong("the directory is not as long as what it holds");
  }

  // The records, from the first to the directory.
  std::size_t pagesMet = 0;
  bool inBlock = false;
  std::uint32_t chapterCharacters = 0;
  for (at = 0; at < directory;) {
    const std::uint32_t kind = bytes.at(at);
    const std::size_t length = U16(bytes, at + 2);
    if (kind == 0xF0 || kind == 0xF1) {
      if (!pageOffsets.contains(at + 4) || bytes.at(at + 1) != 0) {
        wrong("a page is not where the directory says one is");
      }
      ++pagesMet;
      at += 4 + length;
      continue;
    }
    if (!inBlock) {
      WantBlock block;
      block.kind = static_cast<std::uint8_t>(kind);
      block.level = bytes.at(at + 1);
      block.charactersBefore = chapterCharacters;
      BlockStyle& style = block.style;
      std::size_t in = at + 4;
      if (length != 0) {
        style.has = static_cast<std::uint16_t>(U16(bytes, in));
        style.fromBook = static_cast<std::uint16_t>(U16(bytes, in + 2));
        in += 4;
        const auto has = [&style](std::uint16_t field) { return (style.has & field) != 0; };
        const auto length6 = [&bytes, &in] {
          in += 6;
          return LengthIn(bytes, in - 6);
        };
        if (has(1U << 0U)) {
          style.align = bytes.at(in++);
        }
        if (has(1U << 1U)) {
          style.direction = bytes.at(in++);
        }
        if (has(1U << 2U)) {
          style.size = static_cast<std::uint16_t>(U16(bytes, in));
          in += 2;
        }
        if (has(1U << 3U)) {
          style.lineMultiple = static_cast<std::uint16_t>(U16(bytes, in));
          in += 2;
          style.lineHeight = length6();
        }
        style.indent = has(1U << 4U) ? length6() : StyleLength{};
        style.marginLeft = has(1U << 5U) ? length6() : StyleLength{};
        style.marginRight = has(1U << 6U) ? length6() : StyleLength{};
        style.spaceBefore = has(1U << 7U) ? length6() : StyleLength{};
        style.spaceAfter = has(1U << 8U) ? length6() : StyleLength{};
        if (has(1U << 9U)) {
          style.breaking = static_cast<std::uint16_t>(U16(bytes, in));
          in += 2;
        }
        if (has(1U << 10U)) {
          style.quoteDepth = bytes.at(in++);
          style.listDepth = bytes.at(in++);
        }
        if (has(1U << 11U)) {
          style.marker = bytes.at(in);
          style.markerNumber = U32(bytes, in + 1);
          in += 5;
        }
        if (has(1U << 12U)) {
          style.table = static_cast<std::uint16_t>(U16(bytes, in));
          style.row = static_cast<std::uint16_t>(U16(bytes, in + 2));
          style.column = static_cast<std::uint16_t>(U16(bytes, in + 4));
          style.rowSpan = bytes.at(in + 6);
          style.columnSpan = bytes.at(in + 7);
          style.tableBits = bytes.at(in + 8);
          style.tableDepth = bytes.at(in + 9);
          in += 10;
        }
        style.width = has(1U << 13U) ? length6() : StyleLength{};
        style.height = has(1U << 14U) ? length6() : StyleLength{};
      }
      if (in != at + 4 + length) {
        wrong("a style record is not as long as its fields");
      }
      walked.blocks.push_back(block);
      walked.blockOffsets.push_back(static_cast<std::uint32_t>(at));
      inBlock = true;
      at += 4 + length;
      continue;
    }
    WantBlock& block = walked.blocks.back();
    const std::size_t in = at + 4;
    if (length > 4088) {
      wrong("an item is longer than an item may be");
    }
    switch (kind) {
      case 1:
        walked.textItems.push_back(length);
        if (!IsWholeCharacters(Text(bytes, in, length))) {
          wrong("a text item is empty, or begins or ends part way through a character");
        }
        if (!block.items.empty() && block.items.back().kind == ItemKind::kText) {
          block.items.back().text += Text(bytes, in, length);
        } else {
          block.items.push_back(ItemOf(ItemKind::kText, Text(bytes, in, length)));
        }
        block.characters += static_cast<std::uint32_t>(length);
        break;
      case 2: {
        RunStyle run;
        run.bits = static_cast<std::uint16_t>(U16(bytes, in));
        run.fromBook = static_cast<std::uint16_t>(U16(bytes, in + 2));
        run.familyClass = bytes.at(in + 4) & 7U;
        run.familyFromBook = (bytes.at(in + 4) & 8U) != 0;
        run.language = bytes.at(in + 5);
        run.size = static_cast<std::uint16_t>(U16(bytes, in + 6));
        WantItem item = ItemOf(ItemKind::kStyle);
        item.run = run;
        block.items.push_back(item);
        if (length != 8 || (bytes.at(in + 4) & 0xF0U) != 0) {
          wrong("a style item");
        }
        break;
      }
      case 3: {
        WantItem item = ItemOf(ItemKind::kLinkStart, Text(bytes, in + 3, bytes.at(in + 2)));
        item.link = static_cast<LinkKind>(bytes.at(at + 1));
        item.chapter = static_cast<std::uint16_t>(U16(bytes, in));
        block.items.push_back(item);
        if (length != 3U + bytes.at(in + 2)) {
          wrong("a link's length");
        }
        break;
      }
      case 4:
        block.items.push_back(ItemOf(ItemKind::kLinkEnd));
        if (length != 0) {
          wrong("a link's end has bytes");
        }
        break;
      case 5: {
        WantItem item = ItemOf(ItemKind::kPicture, Text(bytes, in + 16, length - 16));
        item.picture = static_cast<std::uint16_t>(U16(bytes, in));
        item.bits = static_cast<std::uint16_t>(U16(bytes, in + 2));
        item.width = LengthIn(bytes, in + 4);
        item.height = LengthIn(bytes, in + 10);
        block.items.push_back(item);
        ++block.characters;
        break;
      }
      case 6: {
        block.characters = std::max<std::uint32_t>(block.characters, 1);
        if (length != 8 || U32(bytes, in) != block.characters) {
          wrong("a block's end does not say how many characters the block is");
        }
        // The checksum is of the block's own bytes, the pages that lie
        // among them left out: so of it as the walk has met it.
        chapterCharacters += block.characters;
        inBlock = false;
        break;
      }
      default:
        wrong("an item of a kind there is none of");
        break;
    }
    at += 4 + length;
  }
  if (inBlock || at != directory) {
    wrong("the records do not end at the directory, between two blocks");
  }
  if (pagesMet != walked.indexPages + walked.anchorPages) {
    wrong("not as many pages among the records as the directory has");
  }
  // The index says where each block was met, and what was before it.
  for (std::size_t i = 0; i < walked.index.size() && i < walked.blocks.size(); ++i) {
    if (walked.index.at(i).at(0) != walked.blockOffsets.at(i) ||
        walked.index.at(i).at(1) != walked.blocks.at(i).charactersBefore ||
        walked.index.at(i).at(3) != walked.blocks.at(i).kind) {
      wrong("the index's record of block " + std::to_string(i));
    }
    walked.blocks.at(i).source = walked.index.at(i).at(2);
  }
  return walked;
}

// A block's checksum, worked out here: of its bytes from its head to the
// characters in its end, the pages that lie among its items left out.
bool BlockChecksumsHold(const Bytes& file, const BundleChapter& record) {
  const Bytes bytes(file.begin() + record.segmentOffset, file.begin() + record.segmentOffset + record.segmentBytes);
  bool inBlock = false;
  std::uint32_t checksum = 0;
  for (std::size_t at = 0; at < record.directoryOffset;) {
    const std::uint32_t kind = bytes.at(at);
    const std::size_t length = U16(bytes, at + 2);
    if (kind == 0xF0 || kind == 0xF1) {
      at += 4 + length;
      continue;
    }
    if (inBlock && kind == 6) {
      checksum = bn::core::Crc32(std::span(bytes).subspan(at, 8), checksum);
      if (checksum != U32(bytes, at + 8)) {
        return false;
      }
      inBlock = false;
      checksum = 0;
    } else {
      checksum = bn::core::Crc32(std::span(bytes).subspan(at, 4 + length), checksum);
      inBlock = true;
    }
    at += 4 + length;
  }
  return !inBlock;
}

// How many blocks, index pages and anchor pages a segment's records are,
// counted here from the four bytes each begins with.
std::array<std::size_t, 3> CountRecords(const Bytes& file, const BundleChapter& record) {
  std::array<std::size_t, 3> counts{};
  bool inBlock = false;
  for (std::size_t at = record.segmentOffset; at < std::size_t{record.segmentOffset} + record.directoryOffset;) {
    const std::uint32_t kind = file[at];
    if (kind == 0xF0 || kind == 0xF1) {
      ++counts.at(kind == 0xF0 ? 1 : 2);
    } else if (!inBlock) {
      ++counts[0];
      inBlock = true;
    } else if (kind == 6) {
      inBlock = false;
    }
    at += 4 + (file[at + 2] | (std::size_t{file[at + 3]} << 8U));
  }
  return counts;
}

struct SegmentFixture {
  SegmentFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Makes a bundle of a book of so many documents, and opens it to be
  // changed.
  void MakeBundle(std::uint16_t documents) {
    BundleMaker maker(page);
    ASSERT_EQ(file.Open(storage, kPath, OpenMode::kCreate), StorageStatus::kOk);
    ASSERT_EQ(maker.Begin(file), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
    for (std::uint32_t n = 0; n < documents; ++n) {
      ASSERT_EQ(maker.WriteFile(BundleFile{}), BundleStatus::kOk);
    }
    ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
    BundleBook book;
    book.documents = documents;
    ASSERT_EQ(maker.Finish(book), BundleStatus::kOk);
    while (maker.Step()) {
    }
    ASSERT_EQ(maker.Status(), BundleStatus::kOk);
    file.Close();
    ASSERT_EQ(OpenBundle(), BundleStatus::kOk);
  }

  BundleStatus OpenBundle() {
    bundle.Close();
    if (file.Open(storage, kPath, OpenMode::kUpdate) != StorageStatus::kOk) {
      return BundleStatus::kStoreFailed;
    }
    return bundle.Open(file);
  }

  // Gives one thing to the writer, and holds the call to what a call may
  // write: two pages, each whole and where the last one ended.
  SegmentStatus Give(const Op& op) {
    const std::size_t before = storage.written.size();
    SegmentStatus status = SegmentStatus::kOk;
    switch (op.what) {
      case Op::What::kBegin:
        status = writer.BeginBlock(op.kind, op.level, op.style, op.source);
        break;
      case Op::What::kText:
        status = writer.Text(op.text);
        break;
      case Op::What::kStyle:
        status = writer.Style(op.run);
        break;
      case Op::What::kLinkStart:
        status = writer.LinkStart(op.link, op.chapter, op.text);
        break;
      case Op::What::kLinkEnd:
        status = writer.LinkEnd();
        break;
      case Op::What::kPicture:
        status = writer.Picture(op.picture, op.bits, op.width, op.height, op.text);
        break;
      case Op::What::kEnd:
        status = writer.EndBlock();
        break;
      case Op::What::kAnchor:
        status = writer.Anchor(op.text);
        break;
    }
    CheckWrites(before, 2);
    return status;
  }

  void CheckWrites(std::size_t before, std::size_t most) {
    EXPECT_LE(storage.written.size(), before + most) << "a call wrote more pages than it may";
    mostWrites = std::max(mostWrites, storage.written.size() - before);
    for (std::size_t i = before; i < storage.written.size(); ++i) {
      EXPECT_EQ(storage.written[i].offset, segmentBegin + (pagesWritten * 4096)) << "a page out of its turn";
      EXPECT_TRUE(storage.written[i].size == 4096 || lastPage) << "a write that is not a whole page";
      ++pagesWritten;
    }
  }

  // Writes a chapter's segment where the segments end, to its last byte.
  // `sizes` gives the size of each picture that has one.
  SegmentStatus Write(const std::vector<Op>& ops, std::uint32_t sourceBytes = 0,
                      const std::map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>>& sizes = {},
                      ChapterState state = ChapterState::kWhole) {
    segmentBegin = bundle.DataEnd();
    pagesWritten = 0;
    lastPage = false;
    SegmentStatus status = writer.Begin(file, bundle.DataEnd());
    for (std::size_t i = 0; i < ops.size() && status == SegmentStatus::kOk; ++i) {
      status = Give(ops[i]);
    }
    if (status != SegmentStatus::kOk) {
      return status;
    }
    std::size_t before = storage.written.size();
    status = writer.BeginEndBlocks();
    CheckWrites(before, 2);
    const auto steps = [this] {
      for (bool more = true; more;) {
        const std::size_t mark = storage.written.size();
        more = writer.Step();
        CheckWrites(mark, 1);
      }
      return writer.Status();
    };
    status = status == SegmentStatus::kOk ? steps() : status;
    for (std::size_t i = 0; i < writer.Pictures() && status == SegmentStatus::kOk; ++i) {
      if (const auto found = sizes.find(writer.PictureNumber(i)); found != sizes.end()) {
        status = writer.SetPictureSize(i, found->second.first, found->second.second);
      }
    }
    // Beginning the directory is a call like any other: its first bytes
    // may be the ones that fill a page.
    before = storage.written.size();
    status = status == SegmentStatus::kOk ? writer.BeginFinish(state, 0, sourceBytes) : status;
    CheckWrites(before, 2);
    lastPage = true;
    return status == SegmentStatus::kOk ? steps() : status;
  }

  // Writes a chapter and commits it as the chapter of that number.
  void WriteAndCommit(std::size_t chapter, const std::vector<Op>& ops, std::uint32_t sourceBytes = 0) {
    ASSERT_EQ(Write(ops, sourceBytes), SegmentStatus::kOk);
    ASSERT_EQ(bundle.Commit(chapter, writer.Chapter()), BundleStatus::kOk);
  }

  // Opens the chapter of that number to be read.
  SegmentStatus OpenChapter(std::size_t chapter) {
    BundleChapter record;
    if (bundle.ReadChapter(chapter, record) != BundleStatus::kOk) {
      return SegmentStatus::kStoreFailed;
    }
    return reader.Open(file, record);
  }

  // Reads the next block with the reader, to its end: how the reading
  // ended, and what the block was. Every call is held to what it may ask
  // of the file.
  SegmentStatus ReadWholeBlock(WantBlock& block) {
    block = WantBlock{};
    SegmentBlock head;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      const std::size_t before = storage.reads.size();
      status = reader.ReadBlock(head);
      CheckReads(before);
    }
    if (status != SegmentStatus::kOk) {
      return status;
    }
    block.kind = head.kind;
    block.level = head.level;
    block.style = head.style;
    block.charactersBefore = head.charactersBefore;
    lastBlockIndex = head.index;
    lastBlockOffset = head.offset;
    for (;;) {
      SegmentItem item;
      const std::size_t before = storage.reads.size();
      status = reader.NextItem(item);
      CheckReads(before);
      if (status == SegmentStatus::kMore) {
        continue;
      }
      if (status != SegmentStatus::kOk) {
        return status;
      }
      if (item.kind == ItemKind::kEnd) {
        block.characters = item.characters;
        return SegmentStatus::kOk;
      }
      if (item.kind == ItemKind::kText && !block.items.empty() && block.items.back().kind == ItemKind::kText) {
        block.items.back().text += item.text;
        continue;
      }
      block.items.push_back({.kind = item.kind,
                             .text = std::string(item.text),
                             .run = item.style,
                             .link = item.link,
                             .chapter = item.chapter,
                             .picture = item.picture,
                             .bits = item.pictureBits,
                             .width = item.width,
                             .height = item.height});
    }
  }

  void CheckReads(std::size_t before) {
    std::size_t bytes = 0;
    for (std::size_t i = before; i < storage.reads.size(); ++i) {
      bytes += storage.reads[i].size;
    }
    EXPECT_LE(storage.reads.size() - before, 2U) << "more than two reads of storage in one call";
    EXPECT_LE(bytes, 8192U);
  }

  // Does a piece of the reader's work a step at a time, each held to what
  // a step may ask of the file. Returns how many steps it took, the last
  // of them the one that said there was no more.
  int Steps() {
    int steps = 0;
    for (bool more = true; more; ++steps) {
      const std::size_t before = storage.reads.size();
      more = reader.Step();
      CheckReads(before);
    }
    return steps;
  }

  const Bytes& OnCard() const { return storage.Contents(kPath); }

  FakeStorage storage;
  alignas(4) std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  alignas(4) std::array<std::uint8_t, 4096> readerPage{};
  std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(bn::epub::kSegmentWriterBytes, 0xEE);
  BlockFile file{fileBuffer};
  Bundle bundle{page};
  SegmentWriter writer{memory};
  SegmentReader reader{readerPage};
  std::uint32_t segmentBegin = 0;
  std::size_t pagesWritten = 0;
  bool lastPage = false;
  std::size_t mostWrites = 0;
  std::uint32_t lastBlockIndex = 0;
  std::uint32_t lastBlockOffset = 0;
};

// What the reader gives of a block, with what the index said of it, is
// what was written.
void ExpectSameBlock(const WantBlock& got, const WantBlock& want, std::size_t number) {
  EXPECT_EQ(got.kind, want.kind) << "block " << number;
  EXPECT_EQ(got.level, want.level) << "block " << number;
  EXPECT_TRUE(got.style == want.style) << "the style of block " << number;
  EXPECT_EQ(got.charactersBefore, want.charactersBefore) << "block " << number;
  EXPECT_EQ(got.characters, want.characters) << "block " << number;
  ASSERT_EQ(got.items.size(), want.items.size()) << "block " << number;
  for (std::size_t i = 0; i < got.items.size(); ++i) {
    EXPECT_TRUE(got.items[i] == want.items[i]) << "item " << i << " of block " << number;
  }
}

// A number of two bytes of a style record.
std::uint32_t Pair(std::span<const std::uint8_t> bytes, std::size_t at) {
  return bytes[at] | (std::uint32_t{bytes[at + 1]} << 8U);
}

TEST(BlockStyle, IsWrittenAsOnlyTheFieldsItHasAndReadBackTheSame) {
  // Nothing said is nothing written.
  std::array<std::uint8_t, bn::epub::kBlockStyleMostBytes> bytes{};
  EXPECT_EQ(bn::epub::StoreBlockStyle(BlockStyle{}, bytes), 0U);
  BlockStyle read;
  read.has = 5;
  ASSERT_TRUE(bn::epub::LoadBlockStyle({}, read));
  EXPECT_TRUE(read == BlockStyle{});

  // Each field alone is as long as the format says, after the two masks.
  const std::array<std::size_t, 15> lengths = {1, 1, 2, 8, 6, 6, 6, 6, 6, 2, 2, 5, 10, 6, 6};
  Random random(7);
  for (std::size_t field = 0; field < lengths.size(); ++field) {
    BlockStyle style = SomeStyle(random);
    style.has = static_cast<std::uint16_t>(1U << field);
    style.fromBook = 0xFFFF;  // And a field that is not there cannot be the book's.
    const std::size_t length = bn::epub::StoreBlockStyle(style, bytes);
    EXPECT_EQ(length, 4 + lengths.at(field)) << field;
    EXPECT_EQ(Pair(bytes, 0), 1U << field);
    EXPECT_EQ(Pair(bytes, 2), 1U << field);
    ASSERT_TRUE(bn::epub::LoadBlockStyle(std::span(bytes).first(length), read)) << field;
    EXPECT_EQ(read.has, style.has);
    // Cut short or with a byte over, it is no record.
    EXPECT_FALSE(bn::epub::LoadBlockStyle(std::span(bytes).first(length - 1), read)) << field;
    EXPECT_FALSE(bn::epub::LoadBlockStyle(std::span(bytes).first(length + 1), read)) << field;
  }

  // Every field at once is the longest a record is, and comes back as it
  // was; and so do a few hundred mixtures.
  BlockStyle all = SomeStyle(random);
  all.has = bn::epub::kBlockFields;
  all.fromBook = 0x5A5A;
  all.align = 5;
  all.direction = 1;
  all.size = 1234;
  all.lineMultiple = 1500;
  all.lineHeight = {.em = -64, .px = 80, .percent = 640};
  all.indent = {.em = -96, .px = 0, .percent = 0};
  all.marginLeft = {.em = 128, .px = 16, .percent = 320};
  all.marginRight = {.em = 1, .px = 2, .percent = 3};
  all.spaceBefore = {.em = 32, .px = 0, .percent = 0};
  all.spaceAfter = {.em = 0, .px = 96, .percent = 0};
  all.breaking = 0x01FF;
  all.quoteDepth = 2;
  all.listDepth = 3;
  all.marker = 4;
  all.markerNumber = 0x12345678;
  all.table = 2;
  all.row = 300;
  all.column = 4;
  all.rowSpan = 2;
  all.columnSpan = 3;
  all.tableBits = 1;
  all.tableDepth = 1;
  all.width = {.em = 0, .px = 2400, .percent = 0};
  all.height = {.em = 640, .px = 0, .percent = 0};
  ASSERT_EQ(bn::epub::StoreBlockStyle(all, bytes), bn::epub::kBlockStyleMostBytes);
  ASSERT_TRUE(bn::epub::LoadBlockStyle(bytes, read));
  EXPECT_TRUE(read == all);
  // The fields are in the order the format gives: the alignment first,
  // the height last.
  EXPECT_EQ(bytes[4], 5);
  EXPECT_EQ(bytes[5], 1);
  EXPECT_EQ(Pair(bytes, 6), 1234U);
  EXPECT_EQ(Pair(bytes, 8), 1500U);
  EXPECT_EQ(Pair(bytes, 71), 640U) << "the height's first amount";
  for (int i = 0; i < 400; ++i) {
    BlockStyle style = SomeStyle(random);
    const std::size_t length = bn::epub::StoreBlockStyle(style, bytes);
    ASSERT_TRUE(bn::epub::LoadBlockStyle(std::span(bytes).first(length), read)) << i;
    style.fromBook &= style.has;
    EXPECT_TRUE(read == style) << i;
  }

  // What is no record: a field there is none of, a field marked the
  // book's that is not there, an alignment or a direction there is none
  // of, and masks with no field.
  ASSERT_EQ(bn::epub::StoreBlockStyle(all, bytes), bn::epub::kBlockStyleMostBytes);
  const auto spoiled = [&bytes, &read](std::size_t at, std::uint8_t value, std::size_t length) {
    std::array<std::uint8_t, bn::epub::kBlockStyleMostBytes + 4> copy{};
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    copy.at(at) = value;
    return !bn::epub::LoadBlockStyle(std::span(copy).first(length), read);
  };
  EXPECT_TRUE(spoiled(1, 0xFF, bn::epub::kBlockStyleMostBytes)) << "a field there is none of";
  EXPECT_TRUE(spoiled(4, 6, bn::epub::kBlockStyleMostBytes)) << "an alignment there is none of";
  EXPECT_TRUE(spoiled(5, 2, bn::epub::kBlockStyleMostBytes)) << "a direction there is none of";
  BlockStyle one;
  one.has = bn::epub::kBlockSize;
  ASSERT_EQ(bn::epub::StoreBlockStyle(one, bytes), 6U);
  EXPECT_TRUE(spoiled(2, 5, 6)) << "the book's, and not there";
  EXPECT_TRUE(spoiled(0, 0, 4)) << "masks with no field";
  EXPECT_FALSE(spoiled(4, 9, 6)) << "(a size is any number)";
}

TEST(Segment, IsLaidOutAsTheFormatSays) {
  // A chapter small enough to set its bytes down here one by one.
  SegmentFixture f;
  f.MakeBundle(3);
  BlockStyle style;
  style.has = bn::epub::kBlockAlign | bn::epub::kBlockBreaking;
  style.fromBook = bn::epub::kBlockAlign;
  style.align = 3;
  style.breaking = bn::epub::kBreakKeepWithNext | 0x0100U;
  RunStyle run;
  run.bits = bn::epub::kTextItalic;
  run.fromBook = bn::epub::kTextItalic;
  run.familyClass = 2;
  run.familyFromBook = true;
  run.size = 900;
  const std::vector<Op> ops = {
      AnchorOp("top"),
      Begin(SegmentBlockKind::kHeading, 100, style, 2),
      TextOp("On"),
      TextOp("e"),
      Simple(Op::What::kEnd),
      Begin(SegmentBlockKind::kParagraph, 180),
      TextOp("ab"),
      StyleOp(run),
      LinkOp(LinkKind::kNote, 7, "n1"),
      TextOp("c"),
      AnchorOp("mid"),
      Simple(Op::What::kLinkEnd),
      PictureOp(9, bn::epub::kPictureInLine, {.em = 64, .px = 0, .percent = 0}, {.em = 0, .px = 80, .percent = 0}, "x"),
      Simple(Op::What::kEnd),
      Begin(SegmentBlockKind::kRule, 260),
      Simple(Op::What::kEnd),
  };
  const std::uint32_t begin = f.bundle.DataEnd();
  ASSERT_EQ(f.Write(ops, 4321, {{9, {640, 480}}}), SegmentStatus::kOk);
  const BundleChapter record = f.writer.Chapter();
  const Bytes& card = f.OnCard();
  const Bytes bytes(card.begin() + begin, card.end());
  const Bytes blocks = {
      // The heading: its head, its style record, its text in one item, its end.
      2, 2, 7, 0, /**/ 0x01, 0x02, 0x01, 0x00, 3, 0x02, 0x01,  //
      1, 0, 3, 0, 'O',       'n',  'e',                        //
      6, 0, 8, 0, 3,         0,    0,    0,
  };
  ASSERT_GE(bytes.size(), blocks.size() + 4);
  EXPECT_EQ(Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(blocks.size())), blocks);
  std::size_t at = blocks.size();
  EXPECT_EQ(U32(bytes, at), Crc(bytes, 0, at)) << "the heading's checksum, of all of it before";
  at += 4;
  const std::size_t second = at;
  const Bytes paragraph = {
      1, 0, 0,  0,                                               // No style record.
      1, 0, 2,  0, 'a',  'b',                                    //
      2, 0, 8,  0, 0x02, 0x00, 0x02, 0x00, 0x0A, 0, 0x84, 0x03,  // Italic, the book's; sans, the book's; 900.
      3, 1, 5,  0, 7,    0,    2,    'n',  '1',                  // A note's link, to chapter 7.
      1, 0, 1,  0, 'c',                                          //
      4, 0, 0,  0,                                               //
      5, 0, 17, 0, 9,    0,    1,    0,    64,   0, 0,    0,    0, 0, 0, 0, 80, 0, 0, 0, 'x',  //
      6, 0, 8,  0, 4,    0,    0,    0,
  };
  EXPECT_EQ(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                  bytes.begin() + static_cast<std::ptrdiff_t>(at + paragraph.size())),
            paragraph);
  at += paragraph.size();
  EXPECT_EQ(U32(bytes, at), Crc(bytes, second, at));
  at += 4;
  const std::size_t third = at;
  const Bytes rule = {5, 0, 0, 0, 6, 0, 8, 0, 1, 0, 0, 0};  // A rule counts one character.
  EXPECT_EQ(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                  bytes.begin() + static_cast<std::ptrdiff_t>(at + rule.size())),
            rule);
  at += rule.size();
  EXPECT_EQ(U32(bytes, at), Crc(bytes, third, at));
  at += 4;

  // The one index page: its head, and a record for each block.
  EXPECT_EQ(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4)),
            (Bytes{0xF0, 0, 48, 0}));
  const std::size_t index = at + 4;
  const std::array<std::array<std::uint32_t, 5>, 3> records = {{{0, 0, 100, 2, 2},
                                                                {static_cast<std::uint32_t>(second), 3, 180, 1, 0},
                                                                {static_cast<std::uint32_t>(third), 7, 260, 5, 0}}};
  for (std::size_t i = 0; i < 3; ++i) {
    const std::size_t entry = index + (i * 16);
    EXPECT_EQ(U32(bytes, entry), records.at(i).at(0)) << i;
    EXPECT_EQ(U32(bytes, entry + 4), records.at(i).at(1)) << i;
    EXPECT_EQ(U32(bytes, entry + 8), records.at(i).at(2)) << i;
    EXPECT_EQ(bytes.at(entry + 12), records.at(i).at(3)) << i;
    EXPECT_EQ(bytes.at(entry + 13), records.at(i).at(4)) << "the low byte of its bits for breaking";
    EXPECT_EQ(U16(bytes, entry + 14), 0U);
  }
  at = index + 48;
  // The one anchor page: an id before the first block, and one three
  // characters into the second.
  const Bytes anchors = {0xF1, 0, 20, 0, 3, 't', 'o', 'p', 0, 0, 0, 0, 0, 0, 3, 'm', 'i', 'd', 1, 0, 3, 0, 0, 0};
  EXPECT_EQ(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                  bytes.begin() + static_cast<std::ptrdiff_t>(at + anchors.size())),
            anchors);
  const std::size_t anchorPage = at + 4;
  at += anchors.size();

  // The directory.
  EXPECT_EQ(record.directoryOffset, at);
  EXPECT_EQ(record.directoryBytes, 20U + 12 + 12 + 8);
  EXPECT_EQ(record.segmentBytes, at + 52);
  EXPECT_EQ(record.segmentOffset, begin);
  ASSERT_EQ(bytes.size(), at + 52);
  EXPECT_EQ(record.directoryChecksum, Crc(bytes, at, at + 52));
  EXPECT_EQ(U16(bytes, at), 1U);
  EXPECT_EQ(U16(bytes, at + 2), 1U);
  EXPECT_EQ(U16(bytes, at + 4), 1U);
  EXPECT_EQ(U16(bytes, at + 6), 0U);
  EXPECT_EQ(U32(bytes, at + 8), 3U);
  EXPECT_EQ(U32(bytes, at + 12), 8U);
  EXPECT_EQ(U32(bytes, at + 16), 4321U);
  EXPECT_EQ(U32(bytes, at + 20), index);
  EXPECT_EQ(U32(bytes, at + 24), 0U);
  EXPECT_EQ(U32(bytes, at + 28), Crc(bytes, index, index + 48));
  EXPECT_EQ(U32(bytes, at + 32), anchorPage);
  EXPECT_EQ(U16(bytes, at + 36), 20U);
  EXPECT_EQ(U16(bytes, at + 38), 2U);
  EXPECT_EQ(U32(bytes, at + 40), Crc(bytes, anchorPage, anchorPage + 20));
  EXPECT_EQ(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at + 44), bytes.end()),
            (Bytes{9, 0, 0x80, 0x02, 0xE0, 0x01, 1, 0}));

  // And the chapter's record.
  EXPECT_EQ(record.state, ChapterState::kWhole);
  EXPECT_EQ(record.blocks, 3U);
  EXPECT_EQ(record.characters, 8U);
  EXPECT_EQ(record.leftOut, 0U);
  // Its running text by class: the heading's is left out, "ab" is of no
  // class, and "c" is sans.
  EXPECT_EQ(record.classCharacters, (std::array<std::uint32_t, 6>{2, 0, 1, 0, 0, 0}));
  // Every character is in the first row of 256.
  std::array<std::uint8_t, 32> rows{};
  rows[0] = 1;
  EXPECT_EQ(record.characterRows, rows);
}

TEST(Segment, HoldsWhatWasWrittenWhateverTheChapter) {
  for (const std::uint32_t seed : {1U, 2U, 3U, 6U}) {
    SegmentFixture f;
    f.MakeBundle(4);
    // The languages a style item may name, in the order they were told.
    ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
    const std::size_t blocks = seed == 6 ? 0 : 60 + (seed * 90);
    const std::vector<Op> ops = MakeChapter(seed, blocks, 3);
    const Want want = Expected(ops);
    f.segmentBegin = f.bundle.DataEnd();
    for (const std::string_view tag : {"fr", "grc", "en-GB", "fr"}) {
      const std::uint8_t number = f.writer.Language(tag);
      EXPECT_EQ(number, tag == "fr" ? 1 : tag == "grc" ? 2 : 3);
    }
    SegmentStatus status = SegmentStatus::kOk;
    for (std::size_t i = 0; i < ops.size() && status == SegmentStatus::kOk; ++i) {
      status = f.Give(ops[i]);
    }
    ASSERT_EQ(status, SegmentStatus::kOk) << seed;
    EXPECT_EQ(f.writer.Blocks(), want.blocks.size());
    EXPECT_EQ(f.writer.Characters(), want.characters);
    ASSERT_EQ(f.writer.BeginEndBlocks(), SegmentStatus::kOk);
    while (f.writer.Step()) {
    }
    ASSERT_EQ(f.writer.Pictures(), want.pictures.size());
    std::size_t place = 0;
    for (const std::uint16_t number : want.pictures) {
      EXPECT_EQ(f.writer.PictureNumber(place), number) << "the pictures in the order of their numbers";
      if (number % 3 != 0) {
        ASSERT_EQ(f.writer.SetPictureSize(place, static_cast<std::uint16_t>(100 + number),
                                          static_cast<std::uint16_t>(200 + number)),
                  SegmentStatus::kOk);
      }
      ++place;
    }
    ASSERT_EQ(f.writer.BeginFinish(ChapterState::kCutShort, 9, 777'000), SegmentStatus::kOk);
    while (f.writer.Step()) {
    }
    ASSERT_EQ(f.writer.Status(), SegmentStatus::kOk);
    const BundleChapter record = f.writer.Chapter();
    EXPECT_EQ(record.state, ChapterState::kCutShort);
    EXPECT_EQ(record.reason, 9);
    EXPECT_EQ(record.blocks, want.blocks.size());
    EXPECT_EQ(record.characters, want.characters);
    EXPECT_EQ(record.leftOut, 0U);
    EXPECT_EQ(record.segmentOffset + record.segmentBytes, f.OnCard().size());

    // The bytes, read here by the format alone.
    const Walked walked = Walk(f.OnCard(), record);
    EXPECT_TRUE(walked.wrong.empty()) << seed << ": " << (walked.wrong.empty() ? "" : walked.wrong.front());
    EXPECT_TRUE(BlockChecksumsHold(f.OnCard(), record)) << seed;
    EXPECT_EQ(walked.blocksSaid, want.blocks.size());
    EXPECT_EQ(walked.charactersSaid, want.characters);
    EXPECT_EQ(walked.sourceBytes, 777'000U);
    EXPECT_EQ(walked.indexPages, (want.blocks.size() + 255) / 256);
    ASSERT_EQ(walked.blocks.size(), want.blocks.size());
    for (std::size_t i = 0; i < want.blocks.size(); ++i) {
      ExpectSameBlock(walked.blocks[i], want.blocks[i], i);
      EXPECT_EQ(walked.blocks[i].source, want.blocks[i].source) << i;
      EXPECT_EQ(walked.index.at(i).at(4),
                (want.blocks[i].style.has & bn::epub::kBlockBreaking) != 0 ? want.blocks[i].style.breaking & 0xFFU : 0U)
          << i;
    }
    EXPECT_TRUE(walked.anchors == want.anchors) << seed;
    EXPECT_EQ(walked.languages, (std::vector<std::string>{"fr", "grc", "en-GB"}));
    ASSERT_EQ(walked.pictures.size(), want.pictures.size());
    place = 0;
    for (const std::uint16_t number : want.pictures) {
      const bool sized = number % 3 != 0;
      EXPECT_EQ(walked.pictures.at(place), (std::array<std::uint32_t, 4>{number, sized ? 100U + number : 0U,
                                                                         sized ? 200U + number : 0U, sized ? 1U : 0U}))
          << number;
      ++place;
    }
    // Text given piece after piece is put together, into items of a
    // kilobyte at the most.
    for (const std::size_t length : walked.textItems) {
      EXPECT_LE(length, 1020U);
    }
    EXPECT_LE(f.mostWrites, 2U);

    // Committed, it is a chapter of the bundle; and the reader gives the
    // same, block after block from the first.
    ASSERT_EQ(f.bundle.Commit(2, record), BundleStatus::kOk);
    ASSERT_EQ(f.OpenBundle(), BundleStatus::kOk);
    ASSERT_EQ(f.OpenChapter(2), SegmentStatus::kOk);
    EXPECT_EQ(f.reader.Blocks(), want.blocks.size());
    EXPECT_EQ(f.reader.Characters(), want.characters);
    EXPECT_EQ(f.reader.SourceBytes(), 777'000U);
    EXPECT_EQ(f.reader.IndexedBlocks(), want.blocks.size());
    EXPECT_EQ(f.reader.Languages(), 3U);
    EXPECT_EQ(f.reader.Language(0), "");
    EXPECT_EQ(f.reader.Language(1), "fr");
    EXPECT_EQ(f.reader.Language(2), "grc");
    EXPECT_EQ(f.reader.Language(3), "en-GB");
    EXPECT_EQ(f.reader.Language(4), "");
    ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
    for (std::size_t i = 0; i < want.blocks.size(); ++i) {
      WantBlock got;
      ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk) << "block " << i << " of chapter " << seed;
      ExpectSameBlock(got, want.blocks[i], i);
      EXPECT_EQ(f.lastBlockIndex, i);
      EXPECT_EQ(f.lastBlockOffset, walked.blockOffsets.at(i));
    }
    WantBlock none;
    EXPECT_EQ(f.ReadWholeBlock(none), SegmentStatus::kEnd);
    EXPECT_EQ(f.reader.UnknownItems(), 0U);

    // Each picture, by its number and by its place.
    place = 0;
    for (const std::uint16_t number : want.pictures) {
      SegmentPicture picture;
      bool found = false;
      ASSERT_EQ(f.reader.ReadPicture(number, picture, found), SegmentStatus::kOk);
      ASSERT_TRUE(found) << number;
      EXPECT_EQ(picture.number, number);
      EXPECT_EQ(picture.sized, number % 3 != 0);
      EXPECT_EQ(picture.width, number % 3 != 0 ? 100 + number : 0);
      EXPECT_EQ(picture.height, number % 3 != 0 ? 200 + number : 0);
      SegmentPicture byPlace;
      ASSERT_EQ(f.reader.ReadPictureAt(place, byPlace), SegmentStatus::kOk);
      EXPECT_EQ(byPlace.number, number);
      EXPECT_EQ(byPlace.width, picture.width);
      ++place;
    }
    SegmentPicture picture;
    bool found = true;
    ASSERT_EQ(f.reader.ReadPicture(50, picture, found), SegmentStatus::kOk);
    EXPECT_FALSE(found) << "a picture the chapter does not show";
  }
}

TEST(SegmentReader, FindsAPlaceByItsBlockItsCharactersItsSourceAndItsId) {
  SegmentFixture f;
  f.MakeBundle(2);
  const std::vector<Op> ops = MakeChapter(11, 900, 0, false);
  const Want want = Expected(ops);
  f.WriteAndCommit(1, ops, 5'000'000);
  ASSERT_EQ(f.OpenChapter(1), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.IndexPages(), 4U);
  const Walked walked = Walk(f.OnCard(), f.writer.Chapter());
  ASSERT_TRUE(walked.wrong.empty());

  const auto expectPlace = [&](std::size_t block, const std::string& what) {
    const SegmentPlace& place = f.reader.Place();
    ASSERT_TRUE(place.found) << what;
    EXPECT_FALSE(place.atEnd) << what;
    EXPECT_EQ(place.block, block) << what;
    EXPECT_EQ(place.offset, walked.blockOffsets.at(block)) << what;
    EXPECT_EQ(place.charactersBefore, want.blocks.at(block).charactersBefore) << what;
    EXPECT_EQ(place.source, want.blocks.at(block).source) << what;
    EXPECT_EQ(place.kind, want.blocks.at(block).kind) << what;
  };

  // By its number: two steps, the page's entry and the page.
  for (const std::size_t block : {0U, 1U, 255U, 256U, 257U, 511U, 640U, 899U}) {
    ASSERT_EQ(f.reader.BeginFindBlock(static_cast<std::uint32_t>(block)), SegmentStatus::kOk);
    EXPECT_EQ(f.Steps(), 2);
    ASSERT_EQ(f.reader.Status(), SegmentStatus::kOk);
    expectPlace(block, "block " + std::to_string(block));
    EXPECT_EQ(f.reader.Place().into, 0U);
    // And from there the block is read.
    ASSERT_EQ(f.reader.Seek(f.reader.Place()), SegmentStatus::kOk);
    WantBlock got;
    ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk);
    ExpectSameBlock(got, want.blocks.at(block), block);
  }
  ASSERT_EQ(f.reader.FindBlock(900), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found) << "a block the chapter does not have";

  // By its characters: the first and the last of every block, and what is
  // past the end.
  for (std::size_t block = 0; block < want.blocks.size(); block += 7) {
    const WantBlock& wanted = want.blocks[block];
    for (const std::uint32_t into : {0U, wanted.characters / 2, wanted.characters - 1}) {
      ASSERT_EQ(f.reader.BeginFindCharacters(wanted.charactersBefore + into), SegmentStatus::kOk);
      EXPECT_EQ(f.Steps(), 2);
      ASSERT_EQ(f.reader.Status(), SegmentStatus::kOk);
      expectPlace(block, "the characters of block " + std::to_string(block));
      EXPECT_EQ(f.reader.Place().into, into);
    }
  }
  for (const std::uint32_t past : {want.characters, want.characters + 1, 0xFFFFFFFFU}) {
    ASSERT_EQ(f.reader.FindCharacters(past), SegmentStatus::kOk);
    EXPECT_TRUE(f.reader.Place().found);
    EXPECT_TRUE(f.reader.Place().atEnd);
    EXPECT_EQ(f.reader.Place().block, 900U);
    EXPECT_EQ(f.reader.Place().charactersBefore, want.characters);
    EXPECT_EQ(f.reader.Seek(f.reader.Place()), SegmentStatus::kMisused) << "there is no block at the end";
  }

  // By where it began in the source: the last block that began at or
  // before the place. Nine steps at the most for four pages.
  int mostSteps = 0;
  std::vector<std::size_t> looked = {256, 257, 512, 768};  // The first block of each page, which the search turns on.
  for (std::size_t block = 0; block < want.blocks.size(); block += 5) {
    looked.push_back(block);
  }
  for (const std::size_t block : looked) {
    // The last block with this one's source, where several have the same.
    std::size_t last = block;
    while (last + 1 < want.blocks.size() && want.blocks[last + 1].source == want.blocks[block].source) {
      ++last;
    }
    const std::uint32_t next =
        last + 1 < want.blocks.size() ? want.blocks[last + 1].source : want.blocks[last].source + 1000;
    for (const std::uint32_t source : {want.blocks[block].source, next - 1}) {
      ASSERT_EQ(f.reader.BeginFindSource(source), SegmentStatus::kOk);
      mostSteps = std::max(mostSteps, f.Steps());
      ASSERT_EQ(f.reader.Status(), SegmentStatus::kOk);
      expectPlace(last, "the source of block " + std::to_string(block));
    }
  }
  EXPECT_LE(mostSteps, 6);
  EXPECT_GE(mostSteps, 4);
  ASSERT_EQ(f.reader.FindSource(0), SegmentStatus::kOk);
  expectPlace(0, "a place before the first block");
  ASSERT_EQ(f.reader.FindSource(0xFFFFFFFFU), SegmentStatus::kOk);
  expectPlace(899, "a place after the last");

  // By its id: each, and the first where a name comes twice.
  ASSERT_GT(want.anchors.size(), 100U);
  ASSERT_GT(f.reader.AnchorPages(), 1U);
  for (std::size_t i = 0; i < want.anchors.size(); i += 3) {
    const WantAnchor& anchor = want.anchors[i];
    ASSERT_EQ(f.reader.BeginFindAnchor(anchor.name), SegmentStatus::kOk);
    f.Steps();
    ASSERT_EQ(f.reader.Status(), SegmentStatus::kOk);
    if (anchor.block == want.blocks.size()) {
      EXPECT_TRUE(f.reader.Place().atEnd) << anchor.name;
      EXPECT_EQ(f.reader.Place().charactersBefore, want.characters);
      continue;
    }
    expectPlace(anchor.block, anchor.name);
    EXPECT_EQ(f.reader.Place().into, anchor.into) << anchor.name;
  }
  ASSERT_EQ(f.reader.FindAnchor("no-such-id"), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
  ASSERT_EQ(f.reader.FindAnchor(""), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
  ASSERT_EQ(f.reader.FindAnchor(std::string(256, 'x')), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
}

TEST(SegmentReader, TakesTheFirstOfTwoIdsOfOneNameAndAnIdBetweenBlocksIsAtTheNext) {
  SegmentFixture f;
  f.MakeBundle(1);
  const std::vector<Op> ops = {
      Begin(SegmentBlockKind::kParagraph, 10),
      TextOp("first"),
      AnchorOp("twice"),
      Simple(Op::What::kEnd),
      AnchorOp("between"),
      AnchorOp(""),
      AnchorOp("twice"),
      Begin(SegmentBlockKind::kParagraph, 20),
      TextOp("second"),
      Simple(Op::What::kEnd),
      AnchorOp("after"),
  };
  f.WriteAndCommit(0, ops);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.FindAnchor("twice"), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().block, 0U);
  EXPECT_EQ(f.reader.Place().into, 5U);
  ASSERT_EQ(f.reader.FindAnchor("between"), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().block, 1U);
  EXPECT_EQ(f.reader.Place().into, 0U);
  EXPECT_EQ(f.reader.Place().charactersBefore, 5U);
  ASSERT_EQ(f.reader.FindAnchor("after"), SegmentStatus::kOk);
  EXPECT_TRUE(f.reader.Place().found);
  EXPECT_TRUE(f.reader.Place().atEnd);
  EXPECT_EQ(f.reader.Place().block, 2U);
  EXPECT_EQ(f.reader.Place().charactersBefore, 11U);
  // A name of no bytes was no id: three were kept.
  const Walked walked = Walk(f.OnCard(), f.writer.Chapter());
  EXPECT_EQ(walked.anchors.size(), 4U);
}

TEST(SegmentWriter, CutsLongTextIntoItemsAtCharactersAndCountsTheRowsItUses) {
  SegmentFixture f;
  f.MakeBundle(1);
  // Characters of three bytes, given so that an item's last room is one
  // or two bytes short of a character; then text that is no characters.
  std::string text;
  for (int i = 0; i < 2000; ++i) {
    text += "\xE6\xBC\xA2";
  }
  std::vector<Op> ops = {Begin(SegmentBlockKind::kParagraph, 0), TextOp("a")};
  for (std::size_t at = 0; at < text.size(); at += 3000) {
    ops.push_back(TextOp(text.substr(at, 3000)));
  }
  ops.push_back(Simple(Op::What::kEnd));
  ops.push_back(Begin(SegmentBlockKind::kParagraph, 1));
  ops.push_back(TextOp(std::string(3000, '\x80')));
  ops.push_back(TextOp("\xF0\x9F\x98\x80 \xCE\xB1 z"));
  ops.push_back(Simple(Op::What::kEnd));
  ASSERT_EQ(f.Write(ops), SegmentStatus::kOk);
  const Walked walked = Walk(f.OnCard(), f.writer.Chapter());
  // The second block's first text is what nothing can cut rightly: the
  // walk says so, and nothing else.
  for (const std::string& wrong : walked.wrong) {
    EXPECT_EQ(wrong, "a text item is empty, or begins or ends part way through a character");
  }
  ASSERT_EQ(walked.blocks.size(), 2U);
  ASSERT_EQ(walked.blocks[0].items.size(), 1U);
  EXPECT_EQ(walked.blocks[0].items[0].text, "a" + text);
  EXPECT_EQ(walked.blocks[0].characters, 6001U);
  // 1 and 339 characters of three bytes are 1,018 bytes: the item ends
  // there, two bytes short, and the others at 1,020.
  ASSERT_GE(walked.textItems.size(), 6U);
  EXPECT_EQ(walked.textItems[0], 1018U);
  EXPECT_EQ(walked.textItems[1], 1020U);
  EXPECT_EQ(walked.blocks[1].characters, 3000U + 9);

  // The rows of 256 its characters are in: 'a', ' ' and 'z' in the first,
  // alpha in the fourth, the Chinese character in row 0x6F, and the face,
  // which is past the first 65,536, in the last.
  std::array<std::uint8_t, 32> rows{};
  rows[0] = 0x09;
  rows[0x6F / 8] = static_cast<std::uint8_t>(1U << (0x6FU % 8U));
  rows[31] = 0x80;
  EXPECT_EQ(f.writer.Chapter().characterRows, rows);

  // More than an item may be at one call is refused.
  ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
  ASSERT_EQ(f.writer.BeginBlock(SegmentBlockKind::kParagraph, 0, {}, 0), SegmentStatus::kOk);
  EXPECT_EQ(f.writer.Text(std::string(4088, 'x')), SegmentStatus::kOk);
  EXPECT_EQ(f.writer.Text(std::string(4089, 'x')), SegmentStatus::kMisused);
}

TEST(SegmentWriter, WritesAPageFromWhicheverCallFillsOne) {
  // A chapter of one block, as long as leaves each number of bytes from 0
  // to 40 short of a page before its directory: so that the page is filled
  // by the text, by the block's end, by the index page, by the first bytes
  // of the directory, or by none of them. Each is a segment that reads
  // back, written in pages that follow one another.
  for (std::size_t bytes = 4010; bytes <= 4060; ++bytes) {
    SegmentFixture f;
    f.MakeBundle(1);
    const std::vector<Op> ops = {Begin(SegmentBlockKind::kParagraph, 7), TextOp(std::string(bytes, 'x')),
                                 Simple(Op::What::kEnd)};
    ASSERT_EQ(f.Write(ops, 99), SegmentStatus::kOk) << bytes;
    const BundleChapter record = f.writer.Chapter();
    // The block's head, four text items with their heads, its end; then
    // the index page with its head.
    EXPECT_EQ(record.directoryOffset, 4 + (4 * 4) + bytes + 12 + 4 + 16) << bytes;
    EXPECT_EQ(f.OnCard().size(), record.segmentOffset + record.segmentBytes) << bytes;
    const Walked walked = Walk(f.OnCard(), record);
    EXPECT_TRUE(walked.wrong.empty()) << bytes << ": " << (walked.wrong.empty() ? "" : walked.wrong.front());
    ASSERT_EQ(f.bundle.Commit(0, record), BundleStatus::kOk) << bytes;
    ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk) << bytes;
    ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
    WantBlock got;
    ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk) << bytes;
    EXPECT_EQ(got.characters, bytes);
  }
}

TEST(SegmentWriter, WritesAnIndexPageAfterEvery256BlocksAndLeavesBlocksPast65536OutOfTheIndex) {
  SegmentFixture f;
  f.MakeBundle(1);
  // The file already has bytes where the segment is to go, as it has
  // after a chapter that was given up: they are written over. (And the
  // stand-in for storage is slow to make a file longer.)
  ASSERT_TRUE(f.file.Seek(f.bundle.DataEnd()));
  ASSERT_TRUE(f.file.Write(Bytes(3'000'000, 0xDD)));
  ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
  f.segmentBegin = f.bundle.DataEnd();
  const std::uint32_t blocks = 65536 + 300;
  for (std::uint32_t i = 0; i < blocks; ++i) {
    const std::size_t before = f.storage.written.size();
    ASSERT_EQ(f.writer.BeginBlock(SegmentBlockKind::kParagraph, 0, {}, i * 10), SegmentStatus::kOk);
    ASSERT_EQ(f.writer.Text(std::to_string(i)), SegmentStatus::kOk);
    if (i == 65535 || i == 65536 || i == 65600) {
      ASSERT_EQ(f.writer.Anchor("in-" + std::to_string(i)), SegmentStatus::kOk);
    }
    ASSERT_EQ(f.writer.EndBlock(), SegmentStatus::kOk);
    f.CheckWrites(before, 2);
  }
  ASSERT_EQ(f.Give(AnchorOp("after-all")), SegmentStatus::kOk);
  ASSERT_EQ(f.writer.BeginEndBlocks(), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  ASSERT_EQ(f.writer.BeginFinish(ChapterState::kWhole, 0, 0), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  ASSERT_EQ(f.writer.Status(), SegmentStatus::kOk);
  const BundleChapter record = f.writer.Chapter();
  EXPECT_EQ(record.blocks, blocks);
  // 300 blocks and three ids left out: the two in blocks that are not
  // indexed, and the one after them all.
  EXPECT_EQ(record.leftOut, 303U);
  // Every block is there, with 256 index pages among them, all full, and
  // the one page of ids.
  EXPECT_EQ(CountRecords(f.OnCard(), record), (std::array<std::size_t, 3>{blocks, 256, 1}));
  const std::size_t directory = record.segmentOffset + record.directoryOffset;
  EXPECT_EQ(U16(f.OnCard(), directory), 256U);
  EXPECT_EQ(U16(f.OnCard(), directory + 2), 1U);
  EXPECT_EQ(record.directoryBytes, 20U + (256 * 12) + 12);
  const std::size_t lastIndexPage = record.segmentOffset + U32(f.OnCard(), directory + 20 + (std::size_t{255} * 12));
  EXPECT_EQ(U16(f.OnCard(), lastIndexPage - 2), 4096U) << "the last index page is full";
  EXPECT_EQ(U32(f.OnCard(), lastIndexPage + (std::size_t{255} * 16) + 8), 655350U)
      << "and its last record is of block 65,535";

  // The blocks past the index are read by going on from the last that is
  // in it.
  ASSERT_EQ(f.bundle.Commit(0, record), BundleStatus::kOk);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Blocks(), blocks);
  EXPECT_EQ(f.reader.IndexedBlocks(), 65536U);
  EXPECT_EQ(f.reader.IndexPages(), 256U);
  ASSERT_EQ(f.reader.FindBlock(65536), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
  ASSERT_EQ(f.reader.FindAnchor("in-65535"), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().block, 65535U);
  EXPECT_EQ(f.reader.Place().into, 5U);
  for (const std::string_view leftOut : {"in-65536", "in-65600", "after-all"}) {
    ASSERT_EQ(f.reader.FindAnchor(leftOut), SegmentStatus::kOk);
    EXPECT_FALSE(f.reader.Place().found) << leftOut;
  }
  ASSERT_EQ(f.reader.FindBlock(65535), SegmentStatus::kOk);
  ASSERT_TRUE(f.reader.Place().found);
  ASSERT_EQ(f.reader.Seek(f.reader.Place()), SegmentStatus::kOk);
  for (std::uint32_t i = 65535; i < blocks; ++i) {
    WantBlock got;
    ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk) << i;
    ASSERT_EQ(got.items.size(), 1U);
    EXPECT_EQ(got.items[0].text, std::to_string(i));
    EXPECT_EQ(f.lastBlockIndex, i);
  }
  WantBlock none;
  EXPECT_EQ(f.ReadWholeBlock(none), SegmentStatus::kEnd);
  // A count of characters in a block past the index comes to the last
  // block that is in it, and says how far on.
  const std::uint32_t characters = record.characters - 2;
  ASSERT_EQ(f.reader.FindCharacters(characters), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().block, 65535U);
  EXPECT_EQ(f.reader.Place().into, characters - f.reader.Place().charactersBefore);
  EXPECT_GT(f.reader.Place().into, 1000U);
  // A source place: the nine steps of 256 pages, twice over, and one.
  ASSERT_EQ(f.reader.BeginFindSource(123'456), SegmentStatus::kOk);
  EXPECT_LE(f.Steps(), 18);
  EXPECT_EQ(f.reader.Place().block, 12345U);
}

TEST(SegmentWriter, LeavesOutAndCountsWhatThereIsNoRoomFor) {
  SegmentFixture f;
  f.MakeBundle(1);
  ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
  f.segmentBegin = f.bundle.DataEnd();
  // Sixteen languages, one with a tag too long: fifteen are kept.
  EXPECT_EQ(f.writer.Language(std::string(24, 'x')), 0);
  EXPECT_EQ(f.writer.Language(""), 0);
  for (int i = 0; i < 15; ++i) {
    EXPECT_EQ(f.writer.Language("l" + std::to_string(i)), i + 1);
  }
  EXPECT_EQ(f.writer.Language("one-more"), 0);
  EXPECT_EQ(f.writer.Language(std::string(23, 'y')), 0) << "(of a length a tag may be, but there are fifteen)";
  EXPECT_EQ(f.writer.Language("l7"), 8);
  EXPECT_EQ(f.writer.LeftOut(), 3U);

  // An id with a name too long; and ids enough to fill every page there
  // may be: 128 pages of sixteen ids of 249 bytes each, in a block and
  // between blocks.
  ASSERT_EQ(f.Give(AnchorOp(std::string(256, 'n'))), SegmentStatus::kOk);
  EXPECT_EQ(f.writer.LeftOut(), 4U);
  ASSERT_EQ(f.Give(Begin(SegmentBlockKind::kParagraph, 0)), SegmentStatus::kOk);
  const auto name = [](int number) {
    std::string text = std::to_string(number);
    text.append(249 - text.size(), '.');
    return text;
  };
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(f.Give(TextOp("word ")), SegmentStatus::kOk);
    ASSERT_EQ(f.Give(AnchorOp(name(i))), SegmentStatus::kOk);
  }
  ASSERT_EQ(f.Give(Simple(Op::What::kEnd)), SegmentStatus::kOk);
  for (int i = 1000; i < 2100; ++i) {
    ASSERT_EQ(f.Give(AnchorOp(name(i))), SegmentStatus::kOk);
  }
  EXPECT_EQ(f.writer.LeftOut(), 4U + (2100 - (128 * 16)));

  // And 600 pictures in one block, of which the directory has 512.
  ASSERT_EQ(f.Give(Begin(SegmentBlockKind::kParagraph, 10)), SegmentStatus::kOk);
  for (int i = 0; i < 600; ++i) {
    ASSERT_EQ(f.Give(PictureOp(static_cast<std::uint16_t>(5000 - i), 0, {}, {}, "")), SegmentStatus::kOk);
    ASSERT_EQ(f.Give(PictureOp(bn::epub::kSegmentNoPicture, 0, {}, {}, "")), SegmentStatus::kOk);
  }
  ASSERT_EQ(f.Give(Simple(Op::What::kEnd)), SegmentStatus::kOk);
  f.writer.CountLeftOut(5);
  ASSERT_EQ(f.writer.BeginEndBlocks(), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  EXPECT_EQ(f.writer.Pictures(), 512U);
  ASSERT_EQ(f.writer.BeginFinish(ChapterState::kWhole, 0, 0), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  ASSERT_EQ(f.writer.Status(), SegmentStatus::kOk);
  const BundleChapter record = f.writer.Chapter();
  EXPECT_EQ(record.leftOut, 4U + 52 + 88 + 5);
  const Walked walked = Walk(f.OnCard(), record);
  EXPECT_TRUE(walked.wrong.empty()) << (walked.wrong.empty() ? "" : walked.wrong.front());
  EXPECT_EQ(walked.anchorPages, 128U);
  ASSERT_EQ(walked.anchors.size(), 2048U);
  EXPECT_EQ(walked.anchors[999].block, 0U);
  EXPECT_EQ(walked.anchors[999].into, 5000U);
  EXPECT_EQ(walked.anchors[1000].block, 1U);
  EXPECT_EQ(walked.anchors[2047].name, name(2047));
  EXPECT_EQ(walked.languages.size(), 15U);
  // The pictures the directory has are the first 512 that were met, in
  // the order of their numbers.
  ASSERT_EQ(walked.pictures.size(), 512U);
  EXPECT_EQ(walked.pictures.front().at(0), 5000U - 511);
  EXPECT_EQ(walked.pictures.back().at(0), 5000U);
  // The block's 1,200 pictures are all there, and each is a character.
  EXPECT_EQ(walked.blocks.at(1).characters, 1200U);

  // A page of ids that fills part way through a block lies between two of
  // the block's items: the reader passes over it, and the block is whole.
  ASSERT_EQ(f.bundle.Commit(0, record), BundleStatus::kOk);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  WantBlock got;
  ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk);
  ASSERT_EQ(got.items.size(), 1U);
  EXPECT_EQ(got.items[0].text.size(), 5000U);
  ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk);
  EXPECT_EQ(got.items.size(), 1200U);
  // The last id kept, and one that was left out.
  const std::string lastKept = name(2047);  // It has to stay while it is looked for.
  ASSERT_EQ(f.reader.BeginFindAnchor(lastKept), SegmentStatus::kOk);
  EXPECT_EQ(f.Steps(), (2 * 128) + 2) << "every page's entry and the page, and then the block's";
  EXPECT_EQ(f.reader.Place().block, 1U);
  ASSERT_EQ(f.reader.FindAnchor(name(2048)), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
}

TEST(SegmentWriter, RefusesWhatIsOutOfTurnAndStaysRefused) {
  SegmentFixture f;
  f.MakeBundle(1);
  const auto begun = [&f] {
    EXPECT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
    return &f.writer;
  };
  // Nothing before Begin(), and a place that is no sector's start or is
  // past the file's end.
  EXPECT_EQ(f.writer.BeginBlock(SegmentBlockKind::kParagraph, 0, {}, 0), SegmentStatus::kMisused);
  EXPECT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd() + 100), SegmentStatus::kMisused);
  EXPECT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd() + 512), SegmentStatus::kMisused);
  // Memory that is too little.
  std::vector<std::uint8_t> little(bn::epub::kSegmentWriterBytes - 1);
  SegmentWriter cramped{std::span<std::uint8_t>(little)};
  EXPECT_EQ(cramped.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kMisused);

  EXPECT_EQ(begun()->Text("x"), SegmentStatus::kMisused) << "text outside a block";
  EXPECT_EQ(f.writer.Text("x"), SegmentStatus::kMisused) << "and it stays so";
  EXPECT_EQ(f.writer.BeginBlock(SegmentBlockKind::kParagraph, 0, {}, 0), SegmentStatus::kMisused);
  EXPECT_EQ(begun()->EndBlock(), SegmentStatus::kMisused);
  EXPECT_EQ(begun()->BeginBlock(SegmentBlockKind::kHeading, 0, {}, 0), SegmentStatus::kMisused)
      << "a heading of no level";
  EXPECT_EQ(begun()->BeginBlock(SegmentBlockKind::kHeading, 7, {}, 0), SegmentStatus::kMisused);
  EXPECT_EQ(begun()->BeginBlock(SegmentBlockKind::kParagraph, 1, {}, 0), SegmentStatus::kMisused);
  EXPECT_EQ(begun()->BeginBlock(static_cast<SegmentBlockKind>(0), 0, {}, 0), SegmentStatus::kMisused);
  EXPECT_EQ(begun()->BeginBlock(static_cast<SegmentBlockKind>(0xF0), 0, {}, 0), SegmentStatus::kMisused);

  const auto inBlock = [&begun] {
    SegmentWriter* writer = begun();
    EXPECT_EQ(writer->BeginBlock(SegmentBlockKind::kParagraph, 0, {}, 0), SegmentStatus::kOk);
    return writer;
  };
  EXPECT_EQ(inBlock()->BeginBlock(SegmentBlockKind::kParagraph, 0, {}, 0), SegmentStatus::kMisused)
      << "a block in a block";
  EXPECT_EQ(inBlock()->LinkEnd(), SegmentStatus::kMisused) << "no link is open";
  EXPECT_EQ(inBlock()->LinkStart(LinkKind::kInside, 0, std::string(256, 'f')), SegmentStatus::kMisused);
  EXPECT_EQ(inBlock()->Picture(1, 0, {}, {}, std::string(256, 'a')), SegmentStatus::kMisused);
  EXPECT_EQ(inBlock()->BeginEndBlocks(), SegmentStatus::kMisused) << "a block is open";
  RunStyle run;
  run.language = 1;
  EXPECT_EQ(inBlock()->Style(run), SegmentStatus::kMisused) << "a language that was never told";
  run.language = 0;
  run.familyClass = 8;
  EXPECT_EQ(inBlock()->Style(run), SegmentStatus::kMisused);
  SegmentWriter* writer = inBlock();
  ASSERT_EQ(writer->LinkStart(LinkKind::kInside, 0, ""), SegmentStatus::kOk);
  EXPECT_EQ(writer->LinkStart(LinkKind::kInside, 0, ""), SegmentStatus::kMisused) << "a link in a link";
  writer = inBlock();
  ASSERT_EQ(writer->LinkStart(LinkKind::kOutside, 0, ""), SegmentStatus::kOk);
  EXPECT_EQ(writer->EndBlock(), SegmentStatus::kMisused) << "a block ended with a link open";

  // The end in its order: the pictures' sizes only once the blocks are
  // ended, to their last step; and a state that is one a segment has.
  writer = begun();
  EXPECT_EQ(writer->BeginFinish(ChapterState::kWhole, 0, 0), SegmentStatus::kMisused);
  writer = begun();
  ASSERT_EQ(writer->BeginEndBlocks(), SegmentStatus::kOk);
  EXPECT_EQ(writer->SetPictureSize(0, 1, 1), SegmentStatus::kMisused);
  writer = begun();
  ASSERT_EQ(writer->BeginEndBlocks(), SegmentStatus::kOk);
  while (writer->Step()) {
  }
  EXPECT_EQ(writer->SetPictureSize(0, 1, 1), SegmentStatus::kMisused) << "a picture there is none of";
  writer = begun();
  ASSERT_EQ(writer->BeginEndBlocks(), SegmentStatus::kOk);
  while (writer->Step()) {
  }
  EXPECT_EQ(writer->BeginFinish(ChapterState::kRefused, 0, 0), SegmentStatus::kMisused);

  // A chapter of nothing at all is a segment: a directory, and no more.
  writer = begun();
  ASSERT_EQ(writer->BeginEndBlocks(), SegmentStatus::kOk);
  while (writer->Step()) {
  }
  ASSERT_EQ(writer->BeginFinish(ChapterState::kWhole, 0, 55), SegmentStatus::kOk);
  while (writer->Step()) {
  }
  ASSERT_EQ(writer->Status(), SegmentStatus::kOk);
  EXPECT_EQ(writer->Chapter().segmentBytes, 20U);
  EXPECT_EQ(writer->Chapter().directoryOffset, 0U);
  ASSERT_EQ(f.bundle.Commit(0, writer->Chapter()), BundleStatus::kOk);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Blocks(), 0U);
  EXPECT_EQ(f.reader.SourceBytes(), 55U);
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  SegmentBlock block;
  EXPECT_EQ(f.reader.ReadBlock(block), SegmentStatus::kEnd);
  ASSERT_EQ(f.reader.FindCharacters(0), SegmentStatus::kOk);
  EXPECT_TRUE(f.reader.Place().atEnd);
  ASSERT_EQ(f.reader.FindSource(0), SegmentStatus::kOk);
  EXPECT_FALSE(f.reader.Place().found);
}

TEST(SegmentWriter, EndsInAStatusWhenStorageFails) {
  // Storage failing at each of a chapter's writes in turn.
  const std::vector<Op> ops = MakeChapter(21, 50);
  std::size_t writes = 0;
  {
    SegmentFixture f;
    f.MakeBundle(1);
    const std::size_t before = f.storage.written.size();
    ASSERT_EQ(f.Write(ops), SegmentStatus::kOk);
    writes = f.storage.written.size() - before;
  }
  ASSERT_GT(writes, 10U);
  for (std::size_t failing = 0; failing < writes; ++failing) {
    SegmentFixture f;
    f.MakeBundle(1);
    const std::size_t before = f.storage.written.size();
    ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
    SegmentStatus status = SegmentStatus::kOk;
    const auto arm = [&f, before, failing] {
      f.storage.failWrites =
          f.storage.written.size() - before >= failing ? StorageStatus::kNoSpace : StorageStatus::kOk;
    };
    for (std::size_t i = 0; i < ops.size() && status == SegmentStatus::kOk; ++i) {
      arm();
      switch (ops[i].what) {
        case Op::What::kBegin:
          status = f.writer.BeginBlock(ops[i].kind, ops[i].level, ops[i].style, ops[i].source);
          break;
        case Op::What::kText:
          status = f.writer.Text(ops[i].text);
          break;
        case Op::What::kStyle:
          status = f.writer.Style(ops[i].run);
          break;
        case Op::What::kLinkStart:
          status = f.writer.LinkStart(ops[i].link, ops[i].chapter, ops[i].text);
          break;
        case Op::What::kLinkEnd:
          status = f.writer.LinkEnd();
          break;
        case Op::What::kPicture:
          status = f.writer.Picture(ops[i].picture, ops[i].bits, ops[i].width, ops[i].height, ops[i].text);
          break;
        case Op::What::kEnd:
          status = f.writer.EndBlock();
          break;
        case Op::What::kAnchor:
          status = f.writer.Anchor(ops[i].text);
          break;
      }
    }
    if (status == SegmentStatus::kOk) {
      status = f.writer.BeginEndBlocks();
      for (bool more = status == SegmentStatus::kOk; more;) {
        arm();
        more = f.writer.Step();
      }
      status = f.writer.Status();
    }
    if (status == SegmentStatus::kOk) {
      arm();
      status = f.writer.BeginFinish(ChapterState::kWhole, 0, 0);
      for (bool more = status == SegmentStatus::kOk; more;) {
        arm();
        more = f.writer.Step();
      }
      status = f.writer.Status();
    }
    EXPECT_EQ(status, SegmentStatus::kStoreFailed) << "with write " << failing << " failing";
    EXPECT_EQ(f.writer.Status(), SegmentStatus::kStoreFailed);
    EXPECT_EQ(f.writer.EndBlock(), SegmentStatus::kStoreFailed) << "and nothing more is taken";
    // The bundle is as it was: the chapter is not imported.
    f.storage.failWrites = StorageStatus::kOk;
    ASSERT_EQ(f.OpenBundle(), BundleStatus::kOk);
    EXPECT_EQ(f.bundle.DataEnd(), f.bundle.DataBegin());
  }
}

TEST(SegmentReader, SaysASegmentIsDamagedWhateverByteOfItIsChanged) {
  // A chapter of a few pages, with two index pages and two anchor pages;
  // and every byte of it changed in turn. Whatever is then asked either
  // ends in a status that says so, or gives what the chapter held: never
  // something else.
  SegmentFixture f;
  f.MakeBundle(1);
  const std::vector<Op> ops = MakeSmallChapter(31, 260);
  const Want want = Expected(ops);
  ASSERT_EQ(f.writer.Begin(f.file, f.bundle.DataEnd()), SegmentStatus::kOk);
  ASSERT_EQ(f.writer.Language("fr"), 1);
  ASSERT_EQ(f.writer.Language("de"), 2);
  SegmentStatus written = SegmentStatus::kOk;
  f.segmentBegin = f.bundle.DataEnd();
  for (std::size_t i = 0; i < ops.size() && written == SegmentStatus::kOk; ++i) {
    written = f.Give(ops[i]);
  }
  ASSERT_EQ(written, SegmentStatus::kOk);
  ASSERT_EQ(f.writer.BeginEndBlocks(), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  ASSERT_EQ(f.writer.BeginFinish(ChapterState::kWhole, 0, 99), SegmentStatus::kOk);
  while (f.writer.Step()) {
  }
  ASSERT_EQ(f.writer.Status(), SegmentStatus::kOk);
  const BundleChapter record = f.writer.Chapter();
  ASSERT_EQ(f.bundle.Commit(0, record), BundleStatus::kOk);
  const Bytes sound = f.OnCard();
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.IndexPages(), 2U);
  ASSERT_GE(f.reader.AnchorPages(), 2U);

  // Everything the reader can be asked of the chapter. True if all of it
  // came to what the chapter held; false if something ended in a status
  // that is not kOk. Anything else fails the test.
  const auto readsAsWritten = [&f, &want](const std::string& what) {
    if (f.OpenChapter(0) != SegmentStatus::kOk) {
      return false;
    }
    if (f.reader.Blocks() != want.blocks.size() || f.reader.Characters() != want.characters ||
        f.reader.SourceBytes() != 99 || f.reader.Language(1) != "fr" || f.reader.Language(2) != "de") {
      ADD_FAILURE() << what << ": the directory was taken and says something else";
      return false;
    }
    if (f.reader.SeekStart() != SegmentStatus::kOk) {
      return false;
    }
    for (std::size_t i = 0; i < want.blocks.size(); ++i) {
      WantBlock got;
      if (f.ReadWholeBlock(got) != SegmentStatus::kOk) {
        return false;
      }
      if (got.kind != want.blocks[i].kind || got.level != want.blocks[i].level ||
          !(got.style == want.blocks[i].style) || got.items != want.blocks[i].items ||
          got.characters != want.blocks[i].characters) {
        ADD_FAILURE() << what << ": block " << i << " was given, and is not what was written";
        return false;
      }
    }
    WantBlock none;
    if (f.ReadWholeBlock(none) != SegmentStatus::kEnd) {
      return false;
    }
    // A block of each index page; and the first id and the last, which is
    // looked for through every anchor page.
    for (const std::size_t block : {std::size_t{7}, want.blocks.size() - 1}) {
      if (f.reader.FindBlock(static_cast<std::uint32_t>(block)) != SegmentStatus::kOk) {
        return false;
      }
      if (f.reader.Place().charactersBefore != want.blocks[block].charactersBefore ||
          f.reader.Place().source != want.blocks[block].source || f.reader.Place().kind != want.blocks[block].kind) {
        ADD_FAILURE() << what << ": the index's record of block " << block << " was given, and is something else";
        return false;
      }
    }
    for (const std::size_t i : {std::size_t{0}, want.anchors.size() - 1}) {
      if (f.reader.FindAnchor(want.anchors[i].name) != SegmentStatus::kOk) {
        return false;
      }
      const SegmentPlace& place = f.reader.Place();
      if (!place.found || place.block != want.anchors[i].block ||
          (!place.atEnd && place.into != want.anchors[i].into)) {
        ADD_FAILURE() << what << ": the id " << want.anchors[i].name << " was found somewhere else";
        return false;
      }
    }
    for (std::size_t i = 0; i < f.reader.Pictures(); ++i) {
      SegmentPicture picture;
      if (f.reader.ReadPictureAt(i, picture) != SegmentStatus::kOk) {
        return false;
      }
    }
    return true;
  };
  ASSERT_TRUE(readsAsWritten("nothing changed"));

  std::size_t noticed = 0;
  std::size_t harmless = 0;
  const std::size_t begin = record.segmentOffset;
  const std::size_t end = begin + record.segmentBytes;
  // Every byte of the directory, and one byte in seventeen of what is
  // before it; each with a bit of its own changed.
  for (std::size_t at = begin; at < end; at += (at >= begin + record.directoryOffset ? 1 : 17)) {
    Bytes changed = sound;
    changed[at] ^= static_cast<std::uint8_t>(1U << (at % 8));
    f.file.Close();
    f.storage.Put(kPath, changed);
    ASSERT_EQ(f.OpenBundle(), BundleStatus::kOk);
    if (readsAsWritten("byte " + std::to_string(at - begin) + " of the segment")) {
      ++harmless;
    } else {
      ++noticed;
    }
  }
  // The only bytes that can change and leave the chapter as it was are in
  // the four bytes before a page, which no checksum is of: the byte that
  // says nothing, and the bit that makes one kind of page the other.
  EXPECT_GT(noticed, 1000U);
  EXPECT_LE(harmless, 4U);
}

TEST(SegmentWriter, TakesABlockThatBeginsBeforeTheLastAsBeginningWhereTheLastDid) {
  // The index is searched by where a block began, which only works if
  // that never goes back.
  SegmentFixture f;
  f.MakeBundle(1);
  const std::vector<Op> ops = {
      Begin(SegmentBlockKind::kParagraph, 50), TextOp("a"), Simple(Op::What::kEnd),
      Begin(SegmentBlockKind::kParagraph, 20), TextOp("b"), Simple(Op::What::kEnd),
      Begin(SegmentBlockKind::kParagraph, 70), TextOp("c"), Simple(Op::What::kEnd),
      Begin(SegmentBlockKind::kParagraph, 60), TextOp("d"), Simple(Op::What::kEnd),
  };
  f.WriteAndCommit(0, ops);
  const Walked walked = Walk(f.OnCard(), f.writer.Chapter());
  ASSERT_EQ(walked.index.size(), 4U);
  EXPECT_EQ(walked.index[0].at(2), 50U);
  EXPECT_EQ(walked.index[1].at(2), 50U);
  EXPECT_EQ(walked.index[2].at(2), 70U);
  EXPECT_EQ(walked.index[3].at(2), 70U);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.FindSource(60), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().block, 1U);
}

TEST(SegmentReader, ForgetsOneChapterWhenItOpensAnother) {
  SegmentFixture f;
  f.MakeBundle(2);
  const std::vector<Op> first = MakeChapter(71, 300, 0, false);
  const std::vector<Op> second = MakeChapter(72, 300, 0, false);
  const Want wantFirst = Expected(first);
  const Want wantSecond = Expected(second);
  f.WriteAndCommit(0, first);
  f.WriteAndCommit(1, second);
  // A block and an id of the one, then the same of the other: nothing of
  // the first's directory is taken for the second's.
  for (const int round : {0, 1, 2}) {
    const Want& want = round == 1 ? wantSecond : wantFirst;
    ASSERT_EQ(f.OpenChapter(round == 1 ? 1 : 0), SegmentStatus::kOk);
    for (const std::size_t block : {std::size_t{290}, std::size_t{3}}) {
      ASSERT_EQ(f.reader.FindBlock(static_cast<std::uint32_t>(block)), SegmentStatus::kOk) << round;
      EXPECT_EQ(f.reader.Place().charactersBefore, want.blocks.at(block).charactersBefore) << round;
      EXPECT_EQ(f.reader.Place().source, want.blocks.at(block).source) << round;
    }
    const WantAnchor& anchor = want.anchors.at(want.anchors.size() / 2);
    ASSERT_EQ(f.reader.FindAnchor(anchor.name), SegmentStatus::kOk) << round;
    ASSERT_TRUE(f.reader.Place().found) << round;
    EXPECT_EQ(f.reader.Place().block, anchor.block) << round;
  }
}

// Writes a segment of one block made of the bytes given (its head, its
// style record and its items, but not its end), with the end, the
// checksum, an index page and a directory worked out here; and commits it
// as the chapter. `characters` is what the block's end says it is, and
// `languages` how many languages the directory names.
void CommitOneBlock(SegmentFixture& f, Bytes block, std::uint32_t characters, std::uint8_t languages = 0) {
  const auto add32 = [](Bytes& to, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      to.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  };
  const std::uint8_t kind = block.at(0);
  block.insert(block.end(), {6, 0, 8, 0});
  add32(block, characters);
  add32(block, bn::core::Crc32(block));
  Bytes index;
  add32(index, 0);
  add32(index, 0);
  add32(index, 0);
  index.insert(index.end(), {kind, 0, 0, 0});
  Bytes segment = block;
  segment.insert(segment.end(), {0xF0, 0, 16, 0});
  const auto indexAt = static_cast<std::uint32_t>(segment.size());
  segment.insert(segment.end(), index.begin(), index.end());
  Bytes directory = {1, 0, 0, 0, 0, 0, languages, 0};
  add32(directory, 1);
  add32(directory, characters);
  add32(directory, 0);
  add32(directory, indexAt);
  add32(directory, 0);
  add32(directory, bn::core::Crc32(index));
  for (std::uint8_t language = 0; language < languages; ++language) {
    directory.insert(directory.end(), {2, 'l', static_cast<std::uint8_t>('a' + language)});
  }
  BundleChapter record;
  record.state = ChapterState::kWhole;
  record.segmentOffset = f.bundle.DataEnd();
  record.directoryOffset = static_cast<std::uint32_t>(segment.size());
  record.directoryBytes = static_cast<std::uint32_t>(directory.size());
  record.directoryChecksum = bn::core::Crc32(directory);
  record.blocks = 1;
  record.characters = characters;
  segment.insert(segment.end(), directory.begin(), directory.end());
  record.segmentBytes = static_cast<std::uint32_t>(segment.size());
  ASSERT_TRUE(f.file.Seek(f.bundle.DataEnd()));
  ASSERT_TRUE(f.file.Write(segment));
  ASSERT_EQ(f.bundle.Commit(0, record), BundleStatus::kOk);
}

TEST(SegmentReader, TakesABlockThatSaysWhatCannotBeForDamaged) {
  // Blocks written here a byte at a time, each with the checksum its
  // bytes come to: what is wrong with one is not for the checksum to say.
  struct Case {
    const char* what;
    Bytes block;
    std::uint32_t characters;
    std::uint8_t languages;
    SegmentStatus status;
  };
  Bytes longText = {1, 0, 0, 0, 1, 0, 0xF9, 0x0F};  // A text item of 4,089 bytes.
  longText.resize(longText.size() + 4089, 'x');
  Bytes mostText = {1, 0, 0, 0, 1, 0, 0xF8, 0x0F};  // And one of 4,088, which is the most.
  mostText.resize(mostText.size() + 4088, 'x');
  Bytes longAlt = {1, 0, 0, 0, 5, 0, 16, 1, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // An alt of 256 bytes.
  longAlt.resize(longAlt.size() + 256, 'a');
  Bytes longStyle = {1, 0, 78, 0};  // A style record of 78 bytes.
  longStyle.resize(longStyle.size() + 78, 1);
  const std::vector<Case> cases = {
      {"a block as it should be", {1, 0, 0, 0, 1, 0, 2, 0, 'o', 'k'}, 2, 0, SegmentStatus::kOk},
      {"more characters than its text is", {1, 0, 0, 0, 1, 0, 2, 0, 'o', 'k'}, 3, 0, SegmentStatus::kDamaged},
      {"fewer", {1, 0, 0, 0, 1, 0, 2, 0, 'o', 'k'}, 1, 0, SegmentStatus::kDamaged},
      {"a block of nothing that says it is no characters", {5, 0, 0, 0}, 0, 0, SegmentStatus::kDamaged},
      {"a block of nothing, which is one character", {5, 0, 0, 0}, 1, 0, SegmentStatus::kOk},
      {"a picture, which is one",
       {4, 0, 0, 0, 5, 0, 16, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
       1,
       0,
       SegmentStatus::kOk},
      {"a picture cut short",
       {4, 0, 0, 0, 5, 0, 15, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
       1,
       0,
       SegmentStatus::kDamaged},
      {"a picture's text longer than a name", longAlt, 1, 0, SegmentStatus::kDamaged},
      {"a style of the book's own language",
       {1, 0, 0, 0, 2, 0, 8, 0, 1, 0, 1, 0, 2, 0, 0x84, 3},
       1,
       0,
       SegmentStatus::kOk},
      {"a style of a language the chapter has",
       {1, 0, 0, 0, 2, 0, 8, 0, 1, 0, 1, 0, 2, 2, 0x84, 3},
       1,
       2,
       SegmentStatus::kOk},
      {"a style of a language it has not",
       {1, 0, 0, 0, 2, 0, 8, 0, 1, 0, 1, 0, 2, 3, 0x84, 3},
       1,
       2,
       SegmentStatus::kDamaged},
      {"a style a byte short", {1, 0, 0, 0, 2, 0, 7, 0, 1, 0, 1, 0, 2, 0, 0x84}, 1, 0, SegmentStatus::kDamaged},
      {"a link of no kind", {1, 0, 0, 0, 3, 3, 3, 0, 7, 0, 0, 4, 0, 0, 0}, 1, 0, SegmentStatus::kDamaged},
      {"a link whose name is not as long as it says",
       {1, 0, 0, 0, 3, 0, 5, 0, 7, 0, 4, 'a', 'b'},
       1,
       0,
       SegmentStatus::kDamaged},
      {"a link with no room for its start", {1, 0, 0, 0, 3, 0, 2, 0, 7, 0}, 1, 0, SegmentStatus::kDamaged},
      {"a link's end with something in it", {1, 0, 0, 0, 4, 0, 1, 0, 'x'}, 1, 0, SegmentStatus::kDamaged},
      {"an item of no kind at all", {1, 0, 0, 0, 0, 0, 0, 0}, 1, 0, SegmentStatus::kDamaged},
      {"an item longer than an item may be", longText, 4089, 0, SegmentStatus::kDamaged},
      {"an item as long as one may be", mostText, 4088, 0, SegmentStatus::kOk},
      {"a block of no kind", {0, 0, 0, 0, 1, 0, 1, 0, 'x'}, 1, 0, SegmentStatus::kDamaged},
      {"a style record too short to be one", {1, 0, 3, 0, 1, 0, 0}, 1, 0, SegmentStatus::kDamaged},
      {"a style record longer than any is", longStyle, 1, 0, SegmentStatus::kDamaged},
  };
  for (const Case& made : cases) {
    SegmentFixture f;
    f.MakeBundle(1);
    CommitOneBlock(f, made.block, made.characters, made.languages);
    ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk) << made.what;
    ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
    WantBlock got;
    EXPECT_EQ(f.ReadWholeBlock(got), made.status) << made.what;
    if (made.status == SegmentStatus::kOk) {
      EXPECT_EQ(got.characters, made.characters) << made.what;
      EXPECT_EQ(f.ReadWholeBlock(got), SegmentStatus::kEnd) << made.what;
    }
  }
}

TEST(SegmentReader, PassesOverAnItemOfAKindItDoesNotKnowAndGivesABlockOfOneAsItsNumber) {
  SegmentFixture f;
  f.MakeBundle(1);
  // A block of the kind 9, with an item of the kind 12 between two of
  // text: the item is part of the block, and of its checksum.
  CommitOneBlock(f, {9, 0, 0, 0, 1, 0, 1, 0, 'a', 12, 7, 3, 0, '?', '?', '?', 1, 0, 1, 0, 'b'}, 2);
  ASSERT_EQ(f.OpenChapter(0), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.UnknownItems(), 0U);
  WantBlock got;
  ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk);
  EXPECT_EQ(got.kind, 9);
  EXPECT_STREQ(bn::epub::SegmentBlockKindName(got.kind), "?");
  ASSERT_EQ(got.items.size(), 1U);
  EXPECT_EQ(got.items[0].text, "ab");
  EXPECT_EQ(got.characters, 2U);
  EXPECT_EQ(f.reader.UnknownItems(), 1U);
  // The index says the kind too.
  ASSERT_EQ(f.reader.FindBlock(0), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.Place().kind, 9);
}

TEST(SegmentReader, RefusesARecordThatIsOfNoSegmentAndWhatIsAskedOutOfTurn) {
  SegmentFixture f;
  f.MakeBundle(2);
  f.WriteAndCommit(0, MakeChapter(41, 20));
  BundleChapter record;
  ASSERT_EQ(f.bundle.ReadChapter(0, record), BundleStatus::kOk);
  // Nothing of a segment that is not open.
  SegmentBlock block;
  SegmentItem item;
  EXPECT_EQ(f.reader.SeekStart(), SegmentStatus::kMisused);
  EXPECT_EQ(f.reader.ReadBlock(block), SegmentStatus::kMisused);
  EXPECT_EQ(f.reader.FindBlock(0), SegmentStatus::kMisused);
  EXPECT_FALSE(f.reader.Step());
  EXPECT_EQ(f.reader.Language(1), "");

  // A chapter that was not imported has no segment to open.
  BundleChapter other;
  ASSERT_EQ(f.bundle.ReadChapter(1, other), BundleStatus::kOk);
  EXPECT_EQ(f.reader.Open(f.file, other), SegmentStatus::kMisused);
  // A record that says what cannot be.
  const auto opens = [&f](BundleChapter changed) { return f.reader.Open(f.file, changed); };
  BundleChapter changed = record;
  changed.segmentOffset = static_cast<std::uint32_t>(f.OnCard().size());
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged) << "a segment past the file's end";
  changed = record;
  changed.directoryOffset = record.segmentBytes - 10;
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged) << "a directory that runs out of its segment";
  changed = record;
  changed.directoryBytes = 19;
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged) << "a directory too short to be one";
  changed = record;
  changed.directoryChecksum ^= 1U;
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged);
  changed = record;
  ++changed.blocks;
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged) << "a record and a directory that do not agree";
  changed = record;
  ++changed.characters;
  EXPECT_EQ(opens(changed), SegmentStatus::kDamaged);
  EXPECT_FALSE(f.reader.IsOpen());

  // Open: one thing at a time, and an item only of a block whose head was
  // read.
  ASSERT_EQ(f.reader.BeginOpen(f.file, record), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.SeekStart(), SegmentStatus::kMisused) << "while it is being opened";
  while (f.reader.Step()) {
  }
  ASSERT_EQ(f.reader.Status(), SegmentStatus::kOk);
  ASSERT_TRUE(f.reader.IsOpen());
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.NextItem(item), SegmentStatus::kMisused);
  ASSERT_EQ(f.reader.ReadBlock(block), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.ReadBlock(block), SegmentStatus::kMisused) << "its items are not read yet";
  ASSERT_EQ(f.reader.BeginFindBlock(3), SegmentStatus::kOk);
  EXPECT_EQ(f.reader.BeginFindBlock(4), SegmentStatus::kMisused) << "while something is being looked for";
  EXPECT_EQ(f.reader.NextItem(item), SegmentStatus::kMisused);
  SegmentPicture picture;
  bool found = false;
  EXPECT_EQ(f.reader.ReadPicture(1, picture, found), SegmentStatus::kMisused);
  while (f.reader.Step()) {
  }
  EXPECT_EQ(f.reader.Place().block, 3U);
  SegmentPlace nowhere;
  EXPECT_EQ(f.reader.Seek(nowhere), SegmentStatus::kMisused) << "a place that was not found";
  f.reader.Close();
  EXPECT_FALSE(f.reader.IsOpen());
}

TEST(SegmentReader, EndsInAStatusWhenStorageFails) {
  SegmentFixture f;
  f.MakeBundle(1);
  const std::vector<Op> ops = MakeChapter(51, 400);
  const Want want = Expected(ops);
  f.WriteAndCommit(0, ops);
  BundleChapter record;
  ASSERT_EQ(f.bundle.ReadChapter(0, record), BundleStatus::kOk);
  f.storage.failReads = true;
  // The file's own block may hold what is asked for: so each is asked
  // from a file opened afresh.
  const auto fresh = [&f] {
    f.storage.failReads = false;
    EXPECT_EQ(f.OpenBundle(), BundleStatus::kOk);
  };
  fresh();
  f.storage.failReads = true;
  EXPECT_EQ(f.reader.Open(f.file, record), SegmentStatus::kStoreFailed);
  EXPECT_FALSE(f.reader.IsOpen());
  fresh();
  ASSERT_EQ(f.reader.Open(f.file, record), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.FindBlock(399), SegmentStatus::kOk);
  const SegmentPlace last = f.reader.Place();
  f.storage.failReads = true;
  EXPECT_EQ(f.reader.FindBlock(0), SegmentStatus::kStoreFailed);
  EXPECT_EQ(f.reader.FindCharacters(1), SegmentStatus::kStoreFailed);
  EXPECT_EQ(f.reader.FindSource(100), SegmentStatus::kStoreFailed);
  EXPECT_EQ(f.reader.FindAnchor(want.anchors.at(0).name), SegmentStatus::kStoreFailed);
  EXPECT_TRUE(f.reader.IsOpen()) << "the segment is still the one that was opened";
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  SegmentBlock block;
  EXPECT_EQ(f.reader.ReadBlock(block), SegmentStatus::kStoreFailed);
  f.storage.failReads = false;
  ASSERT_EQ(f.reader.Seek(last), SegmentStatus::kOk);
  ASSERT_EQ(f.reader.ReadBlock(block), SegmentStatus::kOk);
  // And storage working again, the reading goes on from where it was.
  f.storage.failReads = false;
  ASSERT_EQ(f.reader.SeekStart(), SegmentStatus::kOk);
  WantBlock got;
  ASSERT_EQ(f.ReadWholeBlock(got), SegmentStatus::kOk);
  ExpectSameBlock(got, want.blocks.at(0), 0);
}

TEST(Segment, AChapterIsThereOrNotWheneverThePowerGoes) {
  // Two chapters imported, the power going at every change of the card
  // while the second is written and committed. What is left opens as a
  // bundle; the first chapter reads as it was written; and the second is
  // either not imported or reads whole.
  const std::vector<Op> first = MakeChapter(61, 25);
  const std::vector<Op> second = MakeChapter(62, 35);
  const Want wantFirst = Expected(first);
  const Want wantSecond = Expected(second);
  int changes = 0;
  {
    SegmentFixture f;
    f.MakeBundle(3);
    f.WriteAndCommit(0, first);
    const int before = f.storage.syncs + static_cast<int>(f.storage.written.size());
    f.WriteAndCommit(2, second);
    changes = f.storage.syncs + static_cast<int>(f.storage.written.size()) - before;
  }
  ASSERT_GT(changes, 10);
  int imported = 0;
  for (int cut = 0; cut <= changes; ++cut) {
    for (const FakeStorage::Unsynced unsynced :
         {FakeStorage::Unsynced::kLost, FakeStorage::Unsynced::kKept, FakeStorage::Unsynced::kInPlace}) {
      SegmentFixture f;
      f.MakeBundle(3);
      f.WriteAndCommit(0, first);
      f.storage.unsynced = unsynced;
      f.storage.tornBytes = static_cast<std::size_t>(cut) * 97 % 4096;
      f.storage.tornSpoilsSectors = cut % 2 == 0;
      f.storage.changesUntilPowerOff = cut;
      if (f.Write(second) == SegmentStatus::kOk) {
        static_cast<void>(f.bundle.Commit(2, f.writer.Chapter()));
      }
      f.file.Close();
      f.storage.PowerOn();
      ASSERT_EQ(f.OpenBundle(), BundleStatus::kOk) << "cut " << cut;
      const auto readsAs = [&f](std::size_t chapter, const Want& want) {
        if (f.OpenChapter(chapter) != SegmentStatus::kOk || f.reader.SeekStart() != SegmentStatus::kOk) {
          return false;
        }
        for (const WantBlock& wanted : want.blocks) {
          WantBlock got;
          if (f.ReadWholeBlock(got) != SegmentStatus::kOk || got.items != wanted.items) {
            return false;
          }
        }
        return true;
      };
      EXPECT_TRUE(readsAs(0, wantFirst)) << "the chapter committed before was lost at cut " << cut;
      BundleChapter record;
      ASSERT_EQ(f.bundle.ReadChapter(2, record), BundleStatus::kOk);
      if (record.state == ChapterState::kNotImported) {
        continue;
      }
      ++imported;
      EXPECT_TRUE(readsAs(2, wantSecond)) << "a chapter that is there is not whole, at cut " << cut;
    }
  }
  EXPECT_GT(imported, 0);
}

}  // namespace
