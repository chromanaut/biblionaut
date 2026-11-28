#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/checksum.h"
#include "epub/segment.h"
#include "segment_format.h"

namespace bn::epub {

namespace {

using segment_format::Get16;
using segment_format::Get32;
using segment_format::kAnchorOther;
using segment_format::kDirectoryHead;
using segment_format::kHead;
using segment_format::kIndexRecord;
using segment_format::kPage;
using segment_format::kPageEntry;
using segment_format::kPictureEntry;

constexpr std::size_t kMostTags = kSegmentLanguages * (kSegmentMostLanguage + 1);
// The longest a directory can be: every page, every picture, every tag.
constexpr std::size_t kMostDirectory = kDirectoryHead + ((kSegmentIndexPages + kSegmentAnchorPages) * kPageEntry) +
                                       (kSegmentPictures * kPictureEntry) + kMostTags;
// How many of the directory's entries are kept at once.
constexpr std::size_t kEntriesKept = 8;
constexpr std::uint8_t kFamilyClassBits = 7;
constexpr std::uint8_t kFamilyFromBook = 8;

std::string_view TextOf(std::span<const std::uint8_t> bytes) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as the characters they are.
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool IsPage(std::uint8_t kind) noexcept { return kind == kSegmentIndexPage || kind == kSegmentAnchorPage; }

StyleLength LengthAt(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  StyleLength length;
  length.em = static_cast<std::int16_t>(Get16(bytes, at));
  length.px = static_cast<std::int16_t>(Get16(bytes, at + 2));
  length.percent = static_cast<std::int16_t>(Get16(bytes, at + 4));
  return length;
}

// Of the `count` records of `bytes` each in `page`, the last whose number
// at `at` is `value` or less; the first, if none is.
std::size_t LastAtOrBefore(std::span<const std::uint8_t> page, std::size_t count, std::size_t bytes, std::size_t at,
                           std::uint32_t value) noexcept {
  std::size_t low = 0;
  std::size_t high = count;
  while (high - low > 1) {
    const std::size_t middle = (low + high) / 2;
    if (Get32(page, (middle * bytes) + at) <= value) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

}  // namespace

SegmentReader::SegmentReader(std::span<std::uint8_t> page) noexcept
    : page_(page.size() >= kPage ? page.first(kPage) : std::span<std::uint8_t>()) {}

void SegmentReader::Close() noexcept {
  file_ = nullptr;
  open_ = false;
  job_ = Job::kNone;
  status_ = SegmentStatus::kMisused;
  entriesCount_ = 0;
  anchorsBytes_ = 0;
  inBlock_ = false;
  position_ = 0;
  blockIndex_ = 0;
  charactersBefore_ = 0;
  unknownItems_ = 0;
  place_ = SegmentPlace{};
}

SegmentStatus SegmentReader::Fail(SegmentStatus status) noexcept {
  if (job_ == Job::kOpening) {
    open_ = false;
  }
  job_ = Job::kNone;
  status_ = status;
  return status;
}

SegmentStatus SegmentReader::Done(SegmentStatus status) noexcept {
  job_ = Job::kNone;
  status_ = status;
  return status;
}

bool SegmentReader::ReadAt(std::uint32_t offset, std::span<std::uint8_t> dst) noexcept {
  const std::uint64_t at = std::uint64_t{segmentOffset_} + offset;
  return at + dst.size() <= file_->Size() && file_->ReadAt(static_cast<std::uint32_t>(at), dst);
}

std::uint32_t SegmentReader::IndexedBlocks() const noexcept {
  return std::min<std::uint32_t>(blocks_, kSegmentIndexedBlocks);
}

std::size_t SegmentReader::RecordsInPage(std::size_t number) const noexcept {
  return number + 1 < indexPages_ ? kSegmentBlocksInPage : IndexedBlocks() - (number * kSegmentBlocksInPage);
}

SegmentStatus SegmentReader::BeginOpen(core::BlockFile& file, const BundleChapter& record) noexcept {
  Close();
  if (page_.empty() || !file.IsOpen() ||
      (record.state != ChapterState::kWhole && record.state != ChapterState::kCutShort)) {
    return status_;
  }
  status_ = SegmentStatus::kDamaged;
  if (record.segmentBytes == 0 || std::uint64_t{record.segmentOffset} + record.segmentBytes > file.Size() ||
      record.directoryBytes < kDirectoryHead || record.directoryBytes > kMostDirectory ||
      std::uint64_t{record.directoryOffset} + record.directoryBytes > record.segmentBytes) {
    return status_;
  }
  file_ = &file;
  segmentOffset_ = record.segmentOffset;
  directoryOffset_ = record.directoryOffset;
  directoryBytes_ = record.directoryBytes;
  directoryChecksum_ = record.directoryChecksum;
  recordBlocks_ = record.blocks;
  recordCharacters_ = record.characters;
  job_ = Job::kOpening;
  stage_ = Stage::kDirectory;
  done_ = 0;
  checksum_ = 0;
  tagsBytes_ = 0;
  status_ = SegmentStatus::kOk;
  return status_;
}

SegmentStatus SegmentReader::Open(core::BlockFile& file, const BundleChapter& record) noexcept {
  if (BeginOpen(file, record) == SegmentStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

bool SegmentReader::Step() noexcept {
  switch (job_) {
    case Job::kNone:
      return false;
    case Job::kOpening:
      anchorsBytes_ = 0;
      StepOpen();
      break;
    case Job::kFindBlock:
    case Job::kFindCharacters:
    case Job::kFindSource:
    case Job::kFindAnchor:
    case Job::kReadAnchors:
      anchorsBytes_ = 0;
      StepFind();
      break;
  }
  return job_ != Job::kNone;
}

void SegmentReader::StepOpen() noexcept {
  const std::size_t bytes = std::min<std::size_t>(kPage, directoryBytes_ - done_);
  const std::span<std::uint8_t> piece = page_.first(bytes);
  if (!ReadAt(directoryOffset_ + done_, piece)) {
    Fail(SegmentStatus::kStoreFailed);
    return;
  }
  checksum_ = core::Crc32(piece, checksum_);
  if (done_ == 0) {
    // What the directory begins with, which says how long the rest is.
    indexPages_ = Get16(piece, segment_format::kDirectoryAtIndexPages);
    anchorPages_ = Get16(piece, segment_format::kDirectoryAtAnchorPages);
    pictures_ = Get16(piece, segment_format::kDirectoryAtPictures);
    languages_ = Get16(piece, segment_format::kDirectoryAtLanguages);
    blocks_ = Get32(piece, segment_format::kDirectoryAtBlocks);
    characters_ = Get32(piece, segment_format::kDirectoryAtCharacters);
    sourceBytes_ = Get32(piece, segment_format::kDirectoryAtSourceBytes);
    const std::size_t pagesWanted = (IndexedBlocks() + kSegmentBlocksInPage - 1) / kSegmentBlocksInPage;
    const std::size_t before =
        kDirectoryHead + ((indexPages_ + anchorPages_) * kPageEntry) + (pictures_ * kPictureEntry);
    if (indexPages_ != pagesWanted || anchorPages_ > kSegmentAnchorPages || pictures_ > kSegmentPictures ||
        languages_ > kSegmentLanguages || blocks_ != recordBlocks_ || characters_ != recordCharacters_ ||
        before > directoryBytes_ || directoryBytes_ - before > kMostTags) {
      Fail(SegmentStatus::kDamaged);
      return;
    }
    tagsBytes_ = directoryBytes_ - before;
  }
  // The languages' tags are the directory's last bytes: they are kept as
  // they go by.
  const std::size_t tagsBegin = directoryBytes_ - tagsBytes_;
  if (done_ + bytes > tagsBegin) {
    const std::size_t from = std::max<std::size_t>(tagsBegin, done_);
    std::memcpy(tags_.data() + (from - tagsBegin), piece.data() + (from - done_), done_ + bytes - from);
  }
  done_ += static_cast<std::uint32_t>(bytes);
  if (done_ < directoryBytes_) {
    return;
  }
  if (checksum_ != directoryChecksum_ || !CheckDirectory()) {
    Fail(SegmentStatus::kDamaged);
    return;
  }
  open_ = true;
  Done(SegmentStatus::kOk);
}

bool SegmentReader::CheckDirectory() noexcept {
  // As many tags as it says, each of a length a tag may be, and nothing
  // over.
  std::size_t at = 0;
  for (std::size_t i = 0; i < languages_; ++i) {
    if (at >= tagsBytes_ || tags_[at] == 0 || tags_[at] > kSegmentMostLanguage || at + 1 + tags_[at] > tagsBytes_) {
      return false;
    }
    at += 1 + tags_[at];
  }
  return at == tagsBytes_;
}

std::string_view SegmentReader::Language(std::size_t number) const noexcept {
  if (!open_ || number == 0 || number > languages_) {
    return {};
  }
  std::size_t at = 0;
  for (std::size_t i = 1; i < number; ++i) {
    at += 1 + tags_[at];
  }
  return TextOf(std::span(tags_).subspan(at + 1, tags_[at]));
}

SegmentStatus SegmentReader::ReadPictureAt(std::size_t index, SegmentPicture& picture) noexcept {
  if (!open_ || job_ != Job::kNone || index >= pictures_) {
    return SegmentStatus::kMisused;
  }
  std::array<std::uint8_t, kPictureEntry> entry{};
  const std::size_t at = kDirectoryHead + ((indexPages_ + anchorPages_) * kPageEntry) + (index * kPictureEntry);
  if (!ReadAt(directoryOffset_ + static_cast<std::uint32_t>(at), entry)) {
    return SegmentStatus::kStoreFailed;
  }
  picture.number = Get16(entry, 0);
  picture.width = Get16(entry, 2);
  picture.height = Get16(entry, 4);
  picture.sized = entry[6] != 0;
  return SegmentStatus::kOk;
}

SegmentStatus SegmentReader::ReadPicture(std::uint16_t number, SegmentPicture& picture, bool& found) noexcept {
  found = false;
  if (!open_ || job_ != Job::kNone) {
    return SegmentStatus::kMisused;
  }
  if (pictures_ == 0) {
    return SegmentStatus::kOk;
  }
  anchorsBytes_ = 0;
  const std::span<std::uint8_t> entries = page_.first(pictures_ * kPictureEntry);
  const std::size_t at = kDirectoryHead + ((indexPages_ + anchorPages_) * kPageEntry);
  if (!ReadAt(directoryOffset_ + static_cast<std::uint32_t>(at), entries)) {
    return SegmentStatus::kStoreFailed;
  }
  std::size_t low = 0;
  std::size_t high = pictures_;
  while (low < high) {
    const std::size_t middle = (low + high) / 2;
    if (Get16(entries, middle * kPictureEntry) < number) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < pictures_ && Get16(entries, low * kPictureEntry) == number) {
    const std::span<const std::uint8_t> entry = entries.subspan(low * kPictureEntry, kPictureEntry);
    picture.number = number;
    picture.width = Get16(entry, 2);
    picture.height = Get16(entry, 4);
    picture.sized = entry[6] != 0;
    found = true;
  }
  return SegmentStatus::kOk;
}

bool SegmentReader::LoadEntry(bool anchors, std::size_t number, Entry& entry) noexcept {
  if (entriesCount_ == 0 || entriesAnchors_ != anchors || number < entriesFirst_ ||
      number >= entriesFirst_ + entriesCount_) {
    const std::size_t all = anchors ? anchorPages_ : indexPages_;
    const std::size_t count = std::min(kEntriesKept, all - number);
    const std::size_t at = kDirectoryHead + ((anchors ? indexPages_ : 0) + number) * kPageEntry;
    entriesCount_ = 0;
    if (!ReadAt(directoryOffset_ + static_cast<std::uint32_t>(at), std::span(entries_).first(count * kPageEntry))) {
      Fail(SegmentStatus::kStoreFailed);
      return false;
    }
    entriesAnchors_ = anchors;
    entriesFirst_ = number;
    entriesCount_ = count;
  }
  const std::span<const std::uint8_t> bytes =
      std::span(entries_).subspan((number - entriesFirst_) * kPageEntry, kPageEntry);
  entry.offset = Get32(bytes, 0);
  entry.first = Get32(bytes, 4);
  entry.bytes = Get16(bytes, 4);
  entry.ids = Get16(bytes, 6);
  entry.checksum = Get32(bytes, 8);
  return true;
}

bool SegmentReader::LoadIndexPage(const Entry& entry, std::size_t number, std::size_t& records) noexcept {
  records = RecordsInPage(number);
  const std::span<std::uint8_t> page = page_.first(records * kIndexRecord);
  if (std::uint64_t{entry.offset} + page.size() > directoryOffset_) {
    Fail(SegmentStatus::kDamaged);
    return false;
  }
  if (!ReadAt(entry.offset, page)) {
    Fail(SegmentStatus::kStoreFailed);
    return false;
  }
  if (core::Crc32(page) != entry.checksum) {
    Fail(SegmentStatus::kDamaged);
    return false;
  }
  return true;
}

bool SegmentReader::PlaceFromRecord(std::size_t number, std::size_t record) noexcept {
  const std::span<const std::uint8_t> bytes = page_.subspan(record * kIndexRecord, kIndexRecord);
  place_ = SegmentPlace{};
  place_.found = true;
  place_.block = static_cast<std::uint32_t>((number * kSegmentBlocksInPage) + record);
  place_.offset = Get32(bytes, segment_format::kIndexAtOffset);
  place_.charactersBefore = Get32(bytes, segment_format::kIndexAtCharacters);
  place_.source = Get32(bytes, segment_format::kIndexAtSource);
  place_.kind = bytes[segment_format::kIndexAtKind];
  place_.breaking = bytes[segment_format::kIndexAtBreaking];
  if (place_.offset >= directoryOffset_) {
    place_ = SegmentPlace{};
    Fail(SegmentStatus::kDamaged);
    return false;
  }
  return true;
}

void SegmentReader::PlaceAtEnd() noexcept {
  place_ = SegmentPlace{};
  place_.found = true;
  place_.atEnd = true;
  place_.block = blocks_;
  place_.charactersBefore = characters_;
}

SegmentStatus SegmentReader::BeginFind(Job job) noexcept {
  if (!open_ || job_ != Job::kNone) {
    return SegmentStatus::kMisused;
  }
  job_ = job;
  status_ = SegmentStatus::kOk;
  place_ = SegmentPlace{};
  resolving_ = false;
  into_ = 0;
  return status_;
}

SegmentStatus SegmentReader::BeginFindBlock(std::uint32_t block) noexcept {
  if (const SegmentStatus status = BeginFind(Job::kFindBlock); status != SegmentStatus::kOk) {
    return status;
  }
  if (block >= IndexedBlocks()) {
    return Done(SegmentStatus::kOk);
  }
  target_ = block;
  number_ = block / kSegmentBlocksInPage;
  stage_ = Stage::kEntry;
  return status_;
}

SegmentStatus SegmentReader::BeginFindCharacters(std::uint32_t characters) noexcept {
  if (const SegmentStatus status = BeginFind(Job::kFindCharacters); status != SegmentStatus::kOk) {
    return status;
  }
  if (characters >= characters_ || indexPages_ == 0) {
    PlaceAtEnd();
    return Done(SegmentStatus::kOk);
  }
  target_ = characters;
  stage_ = Stage::kEntries;
  return status_;
}

SegmentStatus SegmentReader::BeginFindSource(std::uint32_t source) noexcept {
  if (const SegmentStatus status = BeginFind(Job::kFindSource); status != SegmentStatus::kOk) {
    return status;
  }
  if (indexPages_ == 0) {
    return Done(SegmentStatus::kOk);
  }
  target_ = source;
  low_ = 0;
  high_ = indexPages_;
  stage_ = Stage::kEntry;
  return status_;
}

SegmentStatus SegmentReader::BeginFindAnchor(std::string_view name) noexcept {
  if (const SegmentStatus status = BeginFind(Job::kFindAnchor); status != SegmentStatus::kOk) {
    return status;
  }
  if (anchorPages_ == 0 || name.empty() || name.size() > kSegmentMostName) {
    return Done(SegmentStatus::kOk);
  }
  name_ = name;
  number_ = 0;
  stage_ = Stage::kEntry;
  return status_;
}

SegmentStatus SegmentReader::FindBlock(std::uint32_t block) noexcept {
  if (const SegmentStatus status = BeginFindBlock(block); status != SegmentStatus::kOk) {
    return status;
  }
  while (Step()) {
  }
  return status_;
}

SegmentStatus SegmentReader::FindCharacters(std::uint32_t characters) noexcept {
  if (const SegmentStatus status = BeginFindCharacters(characters); status != SegmentStatus::kOk) {
    return status;
  }
  while (Step()) {
  }
  return status_;
}

SegmentStatus SegmentReader::FindSource(std::uint32_t source) noexcept {
  if (const SegmentStatus status = BeginFindSource(source); status != SegmentStatus::kOk) {
    return status;
  }
  while (Step()) {
  }
  return status_;
}

SegmentStatus SegmentReader::FindAnchor(std::string_view name) noexcept {
  if (const SegmentStatus status = BeginFindAnchor(name); status != SegmentStatus::kOk) {
    return status;
  }
  while (Step()) {
  }
  return status_;
}

SegmentStatus SegmentReader::BeginReadAnchors(std::size_t page) noexcept {
  if (const SegmentStatus status = BeginFind(Job::kReadAnchors); status != SegmentStatus::kOk) {
    return status;
  }
  if (page >= anchorPages_) {
    return Done(SegmentStatus::kMisused);
  }
  number_ = page;
  stage_ = Stage::kEntry;
  return status_;
}

bool SegmentReader::NextAnchor(SegmentAnchor& anchor) noexcept {
  if (!open_ || job_ != Job::kNone || anchorsAt_ >= anchorsBytes_) {
    return false;
  }
  // The page was gone through when it was read: every id is whole.
  const std::span<const std::uint8_t> page = page_.first(anchorsBytes_);
  const std::size_t length = page[anchorsAt_];
  anchor.name = TextOf(page.subspan(anchorsAt_ + 1, length));
  anchor.block = Get16(page, anchorsAt_ + 1 + length);
  anchor.into = Get32(page, anchorsAt_ + 3 + length);
  anchorsAt_ += length + kAnchorOther;
  return true;
}

void SegmentReader::StepFind() noexcept {
  const bool anchors = job_ == Job::kReadAnchors || (job_ == Job::kFindAnchor && !resolving_);
  if (stage_ == Stage::kEntries) {
    // Every entry for an index page, to choose the page by the
    // characters before it.
    const std::span<std::uint8_t> entries = page_.first(indexPages_ * kPageEntry);
    if (!ReadAt(directoryOffset_ + static_cast<std::uint32_t>(kDirectoryHead), entries)) {
      Fail(SegmentStatus::kStoreFailed);
      return;
    }
    number_ = LastAtOrBefore(entries, indexPages_, kPageEntry, 4, target_);
    entry_.offset = Get32(entries, number_ * kPageEntry);
    entry_.first = Get32(entries, (number_ * kPageEntry) + 4);
    entry_.checksum = Get32(entries, (number_ * kPageEntry) + 8);
    stage_ = Stage::kPage;
    return;
  }
  if (stage_ == Stage::kEntry) {
    if (job_ == Job::kFindSource) {
      number_ = high_ - low_ > 1 ? (low_ + high_) / 2 : low_;
    }
    if (LoadEntry(anchors, number_, entry_)) {
      stage_ = Stage::kPage;
    }
    return;
  }
  if (anchors) {
    SearchAnchorPage();
    return;
  }
  std::size_t records = 0;
  if (!LoadIndexPage(entry_, number_, records)) {
    return;
  }
  std::size_t record = 0;
  if (job_ == Job::kFindSource) {
    if (high_ - low_ > 1) {
      // The pages are in the document's order: this one's first block
      // says which side of it the place is.
      if (Get32(page_, segment_format::kIndexAtSource) <= target_) {
        low_ = number_;
      } else {
        high_ = number_;
      }
      if (high_ - low_ > 1 || low_ != number_) {
        stage_ = Stage::kEntry;
        return;
      }
    }
    record = LastAtOrBefore(page_, records, kIndexRecord, segment_format::kIndexAtSource, target_);
  } else if (job_ == Job::kFindCharacters) {
    record = LastAtOrBefore(page_, records, kIndexRecord, segment_format::kIndexAtCharacters, target_);
  } else {
    record = target_ % kSegmentBlocksInPage;
    if (record >= records) {
      Fail(SegmentStatus::kDamaged);
      return;
    }
  }
  if (!PlaceFromRecord(number_, record)) {
    return;
  }
  if (job_ == Job::kFindCharacters) {
    place_.into = target_ >= place_.charactersBefore ? target_ - place_.charactersBefore : 0;
  } else {
    place_.into = into_;
  }
  Done(SegmentStatus::kOk);
}

void SegmentReader::SearchAnchorPage() noexcept {
  if (entry_.bytes > kPage || std::uint64_t{entry_.offset} + entry_.bytes > directoryOffset_) {
    Fail(SegmentStatus::kDamaged);
    return;
  }
  const std::span<std::uint8_t> page = page_.first(entry_.bytes);
  if (!ReadAt(entry_.offset, page)) {
    Fail(SegmentStatus::kStoreFailed);
    return;
  }
  if (core::Crc32(page) != entry_.checksum) {
    Fail(SegmentStatus::kDamaged);
    return;
  }
  // Every id of the page, whole and in a block there is; and the first
  // of the name that is looked for, if one is.
  std::size_t ids = 0;
  bool found = false;
  std::uint32_t block = 0;
  for (std::size_t at = 0; at < page.size(); ++ids) {
    const std::size_t length = page[at];
    if (length == 0 || at + length + kAnchorOther > page.size() ||
        (Get16(page, at + 1 + length) != blocks_ && Get16(page, at + 1 + length) >= IndexedBlocks())) {
      Fail(SegmentStatus::kDamaged);
      return;
    }
    if (!found && job_ == Job::kFindAnchor && name_ == TextOf(page.subspan(at + 1, length))) {
      found = true;
      block = Get16(page, at + 1 + length);
      into_ = Get32(page, at + 3 + length);
    }
    at += length + kAnchorOther;
  }
  if (ids != entry_.ids) {
    Fail(SegmentStatus::kDamaged);
    return;
  }
  if (job_ == Job::kReadAnchors) {
    anchorsBytes_ = page.size();
    anchorsAt_ = 0;
    Done(SegmentStatus::kOk);
    return;
  }
  if (found) {
    if (block == blocks_) {
      PlaceAtEnd();
      Done(SegmentStatus::kOk);
      return;
    }
    // Where that block is: its record in the index.
    resolving_ = true;
    target_ = block;
    number_ = block / kSegmentBlocksInPage;
    stage_ = Stage::kEntry;
    return;
  }
  ++number_;
  if (number_ == anchorPages_) {
    Done(SegmentStatus::kOk);  // The chapter has no such id.
    return;
  }
  stage_ = Stage::kEntry;
}

SegmentStatus SegmentReader::Seek(const SegmentPlace& place) noexcept {
  if (!open_ || job_ != Job::kNone || !place.found || place.atEnd || place.offset >= directoryOffset_) {
    return SegmentStatus::kMisused;
  }
  position_ = place.offset;
  blockIndex_ = place.block;
  charactersBefore_ = place.charactersBefore;
  inBlock_ = false;
  return SegmentStatus::kOk;
}

SegmentStatus SegmentReader::SeekStart() noexcept {
  if (!open_ || job_ != Job::kNone) {
    return SegmentStatus::kMisused;
  }
  position_ = 0;
  blockIndex_ = 0;
  charactersBefore_ = 0;
  inBlock_ = false;
  return SegmentStatus::kOk;
}

SegmentStatus SegmentReader::ReadBlock(SegmentBlock& block) noexcept {
  if (!open_ || job_ != Job::kNone || inBlock_) {
    return SegmentStatus::kMisused;
  }
  anchorsBytes_ = 0;
  if (blockIndex_ >= blocks_) {
    return SegmentStatus::kEnd;
  }
  // The directory says there is a block more: its head has to be here.
  std::array<std::uint8_t, kHead> head{};
  if (std::uint64_t{position_} + kHead > directoryOffset_) {
    return SegmentStatus::kDamaged;
  }
  if (!ReadAt(position_, head)) {
    return SegmentStatus::kStoreFailed;
  }
  const std::uint8_t kind = head[0];
  const std::size_t length = Get16(head, 2);
  if (std::uint64_t{position_} + kHead + length > directoryOffset_) {
    return SegmentStatus::kDamaged;
  }
  if (IsPage(kind)) {
    position_ += static_cast<std::uint32_t>(kHead + length);
    return SegmentStatus::kMore;
  }
  if (kind == 0 || kind >= kSegmentIndexPage || length > kBlockStyleMostBytes) {
    return SegmentStatus::kDamaged;
  }
  const std::span<std::uint8_t> record = page_.first(length);
  if (!ReadAt(position_ + static_cast<std::uint32_t>(kHead), record)) {
    return SegmentStatus::kStoreFailed;
  }
  if (!LoadBlockStyle(record, block.style)) {
    return SegmentStatus::kDamaged;
  }
  block.kind = kind;
  block.level = head[1];
  block.index = blockIndex_;
  block.offset = position_;
  block.charactersBefore = charactersBefore_;
  blockChecksum_ = core::Crc32(record, core::Crc32(head));
  blockCharacters_ = 0;
  position_ += static_cast<std::uint32_t>(kHead + length);
  inBlock_ = true;
  return SegmentStatus::kOk;
}

SegmentStatus SegmentReader::NextItem(SegmentItem& item) noexcept {
  if (!open_ || job_ != Job::kNone || !inBlock_) {
    return SegmentStatus::kMisused;
  }
  anchorsBytes_ = 0;
  std::array<std::uint8_t, kHead> head{};
  if (std::uint64_t{position_} + kHead > directoryOffset_) {
    inBlock_ = false;
    return SegmentStatus::kDamaged;
  }
  if (!ReadAt(position_, head)) {
    return SegmentStatus::kStoreFailed;
  }
  const std::uint8_t kind = head[0];
  const std::size_t length = Get16(head, 2);
  if (std::uint64_t{position_} + kHead + length > directoryOffset_) {
    inBlock_ = false;
    return SegmentStatus::kDamaged;
  }
  if (IsPage(kind)) {
    position_ += static_cast<std::uint32_t>(kHead + length);
    return SegmentStatus::kMore;
  }
  if (kind == 0 || kind >= kSegmentIndexPage || length > kSegmentMostItem) {
    inBlock_ = false;
    return SegmentStatus::kDamaged;
  }
  const std::span<std::uint8_t> bytes = page_.first(length);
  if (!ReadAt(position_ + static_cast<std::uint32_t>(kHead), bytes)) {
    return SegmentStatus::kStoreFailed;
  }
  position_ += static_cast<std::uint32_t>(kHead + length);
  item = SegmentItem{};
  bool sound = true;
  if (kind == static_cast<std::uint8_t>(ItemKind::kEnd)) {
    // The block's characters, and the checksum of everything of the
    // block before the checksum itself.
    inBlock_ = false;
    if (length != segment_format::kEndBytes) {
      return SegmentStatus::kDamaged;
    }
    const std::uint32_t characters = Get32(bytes, 0);
    const std::uint32_t checksum = core::Crc32(bytes.first(4), core::Crc32(head, blockChecksum_));
    if (checksum != Get32(bytes, 4) || characters != (blockCharacters_ == 0 ? 1U : blockCharacters_)) {
      return SegmentStatus::kDamaged;
    }
    item.kind = ItemKind::kEnd;
    item.characters = characters;
    ++blockIndex_;
    charactersBefore_ += characters;
    return SegmentStatus::kOk;
  }
  blockChecksum_ = core::Crc32(bytes, core::Crc32(head, blockChecksum_));
  switch (kind) {
    case static_cast<std::uint8_t>(ItemKind::kText):
      item.kind = ItemKind::kText;
      item.text = TextOf(bytes);
      blockCharacters_ += static_cast<std::uint32_t>(length);
      break;
    case static_cast<std::uint8_t>(ItemKind::kStyle):
      item.kind = ItemKind::kStyle;
      sound = length == segment_format::kStyleBytes;
      if (sound) {
        item.style.bits = Get16(bytes, 0);
        item.style.fromBook = Get16(bytes, 2);
        item.style.familyClass = bytes[4] & kFamilyClassBits;
        item.style.familyFromBook = (bytes[4] & kFamilyFromBook) != 0;
        item.style.language = bytes[5];
        item.style.size = Get16(bytes, 6);
        sound = item.style.language <= languages_;
      }
      break;
    case static_cast<std::uint8_t>(ItemKind::kLinkStart):
      item.kind = ItemKind::kLinkStart;
      sound = length >= segment_format::kLinkBytes && length == segment_format::kLinkBytes + bytes[2] &&
              head[1] <= static_cast<std::uint8_t>(LinkKind::kOutside);
      if (sound) {
        item.link = static_cast<LinkKind>(head[1]);
        item.chapter = Get16(bytes, 0);
        item.text = TextOf(bytes.subspan(segment_format::kLinkBytes));
      }
      break;
    case static_cast<std::uint8_t>(ItemKind::kLinkEnd):
      item.kind = ItemKind::kLinkEnd;
      sound = length == 0;
      break;
    case static_cast<std::uint8_t>(ItemKind::kPicture):
      item.kind = ItemKind::kPicture;
      sound = length >= segment_format::kPictureBytes && length <= segment_format::kPictureBytes + kSegmentMostName;
      if (sound) {
        item.picture = Get16(bytes, 0);
        item.pictureBits = Get16(bytes, 2);
        item.width = LengthAt(bytes, 4);
        item.height = LengthAt(bytes, 10);
        item.text = TextOf(bytes.subspan(segment_format::kPictureBytes));
        ++blockCharacters_;
      }
      break;
    default:
      // A kind this does not know: it is part of the block, and nothing
      // is made of it.
      ++unknownItems_;
      return SegmentStatus::kMore;
  }
  if (!sound) {
    inBlock_ = false;
    return SegmentStatus::kDamaged;
  }
  return SegmentStatus::kOk;
}

}  // namespace bn::epub
