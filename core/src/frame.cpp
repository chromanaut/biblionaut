#include "core/frame.h"

#include <array>

#include "core/checksum.h"
#include "core/endian.h"

namespace bn::core {

namespace {

constexpr std::uint8_t kDelimiter = 0x00;
constexpr std::uint8_t kFullBlock = 0xFF;  // A COBS block of 254 non-zero bytes.
constexpr std::size_t kChecksumSize = 4;

}  // namespace

FrameWriter::FrameWriter(FrameType type, std::span<std::uint8_t> out) noexcept : out_(out) {
  Emit(kDelimiter);
  Emit(0);  // Placeholder for the first block's length byte, at codeIndex_.
  Put(static_cast<std::uint8_t>(type));
}

void FrameWriter::Emit(std::uint8_t byte) noexcept {
  if (size_ < out_.size()) {
    out_[size_] = byte;
  } else {
    overflowed_ = true;
  }
  ++size_;
}

void FrameWriter::EncodeByte(std::uint8_t byte) noexcept {
  if (byte != 0) {
    Emit(byte);
    ++code_;
  }
  if (byte == 0 || code_ == kFullBlock) {
    // Close the block: its length byte says how far the next zero is.
    if (codeIndex_ < out_.size()) {
      out_[codeIndex_] = code_;
    }
    codeIndex_ = size_;
    Emit(0);
    code_ = 1;
  }
}

void FrameWriter::Put(std::uint8_t byte) noexcept {
  crc_ = Crc32(std::span<const std::uint8_t>(&byte, 1), crc_);
  EncodeByte(byte);
}

void FrameWriter::Put(std::span<const std::uint8_t> bytes) noexcept {
  crc_ = Crc32(bytes, crc_);
  for (const std::uint8_t byte : bytes) {
    EncodeByte(byte);
  }
}

void FrameWriter::PutU16(std::uint16_t value) noexcept {
  std::array<std::uint8_t, 2> bytes{};
  StoreLe16(bytes, value);
  Put(bytes);
}

void FrameWriter::PutU32(std::uint32_t value) noexcept {
  std::array<std::uint8_t, 4> bytes{};
  StoreLe32(bytes, value);
  Put(bytes);
}

std::span<const std::uint8_t> FrameWriter::Finish() noexcept {
  std::array<std::uint8_t, kChecksumSize> checksum{};
  StoreLe32(checksum, crc_);
  for (const std::uint8_t byte : checksum) {
    EncodeByte(byte);
  }
  if (codeIndex_ < out_.size()) {
    out_[codeIndex_] = code_;
  }
  Emit(kDelimiter);
  if (overflowed_) {
    return {};
  }
  return out_.first(size_);
}

std::optional<Frame> DecodeFrame(std::span<const std::uint8_t> encoded, std::span<std::uint8_t> scratch) noexcept {
  if (scratch.size() < encoded.size()) {
    return std::nullopt;
  }

  // Undo COBS: each block starts with a length byte; a zero follows the block
  // unless the block was full or it is the last one.
  std::size_t decoded = 0;
  std::size_t position = 0;
  while (position < encoded.size()) {
    const std::uint8_t code = encoded[position++];
    if (code == 0) {
      return std::nullopt;
    }
    const std::size_t count = static_cast<std::size_t>(code) - 1;
    if (count > encoded.size() - position) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t byte = encoded[position++];
      if (byte == 0) {
        return std::nullopt;
      }
      scratch[decoded++] = byte;
    }
    if (code != kFullBlock && position < encoded.size()) {
      scratch[decoded++] = 0;
    }
  }

  if (decoded < 1 + kChecksumSize) {
    return std::nullopt;
  }
  const std::size_t bodySize = decoded - kChecksumSize;
  const std::uint32_t expected = LoadLe32(scratch.subspan(bodySize).first<kChecksumSize>());
  if (Crc32(scratch.first(bodySize)) != expected) {
    return std::nullopt;
  }
  return Frame{.type = static_cast<FrameType>(scratch[0]), .payload = scratch.subspan(1, bodySize - 1)};
}

}  // namespace bn::core
