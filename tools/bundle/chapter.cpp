#include "chapter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/segment.h"
#include "support/one_file.h"

namespace bn::tools {

namespace {

using core::StorageStatus;
using epub::BlockStyle;
using epub::Bundle;
using epub::BundleChapter;
using epub::BundleStatus;
using epub::ChapterState;
using epub::ItemKind;
using epub::LinkKind;
using epub::RunStyle;
using epub::SegmentAnchor;
using epub::SegmentBlock;
using epub::SegmentBlockKind;
using epub::SegmentItem;
using epub::SegmentPicture;
using epub::SegmentPlace;
using epub::SegmentReader;
using epub::SegmentStatus;
using epub::SegmentWriter;
using epub::StyleLength;

// A bundle's file, open, with the bundle it is.
class OpenBundle {
 public:
  OpenBundle(const std::string& path, bool toChange) {
    core::Storage* storage = nullptr;
    if (toChange) {
      storage = &changed_.emplace(path);
    } else {
      storage = &read_.emplace(path);
    }
    if (storage->Start() != StorageStatus::kOk ||
        file.Open(*storage, "/bundle", toChange ? core::OpenMode::kUpdate : core::OpenMode::kRead) !=
            StorageStatus::kOk) {
      std::fprintf(stderr, "error: cannot %s %s\n", toChange ? "change" : "read", path.c_str());
      return;
    }
    if (const BundleStatus status = bundle.Open(file); status != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: %s\n", path.c_str(), epub::BundleStatusName(status));
      return;
    }
    ok = true;
  }

  // Reads the record of a chapter that was imported. False, with why
  // said, if it was not or there is none.
  bool Imported(std::size_t number, BundleChapter& record) {
    if (number >= bundle.Book().documents || bundle.ReadChapter(number, record) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: the bundle has no chapter %zu\n", number);
      return false;
    }
    if (record.state != ChapterState::kWhole && record.state != ChapterState::kCutShort) {
      std::fprintf(stderr, "error: chapter %zu is not imported\n", number);
      return false;
    }
    return true;
  }

 private:
  // The file, as one that is read or as one that is changed.
  std::optional<OneFile> read_;
  std::optional<OneNewFile> changed_;
  std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(epub::kBundlePageBytes);
  std::vector<std::uint8_t> page_ = std::vector<std::uint8_t>(epub::kBundlePageBytes);

 public:
  core::BlockFile file{buffer_};
  Bundle bundle{page_};
  bool ok = false;
};

// The names of a block's fields, in the order of their bits.
constexpr std::array<std::string_view, 15> kFieldNames = {"align",  "direction", "size",   "line",  "indent",
                                                          "left",   "right",     "before", "after", "breaking",
                                                          "depths", "marker",    "table",  "width", "height"};
constexpr std::array<std::string_view, 3> kLinkNames = {"inside", "note", "outside"};
constexpr std::array<std::string_view, 6> kKindNames = {"paragraph", "heading", "list-item",
                                                        "picture",   "rule",    "table-cell"};

void AddNumbers(std::string& out, std::initializer_list<long long> numbers) {
  bool first = true;
  for (const long long number : numbers) {
    out.append(first ? "" : ",").append(std::to_string(number));
    first = false;
  }
}

void AddLength(std::string& out, const StyleLength& length) { AddNumbers(out, {length.em, length.px, length.percent}); }

// A block's style as the fields of its line.
std::string Fields(const BlockStyle& style) {
  std::string out;
  for (std::size_t field = 0; field < kFieldNames.size(); ++field) {
    const auto bit = static_cast<std::uint16_t>(1U << field);
    if ((style.has & bit) == 0) {
      continue;
    }
    out.append(" ").append(kFieldNames.at(field)).append("=");
    switch (bit) {
      case epub::kBlockAlign:
        AddNumbers(out, {style.align});
        break;
      case epub::kBlockDirection:
        AddNumbers(out, {style.direction});
        break;
      case epub::kBlockSize:
        AddNumbers(out, {style.size});
        break;
      case epub::kBlockLineHeight:
        AddNumbers(out, {style.lineMultiple, style.lineHeight.em, style.lineHeight.px, style.lineHeight.percent});
        break;
      case epub::kBlockIndent:
        AddLength(out, style.indent);
        break;
      case epub::kBlockMarginLeft:
        AddLength(out, style.marginLeft);
        break;
      case epub::kBlockMarginRight:
        AddLength(out, style.marginRight);
        break;
      case epub::kBlockSpaceBefore:
        AddLength(out, style.spaceBefore);
        break;
      case epub::kBlockSpaceAfter:
        AddLength(out, style.spaceAfter);
        break;
      case epub::kBlockBreaking:
        AddNumbers(out, {style.breaking});
        break;
      case epub::kBlockDepths:
        AddNumbers(out, {style.quoteDepth, style.listDepth});
        break;
      case epub::kBlockMarker:
        AddNumbers(out, {style.marker, style.markerNumber});
        break;
      case epub::kBlockTable:
        AddNumbers(out, {style.table, style.row, style.column, style.rowSpan, style.columnSpan, style.tableBits,
                         style.tableDepth});
        break;
      case epub::kBlockWidth:
        AddLength(out, style.width);
        break;
      default:
        AddLength(out, style.height);
        break;
    }
    if ((style.fromBook & bit) != 0) {
      out.append("*");
    }
  }
  return out;
}

struct Id {
  std::uint32_t block = 0;
  std::uint32_t into = 0;
  std::string name;
};

// Does a piece of the reader's work to its end.
SegmentStatus Finish(SegmentReader& reader, SegmentStatus begun) {
  if (begun != SegmentStatus::kOk) {
    return begun;
  }
  while (reader.Step()) {
  }
  return reader.Status();
}

int Failed(const char* what, SegmentStatus status) {
  std::fprintf(stderr, "error: %s: %s\n", what, epub::SegmentStatusName(status));
  return 1;
}

// The words of a line, a space between each two; and the rest of a line
// after so many words, which is text and may have spaces of its own.
std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t space = std::min(line.find(' ', start), line.size());
    if (space > start) {
      words.push_back(line.substr(start, space - start));
    }
    start = space + 1;
  }
  return words;
}

