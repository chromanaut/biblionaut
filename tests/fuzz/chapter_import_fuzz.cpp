// The fuzz target for the importing of a chapter (epub/chapter_import.h):
// a document of a book made into a segment of a bundle, and read back.
//
// What it is given:
//
//   byte 0   bits 0 to 1: how many bytes a file gives at one reading
//            bits 2 to 3: how much memory the parser has
//            bits 4 to 5: how much the table of rules has
//            bits 6 to 7: which language is the book's own
//   byte 1   bits 0 to 5: after how many times 16 bytes the document
//                         fails, 0 for never
//            bits 6 to 7: at which of its writes storage fails, 0 for none
//   the rest up to four files of a book, a byte of zero between one and
//            the next. A file is its path, a line end, and what it holds.
//            The document is the first whose path ends in ".xhtml".
//
// The document is imported into a bundle made for it, committed, and read
// back with the segment's reader. What has to hold, whatever the bytes:
//
//   - the importing ends, in a number of steps that the book's size
//     bounds;
//   - a step opens one file at the most, reads kMostBytesPerStep bytes of
//     the book at the most, and does not do both; and it writes
//     kMostPagesPerStep pages at the most;
//   - once it has ended, a step does nothing;
//   - if storage did not fail it ends well, with a chapter that is whole,
//     cut short or refused: whole only if the document was read to its
//     end, refused with no segment, and cut short or refused for a reason
//     there is;
//   - a chapter that was imported reads back: every checksum holds, its
//     blocks and characters are those its record counts, and where each
//     block began is in the document and not before where the last did;
//   - a block is of a kind importing writes, and a heading alone has a
//     level; its items are text, style items and pictures; its characters
//     are those of its items, or one if it has none;
//   - text is whole characters of UTF-8, with no byte below a space but a
//     tab, a line feed and a return;
//   - a style item is followed by text, says something other than the one
//     before it did (the first, than plain text), names bits, a family
//     and a language that there are, and calls no bit the book's that is
//     not set;
//   - a picture is one the book does not have, in a line, with text of no
//     more than kSegmentMostName bytes standing for it;
//   - a block that says its first characters are a dropped capital has
//     that many characters at the least, and begins with text or with a
//     picture;
//   - an id is in a block there is, or after the last, no further into it
//     than it is long, and the ids are in the order of the document;
//   - a document that is XML to its end is so whatever pieces it is read
//     in, and comes to the same chapter, where the parser had memory enough
//     both times. (Of one that goes wrong, how much is told of the text
//     just before, and where and for what it is said to go wrong, go with
//     the pieces the parser was given.)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_fakes.h"
#include "core/block_file.h"
#include "core/byte_source.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/chapter_import.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/segment.h"
#include "epub/xml.h"
#include "fuzz_support.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::Bundle;
using bn::epub::BundleChapter;
using bn::epub::BundleStatus;
using bn::epub::ChapterImporter;
using bn::epub::ChapterState;
using bn::epub::ImportReason;
using bn::epub::ImportStatus;
using bn::epub::ItemKind;
using bn::epub::RunStyle;
using bn::epub::SegmentBlock;
using bn::epub::SegmentBlockKind;
using bn::epub::SegmentItem;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::test::FakeBook;
using bn::test::FakeStorage;

constexpr std::array<std::size_t, 4> kPieceBytes = {FakeBook::kNever, 300, 7, 1};
constexpr std::array<std::size_t, 4> kParserBytes = {65536, 24576, 8192, 3072};
constexpr std::array<std::size_t, 4> kTableBytes = {16384, 2048, 200, 40};
constexpr std::array<std::string_view, 4> kLanguages = {"", "en", "fr-CA", "a-tag-that-is-too-long-to-be-one"};
constexpr std::size_t kMostFiles = 4;
// More steps than any book of this size can take.
constexpr std::size_t kMostSteps = 400000;
const char* const kPath = "/book.bundle";

