#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "core/checksum.h"
#include "core/utf8.h"
#include "epub/segment.h"
#include "segment_format.h"

namespace bn::epub {

namespace {

using segment_format::Get16;
using segment_format::kAnchorOther;
using segment_format::kDirectoryHead;
using segment_format::kHead;
using segment_format::kIndexRecord;
using segment_format::kPage;
using segment_format::kPageEntry;
using segment_format::kPictureEntry;
using segment_format::Put16;
using segment_format::Put32;

// The parts of the writer's memory, after its three pages.
constexpr std::size_t kTextItem = 1024;  // A text item being put together, its head with it.
constexpr std::size_t kTextBytes = kTextItem - kHead;
constexpr std::size_t kIndexEntries = kSegmentIndexPages * kPageEntry;
constexpr std::size_t kAnchorEntries = kSegmentAnchorPages * kPageEntry;
constexpr std::size_t kPictureNumbers = kSegmentPictures * 2;
constexpr std::size_t kTags = kSegmentLanguages * (kSegmentMostLanguage + 1);
static_assert((3 * kPage) + kTextItem + kIndexEntries + kAnchorEntries + kPictureNumbers + kTags ==
              kSegmentWriterBytes);
static_assert(kSegmentPictures * kPictureEntry <= kPage);

constexpr std::uint32_t kMostLeftOut = 65535;
constexpr std::uint8_t kMostFamilyClass = 7;
constexpr std::uint8_t kFamilyFromBook = 8;

std::span<const std::uint8_t> BytesOf(std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// A byte that is neither a character of one byte nor the first of a
// longer one.
bool IsInsideCharacter(char byte) noexcept { return (static_cast<std::uint8_t>(byte) & 0xC0U) == 0x80U; }

std::array<std::uint8_t, kHead> HeadOf(std::uint8_t kind, std::uint8_t byte, std::size_t bytes) noexcept {
  std::array<std::uint8_t, kHead> head{};
  head[0] = kind;
  head[1] = byte;
  Put16(head, 2, static_cast<std::uint16_t>(bytes));
  return head;
}

}  // namespace

SegmentWriter::SegmentWriter(std::span<std::uint8_t> memory) noexcept {
  if (memory.size() < kSegmentWriterBytes) {
    return;
  }
  const auto take = [&memory](std::size_t bytes) {
    const std::span<std::uint8_t> part = memory.first(bytes);
    memory = memory.subspan(bytes);
    return part;
  };
  out_ = take(kPage);
  index_ = take(kPage);
  anchors_ = take(kPage);
  text_ = take(kTextItem);
  indexEntries_ = take(kIndexEntries);
  anchorEntries_ = take(kAnchorEntries);
  pictures_ = take(kPictureNumbers);
  tags_ = take(kTags);
}

SegmentStatus SegmentWriter::Fail(SegmentStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kIdle;
  inBlock_ = false;
  waiting_ = {};
  return status;
}

SegmentStatus SegmentWriter::Begin(core::BlockFile& file, std::uint32_t offset) noexcept {
  stage_ = Stage::kIdle;
  status_ = SegmentStatus::kMisused;
  if (out_.empty() || !file.IsOpen() || offset % kBundleSectorBytes != 0 || offset > file.Size()) {
    return status_;
  }
  file_ = &file;
  offset_ = offset;
  flushed_ = 0;
  fill_ = 0;
  inBlock_ = false;
  linkOpen_ = false;
  textFill_ = 0;
  blocks_ = 0;
  characters_ = 0;
  leftOut_ = 0;
  lastSource_ = 0;
  indexPages_ = 0;
  indexRecords_ = 0;
  indexFirst_ = 0;
  anchorPages_ = 0;
  anchorFill_ = 0;
  anchorIds_ = 0;
  pictureCount_ = 0;
  languages_ = 0;
  tagsFill_ = 0;
  waiting_ = {};
  waitingInDirectory_ = false;
  chapter_ = BundleChapter{};
  status_ = SegmentStatus::kOk;
  stage_ = Stage::kBlocks;
  return status_;
}

bool SegmentWriter::Flush() noexcept {
  if (fill_ == 0) {
    return true;
  }
  const std::uint64_t at = std::uint64_t{offset_} + flushed_;
  if (at + fill_ > std::numeric_limits<std::uint32_t>::max()) {
    Fail(SegmentStatus::kTooBig);
    return false;
  }
  if (!file_->Seek(static_cast<std::uint32_t>(at)) || !file_->Write(out_.first(fill_))) {
    Fail(SegmentStatus::kStoreFailed);
    return false;
  }
  flushed_ += static_cast<std::uint32_t>(fill_);
  fill_ = 0;
  return true;
}

bool SegmentWriter::Append(std::span<const std::uint8_t> bytes) noexcept {
  while (!bytes.empty()) {
    const std::size_t take = std::min(out_.size() - fill_, bytes.size());
    std::memcpy(out_.data() + fill_, bytes.data(), take);
    fill_ += take;
    bytes = bytes.subspan(take);
    if (fill_ == out_.size() && !Flush()) {
      return false;
    }
  }
  return true;
}

bool SegmentWriter::AppendToBlock(std::span<const std::uint8_t> bytes) noexcept {
  blockChecksum_ = core::Crc32(bytes, blockChecksum_);
  return Append(bytes);
}

bool SegmentWriter::Drain() noexcept {
  const std::size_t take = std::min(out_.size() - fill_, waiting_.size());
  std::memcpy(out_.data() + fill_, waiting_.data(), take);
  if (waitingInDirectory_) {
    directoryChecksum_ = core::Crc32(waiting_.first(take), directoryChecksum_);
  }
  fill_ += take;
  waiting_ = waiting_.subspan(take);
  if (fill_ == out_.size() && !Flush()) {
    return false;
  }
  return waiting_.empty();
}

bool SegmentWriter::EmitItem(std::uint8_t kind, std::uint8_t byte, std::span<const std::uint8_t> bytes) noexcept {
  return AppendToBlock(HeadOf(kind, byte, bytes.size())) && AppendToBlock(bytes);
}

bool SegmentWriter::EmitText() noexcept {
  if (textFill_ == 0) {
    return true;
  }
  const std::array<std::uint8_t, kHead> head = HeadOf(static_cast<std::uint8_t>(ItemKind::kText), 0, textFill_);
  std::memcpy(text_.data(), head.data(), kHead);
  const std::size_t bytes = kHead + textFill_;
  textFill_ = 0;
  return AppendToBlock(text_.first(bytes));
}

SegmentStatus SegmentWriter::BeginBlock(BlockKind kind, std::uint8_t level, const BlockStyle& style,
                                        std::uint32_t source) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  const bool levelIsRight = kind == BlockKind::kHeading ? (level >= 1 && level <= 6) : level == 0;
  if (stage_ != Stage::kBlocks || inBlock_ || !levelIsRight || static_cast<std::uint8_t>(kind) == 0 ||
      static_cast<std::uint8_t>(kind) >= kSegmentIndexPage) {
    return Fail(SegmentStatus::kMisused);
  }
  std::array<std::uint8_t, kBlockStyleMostBytes> record{};
  const std::size_t bytes = StoreBlockStyle(style, record);
  inBlock_ = true;
  linkOpen_ = false;
  blockKind_ = static_cast<std::uint8_t>(kind);
  blockBreaking_ = (style.has & kBlockBreaking) != 0 ? static_cast<std::uint8_t>(style.breaking) : std::uint8_t{0};
  runClass_ = 0;
  blockStart_ = Bytes();
  blockSource_ = std::max(source, lastSource_);
  blockCharacters_ = 0;
  blockChecksum_ = 0;
  textFill_ = 0;
  if (!AppendToBlock(HeadOf(blockKind_, level, bytes)) || !AppendToBlock(std::span(record).first(bytes))) {
    return status_;
  }
  return SegmentStatus::kOk;
}

void SegmentWriter::CountCharacters(std::string_view text) noexcept {
  for (std::size_t at = 0; at < text.size();) {
    const core::Character character = core::CharacterAt(text, at);
    at += character.bytes;
    if (!character.good) {
      continue;
    }
    // A bit for each row of 256 characters; the last also for everything
    // past the first 65,536.
    const std::uint32_t row = character.codePoint < 65536 ? character.codePoint >> 8U : 255U;
    chapter_.characterRows[row / 8] |= static_cast<std::uint8_t>(1U << (row % 8));
  }
  if (blockKind_ != static_cast<std::uint8_t>(BlockKind::kHeading) && runClass_ < kBundleClasses) {
    chapter_.classCharacters[runClass_] += static_cast<std::uint32_t>(text.size());
  }
}

SegmentStatus SegmentWriter::Text(std::string_view text) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || text.size() > kSegmentMostText) {
    return Fail(SegmentStatus::kMisused);
  }
  CountCharacters(text);
  blockCharacters_ += static_cast<std::uint32_t>(text.size());
  while (!text.empty()) {
    const std::size_t room = kTextBytes - textFill_;
    std::size_t take = std::min(room, text.size());
    if (take < text.size()) {
      // Not all of it goes in this item: the item ends where a character
      // does. Text that is no characters at all is cut where it falls.
      std::size_t cut = take;
      for (int back = 0; back < 3 && cut > 0 && IsInsideCharacter(text[cut]); ++back) {
        --cut;
      }
      if (!IsInsideCharacter(text[cut]) && (cut > 0 || textFill_ > 0)) {
        take = cut;
      }
    }
    std::memcpy(text_.data() + kHead + textFill_, text.data(), take);
    textFill_ += take;
    text.remove_prefix(take);
    if (!text.empty() && !EmitText()) {
      return status_;
    }
  }
  return SegmentStatus::kOk;
}