std::string_view After(std::string_view line, std::size_t words) {
  std::size_t at = 0;
  for (std::size_t word = 0; word < words; ++word) {
    at = line.find(' ', at);
    if (at == std::string_view::npos) {
      return {};
    }
    ++at;
  }
  return line.substr(at);
}

bool Number(std::string_view word, long long low, long long high, long long& value) {
  if (word.empty() || word.size() > 12) {
    return false;
  }
  const std::string text(word);
  char* end = nullptr;
  value = std::strtoll(text.c_str(), &end, 0);
  return end != nullptr && *end == '\0' && value >= low && value <= high;
}

// Text as a line has it, back to its bytes.
bool Unescape(std::string_view text, std::string& out) {
  out.clear();
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text[at] != '\\') {
      out += text[at];
      continue;
    }
    long long byte = 0;
    if (at + 3 >= text.size() || text[at + 1] != 'x' ||
        !Number("0x" + std::string(text.substr(at + 2, 2)), 0, 255, byte)) {
      return false;
    }
    out += static_cast<char>(byte);
    at += 3;
  }
  return true;
}

// The numbers of a field's value, with commas between them, each one that
// fits in 32 bits with its sign.
bool Numbers(std::string_view value, std::vector<long long>& numbers) {
  numbers.clear();
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = std::min(value.find(',', start), value.size());
    long long number = 0;
    if (!Number(value.substr(start, comma - start), -2147483648LL, 4294967295LL, number)) {
      return false;
    }
    numbers.push_back(number);
    start = comma + 1;
  }
  return true;
}

StyleLength LengthOf(const std::vector<long long>& numbers, std::size_t from) {
  return {.em = static_cast<std::int16_t>(numbers.at(from)),
          .px = static_cast<std::int16_t>(numbers.at(from + 1)),
          .percent = static_cast<std::int16_t>(numbers.at(from + 2))};
}