// A document of the fake book, by its name.
class Document final : public bn::epub::ChapterDocument {
 public:
  Document(FakeBook& book, std::string_view name) : book_(book), name_(name) {}

  [[nodiscard]] bn::core::ByteSource* Open(std::span<char> path, std::size_t& length) noexcept override {
    if (name_.size() > path.size()) {
      return nullptr;
    }
    std::copy(name_.begin(), name_.end(), path.begin());
    length = name_.size();
    return book_.Open(name_);
  }

 private:
  FakeBook& book_;
  std::string name_;
};

// Whether text is whole characters of UTF-8, as far as their lengths go,
// with no byte below a space but the three white space may be.
bool IsText(std::string_view text) {
  std::size_t at = 0;
  while (at < text.size()) {
    const auto first = static_cast<std::uint8_t>(text[at]);
    if (first < 0x20 && first != '\t' && first != '\n' && first != '\r') {
      return false;
    }
    const std::size_t length = first < 0x80 ? 1 : first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 0;
    if (length == 0 || at + length > text.size()) {
      return false;
    }
    for (std::size_t i = 1; i < length; ++i) {
      if ((static_cast<std::uint8_t>(text[at + i]) & 0xC0U) != 0x80U) {
        return false;
      }
    }
    at += length;
  }
  return true;
}

// What came of importing the document: how it ended, and the chapter as
// text, a line to each thing, for one run to be set beside another.
struct Outcome {
  ImportStatus status = ImportStatus::kMisused;
  BundleChapter chapter;
  ImportReason reason = ImportReason::kNone;
  std::string told;
};