SegmentStatus SegmentWriter::Style(const RunStyle& style) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || style.familyClass > kMostFamilyClass || style.language > languages_) {
    return Fail(SegmentStatus::kMisused);
  }
  std::array<std::uint8_t, segment_format::kStyleBytes> bytes{};
  Put16(bytes, 0, style.bits);
  Put16(bytes, 2, style.fromBook);
  bytes[4] = static_cast<std::uint8_t>(style.familyClass | (style.familyFromBook ? kFamilyFromBook : 0U));
  bytes[5] = style.language;
  Put16(bytes, 6, style.size);
  runClass_ = style.familyClass;
  if (!EmitText() || !EmitItem(static_cast<std::uint8_t>(ItemKind::kStyle), 0, bytes)) {
    return status_;
  }
  return SegmentStatus::kOk;
}

SegmentStatus SegmentWriter::LinkStart(LinkKind kind, std::uint16_t chapter, std::string_view fragment) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || linkOpen_ || fragment.size() > kSegmentMostName ||
      kind > LinkKind::kOutside) {
    return Fail(SegmentStatus::kMisused);
  }
  linkOpen_ = true;
  std::array<std::uint8_t, segment_format::kLinkBytes> start{};
  Put16(start, 0, chapter);
  start[2] = static_cast<std::uint8_t>(fragment.size());
  if (!EmitText() ||
      !AppendToBlock(HeadOf(static_cast<std::uint8_t>(ItemKind::kLinkStart), static_cast<std::uint8_t>(kind),
                            start.size() + fragment.size())) ||
      !AppendToBlock(start) || !AppendToBlock(BytesOf(fragment))) {
    return status_;
  }
  return SegmentStatus::kOk;
}