// Reads one field of a block's line into `style`.
bool ReadField(std::string_view word, BlockStyle& style) {
  const std::size_t equals = word.find('=');
  if (equals == std::string_view::npos) {
    return false;
  }
  const auto* const name = std::find(kFieldNames.begin(), kFieldNames.end(), word.substr(0, equals));
  if (name == kFieldNames.end()) {
    return false;
  }
  const auto bit = static_cast<std::uint16_t>(1U << static_cast<unsigned>(name - kFieldNames.begin()));
  std::string_view value = word.substr(equals + 1);
  if (value.ends_with('*')) {
    style.fromBook |= bit;
    value.remove_suffix(1);
  }
  std::vector<long long> numbers;
  const std::array<std::size_t, 15> counts = {1, 1, 1, 4, 3, 3, 3, 3, 3, 1, 2, 2, 7, 3, 3};
  if (!Numbers(value, numbers) || numbers.size() != counts.at(static_cast<std::size_t>(name - kFieldNames.begin()))) {
    return false;
  }
  style.has |= bit;
  switch (bit) {
    case epub::kBlockAlign:
      style.align = static_cast<std::uint8_t>(numbers[0]);
      break;
    case epub::kBlockDirection:
      style.direction = static_cast<std::uint8_t>(numbers[0]);
      break;
    case epub::kBlockSize:
      style.size = static_cast<std::uint16_t>(numbers[0]);
      break;
    case epub::kBlockLineHeight:
      style.lineMultiple = static_cast<std::uint16_t>(numbers[0]);
      style.lineHeight = LengthOf(numbers, 1);
      break;
    case epub::kBlockIndent:
      style.indent = LengthOf(numbers, 0);
      break;
    case epub::kBlockMarginLeft:
      style.marginLeft = LengthOf(numbers, 0);
      break;
    case epub::kBlockMarginRight:
      style.marginRight = LengthOf(numbers, 0);
      break;
    case epub::kBlockSpaceBefore:
      style.spaceBefore = LengthOf(numbers, 0);
      break;
    case epub::kBlockSpaceAfter:
      style.spaceAfter = LengthOf(numbers, 0);
      break;
    case epub::kBlockBreaking:
      style.breaking = static_cast<std::uint16_t>(numbers[0]);
      break;
    case epub::kBlockDepths:
      style.quoteDepth = static_cast<std::uint8_t>(numbers[0]);
      style.listDepth = static_cast<std::uint8_t>(numbers[1]);
      break;
    case epub::kBlockMarker:
      style.marker = static_cast<std::uint8_t>(numbers[0]);
      style.markerNumber = static_cast<std::uint32_t>(numbers[1]);
      break;
    case epub::kBlockTable:
      style.table = static_cast<std::uint16_t>(numbers[0]);
      style.row = static_cast<std::uint16_t>(numbers[1]);
      style.column = static_cast<std::uint16_t>(numbers[2]);
      style.rowSpan = static_cast<std::uint8_t>(numbers[3]);
      style.columnSpan = static_cast<std::uint8_t>(numbers[4]);
      style.tableBits = static_cast<std::uint8_t>(numbers[5]);
      style.tableDepth = static_cast<std::uint8_t>(numbers[6]);
      break;
    case epub::kBlockWidth:
      style.width = LengthOf(numbers, 0);
      break;
    default:
      style.height = LengthOf(numbers, 0);
      break;
  }
  return true;
}

// Gives the writer text of any length, in pieces that end where a
// character does.
SegmentStatus GiveText(SegmentWriter& writer, std::string_view text) {
  constexpr std::size_t kPiece = 1000;
  SegmentStatus status = SegmentStatus::kOk;
  while (!text.empty() && status == SegmentStatus::kOk) {
    std::size_t take = std::min(kPiece, text.size());
    while (take < text.size() && take > kPiece - 4 && (static_cast<std::uint8_t>(text[take]) & 0xC0U) == 0x80U) {
      --take;
    }
    status = writer.Text(text.substr(0, take));
    text.remove_prefix(take);
  }
  return status;
}

// What a chapter's text says of the chapter as a whole.
struct Described {
  ChapterState state = ChapterState::kWhole;
  std::uint8_t reason = 0;
  std::uint32_t sourceBytes = 0;
  std::map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>> sizes;
};

