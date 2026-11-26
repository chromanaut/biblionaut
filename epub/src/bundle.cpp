#include "epub/bundle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "core/checksum.h"
#include "core/endian.h"

namespace bn::epub {

namespace {

constexpr std::uint32_t kPage = 4096;
constexpr std::uint32_t kSector = 512;
constexpr std::uint32_t kChapterBytes = 88;
constexpr std::uint32_t kFileBytes = 24;
constexpr std::uint64_t kMostBytes = std::numeric_limits<std::uint32_t>::max();

// The header page: where each thing is in it.
constexpr std::array<std::uint8_t, 4> kMagic = {'B', 'N', 'B', 'K'};
constexpr std::size_t kAtFormat = 4;
constexpr std::size_t kAtTextRules = 6;
constexpr std::size_t kAtImporter = 8;
constexpr std::size_t kAtEpubBytes = 12;
constexpr std::size_t kAtEpubModified = 16;
constexpr std::size_t kAtEpubDirectory = 20;
constexpr std::size_t kAtPackage = 24;
constexpr std::size_t kAtDocuments = 28;
constexpr std::size_t kAtPictures = 30;
constexpr std::size_t kAtContents = 32;
constexpr std::size_t kAtAmiss = 34;
constexpr std::size_t kAtSlotOffset = 36;
constexpr std::size_t kAtSlotBytes = 40;
constexpr std::size_t kAtDataBegin = 44;
constexpr std::size_t kAtTables = 48;  // Twelve bytes each: where, how long, checksum.
constexpr std::size_t kAtMadeBy = 96;
constexpr std::size_t kAtHeaderChecksum = 4092;

// A slot: where each thing is in it. Its checksum is the last four of the
// bytes it uses.
constexpr std::array<std::uint8_t, 4> kSlotMagic = {'B', 'N', 'S', 'L'};
constexpr std::uint32_t kSlotAtSequence = 4;
constexpr std::uint32_t kSlotAtUsed = 8;
constexpr std::uint32_t kSlotAtDataEnd = 12;
constexpr std::uint32_t kSlotAtRecords = 16;
constexpr std::uint32_t kSlotAtRecordBytes = 18;
constexpr std::uint32_t kSlotAtFirstRecord = 20;

// The sequence numbers the two slots of a new bundle are given: the first
// slot is the one in force.
constexpr std::array<std::uint32_t, 2> kFirstSequences = {2, 1};

std::uint16_t Get16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe16(bytes.subspan(at).first<2>());
}

std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe32(bytes.subspan(at).first<4>());
}

void Put16(std::span<std::uint8_t> bytes, std::size_t at, std::uint16_t value) noexcept {
  core::StoreLe16(bytes.subspan(at).first<2>(), value);
}

void Put32(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t value) noexcept {
  core::StoreLe32(bytes.subspan(at).first<4>(), value);
}

std::uint64_t RoundUp(std::uint64_t value, std::uint64_t to) noexcept { return ((value + to - 1) / to) * to; }

// How many of a slot's bytes a book of so many documents uses.
std::uint32_t SlotUsed(std::size_t documents) noexcept {
  return static_cast<std::uint32_t>(kBundleSlotOtherBytes + (documents * kBundleChapterBytes));
}

// A slot goes by a page at a time: `page` holds its bytes from `start`
// bytes into it. These three work on what the page has of some part of
// the slot.

// Puts `bytes`, which belong `at` bytes into the slot, over what the page
// has of them.
void PutIn(std::span<std::uint8_t> page, std::uint32_t start, std::uint32_t at,
           std::span<const std::uint8_t> bytes) noexcept {
  const std::uint64_t from = std::max<std::uint64_t>(at, start);
  const std::uint64_t to =
      std::min<std::uint64_t>(std::uint64_t{at} + bytes.size(), std::uint64_t{start} + page.size());
  if (from < to) {
    const std::span<const std::uint8_t> part =
        bytes.subspan(static_cast<std::size_t>(from - at), static_cast<std::size_t>(to - from));
    std::copy(part.begin(), part.end(), page.begin() + static_cast<std::ptrdiff_t>(from - start));
  }
}

// What the page has of the slot's bytes from `at` up to `end`.
std::span<const std::uint8_t> PartOf(std::span<const std::uint8_t> page, std::uint32_t start, std::uint32_t at,
                                     std::uint32_t end) noexcept {
  const std::uint64_t from = std::max<std::uint64_t>(at, start);
  const std::uint64_t to = std::min<std::uint64_t>(end, std::uint64_t{start} + page.size());
  return from < to ? page.subspan(static_cast<std::size_t>(from - start), static_cast<std::size_t>(to - from))
                   : std::span<const std::uint8_t>{};
}