// Reads a chapter back, checking what any chapter has to be.
std::string ReadBack(BlockFile& file, const BundleChapter& chapter, std::size_t documentBytes) {
  alignas(4) static std::array<std::uint8_t, 4096> page{};
  SegmentReader reader(page);
  BN_FUZZ_CHECK(reader.Open(file, chapter) == SegmentStatus::kOk, "a chapter that was imported does not open");
  BN_FUZZ_CHECK(reader.Blocks() == chapter.blocks && reader.Characters() == chapter.characters,
                "the chapter's record and its directory do not count the same");
  BN_FUZZ_CHECK(reader.SourceBytes() <= documentBytes, "the document is said to be longer than it is");
  BN_FUZZ_CHECK(reader.Languages() <= bn::epub::kSegmentLanguages, "more languages than a chapter may name");
  std::string told = "chapter " + std::to_string(static_cast<unsigned>(chapter.state)) + " " +
                     std::to_string(chapter.reason) + " " + std::to_string(reader.SourceBytes()) + "\n";
  for (std::size_t number = 1; number <= reader.Languages(); ++number) {
    told += "language " + std::string(reader.Language(number)) + "\n";
  }

  // Where each block began, which the index alone has.
  std::vector<std::uint32_t> sources;
  for (std::uint32_t number = 0; number < reader.IndexedBlocks(); ++number) {
    BN_FUZZ_CHECK(reader.FindBlock(number) == SegmentStatus::kOk && reader.Place().found, "a block is not found");
    sources.push_back(reader.Place().source);
    BN_FUZZ_CHECK(sources.back() < documentBytes, "a block began outside the document");
    BN_FUZZ_CHECK(number == 0 || sources[number - 1] <= sources.back(), "a block began before the one before it");
  }

  std::vector<std::uint32_t> lengths;
  std::uint32_t characters = 0;
  BN_FUZZ_CHECK(reader.SeekStart() == SegmentStatus::kOk, "the chapter's start is not found");
  for (;;) {
    SegmentBlock block;
    SegmentStatus status = SegmentStatus::kMore;
    while (status == SegmentStatus::kMore) {
      status = reader.ReadBlock(block);
    }
    if (status == SegmentStatus::kEnd) {
      break;
    }
    BN_FUZZ_CHECK(status == SegmentStatus::kOk, "a block does not read");
    const auto kind = static_cast<SegmentBlockKind>(block.kind);
    BN_FUZZ_CHECK(kind == SegmentBlockKind::kParagraph || kind == SegmentBlockKind::kHeading ||
                      kind == SegmentBlockKind::kListItem || kind == SegmentBlockKind::kRule ||
                      kind == SegmentBlockKind::kTableCell,
                  "a block of a kind importing does not write");
    BN_FUZZ_CHECK(kind == SegmentBlockKind::kHeading ? block.level >= 1 && block.level <= 6 : block.level == 0,
                  "a level that is not a heading's, or a heading with none");
    BN_FUZZ_CHECK(block.charactersBefore == characters, "the characters before a block are not those of the blocks");
    const unsigned dropped =
        (std::uint32_t{block.style.breaking} & bn::epub::kBreakDropped) >> bn::epub::kBreakDroppedShift;
    told += "block " + std::to_string(block.kind) + " " + std::to_string(block.level) + " " +
            std::to_string(lengths.size() < sources.size() ? sources[lengths.size()] : 0) + " " +
            std::to_string(block.style.has) + " " + std::to_string(block.style.fromBook) + " " +
            std::to_string(block.style.breaking) + " " + std::to_string(block.style.size) + "\n";

    std::uint32_t counted = 0;
    RunStyle run;
    bool styled = false;  // The last item was a style item.
    bool first = true;
    for (bool ended = false; !ended;) {
      SegmentItem item;
      status = reader.NextItem(item);
      if (status == SegmentStatus::kMore) {
        continue;
      }
      BN_FUZZ_CHECK(status == SegmentStatus::kOk, "an item does not read");
      BN_FUZZ_CHECK(!styled || item.kind == ItemKind::kText, "a style item with no text after it");
      BN_FUZZ_CHECK(kind != SegmentBlockKind::kRule || item.kind == ItemKind::kEnd, "a rule with something in it");
      styled = false;
      switch (item.kind) {
        case ItemKind::kText:
          BN_FUZZ_CHECK(!item.text.empty(), "an item of no text");
          BN_FUZZ_CHECK(IsText(item.text), "text that is not whole characters, or has a byte no text has");
          counted += static_cast<std::uint32_t>(item.text.size());
          told += "text " + std::string(item.text) + "\n";
          break;
        case ItemKind::kStyle:
          BN_FUZZ_CHECK(!(item.style == run), "a style item that says what the one before it said");
          BN_FUZZ_CHECK((item.style.bits & ~std::uint32_t{0x7F}) == 0 &&
                            (std::uint32_t{item.style.fromBook} & ~std::uint32_t{item.style.bits}) == 0,
                        "a style item with a bit there is none of, or the book's where none is set");
          BN_FUZZ_CHECK(item.style.familyClass < bn::epub::kCssFamilyClasses, "a family there is none of");
          BN_FUZZ_CHECK(item.style.language <= reader.Languages(), "a language the chapter does not name");
          run = item.style;
          styled = true;
          told += "style " + std::to_string(run.bits) + " " + std::to_string(run.fromBook) + " " +
                  std::to_string(run.familyClass) + (run.familyFromBook ? "*" : "") + " " +
                  std::to_string(run.language) + " " + std::to_string(run.size) + "\n";
          break;
        case ItemKind::kPicture:
          BN_FUZZ_CHECK(item.picture == bn::epub::kSegmentNoPicture && item.pictureBits == bn::epub::kPictureInLine,
                        "a picture that is not one the book has not, in a line");
          BN_FUZZ_CHECK(item.text.size() <= bn::epub::kSegmentMostName, "too much text standing for a picture");
          ++counted;
          told += "picture " + std::string(item.text) + "\n";
          break;
        case ItemKind::kEnd:
          BN_FUZZ_CHECK(item.characters == std::max<std::uint32_t>(counted, 1),
                        "a block's characters are not those of its items");
          BN_FUZZ_CHECK(dropped <= counted, "more characters dropped than the block has");
          ended = true;
          break;
        default:
          BN_FUZZ_CHECK(false, "an item importing does not write");
          break;
      }
      BN_FUZZ_CHECK(dropped == 0 || !first || item.kind == ItemKind::kStyle || item.kind == ItemKind::kPicture ||
                        item.kind == ItemKind::kText,
                    "a block that says it begins with a capital and begins with nothing");
      first = first && item.kind == ItemKind::kStyle;
    }
    lengths.push_back(std::max<std::uint32_t>(counted, 1));
    characters += lengths.back();
  }
  BN_FUZZ_CHECK(lengths.size() == chapter.blocks && characters == chapter.characters,
                "the chapter's blocks and characters are not those that read back");
  BN_FUZZ_CHECK(reader.UnknownItems() == 0, "an item of no kind known");

  // The ids, in the document's order.
  std::uint32_t lastBlock = 0;
  std::uint32_t lastInto = 0;
  for (std::size_t anchors = 0; anchors < reader.AnchorPages(); ++anchors) {
    BN_FUZZ_CHECK(reader.BeginReadAnchors(anchors) == SegmentStatus::kOk, "a page of ids does not begin");
    while (reader.Step()) {
    }
    BN_FUZZ_CHECK(reader.Status() == SegmentStatus::kOk, "a page of ids does not read");
    bn::epub::SegmentAnchor anchor;
    while (reader.NextAnchor(anchor)) {
      BN_FUZZ_CHECK(!anchor.name.empty() && anchor.name.size() <= bn::epub::kSegmentMostName,
                    "an id of no length there is");
      BN_FUZZ_CHECK(anchor.block <= lengths.size(), "an id in a block there is not");
      BN_FUZZ_CHECK(anchor.block == lengths.size() ? anchor.into == 0 : anchor.into <= lengths[anchor.block],
                    "an id further into its block than the block is long");
      BN_FUZZ_CHECK(anchor.block > lastBlock || (anchor.block == lastBlock && anchor.into >= lastInto),
                    "an id before the one before it");
      lastBlock = anchor.block;
      lastInto = anchor.into;
      told += "id " + std::string(anchor.name) + " " + std::to_string(anchor.block) + " " +
              std::to_string(anchor.into) + "\n";
    }
  }
  return told;
}