// Does what one line of a chapter's text says. False if the line is not
// one that can be read; `status` is how the writer took it.
bool GiveLine(SegmentWriter& writer, std::string_view line, Described& described, SegmentStatus& status) {
  const std::vector<std::string_view> words = Words(line);
  if (words.empty() || words[0].starts_with('#')) {
    return true;
  }
  const std::string_view what = words[0];
  std::string text;
  std::vector<long long> numbers(words.size(), 0);
  // The numbers among the words from `from`, `count` of them, each in the
  // range given.
  const auto numbered = [&words, &numbers](std::size_t from, std::size_t count, long long low, long long high) {
    if (words.size() < from + count) {
      return false;
    }
    for (std::size_t i = from; i < from + count; ++i) {
      if (!Number(words[i], low, high, numbers[i])) {
        return false;
      }
    }
    return true;
  };
  if (what == "chapter") {
    if (words.size() != 4 || (words[1] != "whole" && words[1] != "cut-short") || !numbered(2, 1, 0, 255) ||
        !numbered(3, 1, 0, 4294967295LL)) {
      return false;
    }
    described.state = words[1] == "whole" ? ChapterState::kWhole : ChapterState::kCutShort;
    described.reason = static_cast<std::uint8_t>(numbers[2]);
    described.sourceBytes = static_cast<std::uint32_t>(numbers[3]);
    return true;
  }
  if (what == "language") {
    // A language the writer has no room for is the book's own: said here,
    // since the styles that name it by its number would be of another.
    return words.size() == 2 && writer.Language(words[1]) != 0;
  }
  if (what == "size") {
    if (words.size() != 4 || !numbered(1, 3, 0, 65535)) {
      return false;
    }
    described.sizes[static_cast<std::uint16_t>(numbers[1])] = {static_cast<std::uint16_t>(numbers[2]),
                                                               static_cast<std::uint16_t>(numbers[3])};
    return true;
  }
  if (what == "block") {
    if (words.size() < 4 || !numbered(2, 1, 0, 255)) {
      return false;
    }
    long long kind = 0;
    if (const auto* const name = std::find(kKindNames.begin(), kKindNames.end(), words[1]); name != kKindNames.end()) {
      kind = 1 + (name - kKindNames.begin());
    } else if (!Number(words[1], 1, 255, kind)) {
      return false;
    }
    // A block whose place in the source is not known is given none: the
    // writer takes it to begin where the block before it did.
    if (words[3] != "-" && !numbered(3, 1, 0, 4294967295LL)) {
      return false;
    }
    BlockStyle style;
    for (std::size_t i = 4; i < words.size(); ++i) {
      if (!ReadField(words[i], style)) {
        return false;
      }
    }
    status = writer.BeginBlock(static_cast<SegmentBlockKind>(kind), static_cast<std::uint8_t>(numbers[2]), style,
                               static_cast<std::uint32_t>(numbers[3]));
    return true;
  }
  if (what == "text") {
    if (!Unescape(After(line, 1), text)) {
      return false;
    }
    status = GiveText(writer, text);
    return true;
  }
  if (what == "style") {
    if (words.size() != 6 || !numbered(1, 2, 0, 65535) || !numbered(4, 1, 0, 255) || !numbered(5, 1, 0, 65535)) {
      return false;
    }
    std::string_view family = words[3];
    RunStyle style;
    style.familyFromBook = family.ends_with('*');
    family.remove_suffix(style.familyFromBook ? 1 : 0);
    long long familyClass = 0;
    if (!Number(family, 0, 255, familyClass)) {
      return false;
    }
    style.bits = static_cast<std::uint16_t>(numbers[1]);
    style.fromBook = static_cast<std::uint16_t>(numbers[2]);
    style.familyClass = static_cast<std::uint8_t>(familyClass);
    style.language = static_cast<std::uint8_t>(numbers[4]);
    style.size = static_cast<std::uint16_t>(numbers[5]);
    status = writer.Style(style);
    return true;
  }
  if (what == "link") {
    const auto* const name =
        words.size() < 3 ? kLinkNames.end() : std::find(kLinkNames.begin(), kLinkNames.end(), words[1]);
    if (name == kLinkNames.end() || !numbered(2, 1, 0, 65535) || !Unescape(After(line, 3), text)) {
      return false;
    }
    status = writer.LinkStart(static_cast<LinkKind>(name - kLinkNames.begin()), static_cast<std::uint16_t>(numbers[2]),
                              text);
    return true;
  }
  if (what == "endlink" && words.size() == 1) {
    status = writer.LinkEnd();
    return true;
  }
  if (what == "picture") {
    if (!numbered(1, 2, 0, 65535) || !numbered(3, 6, -32768, 32767) || !Unescape(After(line, 9), text)) {
      return false;
    }
    status = writer.Picture(static_cast<std::uint16_t>(numbers[1]), static_cast<std::uint16_t>(numbers[2]),
                            LengthOf(numbers, 3), LengthOf(numbers, 6), text);
    return true;
  }
  if (what == "id") {
    if (!Unescape(After(line, 1), text)) {
      return false;
    }
    status = writer.Anchor(text);
    return true;
  }
  if (what == "end" && words.size() == 1) {
    status = writer.EndBlock();
    return true;
  }
  return false;
}

}  // namespace