// Adds to a checksum what the page has of the bytes the slot's checksum is
// of: all it uses but the last four.
std::uint32_t Sum(std::span<const std::uint8_t> page, std::uint32_t start, std::uint32_t used,
                  std::uint32_t checksum) noexcept {
  return core::Crc32(PartOf(page, start, 0, used - 4), checksum);
}

// Finishes a page of a slot that is being written: nothing after the bytes
// the slot uses, the bytes before its checksum added to `checksum`, and
// the checksum put where it goes, if any of it goes on this page. By the
// page it goes on, every byte it is of has gone by.
void FinishSlotPage(std::span<std::uint8_t> page, std::uint32_t start, std::uint32_t used,
                    std::uint32_t& checksum) noexcept {
  if (std::uint64_t{start} + page.size() > used) {
    const std::size_t from = used > start ? used - start : 0;
    std::fill(page.begin() + static_cast<std::ptrdiff_t>(from), page.end(), std::uint8_t{0});
  }
  checksum = Sum(page, start, used, checksum);
  std::array<std::uint8_t, 4> stored{};
  core::StoreLe32(stored, checksum);
  PutIn(page, start, used - 4, stored);
}

}  // namespace

const char* BundleStatusName(BundleStatus status) noexcept {
  switch (status) {
    case BundleStatus::kOk:
      return "ok";
    case BundleStatus::kNotBundle:
      return "not a bundle";
    case BundleStatus::kOtherVersion:
      return "another version";
    case BundleStatus::kDamaged:
      return "damaged";
    case BundleStatus::kStoreFailed:
      return "store failed";
    case BundleStatus::kTooBig:
      return "too big";
    case BundleStatus::kMisused:
      return "misused";
  }
  return "?";
}

void StoreBundleFile(const BundleFile& file, std::span<std::uint8_t, kBundleFileBytes> out) noexcept {
  const std::span<std::uint8_t> bytes = out;
  Put32(bytes, 0, file.entry.headerOffset);
  Put32(bytes, 4, file.entry.compressedSize);
  Put32(bytes, 8, file.entry.size);
  Put32(bytes, 12, file.entry.crc32);
  Put16(bytes, 16, file.entry.method);
  Put16(bytes, 18, file.entry.flags);
  Put16(bytes, 20, file.entry.nameLength);
  bytes[22] = file.kind;
  bytes[23] = file.flags;
}

BundleFile LoadBundleFile(std::span<const std::uint8_t, kBundleFileBytes> in) noexcept {
  const std::span<const std::uint8_t> bytes = in;
  BundleFile file;
  file.entry.headerOffset = Get32(bytes, 0);
  file.entry.compressedSize = Get32(bytes, 4);
  file.entry.size = Get32(bytes, 8);
  file.entry.crc32 = Get32(bytes, 12);
  file.entry.method = Get16(bytes, 16);
  file.entry.flags = Get16(bytes, 18);
  file.entry.nameLength = Get16(bytes, 20);
  file.kind = bytes[22];
  file.flags = bytes[23];
  return file;
}

void StoreBundleChapter(const BundleChapter& chapter, std::span<std::uint8_t, kBundleChapterBytes> out) noexcept {
  const std::span<std::uint8_t> bytes = out;
  bytes[0] = static_cast<std::uint8_t>(chapter.state);
  bytes[1] = chapter.reason;
  Put16(bytes, 2, chapter.leftOut);
  Put32(bytes, 4, chapter.segmentOffset);
  Put32(bytes, 8, chapter.segmentBytes);
  Put32(bytes, 12, chapter.directoryOffset);
  Put32(bytes, 16, chapter.directoryBytes);
  Put32(bytes, 20, chapter.directoryChecksum);
  Put32(bytes, 24, chapter.blocks);
  Put32(bytes, 28, chapter.characters);
  for (std::size_t i = 0; i < kBundleClasses; ++i) {
    Put32(bytes, 32 + (i * 4), chapter.classCharacters[i]);
  }
  std::copy(chapter.characterRows.begin(), chapter.characterRows.end(), bytes.begin() + 56);
}

