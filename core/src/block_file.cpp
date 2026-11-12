#include "core/block_file.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "core/metrics.h"

namespace bn::core {

namespace {

// Calls made to storage through any BlockFile, and the bytes they carried.
Counter sdReads{"sd_reads"};
Counter sdReadBytes{"sd_read_bytes"};
Counter sdWrites{"sd_writes"};
Counter sdWriteBytes{"sd_write_bytes"};
Counter sdSyncs{"sd_syncs"};
// Reads and writes that did not start where the one before, on the same
// file, ended.
Counter sdSeeks{"sd_seeks"};

}  // namespace

StorageStatus BlockFile::Open(Storage& storage, std::string_view path, OpenMode mode) noexcept {
  Close();
  FileHandle file = kNoFile;
  StorageStatus status = storage.Open(path, mode, file);
  if (status != StorageStatus::kOk) {
    return status;
  }
  std::uint32_t size = 0;
  status = storage.Size(file, size);
  if (status != StorageStatus::kOk) {
    storage.Close(file);
    return status;
  }
  storage_ = &storage;
  file_ = file;
  mode_ = mode;
  size_ = size;
  position_ = 0;
  bufferStart_ = 0;
  bufferLength_ = 0;
  storageEnd_ = 0;
  lastStatus_ = StorageStatus::kOk;
  return StorageStatus::kOk;
}

void BlockFile::Close() noexcept {
  if (file_ != kNoFile) {
    storage_->Close(file_);
  }
  storage_ = nullptr;
  file_ = kNoFile;
  size_ = 0;
  position_ = 0;
  bufferLength_ = 0;
}

bool BlockFile::Seek(std::uint32_t position) noexcept {
  if (position > size_) {
    return false;
  }
  position_ = position;
  return true;
}

std::ptrdiff_t BlockFile::Read(std::span<std::uint8_t> dst) noexcept {
  if (!IsOpen()) {
    Fail(StorageStatus::kFailed);
    return kError;
  }
  std::size_t done = 0;
  while (done < dst.size() && position_ < size_) {
    // What the buffer already holds.
    if (position_ >= bufferStart_ && position_ - bufferStart_ < bufferLength_) {
      const std::size_t inBuffer = position_ - bufferStart_;
      const std::size_t count = std::min<std::size_t>(dst.size() - done, bufferLength_ - inBuffer);
      std::memcpy(dst.data() + done, buffer_.data() + inBuffer, count);
      position_ += static_cast<std::uint32_t>(count);
      done += count;
      continue;
    }

    const std::size_t block = buffer_.size();
    const std::span<std::uint8_t> rest = dst.subspan(done);
    if (block == 0 || (position_ % block == 0 && rest.size() >= block)) {
      // Whole blocks, starting on one: straight into the caller's memory.
      // (And with no buffer at all, everything goes this way.)
      std::size_t wanted = block == 0 ? rest.size() : rest.size() - (rest.size() % block);
      wanted = std::min<std::size_t>(wanted, size_ - position_);
      std::size_t read = 0;
      if (!Load(position_, rest.first(wanted), read)) {
        return kError;
      }
      position_ += static_cast<std::uint32_t>(read);
      done += read;
      if (read < wanted) {
        break;  // The file is shorter than it was when it was opened.
      }
      continue;
    }

    // Otherwise the block the position is in, into the buffer.
    const std::uint32_t start = position_ - static_cast<std::uint32_t>(position_ % block);
    const std::size_t wanted = std::min<std::size_t>(block, size_ - start);
    bufferLength_ = 0;
    std::size_t read = 0;
    if (!Load(start, buffer_.first(wanted), read)) {
      return kError;
    }
    bufferStart_ = start;
    bufferLength_ = static_cast<std::uint32_t>(read);
    if (position_ - bufferStart_ >= bufferLength_) {
      break;  // Shorter than it was, as above.
    }
  }
  return static_cast<std::ptrdiff_t>(done);
}

bool BlockFile::ReadExactly(std::span<std::uint8_t> dst) noexcept {
  const std::ptrdiff_t read = Read(dst);
  if (read < 0) {
    return false;
  }
  if (static_cast<std::size_t>(read) != dst.size()) {
    return Fail(StorageStatus::kFailed);
  }
  return true;
}

bool BlockFile::ReadAt(std::uint32_t offset, std::span<std::uint8_t> dst) noexcept {
  if (!IsOpen() || !Seek(offset)) {
    return Fail(StorageStatus::kFailed);
  }
  return ReadExactly(dst);
}

bool BlockFile::Write(std::span<const std::uint8_t> src) noexcept {
  if (!IsOpen() || mode_ != OpenMode::kCreate) {
    return Fail(StorageStatus::kFailed);
  }
  if (src.empty()) {
    return true;
  }
  if (src.size() > std::numeric_limits<std::uint32_t>::max() - position_) {
    return Fail(StorageStatus::kTooBig);
  }
  if (position_ != storageEnd_) {
    sdSeeks.Add();
  }
  const StorageStatus status = storage_->WriteAt(file_, position_, src);
  sdWrites.Add();
  if (status != StorageStatus::kOk) {
    // How much of it reached the card is not known.
    storageEnd_ = position_;
    bufferLength_ = 0;
    return Fail(status);
  }
  sdWriteBytes.Add(static_cast<std::uint32_t>(src.size()));
  position_ += static_cast<std::uint32_t>(src.size());
  storageEnd_ = position_;
  size_ = std::max(size_, position_);
  bufferLength_ = 0;  // What the buffer held may have been written over.
  return true;
}

bool BlockFile::Sync() noexcept {
  if (!IsOpen()) {
    return Fail(StorageStatus::kFailed);
  }
  sdSyncs.Add();
  const StorageStatus status = storage_->Sync(file_);
  return status == StorageStatus::kOk || Fail(status);
}

bool BlockFile::Load(std::uint32_t offset, std::span<std::uint8_t> dst, std::size_t& read) noexcept {
  if (offset != storageEnd_) {
    sdSeeks.Add();
  }
  read = 0;
  const StorageStatus status = storage_->ReadAt(file_, offset, dst, read);
  sdReads.Add();
  if (status != StorageStatus::kOk) {
    storageEnd_ = offset;
    return Fail(status);
  }
  sdReadBytes.Add(static_cast<std::uint32_t>(read));
  storageEnd_ = offset + static_cast<std::uint32_t>(read);
  return true;
}

bool BlockFile::Fail(StorageStatus status) noexcept {
  lastStatus_ = status;
  return false;
}

}  // namespace bn::core