std::string Printable(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == '\\') {
      std::array<char, 8> escaped{};
      std::snprintf(escaped.data(), escaped.size(), "\\x%02X", static_cast<unsigned>(byte));
      out += escaped.data();
    } else {
      out += ch;
    }
  }
  return out;
}

int PrintChapter(const std::string& path, std::size_t number) {
  OpenBundle opened(path, false);
  BundleChapter record;
  if (!opened.ok || !opened.Imported(number, record)) {
    return 1;
  }
  std::vector<std::uint8_t> page(epub::kBundlePageBytes);
  SegmentReader reader(page);
  if (const SegmentStatus status = reader.Open(opened.file, record); status != SegmentStatus::kOk) {
    return Failed("the chapter's directory", status);
  }
  std::printf("chapter %s %u %u\n", record.state == ChapterState::kWhole ? "whole" : "cut-short",
              static_cast<unsigned>(record.reason), static_cast<unsigned>(reader.SourceBytes()));
  for (std::size_t language = 1; language <= reader.Languages(); ++language) {
    std::printf("language %s\n", Printable(reader.Language(language)).c_str());
  }
  for (std::size_t i = 0; i < reader.Pictures(); ++i) {
    SegmentPicture picture;
    if (const SegmentStatus status = reader.ReadPictureAt(i, picture); status != SegmentStatus::kOk) {
      return Failed("a picture's size", status);
    }
    if (picture.sized) {
      std::printf("size %u %u %u\n", static_cast<unsigned>(picture.number), static_cast<unsigned>(picture.width),
                  static_cast<unsigned>(picture.height));
    } else {
      std::printf("# picture %u has no size\n", static_cast<unsigned>(picture.number));
    }
  }

  // Every id, in the order the document has them: each is printed where
  // it is, in its block or between two.
  std::vector<Id> ids;
  for (std::size_t anchors = 0; anchors < reader.AnchorPages(); ++anchors) {
    if (const SegmentStatus status = Finish(reader, reader.BeginReadAnchors(anchors)); status != SegmentStatus::kOk) {
      return Failed("a page of ids", status);
    }
    SegmentAnchor anchor;
    while (reader.NextAnchor(anchor)) {
      ids.push_back({.block = anchor.block, .into = anchor.into, .name = std::string(anchor.name)});
    }
  }
  std::size_t nextId = 0;
  // Prints the ids of `block` that are no further into it than `into`.
  const auto printIds = [&ids, &nextId](std::uint32_t block, std::uint32_t into) {
    while (nextId < ids.size() && ids[nextId].block <= block &&
           (ids[nextId].block < block || ids[nextId].into <= into)) {
      std::printf("id %s\n", Printable(ids[nextId].name).c_str());
      ++nextId;
    }
  };

  bool indexAgrees = true;
  if (const SegmentStatus status = reader.SeekStart(); status != SegmentStatus::kOk) {
    return Failed("the chapter's start", status);
  }
  for (std::uint32_t number32 = 0;; ++number32) {
    // What the index says of the block, where it has it: where it began
    // in the source is nowhere else.
    SegmentPlace place;
    if (number32 < reader.IndexedBlocks()) {
      if (const SegmentStatus status = reader.FindBlock(number32); status != SegmentStatus::kOk) {
        return Failed("the index", status);
      }
      place = reader.Place();
    }
    SegmentBlock block;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      status = reader.ReadBlock(block);
    }
    if (status == SegmentStatus::kEnd) {
      break;
    }
    if (status != SegmentStatus::kOk) {
      return Failed("a block", status);
    }
    if (place.found && (place.offset != block.offset || place.charactersBefore != block.charactersBefore ||
                        place.kind != block.kind)) {
      indexAgrees = false;
      std::printf("# the index says other of the next block than the block does\n");
    }
    // An id at a block's very start is as one between it and the block
    // before: it is printed before the block.
    printIds(block.index, 0);
    const std::string kind = block.kind >= 1 && block.kind <= kKindNames.size()
                                 ? std::string(kKindNames.at(block.kind - 1U))
                                 : std::to_string(block.kind);
    std::printf("block %s %u %s%s\n", kind.c_str(), static_cast<unsigned>(block.level),
                place.found ? std::to_string(place.source).c_str() : "-", Fields(block.style).c_str());
    std::uint32_t into = 0;
    // Text that has been read and not printed: the items of one run of
    // text are printed as one line, or as several where an id is part way
    // through it.
    std::string run;
    const auto printRun = [&run, &into, &ids, &nextId, &printIds, &block] {
      std::string_view text = run;
      std::uint32_t at = into - static_cast<std::uint32_t>(run.size());
      while (!text.empty()) {
        std::size_t piece = text.size();
        if (nextId < ids.size() && ids[nextId].block == block.index && ids[nextId].into - at < piece) {
          piece = ids[nextId].into - at;
        }
        std::printf("text %s\n", Printable(text.substr(0, piece)).c_str());
        text.remove_prefix(piece);
        at += static_cast<std::uint32_t>(piece);
        printIds(block.index, at);
      }
      run.clear();
    };
    for (bool ended = false; !ended;) {
      SegmentItem item;
      status = reader.NextItem(item);
      if (status == SegmentStatus::kMore) {
        continue;
      }
      if (status != SegmentStatus::kOk) {
        return Failed("an item", status);
      }
      if (item.kind == ItemKind::kText) {
        if (run.empty()) {
          printIds(block.index, into);
        }
        run += item.text;
        into += static_cast<std::uint32_t>(item.text.size());
        continue;
      }
      printRun();
      printIds(block.index, into);
      switch (item.kind) {
        case ItemKind::kText:
          break;
        case ItemKind::kStyle:
          std::printf("style %u %u %u%s %u %u\n", static_cast<unsigned>(item.style.bits),
                      static_cast<unsigned>(item.style.fromBook), static_cast<unsigned>(item.style.familyClass),
                      item.style.familyFromBook ? "*" : "", static_cast<unsigned>(item.style.language),
                      static_cast<unsigned>(item.style.size));
          break;
        case ItemKind::kLinkStart:
          std::printf("link %s %u%s%s\n", std::string(kLinkNames.at(static_cast<std::size_t>(item.link))).c_str(),
                      static_cast<unsigned>(item.chapter), item.text.empty() ? "" : " ", Printable(item.text).c_str());
          break;
        case ItemKind::kLinkEnd:
          std::printf("endlink\n");
          break;
        case ItemKind::kPicture:
          std::printf("picture %u %u %d %d %d %d %d %d%s%s\n", static_cast<unsigned>(item.picture),
                      static_cast<unsigned>(item.pictureBits), item.width.em, item.width.px, item.width.percent,
                      item.height.em, item.height.px, item.height.percent, item.text.empty() ? "" : " ",
                      Printable(item.text).c_str());
          ++into;
          break;
        case ItemKind::kEnd:
          // Whatever ids the block has left are at its end.
          printIds(block.index, 0xFFFFFFFFU);
          std::printf("end\n");
          ended = true;
          break;
      }
    }
  }
  // And those after the last block.
  for (; nextId < ids.size(); ++nextId) {
    std::printf("id %s\n", Printable(ids[nextId].name).c_str());
  }

  std::printf(
      "# %u blocks, %u characters, %u left out, %zu index pages, %zu anchor pages, %zu ids, %u items of no "
      "kind known\n",
      static_cast<unsigned>(reader.Blocks()), static_cast<unsigned>(reader.Characters()),
      static_cast<unsigned>(record.leftOut), reader.IndexPages(), reader.AnchorPages(), ids.size(),
      static_cast<unsigned>(reader.UnknownItems()));
  std::printf("# text by class:");
  for (const std::uint32_t characters : record.classCharacters) {
    std::printf(" %u", static_cast<unsigned>(characters));
  }
  std::printf("\n# rows of characters:");
  for (std::size_t row = 0; row < 256; ++row) {
    if ((record.characterRows.at(row / 8) & (1U << (row % 8))) != 0) {
      std::printf(" %02zx", row);
    }
  }
  std::printf("\n");
  if (!indexAgrees) {
    std::fprintf(stderr, "error: the index and the blocks do not agree\n");
    return 1;
  }
  return 0;
}