SegmentStatus SegmentWriter::LinkEnd() noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || !linkOpen_) {
    return Fail(SegmentStatus::kMisused);
  }
  linkOpen_ = false;
  if (!EmitText() || !EmitItem(static_cast<std::uint8_t>(ItemKind::kLinkEnd), 0, {})) {
    return status_;
  }
  return SegmentStatus::kOk;
}

void SegmentWriter::NotePicture(std::uint16_t number) noexcept {
  if (number == kSegmentNoPicture) {
    return;
  }
  // Where it is, or belongs, among those met: they are kept in order.
  std::size_t low = 0;
  std::size_t high = pictureCount_;
  while (low < high) {
    const std::size_t middle = (low + high) / 2;
    if (Get16(pictures_, middle * 2) < number) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < pictureCount_ && Get16(pictures_, low * 2) == number) {
    return;
  }
  if (pictureCount_ == kSegmentPictures) {
    CountLeftOut(1);
    return;
  }
  std::memmove(pictures_.data() + ((low + 1) * 2), pictures_.data() + (low * 2), (pictureCount_ - low) * 2);
  Put16(pictures_, low * 2, number);
  ++pictureCount_;
}

SegmentStatus SegmentWriter::Picture(std::uint16_t number, std::uint16_t bits, const SegmentLength& width,
                                     const SegmentLength& height, std::string_view alt) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || alt.size() > kSegmentMostName) {
    return Fail(SegmentStatus::kMisused);
  }
  std::array<std::uint8_t, segment_format::kPictureBytes> start{};
  Put16(start, 0, number);
  Put16(start, 2, bits);
  Put16(start, 4, static_cast<std::uint16_t>(width.em));
  Put16(start, 6, static_cast<std::uint16_t>(width.px));
  Put16(start, 8, static_cast<std::uint16_t>(width.percent));
  Put16(start, 10, static_cast<std::uint16_t>(height.em));
  Put16(start, 12, static_cast<std::uint16_t>(height.px));
  Put16(start, 14, static_cast<std::uint16_t>(height.percent));
  NotePicture(number);
  ++blockCharacters_;
  if (!EmitText() ||
      !AppendToBlock(HeadOf(static_cast<std::uint8_t>(ItemKind::kPicture), 0, start.size() + alt.size())) ||
      !AppendToBlock(start) || !AppendToBlock(BytesOf(alt))) {
    return status_;
  }
  return SegmentStatus::kOk;
}

