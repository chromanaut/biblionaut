#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Frames: how structured data shares a serial line with plain text.
//
// The line also carries text this code does not control (boot messages, a
// crash dump), so a reader has to be able to pick frames out of arbitrary
// bytes and to recover after losing some. A frame on the wire is:
//
//   0x00  COBS( type, payload..., CRC-32 of type and payload )  0x00
//
// COBS encoding removes every zero byte from what is between the delimiters,
// so a zero only ever means "a frame starts or ends here". Text never
// contains a zero, so anything between zeros that does not decode with a good
// checksum is text.

namespace bn::core {

enum class FrameType : std::uint8_t {
  kHello = 1,    // Protocol version byte, then the firmware version as text.
  kTrace = 2,    // Dropped-record count (u32), then trace records.
  kText = 3,     // One line of log text, without a line ending.
  kMetrics = 4,  // Counters and gauges as one line of text.
};

// Size of the largest encoded frame for a payload of `payloadSize` bytes.
constexpr std::size_t MaxEncodedFrameSize(std::size_t payloadSize) noexcept {
  const std::size_t raw = 1 + payloadSize + 4;  // Type, payload, checksum.
  return 1 + raw + (raw / 254) + 1 + 1;         // Delimiter, COBS data and overhead, delimiter.
}

// Builds one frame directly into a caller's buffer, a piece at a time.
class FrameWriter {
 public:
  FrameWriter(FrameType type, std::span<std::uint8_t> out) noexcept;

  void Put(std::uint8_t byte) noexcept;
  void Put(std::span<const std::uint8_t> bytes) noexcept;
  void PutU16(std::uint16_t value) noexcept;  // Little-endian.
  void PutU32(std::uint32_t value) noexcept;  // Little-endian.

  // Completes the frame and returns it, ready to send. Returns an empty span
  // if the buffer was too small. Call once.
  [[nodiscard]] std::span<const std::uint8_t> Finish() noexcept;

 private:
  void Emit(std::uint8_t byte) noexcept;
  void EncodeByte(std::uint8_t byte) noexcept;

  std::span<std::uint8_t> out_;
  std::size_t size_ = 0;       // Bytes written to out_.
  std::size_t codeIndex_ = 1;  // Where the current COBS block's length byte goes.
  std::uint8_t code_ = 1;      // Length of the current COBS block, plus one.
  std::uint32_t crc_ = 0;
  bool overflowed_ = false;
};

struct Frame {
  FrameType type;
  std::span<const std::uint8_t> payload;  // Points into the buffer given to DecodeFrame().
};

// Decodes the bytes found between two zero delimiters. `scratch` receives the
// decoded bytes and must be at least as large as `encoded`. Returns nothing
// if the bytes are not a frame: not valid COBS, too short, or a checksum
// mismatch.
[[nodiscard]] std::optional<Frame> DecodeFrame(std::span<const std::uint8_t> encoded,
                                               std::span<std::uint8_t> scratch) noexcept;

}  // namespace bn::core