int WriteChapter(const std::string& path, std::size_t number, const std::string& textPath) {
  std::ifstream in(textPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: cannot read %s\n", textPath.c_str());
    return 1;
  }
  OpenBundle opened(path, true);
  if (!opened.ok) {
    return 1;
  }
  if (number >= opened.bundle.Book().documents) {
    std::fprintf(stderr, "error: the bundle has no chapter %zu\n", number);
    return 1;
  }
  std::vector<std::uint8_t> memory(epub::kSegmentWriterBytes);
  SegmentWriter writer(memory);
  SegmentStatus status = writer.Begin(opened.file, opened.bundle.DataEnd());
  Described described;
  std::string line;
  for (std::size_t lineNumber = 1; status == SegmentStatus::kOk && std::getline(in, line); ++lineNumber) {
    if (!GiveLine(writer, line, described, status)) {
      std::fprintf(stderr, "error: %s:%zu: not a line of a chapter\n", textPath.c_str(), lineNumber);
      return 1;
    }
    if (status != SegmentStatus::kOk) {
      std::fprintf(stderr, "error: %s:%zu: %s\n", textPath.c_str(), lineNumber, epub::SegmentStatusName(status));
      return 1;
    }
  }
  const auto steps = [&writer](SegmentStatus begun) {
    if (begun != SegmentStatus::kOk) {
      return begun;
    }
    while (writer.Step()) {
    }
    return writer.Status();
  };
  status = status == SegmentStatus::kOk ? steps(writer.BeginEndBlocks()) : status;
  for (std::size_t i = 0; i < writer.Pictures() && status == SegmentStatus::kOk; ++i) {
    if (const auto size = described.sizes.find(writer.PictureNumber(i)); size != described.sizes.end()) {
      status = writer.SetPictureSize(i, size->second.first, size->second.second);
    }
  }
  status = status == SegmentStatus::kOk
               ? steps(writer.BeginFinish(described.state, described.reason, described.sourceBytes))
               : status;
  if (status != SegmentStatus::kOk) {
    return Failed("writing the chapter", status);
  }
  if (const BundleStatus committed = opened.bundle.Commit(number, writer.Chapter()); committed != BundleStatus::kOk) {
    std::fprintf(stderr, "error: committing the chapter: %s\n", epub::BundleStatusName(committed));
    return 1;
  }
  const BundleChapter& chapter = writer.Chapter();
  std::printf("blocks=%u characters=%u bytes=%u directory=%u left_out=%u pictures=%zu\n",
              static_cast<unsigned>(chapter.blocks), static_cast<unsigned>(chapter.characters),
              static_cast<unsigned>(chapter.segmentBytes), static_cast<unsigned>(chapter.directoryBytes),
              static_cast<unsigned>(chapter.leftOut), writer.Pictures());
  return 0;
}