std::span<const std::uint8_t> SegmentWriter::BeginIndexPage() noexcept {
  const std::span<const std::uint8_t> page = index_.first(indexRecords_ * kIndexRecord);
  if (!Append(HeadOf(kSegmentIndexPage, 0, page.size()))) {
    return {};
  }
  const std::span<std::uint8_t> entry = indexEntries_.subspan(indexPages_ * kPageEntry, kPageEntry);
  Put32(entry, 0, Bytes());
  Put32(entry, 4, indexFirst_);
  Put32(entry, 8, core::Crc32(page));
  ++indexPages_;
  indexRecords_ = 0;
  return page;
}

std::span<const std::uint8_t> SegmentWriter::BeginAnchorPage() noexcept {
  const std::span<const std::uint8_t> page = anchors_.first(anchorFill_);
  if (!Append(HeadOf(kSegmentAnchorPage, 0, page.size()))) {
    return {};
  }
  const std::span<std::uint8_t> entry = anchorEntries_.subspan(anchorPages_ * kPageEntry, kPageEntry);
  Put32(entry, 0, Bytes());
  Put16(entry, 4, static_cast<std::uint16_t>(anchorFill_));
  Put16(entry, 6, static_cast<std::uint16_t>(anchorIds_));
  Put32(entry, 8, core::Crc32(page));
  ++anchorPages_;
  anchorFill_ = 0;
  anchorIds_ = 0;
  return page;
}

SegmentStatus SegmentWriter::EndBlock() noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || !inBlock_ || linkOpen_) {
    return Fail(SegmentStatus::kMisused);
  }
  // A block is never of no characters.
  const std::uint32_t characters = blockCharacters_ == 0 ? 1U : blockCharacters_;
  std::array<std::uint8_t, 4> number{};
  Put32(number, 0, characters);
  if (!EmitText() || !AppendToBlock(HeadOf(static_cast<std::uint8_t>(ItemKind::kEnd), 0, segment_format::kEndBytes)) ||
      !AppendToBlock(number)) {
    return status_;
  }
  Put32(number, 0, blockChecksum_);
  if (!Append(number)) {
    return status_;
  }
  inBlock_ = false;
  if (blocks_ < kSegmentIndexedBlocks) {
    if (indexRecords_ == 0) {
      indexFirst_ = characters_;
    }
    const std::span<std::uint8_t> record = index_.subspan(indexRecords_ * kIndexRecord, kIndexRecord);
    Put32(record, segment_format::kIndexAtOffset, blockStart_);
    Put32(record, segment_format::kIndexAtCharacters, characters_);
    Put32(record, segment_format::kIndexAtSource, blockSource_);
    record[segment_format::kIndexAtKind] = blockKind_;
    record[segment_format::kIndexAtBreaking] = blockBreaking_;
    record[14] = 0;
    record[15] = 0;
    ++indexRecords_;
  } else {
    CountLeftOut(1);
  }
  ++blocks_;
  characters_ += characters;
  lastSource_ = blockSource_;
  if (indexRecords_ == kSegmentBlocksInPage) {
    const std::span<const std::uint8_t> page = BeginIndexPage();
    if (status_ != SegmentStatus::kOk || !Append(page)) {
      return status_;
    }
  }
  return SegmentStatus::kOk;
}