bool LoadBundleChapter(std::span<const std::uint8_t, kBundleChapterBytes> in, BundleChapter& chapter) noexcept {
  const std::span<const std::uint8_t> bytes = in;
  if (bytes[0] > static_cast<std::uint8_t>(ChapterState::kRefused)) {
    return false;
  }
  chapter.state = static_cast<ChapterState>(bytes[0]);
  chapter.reason = bytes[1];
  chapter.leftOut = Get16(bytes, 2);
  chapter.segmentOffset = Get32(bytes, 4);
  chapter.segmentBytes = Get32(bytes, 8);
  chapter.directoryOffset = Get32(bytes, 12);
  chapter.directoryBytes = Get32(bytes, 16);
  chapter.directoryChecksum = Get32(bytes, 20);
  chapter.blocks = Get32(bytes, 24);
  chapter.characters = Get32(bytes, 28);
  for (std::size_t i = 0; i < kBundleClasses; ++i) {
    chapter.classCharacters[i] = Get32(bytes, 32 + (i * 4));
  }
  const std::span<const std::uint8_t> rows = bytes.subspan(56, chapter.characterRows.size());
  std::copy(rows.begin(), rows.end(), chapter.characterRows.begin());
  return true;
}

std::uint32_t BundleSlotBytes(std::size_t documents) noexcept {
  return static_cast<std::uint32_t>(RoundUp(SlotUsed(std::min(documents, kBundleMostOfEach)), kPage));
}

// ---- Making a bundle ----

BundleMaker::BundleMaker(std::span<std::uint8_t> page) noexcept : page_(page) {}

BundleStatus BundleMaker::Fail(BundleStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kIdle;
  return status;
}

bool BundleMaker::WritePage(std::uint32_t offset) noexcept {
  if (!file_->Seek(offset) || !file_->Write(page_)) {
    Fail(BundleStatus::kStoreFailed);
    return false;
  }
  return true;
}

BundleStatus BundleMaker::Begin(core::BlockFile& file) noexcept {
  stage_ = Stage::kIdle;
  status_ = BundleStatus::kMisused;
  if (page_.size() != kPage || !file.IsOpen() || file.Size() != 0) {
    return status_;
  }
  file_ = &file;
  book_ = {};
  tablesBegun_ = 0;
  tableOffset_.fill(0);
  tableBytes_.fill(0);
  tableChecksum_.fill(0);
  filled_ = 0;
  // The header page's place is taken by nothing for now: it is written
  // last, over this, so that the file is no bundle until everything else
  // is in it.
  std::fill(page_.begin(), page_.end(), std::uint8_t{0});
  status_ = BundleStatus::kOk;
  if (!WritePage(0)) {
    return status_;
  }
  pageOffset_ = kPage;
  stage_ = Stage::kTables;
  return status_;
}

BundleStatus BundleMaker::BeginTable(BundleTable table) noexcept {
  if (stage_ != Stage::kTables || static_cast<std::size_t>(table) != tablesBegun_) {
    return BundleStatus::kMisused;
  }
  tableOffset_[tablesBegun_] = pageOffset_ + static_cast<std::uint32_t>(filled_);
  ++tablesBegun_;
  return BundleStatus::kOk;
}

BundleStatus BundleMaker::Write(std::span<const std::uint8_t> bytes) noexcept {
  if (stage_ != Stage::kTables || tablesBegun_ == 0) {
    return BundleStatus::kMisused;
  }
  const std::size_t table = tablesBegun_ - 1;
  // The tables, the slots after them and a segment have to fit in a file.
  if (std::uint64_t{pageOffset_} + filled_ + bytes.size() > kMostBytes / 2) {
    return Fail(BundleStatus::kTooBig);
  }
  tableChecksum_[table] = core::Crc32(bytes, tableChecksum_[table]);
  tableBytes_[table] += static_cast<std::uint32_t>(bytes.size());
  while (!bytes.empty()) {
    const std::size_t count = std::min(bytes.size(), page_.size() - filled_);
    std::copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count),
              page_.begin() + static_cast<std::ptrdiff_t>(filled_));
    filled_ += count;
    bytes = bytes.subspan(count);
    if (filled_ == page_.size()) {
      if (!WritePage(pageOffset_)) {
        return status_;
      }
      pageOffset_ += kPage;
      filled_ = 0;
    }
  }
  return BundleStatus::kOk;
}

BundleStatus BundleMaker::WriteFile(const BundleFile& file) noexcept {
  std::array<std::uint8_t, kBundleFileBytes> record{};
  StoreBundleFile(file, record);
  return Write(record);
}

BundleStatus BundleMaker::Finish(const BundleBook& book) noexcept {
  if (stage_ != Stage::kTables || tablesBegun_ != kBundleTables ||
      tableBytes_[static_cast<std::size_t>(BundleTable::kSpine)] != book.documents * kFileBytes ||
      tableBytes_[static_cast<std::size_t>(BundleTable::kPictures)] != book.pictures * kFileBytes) {
    return BundleStatus::kMisused;
  }
  book_ = book;
  if (filled_ != 0) {
    std::fill(page_.begin() + static_cast<std::ptrdiff_t>(filled_), page_.end(), std::uint8_t{0});
    if (!WritePage(pageOffset_)) {
      return status_;
    }
    pageOffset_ += kPage;
    filled_ = 0;
  }
  slotOffset_ = pageOffset_;
  slotBytes_ = BundleSlotBytes(book_.documents);
  slotUsed_ = SlotUsed(book_.documents);
  if (std::uint64_t{slotOffset_} + (std::uint64_t{slotBytes_} * 2) > kMostBytes / 2) {
    return Fail(BundleStatus::kTooBig);
  }
  slot_ = 0;
  slotDone_ = 0;
  slotChecksum_ = 0;
  stage_ = Stage::kSlots;
  return BundleStatus::kOk;
}