// Imports the book's document into a new bundle, holding every step to
// what a step may do.
Outcome Import(FakeBook& book, std::string_view name, std::size_t parserBytes, std::size_t tableBytes,
               std::string_view language, std::size_t failingWrite) {
  FakeStorage storage;
  BN_FUZZ_CHECK(storage.Start() == StorageStatus::kOk, "the storage did not start");
  alignas(4) static std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) static std::array<std::uint8_t, 4096> page{};
  BlockFile file(fileBuffer);
  {
    bn::epub::BundleMaker maker(page);
    bool made = file.Open(storage, kPath, OpenMode::kCreate) == StorageStatus::kOk &&
                maker.Begin(file) == BundleStatus::kOk &&
                maker.BeginTable(bn::epub::BundleTable::kStrings) == BundleStatus::kOk &&
                maker.BeginTable(bn::epub::BundleTable::kSpine) == BundleStatus::kOk &&
                maker.WriteFile(bn::epub::BundleFile{}) == BundleStatus::kOk &&
                maker.BeginTable(bn::epub::BundleTable::kPictures) == BundleStatus::kOk &&
                maker.BeginTable(bn::epub::BundleTable::kContents) == BundleStatus::kOk;
    bn::epub::BundleBook madeOf;
    madeOf.documents = 1;
    made = made && maker.Finish(madeOf) == BundleStatus::kOk;
    while (made && maker.Step()) {
    }
    BN_FUZZ_CHECK(made && maker.Status() == BundleStatus::kOk, "the bundle was not made");
    file.Close();
  }
  Bundle bundle(page);
  BN_FUZZ_CHECK(
      file.Open(storage, kPath, OpenMode::kUpdate) == StorageStatus::kOk && bundle.Open(file) == BundleStatus::kOk,
      "the bundle does not open");

  std::vector<std::uint8_t> parserMemory(parserBytes);
  bn::epub::XmlParser parser(parserMemory);
  bn::epub::CssReader reader;
  std::vector<std::uint8_t> tableMemory(tableBytes);
  bn::epub::CssRules rules(tableMemory);
  std::vector<char> names(bn::epub::DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style(bn::epub::DocumentSheets::kStyleBytes);
  bn::epub::DocumentSheets sheets(book, parser, reader, rules, names, style);
  sheets.BeginBook();
  // Its memory starts full of something that is nothing of its own.
  alignas(4) static std::array<std::uint8_t, ChapterImporter::kWorkBytes> work{};
  std::fill(work.begin(), work.end(), std::uint8_t{0xEE});
  ChapterImporter importer(rules, work);
  Document document(book, name);

  Outcome outcome;
  const std::size_t writesBefore = storage.written.size();
  BN_FUZZ_CHECK(importer.Begin(document, sheets, parser, file, bundle.DataEnd(), language) == ImportStatus::kOk,
                "importing did not begin");
  bool ended = false;
  for (std::size_t steps = 0; steps < kMostSteps && !ended; ++steps) {
    if (failingWrite != 0 && storage.written.size() - writesBefore + 1 >= failingWrite) {
      storage.failWrites = StorageStatus::kFailed;
    }
    const std::size_t opens = book.opens;
    const std::uint64_t bytes = book.bytesRead;
    const std::size_t written = storage.written.size();
    ended = !importer.Step();
    const std::size_t opened = book.opens - opens;
    const std::uint64_t read = book.bytesRead - bytes;
    BN_FUZZ_CHECK(opened <= 1, "a step opened more than one file");
    BN_FUZZ_CHECK(read <= ChapterImporter::kMostBytesPerStep, "a step read more of the book than a step may");
    BN_FUZZ_CHECK(opened == 0 || read == 0, "a step opened a file and read from one");
    BN_FUZZ_CHECK(storage.written.size() - written <= ChapterImporter::kMostPagesPerStep,
                  "a step wrote more pages than a step may");
  }
  BN_FUZZ_CHECK(ended, "importing did not end in any number of steps");
  const std::size_t writes = storage.written.size();
  BN_FUZZ_CHECK(!importer.Step() && storage.written.size() == writes, "a step did something after the end");

  outcome.status = importer.Status();
  outcome.chapter = importer.Chapter();
  outcome.reason = importer.Reason();
  if (storage.failWrites != StorageStatus::kOk) {
    BN_FUZZ_CHECK(outcome.status == ImportStatus::kStoreFailed || outcome.status == ImportStatus::kOk,
                  "storage failed, and importing ended some other way");
    return outcome;
  }
  BN_FUZZ_CHECK(outcome.status == ImportStatus::kOk, "importing did not end well, with storage that did not fail");
  const BundleChapter& chapter = outcome.chapter;
  BN_FUZZ_CHECK(chapter.state == ChapterState::kWhole || chapter.state == ChapterState::kCutShort ||
                    chapter.state == ChapterState::kRefused,
                "a chapter that is neither whole, cut short nor refused");
  BN_FUZZ_CHECK(
      chapter.reason < bn::epub::kImportReasons && chapter.reason == static_cast<std::uint8_t>(outcome.reason),
      "a reason there is none of, or not the one the importer gives");
  BN_FUZZ_CHECK((chapter.state == ChapterState::kWhole) == (outcome.reason == ImportReason::kNone),
                "a whole chapter with a reason, or one that is not whole with none");
  BN_FUZZ_CHECK(chapter.state != ChapterState::kWhole || parser.Finished(),
                "a whole chapter of a document that was not read to its end");
  BN_FUZZ_CHECK(bundle.Commit(0, chapter) == BundleStatus::kOk, "the chapter was not committed");
  if (chapter.state == ChapterState::kRefused) {
    BN_FUZZ_CHECK(chapter.segmentBytes == 0 && chapter.blocks == 0 && chapter.characters == 0,
                  "a chapter that was refused has a segment");
    outcome.told = "refused " + std::to_string(chapter.reason) + "\n";
    return outcome;
  }
  BN_FUZZ_CHECK(chapter.state == ChapterState::kWhole || chapter.blocks > 0, "a chapter cut short with nothing in it");
  outcome.told = ReadBack(file, chapter, book.Contents(name).size());
  return outcome;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const unsigned failing = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  std::string_view text(reinterpret_cast<const char*>(rest.data()), rest.size());

  // The files: a path, a line end, and the rest.
  FakeBook book;
  std::string document;
  std::size_t files = 0;
  while (!text.empty() && files < kMostFiles) {
    const std::size_t zero = std::min(text.find('\0'), text.size());
    std::string_view file = text;
    file.remove_suffix(text.size() - zero);
    text.remove_prefix(std::min(zero + 1, text.size()));
    const std::size_t line = std::min(file.find('\n'), file.size());
    std::string_view path = file;
    path.remove_suffix(file.size() - line);
    file.remove_prefix(std::min(line + 1, file.size()));
    if (path.empty() || book.Has(path)) {
      continue;
    }
    book.Put(path, file);
    ++files;
    if (document.empty() && path.ends_with(".xhtml")) {
      document = path;
    }
  }
  if (document.empty()) {
    return 0;
  }
  if ((failing & 0x3FU) != 0) {
    book.FailAfter(document, std::size_t{failing & 0x3FU} * 16);
  }
  const std::size_t piece = kPieceBytes[choices & 3U];
  const std::size_t parserBytes = kParserBytes[(choices >> 2U) & 3U];
  const std::size_t tableBytes = kTableBytes[(choices >> 4U) & 3U];
  const std::string_view language = kLanguages[(choices >> 6U) & 3U];
  // The first write, the third, or the ninth.
  const std::array<std::size_t, 4> failingWrites = {0, 1, 3, 9};
  const std::size_t failingWrite = failingWrites[(failing >> 6U) & 3U];

  book.pieceBytes = piece;
  const Outcome outcome = Import(book, document, parserBytes, tableBytes, language, failingWrite);

  // Read in pieces of another size, a document that is XML to its end is
  // the same chapter: where the parser had all the memory a document of
  // this size can ask for both times, since how much it asks for goes with
  // the pieces it is given. A document that goes wrong is not set beside
  // itself: a parser looks at what it has been given a piece at a time, so
  // how much it tells of the text just before the place, where it says the
  // place is, and whether it comes first to where a document cannot be
  // read or to where it is not XML, all go with the pieces.
  if (failingWrite == 0 && (failing & 0x3FU) == 0 && parserBytes == kParserBytes[0]) {
    book.pieceBytes = piece == FakeBook::kNever ? 5 : FakeBook::kNever;
    const Outcome again = Import(book, document, parserBytes, tableBytes, language, 0);
    const bool memory = outcome.reason == ImportReason::kNoMemory || again.reason == ImportReason::kNoMemory;
    const bool whole = outcome.chapter.state == ChapterState::kWhole;
    BN_FUZZ_CHECK(memory || whole == (again.chapter.state == ChapterState::kWhole),
                  "a document is XML to its end read in pieces of one size and not of another");
    BN_FUZZ_CHECK(memory || !whole || outcome.told == again.told,
                  "the book read in pieces of another size comes to another chapter");
  }
  return 0;
}