SegmentStatus SegmentWriter::Anchor(std::string_view name) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks) {
    return Fail(SegmentStatus::kMisused);
  }
  if (name.empty()) {
    return SegmentStatus::kOk;
  }
  // The block in hand, or the one that follows: either way the next to
  // be ended.
  if (name.size() > kSegmentMostName || blocks_ >= kSegmentIndexedBlocks) {
    CountLeftOut(1);
    return SegmentStatus::kOk;
  }
  const std::size_t bytes = name.size() + kAnchorOther;
  if (anchorFill_ + bytes > anchors_.size()) {
    if (anchorPages_ + 1 == kSegmentAnchorPages) {
      CountLeftOut(1);  // This page is the last there may be.
      return SegmentStatus::kOk;
    }
    // The page goes between two items, never inside one: text that is
    // being put together is not in the segment yet, and follows it.
    const std::span<const std::uint8_t> page = BeginAnchorPage();
    if (status_ != SegmentStatus::kOk || !Append(page)) {
      return status_;
    }
  }
  const std::span<std::uint8_t> entry = anchors_.subspan(anchorFill_, bytes);
  entry[0] = static_cast<std::uint8_t>(name.size());
  std::memcpy(entry.data() + 1, name.data(), name.size());
  Put16(entry, 1 + name.size(), static_cast<std::uint16_t>(blocks_));
  Put32(entry, 3 + name.size(), inBlock_ ? blockCharacters_ : 0U);
  anchorFill_ += bytes;
  ++anchorIds_;
  return SegmentStatus::kOk;
}

std::uint8_t SegmentWriter::Language(std::string_view tag) noexcept {
  if (tag.empty() || stage_ != Stage::kBlocks) {
    return 0;
  }
  std::size_t at = 0;
  for (std::size_t number = 1; number <= languages_; ++number) {
    const std::size_t length = tags_[at];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as the characters they are.
    if (tag == std::string_view(reinterpret_cast<const char*>(tags_.data()) + at + 1, length)) {
      return static_cast<std::uint8_t>(number);
    }
    at += 1 + length;
  }
  if (tag.size() > kSegmentMostLanguage || languages_ == kSegmentLanguages) {
    CountLeftOut(1);
    return 0;
  }
  tags_[tagsFill_] = static_cast<std::uint8_t>(tag.size());
  std::memcpy(tags_.data() + tagsFill_ + 1, tag.data(), tag.size());
  tagsFill_ += 1 + tag.size();
  ++languages_;
  return static_cast<std::uint8_t>(languages_);
}

void SegmentWriter::CountLeftOut(std::uint32_t count) noexcept {
  leftOut_ = count > kMostLeftOut - std::min(leftOut_, kMostLeftOut) ? kMostLeftOut : leftOut_ + count;
}

SegmentStatus SegmentWriter::BeginEndBlocks() noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kBlocks || inBlock_) {
    return Fail(SegmentStatus::kMisused);
  }
  stage_ = Stage::kEndIndex;
  waitingInDirectory_ = false;
  return SegmentStatus::kOk;
}

std::uint16_t SegmentWriter::PictureNumber(std::size_t index) const noexcept {
  return index < pictureCount_ ? Get16(pictures_, index * 2) : kSegmentNoPicture;
}

SegmentStatus SegmentWriter::SetPictureSize(std::size_t index, std::uint16_t width, std::uint16_t height) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kPictures || index >= pictureCount_) {
    return Fail(SegmentStatus::kMisused);
  }
  const std::span<std::uint8_t> entry = index_.subspan(index * kPictureEntry, kPictureEntry);
  Put16(entry, 2, width);
  Put16(entry, 4, height);
  entry[6] = 1;
  return SegmentStatus::kOk;
}