void BundleMaker::FillSlotPage() noexcept {
  // A slot of a new bundle is nothing but its first twenty bytes and its
  // checksum: a record of zeros is a chapter not imported.
  std::fill(page_.begin(), page_.end(), std::uint8_t{0});
  std::array<std::uint8_t, kSlotAtFirstRecord> first{};
  const std::span<std::uint8_t> bytes = first;
  std::copy(kSlotMagic.begin(), kSlotMagic.end(), first.begin());
  Put32(bytes, kSlotAtSequence, kFirstSequences[slot_]);
  Put32(bytes, kSlotAtUsed, slotUsed_);
  Put32(bytes, kSlotAtDataEnd, slotOffset_ + (slotBytes_ * 2));
  Put16(bytes, kSlotAtRecords, book_.documents);
  Put16(bytes, kSlotAtRecordBytes, static_cast<std::uint16_t>(kChapterBytes));
  PutIn(page_, slotDone_, 0, first);
  FinishSlotPage(page_, slotDone_, slotUsed_, slotChecksum_);
}

void BundleMaker::FillHeaderPage() noexcept {
  std::fill(page_.begin(), page_.end(), std::uint8_t{0});
  std::copy(kMagic.begin(), kMagic.end(), page_.begin());
  Put16(page_, kAtFormat, kBundleFormat);
  Put16(page_, kAtTextRules, book_.textRules);
  Put16(page_, kAtImporter, book_.importer);
  Put32(page_, kAtEpubBytes, book_.epubBytes);
  Put32(page_, kAtEpubModified, book_.epubModified);
  Put32(page_, kAtEpubDirectory, book_.epubDirectoryChecksum);
  Put32(page_, kAtPackage, book_.packageChecksum);
  Put16(page_, kAtDocuments, book_.documents);
  Put16(page_, kAtPictures, book_.pictures);
  Put16(page_, kAtContents, book_.contents);
  Put16(page_, kAtAmiss, book_.amiss);
  Put32(page_, kAtSlotOffset, slotOffset_);
  Put32(page_, kAtSlotBytes, slotBytes_);
  Put32(page_, kAtDataBegin, slotOffset_ + (slotBytes_ * 2));
  for (std::size_t table = 0; table < kBundleTables; ++table) {
    Put32(page_, kAtTables + (table * 12), tableOffset_[table]);
    Put32(page_, kAtTables + (table * 12) + 4, tableBytes_[table]);
    Put32(page_, kAtTables + (table * 12) + 8, tableChecksum_[table]);
  }
  std::copy(book_.madeBy.begin(), book_.madeBy.end(), page_.begin() + kAtMadeBy);
  Put32(page_, kAtHeaderChecksum, core::Crc32(page_.first(kAtHeaderChecksum)));
}

bool BundleMaker::Step() noexcept {
  switch (stage_) {
    case Stage::kIdle:
    case Stage::kTables:
    case Stage::kDone:
      return false;
    case Stage::kSlots:
      FillSlotPage();
      if (!WritePage(slotOffset_ + (static_cast<std::uint32_t>(slot_) * slotBytes_) + slotDone_)) {
        return false;
      }
      slotDone_ += kPage;
      if (slotDone_ >= slotBytes_) {
        slotDone_ = 0;
        slotChecksum_ = 0;
        ++slot_;
        if (slot_ == 2) {
          stage_ = Stage::kHeader;
        }
      }
      return true;
    case Stage::kHeader:
      FillHeaderPage();
      if (!WritePage(0)) {
        return false;
      }
      stage_ = Stage::kSync;
      return true;
    case Stage::kSync:
      if (!file_->Sync()) {
        Fail(BundleStatus::kStoreFailed);
        return false;
      }
      stage_ = Stage::kDone;
      return false;
  }
  return false;
}

// ---- A bundle that is open ----

Bundle::Bundle(std::span<std::uint8_t> page) noexcept : page_(page) {}

void Bundle::Close() noexcept {
  file_ = nullptr;
  open_ = false;
  job_ = Job::kNone;
  status_ = BundleStatus::kMisused;
}

