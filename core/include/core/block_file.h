#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/byte_source.h"
#include "core/storage.h"

namespace bn::core {

// One open file, with a position in it, a known size and a buffer of its
// own.
//
// An SD card is slow to start a read and quick to carry on with one, so it
// wants to be asked for whole blocks at offsets that are multiples of the
// block size, and as seldom as possible. This asks for the buffer's size
// at a time, at multiples of it, and serves small reads from what it holds;
// a read that is itself at least a block long, starting on a block, goes
// straight to the caller's memory. 4096 bytes suits a card. The buffer is
// the caller's, because it is too big for a stack: give each file its own,
// from an arena or static storage, and keep it alive as long as the file.
//
// Writes are not buffered: each goes to storage as it is made, so write in
// pieces of a sensible size. Nothing written is safe until Sync() says so.
//
// Every read and write of storage made through here is counted (sd_reads,
// sd_read_bytes, sd_writes, sd_write_bytes, sd_syncs), and so is every one
// that did not start where the last on this file ended (sd_seeks), which
// on a card costs more than the others.
//
// Used by one task at a time.
class BlockFile final : public ByteSource {
 public:
  explicit BlockFile(std::span<std::uint8_t> buffer) noexcept : buffer_(buffer) {}

  BlockFile(const BlockFile&) = delete;
  BlockFile& operator=(const BlockFile&) = delete;
  ~BlockFile() { Close(); }

  // Opens `path`, closing whatever was open before. The position is the
  // start of the file.
  [[nodiscard]] StorageStatus Open(Storage& storage, std::string_view path, OpenMode mode) noexcept;
  void Close() noexcept;
  bool IsOpen() const noexcept { return file_ != kNoFile; }

  std::uint32_t Size() const noexcept { return size_; }
  std::uint32_t Position() const noexcept { return position_; }
  std::uint32_t Remaining() const noexcept { return size_ - position_; }

  // Moves to `position`, which may be anywhere up to the end of the file.
  // Costs nothing by itself. Returns false, moving nowhere, if it is past
  // the end.
  [[nodiscard]] bool Seek(std::uint32_t position) noexcept;

  // Reads from the position, moving it on. Fills `dst` unless the file
  // ends first. Returns how many bytes were read, 0 at the end of the file,
  // and kError if storage failed (LastStatus() says how).
  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

  // Reads exactly dst.size() bytes from the position. Returns false if
  // storage failed or the file ended first; the position is then wherever
  // the reading stopped.
  [[nodiscard]] bool ReadExactly(std::span<std::uint8_t> dst) noexcept;

  // The same, from `offset`: Seek() and ReadExactly() in one.
  [[nodiscard]] bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> dst) noexcept;

  // Writes all of `src` at the position, moving it on, over what is there
  // and on past the end. Only for a file opened with kCreate.
  [[nodiscard]] bool Write(std::span<const std::uint8_t> src) noexcept;

  // Returns once everything written is safely on the card.
  [[nodiscard]] bool Sync() noexcept;

  // How the last call that reported failure failed; kOk if none has since
  // the file was opened.
  StorageStatus LastStatus() const noexcept { return lastStatus_; }

 private:
  // One read of storage, counted.
  bool Load(std::uint32_t offset, std::span<std::uint8_t> dst, std::size_t& read) noexcept;
  bool Fail(StorageStatus status) noexcept;

  std::span<std::uint8_t> buffer_;
  Storage* storage_ = nullptr;
  FileHandle file_ = kNoFile;
  OpenMode mode_ = OpenMode::kRead;
  std::uint32_t size_ = 0;
  std::uint32_t position_ = 0;
  // The buffer holds the file's bytes from bufferStart_, bufferLength_ of
  // them.
  std::uint32_t bufferStart_ = 0;
  std::uint32_t bufferLength_ = 0;
  // Where the last read or write of storage ended.
  std::uint32_t storageEnd_ = 0;
  StorageStatus lastStatus_ = StorageStatus::kOk;
};

}  // namespace bn::core
