#include "epub/chapter_import.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "epub/bundle.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_blocks.h"
#include "epub/document_sheets.h"
#include "epub/html_entities.h"
#include "epub/package.h"
#include "epub/segment.h"
#include "epub/style_stack.h"
#include "epub/xml.h"

namespace bn::epub {

namespace {

// White space, as XML has it: a document cannot hold a form feed.
bool IsBlank(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

char Small(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

// Whether an element's name is this one, which is written in small
// letters, in letters of either case.
bool Is(std::string_view name, std::string_view small) noexcept {
  return name.size() == small.size() &&
         std::equal(name.begin(), name.end(), small.begin(), [](char a, char b) { return Small(a) == b; });
}

// So many bytes of `text` from `at`, which it has.
std::string_view Mid(std::string_view text, std::size_t at, std::size_t count) noexcept {
  return {text.data() + at, count};
}

// A part of the caller's memory; none of it if the memory is too small.
std::span<std::uint8_t> Part(std::span<std::uint8_t> work, std::size_t at, std::size_t bytes) noexcept {
  return work.size() < ChapterImporter::kWorkBytes ? std::span<std::uint8_t>{} : work.subspan(at, bytes);
}

std::span<char> PathPart(std::span<std::uint8_t> work) noexcept {
  const std::span<std::uint8_t> part = Part(work, ChapterImporter::kWorkBytes - kMaxBookPath, kMaxBookPath);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters of a path.
  return {reinterpret_cast<char*>(part.data()), part.size()};
}

// How long the character is that begins with this byte of UTF-8; one for a
// byte that begins none.
std::size_t CharacterBytes(char first) noexcept {
  const auto byte = static_cast<std::uint8_t>(first);
  return byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
}

// The character that begins `text`, which is whole characters of UTF-8;
// and for bytes that are none, a number that is no letter and no mark.
std::uint32_t CharacterOf(std::string_view text) noexcept {
  const std::size_t bytes = std::min(CharacterBytes(text.front()), text.size());
  if (bytes == 1) {
    return static_cast<std::uint8_t>(text.front());
  }
  std::uint32_t character = static_cast<std::uint8_t>(text.front()) & (0xFFU >> (bytes + 1));
  for (std::size_t i = 1; i < bytes; ++i) {
    character = (character << 6U) | (static_cast<std::uint8_t>(text[i]) & 0x3FU);
  }
  return character;
}

// A quotation mark, or one of the marks Spanish opens a sentence with:
// what may stand before the letter of a capital that is dropped.
bool IsMark(std::uint32_t character) noexcept {
  constexpr std::array<std::uint32_t, 14> kMarks = {0x22,   0x27,   0xA1,   0xAB,   0xBB,   0xBF,   0x2018,
                                                    0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2039, 0x203A};
  return std::find(kMarks.begin(), kMarks.end(), character) != kMarks.end();
}

bool IsDigit(std::uint32_t character) noexcept { return character >= '0' && character <= '9'; }

// A letter of the Latin, Greek or Cyrillic alphabets, near enough: the
// blocks of Unicode that are those alphabets, but for the signs and marks
// in them that books use.
bool IsLetter(std::uint32_t character) noexcept {
  if (character < 0x80) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
  }
  if (character >= 0xC0 && character <= 0x24F) {
    return character != 0xD7 && character != 0xF7;  // The signs for times and for divided by.
  }
  return (character >= 0x370 && character <= 0x3FF && character != 0x37E && character != 0x387) ||
         (character >= 0x400 && character <= 0x52F && (character < 0x482 || character > 0x489)) ||
         (character >= 0x1E00 && character <= 0x1FFF);
}

// A character as UTF-8, for one below U+10000; how many bytes it is.
std::size_t Encode(std::uint32_t character, std::array<char, 3>& out) noexcept {
  if (character < 0x80) {
    out[0] = static_cast<char>(character);
    return 1;
  }
  if (character < 0x800) {
    out[0] = static_cast<char>(0xC0U | (character >> 6U));
    out[1] = static_cast<char>(0x80U | (character & 0x3FU));
    return 2;
  }
  out[0] = static_cast<char>(0xE0U | (character >> 12U));
  out[1] = static_cast<char>(0x80U | ((character >> 6U) & 0x3FU));
  out[2] = static_cast<char>(0x80U | (character & 0x3FU));
  return 3;
}

SegmentBlockKind KindOf(BlockKind kind) noexcept {
  switch (kind) {
    case BlockKind::kHeading:
      return SegmentBlockKind::kHeading;
    case BlockKind::kListItem:
      return SegmentBlockKind::kListItem;
    case BlockKind::kRule:
      return SegmentBlockKind::kRule;
    case BlockKind::kTableCell:
      return SegmentBlockKind::kTableCell;
    case BlockKind::kParagraph:
      break;
  }
  return SegmentBlockKind::kParagraph;
}

}  // namespace

const char* ImportStatusName(ImportStatus status) noexcept {
  switch (status) {
    case ImportStatus::kOk:
      return "ok";
    case ImportStatus::kStoreFailed:
      return "store failed";
    case ImportStatus::kTooBig:
      return "too big";
    case ImportStatus::kMisused:
      return "misused";
  }
  return "?";
}

const char* ImportReasonName(ImportReason reason) noexcept {
  switch (reason) {
    case ImportReason::kNone:
      return "none";
    case ImportReason::kNoFile:
      return "no file";
    case ImportReason::kUnreadable:
      return "unreadable";
    case ImportReason::kNotXml:
      return "not XML";
    case ImportReason::kNoMemory:
      return "no memory";
  }
  return "?";
}

bool ChapterWithoutDocument(const BundleFile& file, BundleChapter& chapter) noexcept {
  chapter = {};
  if ((file.flags & kBundleFilePresent) == 0) {
    chapter.state = ChapterState::kRefused;
    chapter.reason = static_cast<std::uint8_t>(ImportReason::kNoFile);
    return true;
  }
  if (file.kind != static_cast<std::uint8_t>(MediaType::kXhtml)) {
    chapter.state = ChapterState::kNotDocument;
    return true;
  }
  return false;
}

// The caller's memory: the writer's, then the cascade's, then the blocks',
// then the path's.
ChapterImporter::ChapterImporter(const CssRules& rules, std::span<std::uint8_t> work) noexcept
    : path_(PathPart(work)),
      cascade_(rules, Part(work, kSegmentWriterBytes, kCascadeBytes)),
      blocks_(Part(work, kSegmentWriterBytes + kCascadeBytes, kBlocksBytes)),
      writer_(Part(work, 0, kSegmentWriterBytes)) {}

ImportStatus ChapterImporter::Begin(ChapterDocument& document, DocumentSheets& sheets, XmlParser& parser,
                                    core::BlockFile& file, std::uint32_t offset, std::string_view language) noexcept {
  stage_ = Stage::kIdle;
  if (path_.empty()) {
    status_ = ImportStatus::kMisused;
    return status_;
  }
  document_ = &document;
  sheets_ = &sheets;
  parser_ = &parser;
  file_ = &file;
  offset_ = offset;
  // A tag too long to be one is none.
  bookLanguageLength_ = language.size() <= bookLanguage_.size() ? language.size() : 0;
  std::copy_n(language.begin(), bookLanguageLength_, bookLanguage_.begin());
  pathLength_ = 0;
  status_ = ImportStatus::kOk;
  reason_ = ImportReason::kNone;
  state_ = ChapterState::kWhole;
  chapter_ = {};
  hand_ = {};
  sheetsBegun_ = false;
  stage_ = Stage::kSheets;
  return status_;
}

bool ChapterImporter::End(ImportStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kDone;
  return false;
}

void ChapterImporter::Refuse(ImportReason reason) noexcept {
  reason_ = reason;
  state_ = ChapterState::kRefused;
  chapter_ = {};
  chapter_.state = ChapterState::kRefused;
  chapter_.reason = static_cast<std::uint8_t>(reason);
  stage_ = Stage::kDone;
}

bool ChapterImporter::Wrote(SegmentStatus status) noexcept {
  if (status == SegmentStatus::kOk) {
    return true;
  }
  if (status_ == ImportStatus::kOk) {
    // Nothing more is done with the document: every handler looks at this.
    status_ = status == SegmentStatus::kStoreFailed ? ImportStatus::kStoreFailed
              : status == SegmentStatus::kTooBig    ? ImportStatus::kTooBig
                                                    : ImportStatus::kMisused;
  }
  return false;
}

bool ChapterImporter::Step() noexcept {
  switch (stage_) {
    case Stage::kSheets:
      return StepSheets();
    case Stage::kOpen:
      return StepOpen();
    case Stage::kDocument:
      return StepDocument();
    case Stage::kEndBlocks:
      if (writer_.Step()) {
        return true;
      }
      if (!Wrote(writer_.Status()) ||
          !Wrote(writer_.BeginFinish(state_, static_cast<std::uint8_t>(reason_), parser_->ByteOffset()))) {
        return End(status_);
      }
      stage_ = Stage::kFinish;
      return true;
    case Stage::kFinish:
      if (writer_.Step()) {
        return true;
      }
      if (!Wrote(writer_.Status())) {
        return End(status_);
      }
      chapter_ = writer_.Chapter();
      return End(ImportStatus::kOk);
    case Stage::kIdle:
    case Stage::kDone:
      break;
  }
  return false;
}

bool ChapterImporter::StepSheets() noexcept {
  if (!sheetsBegun_) {
    core::ByteSource* const source = document_->Open(path_, pathLength_);
    if (source == nullptr) {
      Refuse(ImportReason::kNoFile);
      return false;
    }
    sheets_->Begin(Path(), *source);
    sheetsBegun_ = true;
    return true;
  }
  if (sheets_->Step()) {
    return true;
  }
  if (sheets_->Status() != SheetsStatus::kOk) {
    Refuse(ImportReason::kNoFile);
    return false;
  }
  stage_ = Stage::kOpen;
  return true;
}

bool ChapterImporter::StepOpen() noexcept {
  core::ByteSource* const source = document_->Open(path_, pathLength_);
  if (source == nullptr) {
    Refuse(ImportReason::kNoFile);
    return false;
  }
  cascade_.Begin(sheets_->Sheets());
  blocks_.Begin();
  if (!Wrote(writer_.Begin(*file_, offset_))) {
    return End(status_);
  }
  if (parser_->Begin(*source, *this) != XmlStatus::kOk) {
    return End(ImportStatus::kMisused);
  }
  stage_ = Stage::kDocument;
  return true;
}

bool ChapterImporter::StepDocument() noexcept {
  const bool more = parser_->Step();
  if (status_ != ImportStatus::kOk) {
    return End(status_);
  }
  if (more) {
    return true;
  }
  if (!parser_->Finished()) {
    switch (parser_->Status()) {
      case XmlStatus::kNoMemory:
        reason_ = ImportReason::kNoMemory;
        break;
      case XmlStatus::kSourceFailed:
        reason_ = ImportReason::kUnreadable;
        break;
      default:
        reason_ = ImportReason::kNotXml;
        break;
    }
  }
  // Whatever was open when the document ended, or went wrong, ends there.
  if (blocks_.HasBlock()) {
    Ended();
  }
  if (status_ != ImportStatus::kOk) {
    return End(status_);
  }
  if (reason_ != ImportReason::kNone && writer_.Blocks() == 0) {
    Refuse(reason_);
    return false;
  }
  state_ = reason_ == ImportReason::kNone ? ChapterState::kWhole : ChapterState::kCutShort;
  // What was left out for a limit that is not the writer's.
  writer_.CountLeftOut(blocks_.Styles().LeftOut(StyleLeftOut::kTooDeep) +
                       blocks_.Styles().LeftOut(StyleLeftOut::kLanguage) + cascade_.TooDeep());
  if (!Wrote(writer_.BeginEndBlocks())) {
    return End(status_);
  }
  stage_ = Stage::kEndBlocks;
  return true;
}

bool ChapterImporter::Emit(const RunStyle& run) noexcept {
  RunStyle item = run;
  if (run.language != 0) {
    // The writer numbers the languages in the order it is asked. What is
    // kept is one more than its number, and nothing for one not asked yet.
    std::uint8_t& asked = hand_.languages[run.language];
    if (asked == 0) {
      asked = static_cast<std::uint8_t>(writer_.Language(blocks_.Styles().Language(run.language)) + 1U);
    }
    item.language = static_cast<std::uint8_t>(asked - 1U);
  }
  hand_.run = run;
  return Wrote(writer_.Style(item));
}

RunStyle ChapterImporter::RunOf(const TextStyle& text) const noexcept {
  RunStyle run;
  run.bits = text.bits;
  // A bit is the book's only where it is set. Where a sheet says of every
  // paragraph that its text is not bold, each block would otherwise begin
  // with a style item that changes nothing.
  run.fromBook = static_cast<std::uint16_t>(text.fromBook & text.bits);
  run.familyClass = static_cast<std::uint8_t>(text.family);
  run.familyFromBook = text.familyFromBook;
  // The StyleStack's number for the language, until a style item is
  // written; and none for the book's own.
  if (text.language != 0) {
    const std::string_view tag = blocks_.Styles().Language(text.language);
    const std::string_view book(bookLanguage_.data(), bookLanguageLength_);
    const bool own = tag.size() == book.size() && std::equal(tag.begin(), tag.end(), book.begin(),
                                                             [](char x, char y) { return Small(x) == Small(y); });
    run.language = own ? std::uint8_t{0} : text.language;
  }
  // Of the block's size, to the nearest thousandth.
  if (hand_.size != 0) {
    const std::uint32_t of = ((std::uint32_t{text.size} * 1000U) + (hand_.size / 2U)) / hand_.size;
    run.size = static_cast<std::uint16_t>(std::min<std::uint32_t>(of, 65535));
  }
  return run;
}

BlockStyle ChapterImporter::StyleOf(const BlockRecord& record, unsigned dropped) const noexcept {
  BlockStyle style;
  std::uint32_t has = 0;
  std::uint32_t fromBook = 0;
  const auto field = [&](std::uint16_t bit, bool there, bool book) {
    has |= there ? bit : 0U;
    fromBook |= there && book ? bit : 0U;
  };
  field(kBlockAlign, record.align != StyleAlign::kUnset || record.alignFromBook, record.alignFromBook);
  // Nothing said, as the book's, is the side lines start from.
  style.align = AlignNumber(record.align);
  field(kBlockDirection, record.rightToLeft, false);
  style.direction = record.rightToLeft ? 1 : 0;
  field(kBlockSize, record.size != 1000 || record.sizeFromBook, record.sizeFromBook);
  style.size = record.size;
  field(kBlockLineHeight, record.lineHeight.kind != StyleLineHeight::Kind::kUnset || record.lineHeightFromBook,
        record.lineHeightFromBook);
  style.lineMultiple = record.lineHeight.multiple;
  style.lineHeight = record.lineHeight.length;
  field(kBlockIndent, record.hasIndent, record.indentFromBook);
  style.indent = record.indent;
  field(kBlockMarginLeft, !record.marginLeft.IsZero() || record.marginLeftFromBook, record.marginLeftFromBook);
  style.marginLeft = record.marginLeft;
  field(kBlockMarginRight, !record.marginRight.IsZero() || record.marginRightFromBook, record.marginRightFromBook);
  style.marginRight = record.marginRight;
  field(kBlockSpaceBefore, !record.spaceBefore.IsZero() || record.spaceBeforeFromBook, record.spaceBeforeFromBook);
  style.spaceBefore = record.spaceBefore;

  // The bits are the book's as one thing, so they are said to be only if
  // none of them is the element's own: a heading kept with what follows,
  // or white space as written.
  std::uint32_t breaking = 0;
  breaking |= record.breakBefore ? kBreakPageBefore : 0U;
  breaking |= record.keepWithNext ? kBreakKeepWithNext : 0U;
  breaking |= record.keepTogether ? kBreakKeepTogether : 0U;
  breaking |= record.spacesAsWritten ? kBreakSpaceAsWritten : 0U;
  breaking |= record.floated ? kBreakFloated : 0U;
  breaking |= record.centred ? kBreakCentred : 0U;
  breaking |= std::min(dropped, 3U) << kBreakDroppedShift;
  const bool ownBit = (record.keepWithNext && !record.keepWithNextFromBook) || record.spacesAsWritten;
  field(kBlockBreaking, breaking != 0 || record.keepWithNextFromBook, !ownBit);
  style.breaking = static_cast<std::uint16_t>(breaking);

  field(kBlockDepths, record.quoteDepth != 0 || record.listDepth != 0, false);
  style.quoteDepth = record.quoteDepth;
  style.listDepth = record.listDepth;
  field(kBlockMarker, record.hasMarker, record.markerFromBook);
  style.marker = static_cast<std::uint8_t>(record.marker);
  style.markerNumber = static_cast<std::uint32_t>(record.number);
  field(kBlockWidth, record.centred, true);
  style.width = record.width;
  style.has = static_cast<std::uint16_t>(has);
  style.fromBook = static_cast<std::uint16_t>(fromBook);
  return style;
}

void ChapterImporter::Began() noexcept {
  hand_.record = blocks_.Record();
  hand_.began = hand_.tag;
  hand_.size = hand_.record.size;
  hand_.run = {};
  // A rule is never said to have either.
  hand_.waiting = hand_.record.beginsFloated    ? Waiting::kFloated
                  : hand_.record.hasFirstLetter ? Waiting::kLetter
                                                : Waiting::kNothing;
  if (hand_.waiting == Waiting::kNothing) {
    Settle(0, false);
  }
}

void ChapterImporter::Settle(unsigned dropped, bool letter) noexcept {
  const Waiting was = hand_.waiting;
  hand_.waiting = Waiting::kNothing;
  // A first letter is dropped only if its rule floats it.
  const unsigned said = was == Waiting::kLetter && !hand_.record.firstLetterFloated ? 0 : dropped;
  // The block began at the last tag before whatever began it.
  if (!Wrote(writer_.BeginBlock(KindOf(hand_.record.kind), hand_.record.headingLevel, StyleOf(hand_.record, said),
                                hand_.began))) {
    return;
  }
  hand_.inBlock = true;
  hand_.dropped += said != 0 ? 1 : 0;
  // What it waited with: a picture; or its first characters, the letter in
  // its own style or as they were set.
  if (hand_.firstPicture) {
    static_cast<void>(
        Wrote(writer_.Picture(kSegmentNoPicture, kPictureInLine, {}, {}, {hand_.first.data(), hand_.firstFill})));
  } else if (hand_.firstFill != 0) {
    const RunStyle run = was == Waiting::kLetter && letter ? RunOf(hand_.record.firstLetter) : hand_.firstRun;
    if (!(run == hand_.run)) {
      static_cast<void>(Emit(run));
    }
    static_cast<void>(Wrote(writer_.Text({hand_.first.data(), hand_.firstFill})));
  }
  hand_.firstPicture = false;
  hand_.firstFill = 0;
  hand_.firstCount = 0;
  hand_.firstLetter = false;
}

void ChapterImporter::SettleFloated() noexcept {
  if (hand_.waiting == Waiting::kFloated) {
    Settle(hand_.firstPicture ? 1 : hand_.firstLetter ? hand_.firstCount : 0, false);
  }
}

void ChapterImporter::Ended() noexcept {
  if (hand_.waiting != Waiting::kNothing) {
    Settle(0, false);
  }
  // White space at a block's end is no part of it. None is held there: a
  // block ends where an element that is a block opens or closes.
  hand_.spaced = false;
  hand_.atStart = true;
  hand_.fresh = true;
  if (hand_.inBlock) {
    hand_.inBlock = false;
    static_cast<void>(Wrote(writer_.EndBlock()));
  }
}

void ChapterImporter::Write(const RunStyle& run, std::string_view text) noexcept {
  while (hand_.waiting != Waiting::kNothing && !text.empty()) {
    // The block waits for its first characters: marks, and then a letter.
    const std::size_t bytes = std::min(CharacterBytes(text.front()), text.size());
    const std::uint32_t character = CharacterOf(text);
    const bool letter = IsLetter(character) || (hand_.waiting == Waiting::kLetter && IsDigit(character));
    const bool fits = !hand_.firstPicture && !hand_.firstLetter && (hand_.firstFill == 0 || run == hand_.firstRun) &&
                      (letter || (IsMark(character) && hand_.firstCount < 2));
    if (!fits) {
      Settle(0, false);
      break;
    }
    hand_.firstRun = run;
    std::copy_n(text.begin(), bytes, hand_.first.begin() + static_cast<std::ptrdiff_t>(hand_.firstFill));
    hand_.firstFill += bytes;
    ++hand_.firstCount;
    hand_.firstLetter = letter;
    text.remove_prefix(bytes);
    if (letter && hand_.waiting == Waiting::kLetter) {
      Settle(hand_.firstCount, true);
    }
  }
  if (text.empty() || status_ != ImportStatus::kOk) {
    return;
  }
  if (!(run == hand_.run) && !Emit(run)) {
    return;
  }
  while (!text.empty()) {
    // As much as the writer takes at a time, ending where a character does.
    std::size_t bytes = std::min(text.size(), kSegmentMostText);
    while (bytes < text.size() && bytes > 0 && (static_cast<std::uint8_t>(text[bytes]) & 0xC0U) == 0x80U) {
      --bytes;
    }
    bytes = bytes == 0 ? std::min(text.size(), kSegmentMostText) : bytes;
    if (!Wrote(writer_.Text(Mid(text, 0, bytes)))) {
      return;
    }
    text.remove_prefix(bytes);
  }
}

void ChapterImporter::Put(const RunStyle& run, std::string_view text) noexcept {
  // Whatever is written takes with it what waited to be: the space that
  // white space became, set as that was, and white space that was held,
  // set as this is. There is never both.
  if (hand_.spaced) {
    hand_.spaced = false;
    Write(hand_.spaceStyle, " ");
  }
  if (hand_.heldFill != 0) {
    const std::size_t bytes = hand_.heldFill;
    hand_.heldFill = 0;
    // What there was no room to hold is left out, now that the rest is written.
    hand_.leftOut[static_cast<std::size_t>(ImportLeftOut::kHeld)] += hand_.heldOver;
    writer_.CountLeftOut(hand_.heldOver);
    hand_.heldOver = 0;
    Write(run, {hand_.held.data(), bytes});
  }
  Write(run, text);
  hand_.atStart = false;
  hand_.fresh = false;
}

void ChapterImporter::Collapsed(std::string_view text) noexcept {
  const RunStyle run = RunOf(blocks_.TextNow());
  std::size_t at = 0;
  while (at < text.size() && status_ == ImportStatus::kOk) {
    if (IsBlank(text[at])) {
      // It becomes a space if something is written after it in its line,
      // and is set as the first of it is.
      if (!hand_.atStart && !hand_.spaced) {
        hand_.spaced = true;
        hand_.spaceStyle = run;
      }
      ++at;
      continue;
    }
    std::size_t end = at;
    while (end < text.size() && !IsBlank(text[end])) {
      ++end;
    }
    Put(run, Mid(text, at, end - at));
    at = end;
  }
}

void ChapterImporter::Hold(char blank) noexcept {
  if (blank == '\n') {
    hand_.heldFill = 0;  // Only what follows the last line end is kept.
    hand_.heldOver = 0;
  } else if (hand_.heldFill < hand_.held.size()) {
    hand_.held[hand_.heldFill++] = blank;
  } else {
    ++hand_.heldOver;
  }
}

void ChapterImporter::AsWritten(std::string_view text) noexcept {
  // White space before the block's first text is held; once something has
  // been written, everything is as it is.
  std::size_t at = 0;
  while (hand_.fresh && at < text.size() && IsBlank(text[at])) {
    Hold(text[at]);
    ++at;
  }
  if (at < text.size()) {
    Put(RunOf(blocks_.TextNow()), Mid(text, at, text.size() - at));
  }
}

void ChapterImporter::Told(std::string_view text) noexcept {
  const BlockChange change = blocks_.Text(text);
  if (change.begins) {
    Began();
  }
  const bool asWritten = blocks_.SpacesAsWrittenNow();
  if (!change.shown) {
    // White space where no block is open: if it is as written, a block
    // that begins before any other element does begins with it.
    if (asWritten && !blocks_.InNothing()) {
      for (const char blank : text) {
        Hold(blank);
      }
    }
    return;
  }
  if (InRule()) {
    return;  // A rule has nothing in it.
  }
  if (asWritten) {
    AsWritten(text);
  } else {
    Collapsed(text);
  }
}

void ChapterImporter::PutPicture(const XmlAttributes& attributes) noexcept {
  // The text that stands for it, as much as is kept, ending where a
  // character does.
  std::string_view alt;
  static_cast<void>(attributes.Find("alt", alt));
  std::size_t bytes = std::min(alt.size(), kSegmentMostName);
  while (bytes < alt.size() && bytes > 0 && (static_cast<std::uint8_t>(alt[bytes]) & 0xC0U) == 0x80U) {
    --bytes;
  }
  alt = Mid(alt, 0, bytes);
  if (hand_.waiting == Waiting::kFloated && hand_.firstFill == 0 && !hand_.firstPicture && hand_.heldFill == 0 &&
      alt.size() <= hand_.first.size()) {
    // The first thing of the floated text: it is the capital if the
    // floated text ends with nothing more in it.
    std::copy(alt.begin(), alt.end(), hand_.first.begin());
    hand_.firstFill = alt.size();
    hand_.firstPicture = true;
    hand_.atStart = false;
    hand_.fresh = false;
    return;
  }
  Put(RunOf(blocks_.TextNow()), {});
  if (hand_.waiting != Waiting::kNothing) {
    Settle(0, false);
  }
  static_cast<void>(Wrote(writer_.Picture(kSegmentNoPicture, kPictureInLine, {}, {}, alt)));
}

void ChapterImporter::OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept {
  if (status_ != ImportStatus::kOk) {
    return;
  }
  hand_.tag = parser_->ByteOffset();
  CssStyle style;
  CssStyle firstLetter;
  cascade_.Open(name, attributes, style, firstLetter);
  ElementStyle said;
  const BlockChange change = blocks_.Open(name, attributes, style, firstLetter, said);
  if (change.ends) {
    Ended();
  }
  if (change.boundary) {
    hand_.heldFill = 0;
    hand_.heldOver = 0;
  }
  // An id is a place in the chapter: here, or at the start of the block
  // that follows, or of the one that waits.
  std::string_view id;
  if (attributes.Find("id", id) || (Is(name, "a") && attributes.Find("name", id))) {
    static_cast<void>(Wrote(writer_.Anchor(id)));
  }
  if (change.begins) {
    Began();
  }
  if (change.lineEnds && !InRule()) {
    // No white space stands before a line's end, and none that is not as
    // written after it.
    if (hand_.waiting != Waiting::kNothing) {
      Settle(0, false);
    }
    hand_.spaced = false;
    hand_.heldFill = 0;
    hand_.heldOver = 0;
    static_cast<void>(Wrote(writer_.Text("\n")));
    hand_.atStart = true;
    hand_.fresh = false;
  }
  if (Is(name, "img")) {
    const BlockChange picture = blocks_.Picture();
    if (picture.begins) {
      Began();
    }
    if (picture.shown && !InRule()) {
      PutPicture(attributes);
    }
  }
}

void ChapterImporter::OnEndElement(std::string_view /*name*/) noexcept {
  if (status_ != ImportStatus::kOk) {
    return;
  }
  hand_.tag = parser_->ByteOffset();
  cascade_.Close();
  const BlockChange change = blocks_.Close();
  if (change.floatEnds) {
    SettleFloated();
  }
  if (change.ends) {
    Ended();
  }
  if (change.boundary) {
    hand_.heldFill = 0;
    hand_.heldOver = 0;
  }
}

void ChapterImporter::OnText(std::string_view text) noexcept {
  if (status_ == ImportStatus::kOk) {
    Told(text);
  }
}

void ChapterImporter::OnEntity(std::string_view name) noexcept {
  if (status_ != ImportStatus::kOk) {
    return;
  }
  const std::uint32_t character = HtmlEntity(name);
  if (character == 0) {
    ++hand_.leftOut[static_cast<std::size_t>(ImportLeftOut::kEntity)];
    writer_.CountLeftOut(1);
    return;
  }
  std::array<char, 3> bytes{};
  const std::size_t count = Encode(character, bytes);
  Told({bytes.data(), count});
}

}  // namespace bn::epub