bool Bundle::End(BundleStatus status) noexcept {
  status_ = status;
  job_ = Job::kNone;
  return false;
}

bool Bundle::ReadPage(std::uint32_t offset, std::size_t bytes) noexcept {
  return file_->ReadAt(offset, page_.first(bytes));
}

std::uint32_t Bundle::SlotOffset(std::size_t slot) const noexcept {
  return slotOffset_ + (static_cast<std::uint32_t>(slot) * slotBytes_);
}

BundleStatus Bundle::ReadRecord(std::uint32_t offset, std::span<std::uint8_t> record) noexcept {
  return file_->ReadAt(offset, record) ? BundleStatus::kOk : BundleStatus::kStoreFailed;
}

bool Bundle::IsOf(std::uint32_t epubBytes, std::uint32_t epubModified,
                  std::uint32_t epubDirectoryChecksum) const noexcept {
  return open_ && book_.epubBytes == epubBytes && book_.epubModified == epubModified &&
         book_.epubDirectoryChecksum == epubDirectoryChecksum;
}

Bundle::Place Bundle::Table(BundleTable table) const noexcept {
  const std::size_t index = static_cast<std::size_t>(table);
  return open_ && index < kBundleTables ? tables_[index] : Place{};
}

BundleStatus Bundle::BeginOpen(core::BlockFile& file) noexcept {
  Close();
  if (page_.size() != kPage || !file.IsOpen()) {
    return status_;
  }
  file_ = &file;
  job_ = Job::kOpening;
  stage_ = Stage::kHeader;
  status_ = BundleStatus::kOk;
  return status_;
}