int FindInChapter(const std::string& path, std::size_t number, const std::string& what, const std::string& value) {
  OpenBundle opened(path, false);
  BundleChapter record;
  if (!opened.ok || !opened.Imported(number, record)) {
    return 1;
  }
  std::vector<std::uint8_t> page(epub::kBundlePageBytes);
  SegmentReader reader(page);
  if (const SegmentStatus status = reader.Open(opened.file, record); status != SegmentStatus::kOk) {
    return Failed("the chapter's directory", status);
  }
  long long asked = 0;
  std::string name;
  SegmentStatus status = SegmentStatus::kMisused;
  if (what == "id" && Unescape(value, name)) {
    status = reader.FindAnchor(name);
  } else if (what != "id" && Number(value, 0, 4294967295LL, asked)) {
    const auto place = static_cast<std::uint32_t>(asked);
    status = what == "block"        ? reader.FindBlock(place)
             : what == "characters" ? reader.FindCharacters(place)
             : what == "source"     ? reader.FindSource(place)
                                    : SegmentStatus::kMisused;
  }
  if (status == SegmentStatus::kMisused) {
    std::fprintf(stderr, "error: a place is block NUMBER, characters NUMBER, source NUMBER or id NAME\n");
    return 2;
  }
  if (status != SegmentStatus::kOk) {
    return Failed("looking for the place", status);
  }
  const SegmentPlace& place = reader.Place();
  if (!place.found) {
    std::printf("none\n");
  } else if (place.atEnd) {
    std::printf("end block=%u before=%u\n", static_cast<unsigned>(place.block),
                static_cast<unsigned>(place.charactersBefore));
  } else {
    std::printf("block=%u offset=%u before=%u source=%u kind=%u breaking=%u into=%u\n",
                static_cast<unsigned>(place.block), static_cast<unsigned>(place.offset),
                static_cast<unsigned>(place.charactersBefore), static_cast<unsigned>(place.source),
                static_cast<unsigned>(place.kind), static_cast<unsigned>(place.breaking),
                static_cast<unsigned>(place.into));
  }
  return 0;
}

}  // namespace bn::tools
