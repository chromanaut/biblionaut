#include "epub/book_bundle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/endian.h"
#include "epub/book_path.h"
#include "epub/package_digest.h"

namespace bn::epub {

namespace {

// The most a string of the first table, and a fragment and a label of the
// contents, can be: what the readers that give them keep.
constexpr std::size_t kMostString = 256;
constexpr std::size_t kContentsEntryBytes = 6;

std::span<const std::uint8_t> BytesOf(std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// The first `most` bytes of `text`, or all of it.
std::string_view First(std::string_view text, std::size_t most) noexcept {
  if (text.size() > most) {
    text.remove_suffix(text.size() - most);
  }
  return text;
}

}  // namespace

const char* BookBundleStatusName(BookBundleStatus status) noexcept {
  switch (status) {
    case BookBundleStatus::kOk:
      return "ok";
    case BookBundleStatus::kNoBook:
      return "no book";
    case BookBundleStatus::kUnreadable:
      return "unreadable";
    case BookBundleStatus::kStoreFailed:
      return "store failed";
    case BookBundleStatus::kTooBig:
      return "too big";
    case BookBundleStatus::kMisused:
      return "misused";
  }
  return "?";
}

BookBundler::BookBundler(std::span<std::uint8_t> page, std::span<std::uint8_t> records) noexcept
    : records_(records), maker_(page) {}

bool BookBundler::End(BookBundleStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kIdle;
  return false;
}

bool BookBundler::Wrote(BundleStatus status) noexcept {
  switch (status) {
    case BundleStatus::kOk:
      return true;
    case BundleStatus::kTooBig:
      End(BookBundleStatus::kTooBig);
      break;
    case BundleStatus::kMisused:
      End(BookBundleStatus::kMisused);
      break;
    default:
      End(BookBundleStatus::kStoreFailed);
      break;
  }
  return false;
}

BookBundleStatus BookBundler::Begin(const Package& package, ZipArchive& archive, core::BlockFile& entries,
                                    ZipEntryReader& reader, XmlParser& xml, core::BlockFile& bundle,
                                    const BundleBook& about) noexcept {
  stage_ = Stage::kIdle;
  status_ = BookBundleStatus::kMisused;
  if (records_.size() != kBundlePageBytes) {
    return status_;
  }
  if (package.Status() != PackageStatus::kOk || package.Spine().empty() || !archive.IsOpen() || !entries.IsOpen()) {
    status_ = BookBundleStatus::kNoBook;
    return status_;
  }
  if (package.Spine().size() > kBundleMostOfEach || package.Pictures() > kBundleMostOfEach) {
    status_ = BookBundleStatus::kTooBig;
    return status_;
  }
  package_ = &package;
  archive_ = &archive;
  entries_ = &entries;
  reader_ = &reader;
  xml_ = &xml;
  book_ = about;
  book_.epubBytes = entries.Size();
  book_.epubDirectoryChecksum = 0;
  book_.packageChecksum = PackageChecksum(package);
  book_.documents = static_cast<std::uint16_t>(package.Spine().size());
  book_.pictures = static_cast<std::uint16_t>(package.Pictures());
  book_.contents = 0;
  book_.amiss = package.CutShort() ? kBundlePackageCutShort : std::uint16_t{0};
  done_ = 0;
  passFirst_ = 0;
  passCount_ = 0;
  picturesPresent_ = 0;
  passes_ = 0;
  tocBegun_ = false;
  writeFailed_ = false;
  contents_ = 0;
  status_ = BookBundleStatus::kOk;
  if (!Wrote(maker_.Begin(bundle))) {
    return status_;
  }
  stage_ = Stage::kStrings;
  return status_;
}

BookBundleStatus BookBundler::Make(const Package& package, ZipArchive& archive, core::BlockFile& entries,
                                   ZipEntryReader& reader, XmlParser& xml, core::BlockFile& bundle,
                                   const BundleBook& about) noexcept {
  if (Begin(package, archive, entries, reader, xml, bundle, about) == BookBundleStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

bool BookBundler::Step() noexcept {
  switch (stage_) {
    case Stage::kIdle:
      return false;
    case Stage::kStrings:
      WriteStrings();
      break;
    case Stage::kSpine:
      WriteSpine();
      break;
    case Stage::kPictures:
      GoThroughDirectory();
      break;
    case Stage::kDirectory:
      SumDirectory();
      break;
    case Stage::kContents:
      ReadContents();
      break;
    case Stage::kFinish:
      if (!maker_.Step()) {
        if (Wrote(maker_.Status())) {
          End(BookBundleStatus::kOk);
        }
      }
      break;
  }
  return stage_ != Stage::kIdle;
}

void BookBundler::WriteStrings() noexcept {
  if (!Wrote(maker_.BeginTable(BundleTable::kStrings))) {
    return;
  }
  for (const std::string_view text :
       {package_->Title(), package_->Author(), package_->Language(), package_->Identifier()}) {
    const std::string_view kept = First(text, kMostString);
    std::array<std::uint8_t, 2> length{};
    core::StoreLe16(length, static_cast<std::uint16_t>(kept.size()));
    if (!Wrote(maker_.Write(length)) || !Wrote(maker_.Write(BytesOf(kept)))) {
      return;
    }
  }
  if (Wrote(maker_.BeginTable(BundleTable::kSpine))) {
    done_ = 0;
    stage_ = Stage::kSpine;
  }
}

void BookBundler::WriteSpine() noexcept {
  const std::span<const SpineItem> spine = package_->Spine();
  const std::size_t end = std::min(spine.size(), done_ + kPicturesPerPass);
  for (; done_ < end; ++done_) {
    const SpineItem& item = spine[done_];
    BundleFile file;
    file.kind = static_cast<std::uint8_t>(item.type);
    file.flags =
        static_cast<std::uint8_t>((item.linear ? kBundleFileInRun : 0U) | (item.present ? kBundleFilePresent : 0U));
    if (item.present) {
      file.entry = item.entry;
    }
    if (!Wrote(maker_.WriteFile(file))) {
      return;
    }
  }
  if (done_ == spine.size() && Wrote(maker_.BeginTable(BundleTable::kPictures))) {
    passFirst_ = 0;
    passCount_ = 0;
    BeginPass();
  }
}

void BookBundler::BeginPass() noexcept {
  passFirst_ += passCount_;
  passCount_ = std::min<std::size_t>(package_->Pictures() - passFirst_, kPicturesPerPass);
  if (passCount_ == 0) {
    // No more of them: the directory's own bytes, for their checksum.
    sum_.Begin(*archive_, *entries_, records_);
    stage_ = Stage::kDirectory;
    return;
  }
  // Every picture of this pass has a record of nothing until its file is
  // met.
  std::fill(records_.begin(), records_.end(), std::uint8_t{0});
  cursor_ = {};
  ++passes_;
  stage_ = Stage::kPictures;
}

void BookBundler::GoThroughDirectory() noexcept {
  std::array<char, kMaxBookPath> name{};
  std::uint32_t gone = 0;
  std::uint32_t compares = 0;
  for (std::uint32_t looked = 0;
       looked < kEntriesPerStep && gone < kDirectoryBytesPerStep && compares < kComparesPerStep; ++looked) {
    const bool first = cursor_.index == 0;
    const std::uint32_t before = cursor_.offset;
    ZipEntry entry;
    const ZipStatus status = archive_->Next(cursor_, entry, name);
    if (status == ZipStatus::kEnd) {
      // The pass is over: its records, as far as they were found.
      if (Wrote(maker_.Write(records_.first(passCount_ * kBundleFileBytes)))) {
        BeginPass();
      }
      return;
    }
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      End(BookBundleStatus::kUnreadable);
      return;
    }
    // Where the first entry is, only the archive knows: what is counted
    // for it is the least a record of its name's length takes.
    gone += first ? 46U + entry.nameLength : cursor_.offset - before;
    if (status == ZipStatus::kUnsupported || entry.nameLength > name.size()) {
      continue;  // An entry that cannot be read, or with a name longer than any the manifest's were kept by.
    }
    compares += package_->ManifestItems();
    const Package::Picture picture = package_->PictureOf(std::string_view(name.data(), entry.nameLength));
    if (picture.index < passFirst_ || picture.index >= passFirst_ + passCount_) {
      continue;  // Not a picture, or one of another pass.
    }
    const std::span<std::uint8_t, kBundleFileBytes> record =
        records_.subspan((picture.index - passFirst_) * kBundleFileBytes).first<kBundleFileBytes>();
    if ((LoadBundleFile(record).flags & kBundleFilePresent) != 0) {
      continue;  // The ZIP file has the name twice: the first is the one.
    }
    BundleFile file;
    file.entry = entry;
    file.kind = static_cast<std::uint8_t>(picture.type);
    file.flags = static_cast<std::uint8_t>(kBundleFilePresent | (picture.isCover ? kBundleFileCover : 0U));
    StoreBundleFile(file, record);
    ++picturesPresent_;
  }
}

void BookBundler::SumDirectory() noexcept {
  if (sum_.Step()) {
    return;
  }
  if (sum_.Status() != ZipStatus::kOk) {
    End(BookBundleStatus::kUnreadable);
    return;
  }
  book_.epubDirectoryChecksum = sum_.Checksum();
  if (Wrote(maker_.BeginTable(BundleTable::kContents))) {
    BeginContents();
  }
}

void BookBundler::BeginContents() noexcept {
  stage_ = Stage::kContents;
  entriesWereSound_ = entries_->LastStatus() == core::StorageStatus::kOk;
  const TocStatus status = toc_.Begin(*package_, *entries_, *reader_, *xml_, *this);
  tocBegun_ = status == TocStatus::kOk;
  if (!tocBegun_ && status != TocStatus::kNoToc) {
    book_.amiss |= kBundleContentsNotWhole;  // It has one, which could not be begun on.
  }
}

bool BookBundler::OnEntry(const TocEntry& entry) noexcept {
  if (contents_ == kBundleMostOfEach) {
    book_.amiss |= kBundleContentsNotWhole;
    return false;
  }
  std::array<std::uint8_t, kContentsEntryBytes> start{};
  const std::string_view fragment = First(entry.fragment, 255);
  const std::string_view label = First(entry.label, kMostString);
  start[0] = static_cast<std::uint8_t>(std::min<std::uint32_t>(entry.depth, 255));
  start[1] = static_cast<std::uint8_t>(fragment.size());
  const std::uint16_t chapter =
      entry.document < kBundleNoChapter ? static_cast<std::uint16_t>(entry.document) : kBundleNoChapter;
  core::StoreLe16(std::span(start).subspan<2, 2>(), chapter);
  core::StoreLe16(std::span(start).subspan<4, 2>(), static_cast<std::uint16_t>(label.size()));
  if (maker_.Write(start) != BundleStatus::kOk || maker_.Write(BytesOf(fragment)) != BundleStatus::kOk ||
      maker_.Write(BytesOf(label)) != BundleStatus::kOk) {
    writeFailed_ = true;
    return false;
  }
  ++contents_;
  return true;
}

void BookBundler::ReadContents() noexcept {
  if (tocBegun_ && toc_.Step()) {
    return;
  }
  if (writeFailed_) {
    Wrote(maker_.Status() == BundleStatus::kOk ? BundleStatus::kStoreFailed : maker_.Status());
    return;
  }
  // Storage failing is not the book's doing: the bundle is not made.
  if (entriesWereSound_ && entries_->LastStatus() != core::StorageStatus::kOk) {
    End(BookBundleStatus::kUnreadable);
    return;
  }
  // However the reading of it ended otherwise, the entries that came
  // before stand.
  if (tocBegun_ && (toc_.Status() != TocStatus::kOk || toc_.CutShort())) {
    book_.amiss |= kBundleContentsNotWhole;
  }
  book_.contents = static_cast<std::uint16_t>(contents_);
  if (Wrote(maker_.Finish(book_))) {
    stage_ = Stage::kFinish;
  }
}

}  // namespace bn::epub