BundleStatus Bundle::Open(core::BlockFile& file) noexcept {
  if (BeginOpen(file) == BundleStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

bool Bundle::Step() noexcept {
  if (job_ == Job::kNone) {
    return false;
  }
  switch (stage_) {
    case Stage::kHeader:
      OpenHeader();
      break;
    case Stage::kHeads:
      OpenHead();
      break;
    case Stage::kSlot:
      OpenSlot();
      break;
    case Stage::kFill:
      Fill();
      break;
    case Stage::kSyncSegment:
      if (file_->Sync()) {
        stage_ = Stage::kCopy;
      } else {
        open_ = false;
        End(BundleStatus::kStoreFailed);
      }
      break;
    case Stage::kCopy:
      Copy();
      break;
    case Stage::kSyncSlot:
      if (!file_->Sync()) {
        open_ = false;
        End(BundleStatus::kStoreFailed);
        break;
      }
      // The slot just written is the one in force.
      current_ = 1 - current_;
      ++sequence_;
      dataEnd_ = newDataEnd_;
      totals_.notImported -= before_.state == ChapterState::kNotImported ? 1 : 0;
      totals_.notImported += after_.state == ChapterState::kNotImported ? 1 : 0;
      totals_.characters += after_.characters;
      totals_.characters -= before_.characters;
      for (std::size_t i = 0; i < kBundleClasses; ++i) {
        totals_.classCharacters[i] += after_.classCharacters[i];
        totals_.classCharacters[i] -= before_.classCharacters[i];
      }
      End(BundleStatus::kOk);
      break;
    case Stage::kTable:
      CheckSome();
      break;
  }
  return job_ != Job::kNone;
}

void Bundle::OpenHeader() noexcept {
  if (file_->Size() < kPage) {
    End(BundleStatus::kNotBundle);
    return;
  }
  if (!ReadPage(0, kPage)) {
    End(BundleStatus::kStoreFailed);
    return;
  }
  const std::span<const std::uint8_t> page = page_;
  if (!std::equal(kMagic.begin(), kMagic.end(), page.begin())) {
    End(BundleStatus::kNotBundle);
    return;
  }
  if (Get16(page, kAtFormat) != kBundleFormat) {
    End(BundleStatus::kOtherVersion);
    return;
  }
  if (Get32(page, kAtHeaderChecksum) != core::Crc32(page.first(kAtHeaderChecksum))) {
    End(BundleStatus::kDamaged);
    return;
  }
  book_.textRules = Get16(page, kAtTextRules);
  book_.importer = Get16(page, kAtImporter);
  book_.epubBytes = Get32(page, kAtEpubBytes);
  book_.epubModified = Get32(page, kAtEpubModified);
  book_.epubDirectoryChecksum = Get32(page, kAtEpubDirectory);
  book_.packageChecksum = Get32(page, kAtPackage);
  book_.documents = Get16(page, kAtDocuments);
  book_.pictures = Get16(page, kAtPictures);
  book_.contents = Get16(page, kAtContents);
  book_.amiss = Get16(page, kAtAmiss);
  const std::span<const std::uint8_t> madeBy = page.subspan(kAtMadeBy, book_.madeBy.size());
  std::copy(madeBy.begin(), madeBy.end(), book_.madeBy.begin());
  slotOffset_ = Get32(page, kAtSlotOffset);
  slotBytes_ = Get32(page, kAtSlotBytes);
  dataBegin_ = Get32(page, kAtDataBegin);
  slotUsed_ = SlotUsed(book_.documents);

  // The header passed its checksum; it has still to say what a header
  // says. The tables follow the header page and one another, the slots
  // begin on the page after them, and the segments after the slots.
  std::uint64_t end = kPage;
  for (std::size_t table = 0; table < kBundleTables; ++table) {
    tables_[table] = {.offset = Get32(page, kAtTables + (table * 12)),
                      .bytes = Get32(page, kAtTables + (table * 12) + 4),
                      .checksum = Get32(page, kAtTables + (table * 12) + 8)};
    if (tables_[table].offset != end) {
      End(BundleStatus::kDamaged);
      return;
    }
    end += tables_[table].bytes;
  }
  if (tables_[static_cast<std::size_t>(BundleTable::kSpine)].bytes != book_.documents * kFileBytes ||
      tables_[static_cast<std::size_t>(BundleTable::kPictures)].bytes != book_.pictures * kFileBytes ||
      slotOffset_ != RoundUp(end, kPage) || slotBytes_ != BundleSlotBytes(book_.documents) ||
      dataBegin_ != std::uint64_t{slotOffset_} + (std::uint64_t{slotBytes_} * 2) || file_->Size() < dataBegin_) {
    End(BundleStatus::kDamaged);
    return;
  }
  heads_ = {};
  wanting_ = {};
  slot_ = 0;
  stage_ = Stage::kHeads;
}

void Bundle::OpenHead() noexcept {
  if (!ReadPage(SlotOffset(slot_), kPage)) {
    End(BundleStatus::kStoreFailed);
    return;
  }
  const std::span<const std::uint8_t> page = page_;
  Head& head = heads_[slot_];
  head.sequence = Get32(page, kSlotAtSequence);
  head.dataEnd = Get32(page, kSlotAtDataEnd);
  head.plausible = std::equal(kSlotMagic.begin(), kSlotMagic.end(), page.begin()) &&
                   Get32(page, kSlotAtUsed) == slotUsed_ && Get16(page, kSlotAtRecords) == book_.documents &&
                   Get16(page, kSlotAtRecordBytes) == kChapterBytes && head.dataEnd >= dataBegin_ &&
                   head.dataEnd <= file_->Size() && head.dataEnd % kSector == 0;
  wanting_[slot_] = !head.plausible;
  ++slot_;
  if (slot_ == heads_.size() && !ChooseSlot()) {
    End(BundleStatus::kDamaged);
  }
}

bool Bundle::ChooseSlot() noexcept {
  if (wanting_[0] && wanting_[1]) {
    return false;
  }
  slot_ = wanting_[0] || (!wanting_[1] && heads_[1].sequence > heads_[0].sequence) ? 1 : 0;
  done_ = 0;
  checksum_ = 0;
  counted_ = {};
  recordFilled_ = 0;
  stored_ = {};
  stage_ = Stage::kSlot;
  return true;
}

bool Bundle::CountRecord() noexcept {
  BundleChapter chapter;
  if (!LoadBundleChapter(record_, chapter)) {
    return false;
  }
  if (chapter.segmentBytes != 0) {
    // Its segment is among the segments, and its directory is in it.
    const std::uint64_t end = std::uint64_t{chapter.segmentOffset} + chapter.segmentBytes;
    if (chapter.segmentOffset < dataBegin_ || chapter.segmentOffset % kSector != 0 || end > heads_[slot_].dataEnd ||
        std::uint64_t{chapter.directoryOffset} + chapter.directoryBytes > chapter.segmentBytes) {
      return false;
    }
  }
  counted_.notImported += chapter.state == ChapterState::kNotImported ? 1 : 0;
  counted_.characters += chapter.characters;
  for (std::size_t i = 0; i < kBundleClasses; ++i) {
    counted_.classCharacters[i] += chapter.classCharacters[i];
  }
  return true;
}

bool Bundle::Count(std::span<const std::uint8_t> bytes) noexcept {
  while (!bytes.empty()) {
    const std::size_t count = std::min(bytes.size(), record_.size() - recordFilled_);
    std::copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count),
              record_.begin() + static_cast<std::ptrdiff_t>(recordFilled_));
    recordFilled_ += count;
    bytes = bytes.subspan(count);
    if (recordFilled_ == record_.size()) {
      recordFilled_ = 0;
      if (!CountRecord()) {
        return false;
      }
    }
  }
  return true;
}

