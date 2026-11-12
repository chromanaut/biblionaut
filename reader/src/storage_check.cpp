#include "reader/storage_check.h"

#include <algorithm>
#include <span>
#include <string_view>

#include "core/metrics.h"
#include "core/trace.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

using core::StorageStatus;

// 1 if the last check of storage passed, 0 if it failed.
core::Gauge storageCheck{"storage_check"};

constexpr std::string_view kDirectory = "/biblionaut-check";
constexpr std::string_view kDataFile = "/biblionaut-check/data.bin";
constexpr std::string_view kReplacedFile = "/biblionaut-check/settings";

constexpr std::size_t kFileBytes = StorageCheck::kBlockBytes * StorageCheck::kBlocks;

constexpr std::uint8_t kFirstFill = 0xA1;
constexpr std::size_t kFirstLength = 100;
constexpr std::uint8_t kSecondFill = 0xB2;
constexpr std::size_t kSecondLength = 300;

// The byte the data file holds at `offset`: different from its neighbours
// and from the byte a block further on, so that data landing in the wrong
// place shows.
constexpr std::uint8_t ByteAt(std::size_t offset) noexcept {
  return static_cast<std::uint8_t>((static_cast<std::uint32_t>(offset) * 2654435761U) >> 24U);
}

bool HoldsFileFrom(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] != ByteAt(offset + i)) {
      return false;
    }
  }
  return true;
}

// Where the n-th scattered read begins: all over the file, off the block
// boundaries, in no particular order.
constexpr std::size_t ScatteredOffset(std::size_t n) noexcept {
  return ((n * 104729U) + 12345U) % (kFileBytes - StorageCheck::kScatteredBytes);
}

}  // namespace

StorageCheck::StorageCheck(core::Storage& storage) noexcept
    : storage_(storage), file_(fileBuffer_), replacement_(storage, replacementBuffer_) {}

void StorageCheck::Begin() noexcept {
  if (state_ == State::kRunning) {
    return;
  }
  state_ = State::kRunning;
  failedStep_ = Step::kNone;
  failedStatus_ = StorageStatus::kOk;
  beganTrace_ = BN_TRACE_NOW();
  MoveTo(Step::kMakeDirectory);
}

void StorageCheck::Tick() noexcept {
  if (state_ != State::kRunning) {
    return;
  }
  switch (step_) {
    case Step::kNone:
      break;
    case Step::kMakeDirectory: {
      // Left behind by a run that was cut short, perhaps: that is no
      // failure.
      const StorageStatus status = storage_.MakeDirectory(kDirectory);
      if (status != StorageStatus::kOk && status != StorageStatus::kExists) {
        Fail(status);
        return;
      }
      MoveTo(Step::kCreateFile);
      break;
    }
    case Step::kCreateFile: {
      const StorageStatus status = file_.Open(storage_, kDataFile, core::OpenMode::kCreate);
      if (status != StorageStatus::kOk) {
        Fail(status);
        return;
      }
      MoveTo(Step::kWrite);
      break;
    }
    case Step::kWrite:
      WriteBlock();
      break;
    case Step::kSync:
      if (!file_.Sync()) {
        Fail(file_.LastStatus());
        return;
      }
      file_.Close();
      MoveTo(Step::kReopen);
      break;
    case Step::kReopen: {
      const StorageStatus status = file_.Open(storage_, kDataFile, core::OpenMode::kRead);
      if (status != StorageStatus::kOk || file_.Size() != kFileBytes) {
        Fail(status);
        return;
      }
      MoveTo(Step::kRead);
      break;
    }
    case Step::kRead:
      ReadBlock();
      break;
    case Step::kReadScattered:
      ReadScattered();
      break;
    case Step::kReplace:
      Replace(kFirstFill, kFirstLength);
      break;
    case Step::kReplaceAgain:
      Replace(kSecondFill, kSecondLength);
      break;
    case Step::kReadReplaced:
      ReadReplaced();
      break;
    case Step::kCleanUp:
      CleanUp();
      break;
  }
}

void StorageCheck::WriteBlock() noexcept {
  const std::size_t offset = count_ * kBlockBytes;
  for (std::size_t i = 0; i < data_.size(); ++i) {
    data_[i] = ByteAt(offset + i);
  }
  if (!file_.Write(data_)) {
    Fail(file_.LastStatus());
    return;
  }
  ++count_;
  if (count_ == kBlocks) {
    MoveTo(Step::kSync);
  }
}