SegmentStatus SegmentWriter::BeginFinish(ChapterState state, std::uint8_t reason, std::uint32_t sourceBytes) noexcept {
  if (status_ != SegmentStatus::kOk) {
    return status_;
  }
  if (stage_ != Stage::kPictures || (state != ChapterState::kWhole && state != ChapterState::kCutShort)) {
    return Fail(SegmentStatus::kMisused);
  }
  chapter_.state = state;
  chapter_.reason = reason;
  directoryOffset_ = Bytes();
  directoryChecksum_ = 0;
  std::array<std::uint8_t, kDirectoryHead> head{};
  Put16(head, segment_format::kDirectoryAtIndexPages, static_cast<std::uint16_t>(indexPages_));
  Put16(head, segment_format::kDirectoryAtAnchorPages, static_cast<std::uint16_t>(anchorPages_));
  Put16(head, segment_format::kDirectoryAtPictures, static_cast<std::uint16_t>(pictureCount_));
  Put16(head, segment_format::kDirectoryAtLanguages, static_cast<std::uint16_t>(languages_));
  Put32(head, segment_format::kDirectoryAtBlocks, blocks_);
  Put32(head, segment_format::kDirectoryAtCharacters, characters_);
  Put32(head, segment_format::kDirectoryAtSourceBytes, sourceBytes);
  directoryChecksum_ = core::Crc32(head);
  if (!Append(head)) {
    return status_;
  }
  stage_ = Stage::kDirectory;
  directoryPart_ = 0;
  waitingInDirectory_ = true;
  return SegmentStatus::kOk;
}

bool SegmentWriter::Step() noexcept {
  if (status_ != SegmentStatus::kOk) {
    return false;
  }
  if (!waiting_.empty()) {
    // More of what was begun: the stage moves on once it is all added.
    static_cast<void>(Drain());
    return status_ == SegmentStatus::kOk;
  }
  switch (stage_) {
    case Stage::kEndIndex:
      stage_ = Stage::kEndAnchors;
      if (indexRecords_ > 0) {
        waiting_ = BeginIndexPage();
      }
      return status_ == SegmentStatus::kOk;
    case Stage::kEndAnchors:
      stage_ = Stage::kEndLast;
      if (anchorFill_ > 0) {
        waiting_ = BeginAnchorPage();
      }
      return status_ == SegmentStatus::kOk;
    case Stage::kEndLast:
      // The index page is done with: its memory is the pictures' records
      // from here on, each with no size until one is given.
      for (std::size_t i = 0; i < pictureCount_; ++i) {
        const std::span<std::uint8_t> entry = index_.subspan(i * kPictureEntry, kPictureEntry);
        std::fill(entry.begin(), entry.end(), std::uint8_t{0});
        Put16(entry, 0, Get16(pictures_, i * 2));
      }
      stage_ = Stage::kPictures;
      return false;
    case Stage::kDirectory:
      switch (directoryPart_++) {
        case 0:
          waiting_ = indexEntries_.first(indexPages_ * kPageEntry);
          return true;
        case 1:
          waiting_ = anchorEntries_.first(anchorPages_ * kPageEntry);
          return true;
        case 2:
          waiting_ = index_.first(pictureCount_ * kPictureEntry);
          return true;
        case 3:
          waiting_ = tags_.first(tagsFill_);
          return true;
        default:
          stage_ = Stage::kLastPage;
          return true;
      }
    case Stage::kLastPage:
      if (!Flush()) {
        return false;
      }
      chapter_.leftOut = static_cast<std::uint16_t>(std::min(leftOut_, kMostLeftOut));
      chapter_.segmentOffset = offset_;
      chapter_.segmentBytes = flushed_;
      chapter_.directoryOffset = directoryOffset_;
      chapter_.directoryBytes = flushed_ - directoryOffset_;
      chapter_.directoryChecksum = directoryChecksum_;
      chapter_.blocks = blocks_;
      chapter_.characters = characters_;
      stage_ = Stage::kDone;
      return false;
    case Stage::kIdle:
    case Stage::kBlocks:
    case Stage::kPictures:
    case Stage::kDone:
      return false;
  }
  return false;
}

}  // namespace bn::epub