void Bundle::OpenSlot() noexcept {
  if (!ReadPage(SlotOffset(slot_) + done_, kPage)) {
    End(BundleStatus::kStoreFailed);
    return;
  }
  const std::span<const std::uint8_t> page = page_;
  checksum_ = Sum(page, done_, slotUsed_, checksum_);
  // What the page has of the checksum the slot carries, to set beside the
  // one worked out when all of it has gone by.
  const std::span<const std::uint8_t> carried = PartOf(page, done_, slotUsed_ - 4, slotUsed_);
  if (!carried.empty()) {
    const std::size_t at = std::max(done_, slotUsed_ - 4) - (slotUsed_ - 4);
    std::copy(carried.begin(), carried.end(), stored_.begin() + static_cast<std::ptrdiff_t>(at));
  }
  bool whole = Count(PartOf(page, done_, kSlotAtFirstRecord, slotUsed_ - 4));
  done_ += kPage;
  if (whole && done_ < slotUsed_) {
    return;  // More of it to come.
  }
  whole = whole && core::LoadLe32(stored_) == checksum_;
  if (whole) {
    current_ = slot_;
    sequence_ = heads_[slot_].sequence;
    dataEnd_ = heads_[slot_].dataEnd;
    totals_ = counted_;
    open_ = true;
    End(BundleStatus::kOk);
    return;
  }
  wanting_[slot_] = true;
  if (!ChooseSlot()) {
    End(BundleStatus::kDamaged);
  }
}

BundleStatus Bundle::ReadSpine(std::size_t index, BundleFile& file) noexcept {
  if (!Idle() || index >= book_.documents) {
    return BundleStatus::kMisused;
  }
  std::array<std::uint8_t, kBundleFileBytes> record{};
  const std::uint32_t offset =
      tables_[static_cast<std::size_t>(BundleTable::kSpine)].offset + (static_cast<std::uint32_t>(index) * kFileBytes);
  const BundleStatus status = ReadRecord(offset, record);
  if (status == BundleStatus::kOk) {
    file = LoadBundleFile(record);
  }
  return status;
}

BundleStatus Bundle::ReadPicture(std::size_t index, BundleFile& file) noexcept {
  if (!Idle() || index >= book_.pictures) {
    return BundleStatus::kMisused;
  }
  std::array<std::uint8_t, kBundleFileBytes> record{};
  const std::uint32_t offset = tables_[static_cast<std::size_t>(BundleTable::kPictures)].offset +
                               (static_cast<std::uint32_t>(index) * kFileBytes);
  const BundleStatus status = ReadRecord(offset, record);
  if (status == BundleStatus::kOk) {
    file = LoadBundleFile(record);
  }
  return status;
}

BundleStatus Bundle::ReadChapter(std::size_t index, BundleChapter& chapter) noexcept {
  if (!Idle() || index >= book_.documents) {
    return BundleStatus::kMisused;
  }
  std::array<std::uint8_t, kBundleChapterBytes> record{};
  const std::uint32_t offset =
      SlotOffset(current_) + kSlotAtFirstRecord + (static_cast<std::uint32_t>(index) * kChapterBytes);
  const BundleStatus status = ReadRecord(offset, record);
  if (status != BundleStatus::kOk) {
    return status;
  }
  // The slot passed its checksum with a record here: bytes that are none
  // now were changed since.
  return LoadBundleChapter(record, chapter) ? BundleStatus::kOk : BundleStatus::kDamaged;
}

BundleStatus Bundle::ReadTable(BundleTable table, std::uint32_t at, std::span<std::uint8_t> dst) noexcept {
  const std::size_t index = static_cast<std::size_t>(table);
  if (!Idle() || index >= kBundleTables || std::uint64_t{at} + dst.size() > tables_[index].bytes) {
    return BundleStatus::kMisused;
  }
  return dst.empty() ? BundleStatus::kOk : ReadRecord(tables_[index].offset + at, dst);
}

BundleStatus Bundle::BeginCheck(BundleTable table) noexcept {
  const std::size_t index = static_cast<std::size_t>(table);
  if (!Idle() || index >= kBundleTables) {
    return BundleStatus::kMisused;
  }
  table_ = index;
  done_ = 0;
  checksum_ = 0;
  job_ = Job::kChecking;
  stage_ = Stage::kTable;
  status_ = BundleStatus::kOk;
  return status_;
}

BundleStatus Bundle::Check(BundleTable table) noexcept {
  const BundleStatus began = BeginCheck(table);
  if (began != BundleStatus::kOk) {
    return began;
  }
  while (Step()) {
  }
  return status_;
}