void StorageCheck::ReadBlock() noexcept {
  const std::size_t offset = count_ * kBlockBytes;
  data_.fill(0);
  if (!file_.ReadExactly(data_)) {
    Fail(file_.LastStatus());
    return;
  }
  if (!HoldsFileFrom(data_, offset)) {
    Fail(StorageStatus::kOk);
    return;
  }
  ++count_;
  if (count_ == kBlocks) {
    MoveTo(Step::kReadScattered);
  }
}

void StorageCheck::ReadScattered() noexcept {
  const std::size_t offset = ScatteredOffset(count_);
  const std::span<std::uint8_t> piece = std::span(data_).first(kScatteredBytes);
  std::fill(piece.begin(), piece.end(), std::uint8_t{0});
  if (!file_.ReadAt(static_cast<std::uint32_t>(offset), piece)) {
    Fail(file_.LastStatus());
    return;
  }
  if (!HoldsFileFrom(piece, offset)) {
    Fail(StorageStatus::kOk);
    return;
  }
  ++count_;
  if (count_ == kScatteredReads) {
    file_.Close();
    MoveTo(Step::kReplace);
  }
}

void StorageCheck::Replace(std::uint8_t fill, std::size_t length) noexcept {
  const std::span<std::uint8_t> contents = std::span(data_).first(length);
  std::fill(contents.begin(), contents.end(), fill);
  StorageStatus status = replacement_.Begin(kReplacedFile);
  if (status == StorageStatus::kOk && !replacement_.File().Write(contents)) {
    status = replacement_.File().LastStatus();
    replacement_.Abandon();
  }
  if (status == StorageStatus::kOk) {
    status = replacement_.Commit();
  }
  if (status != StorageStatus::kOk) {
    Fail(status);
    return;
  }
  MoveTo(step_ == Step::kReplace ? Step::kReplaceAgain : Step::kReadReplaced);
}

void StorageCheck::ReadReplaced() noexcept {
  // The way a file written like that is read: recovery first.
  StorageStatus status = core::RecoverReplacement(storage_, kReplacedFile);
  if (status == StorageStatus::kOk) {
    status = file_.Open(storage_, kReplacedFile, core::OpenMode::kRead);
  }
  if (status != StorageStatus::kOk) {
    Fail(status);
    return;
  }
  const std::span<std::uint8_t> contents = std::span(data_).first(kSecondLength);
  std::fill(contents.begin(), contents.end(), std::uint8_t{0});
  const bool read = file_.Size() == kSecondLength && file_.ReadExactly(contents);
  const StorageStatus readStatus = file_.LastStatus();
  file_.Close();
  if (!read) {
    Fail(readStatus);
    return;
  }
  if (!std::all_of(contents.begin(), contents.end(), [](std::uint8_t byte) { return byte == kSecondFill; })) {
    Fail(StorageStatus::kOk);
    return;
  }
  MoveTo(Step::kCleanUp);
}

void StorageCheck::CleanUp() noexcept {
  for (const std::string_view path : {kDataFile, kReplacedFile, kDirectory}) {
    const StorageStatus status = storage_.Remove(path);
    if (status != StorageStatus::kOk) {
      Fail(status);
      return;
    }
  }
  Finish(true);
}

void StorageCheck::MoveTo(Step step) noexcept {
  step_ = step;
  count_ = 0;
}

void StorageCheck::Finish(bool passed) noexcept {
  state_ = passed ? State::kPassed : State::kFailed;
  step_ = Step::kNone;
  storageCheck.Set(passed ? 1 : 0);
  BN_TRACE_TOP_SPAN_SINCE(kSpanStorageCheck, beganTrace_, passed ? 1 : 0);
}

void StorageCheck::Fail(StorageStatus status) noexcept {
  failedStep_ = step_;
  failedStatus_ = status;
  BN_TRACE_INSTANT(kInstantStorageCheckFailed,
                   (static_cast<std::uint32_t>(step_) * 100U) + static_cast<std::uint32_t>(status));
  // Leave as little behind as can be managed; none of this is checked.
  file_.Close();
  replacement_.Abandon();
  (void)core::RecoverReplacement(storage_, kReplacedFile);
  for (const std::string_view path : {kDataFile, kReplacedFile, kDirectory}) {
    (void)storage_.Remove(path);
  }
  Finish(false);
}

}  // namespace bn::reader