void Bundle::CheckSome() noexcept {
  const Place& place = tables_[table_];
  if (done_ < place.bytes) {
    // As far as the end of the page of the file it is on, so that a step
    // is one read of storage.
    const std::uint32_t at = place.offset + done_;
    const std::uint32_t count = std::min<std::uint32_t>(place.bytes - done_, kPage - (at % kPage));
    if (!ReadPage(at, count)) {
      End(BundleStatus::kStoreFailed);
      return;
    }
    checksum_ = core::Crc32(page_.first(count), checksum_);
    done_ += count;
  }
  if (done_ == place.bytes) {
    End(checksum_ == place.checksum ? BundleStatus::kOk : BundleStatus::kDamaged);
  }
}

BundleStatus Bundle::BeginCommit(std::size_t chapter, const BundleChapter& record) noexcept {
  if (!Idle() || chapter >= book_.documents || record.state > ChapterState::kRefused) {
    return BundleStatus::kMisused;
  }
  std::uint64_t end = dataEnd_;
  if (record.segmentBytes != 0) {
    end = RoundUp(std::uint64_t{record.segmentOffset} + record.segmentBytes, kSector);
    if (record.segmentOffset != dataEnd_ || std::uint64_t{record.segmentOffset} + record.segmentBytes > file_->Size() ||
        std::uint64_t{record.directoryOffset} + record.directoryBytes > record.segmentBytes) {
      return BundleStatus::kMisused;
    }
  } else if (record.segmentOffset != 0 || record.directoryOffset != 0 || record.directoryBytes != 0) {
    return BundleStatus::kMisused;
  }
  if (end > kMostBytes || sequence_ == std::numeric_limits<std::uint32_t>::max()) {
    return BundleStatus::kTooBig;
  }
  // The record as it is, for what the chapters come to once it is changed.
  std::array<std::uint8_t, kBundleChapterBytes> was{};
  const std::uint32_t offset =
      SlotOffset(current_) + kSlotAtFirstRecord + (static_cast<std::uint32_t>(chapter) * kChapterBytes);
  if (ReadRecord(offset, was) != BundleStatus::kOk) {
    return BundleStatus::kStoreFailed;
  }
  if (!LoadBundleChapter(was, before_)) {
    return BundleStatus::kDamaged;
  }
  chapter_ = chapter;
  after_ = record;
  StoreBundleChapter(record, changed_);
  newDataEnd_ = static_cast<std::uint32_t>(end);
  done_ = 0;
  checksum_ = 0;
  job_ = Job::kCommitting;
  stage_ = record.segmentBytes != 0 ? Stage::kFill : Stage::kCopy;
  status_ = BundleStatus::kOk;
  return status_;
}

BundleStatus Bundle::Commit(std::size_t chapter, const BundleChapter& record) noexcept {
  const BundleStatus began = BeginCommit(chapter, record);
  if (began != BundleStatus::kOk) {
    return began;
  }
  while (Step()) {
  }
  return status_;
}

void Bundle::Fill() noexcept {
  // Zeros from the segment's end to the end of its last sector, whatever
  // was there, so that the next segment begins on a sector of its own.
  const std::uint32_t end = after_.segmentOffset + after_.segmentBytes;
  if (end < newDataEnd_) {
    const std::span<std::uint8_t> zeros = page_.first(newDataEnd_ - end);
    std::fill(zeros.begin(), zeros.end(), std::uint8_t{0});
    if (!file_->Seek(end) || !file_->Write(zeros)) {
      open_ = false;
      End(BundleStatus::kStoreFailed);
      return;
    }
  }
  stage_ = Stage::kSyncSegment;
}

void Bundle::Copy() noexcept {
  // A page of the slot in force, with the chapter's record, the sequence
  // number and the segments' end changed, to the other slot.
  if (!ReadPage(SlotOffset(current_) + done_, kPage)) {
    open_ = false;
    End(BundleStatus::kStoreFailed);
    return;
  }
  std::array<std::uint8_t, 4> number{};
  core::StoreLe32(number, sequence_ + 1);
  PutIn(page_, done_, kSlotAtSequence, number);
  core::StoreLe32(number, newDataEnd_);
  PutIn(page_, done_, kSlotAtDataEnd, number);
  PutIn(page_, done_, kSlotAtFirstRecord + (static_cast<std::uint32_t>(chapter_) * kChapterBytes), changed_);
  FinishSlotPage(page_, done_, slotUsed_, checksum_);
  if (!file_->Seek(SlotOffset(1 - current_) + done_) || !file_->Write(page_)) {
    open_ = false;
    End(BundleStatus::kStoreFailed);
    return;
  }
  done_ += kPage;
  if (done_ >= slotUsed_) {
    stage_ = Stage::kSyncSlot;
  }
}

}  // namespace bn::epub
