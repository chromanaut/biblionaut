#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/byte_source.h"

namespace bn::core {

// Undoes DEFLATE compression (RFC 1951), which is what an entry in a ZIP
// file holds, and so what an EPUB is made of.
//
// It is a ByteSource that reads another. Compressed bytes are drawn from
// `source` as they are needed, and the plain bytes come out of Read() in
// pieces of whatever size the caller asks for, so a whole file never has to
// be in memory at once in either form. The only memory that grows with the
// data is the window: the last 32 KB of output, which the format lets later
// data refer back to. That is the caller's to provide, since it is far too
// big for a stack; the object itself is under 3 KB and is not for a stack
// either.
//
// It reads ahead: by the time the stream ends it may have taken up to 260
// bytes from `source` that come after it. Where what follows matters, give
// it a source that ends where the stream does.
//
// It trusts nothing about its input. Whatever bytes it is given, it either
// produces the data they stand for or stops with an error; it never reads
// or writes outside its own memory, and never loops without making
// progress.
//
// Used by one task at a time.
class Inflater final : public ByteSource {
 public:
  // The window it needs: exactly this much, or more.
  static constexpr std::size_t kWindowBytes = 32768;

  enum class Status : std::uint8_t {
    kRunning,       // There is, or may be, more to come.
    kDone,          // The stream ended as it should; everything has been read.
    kBadData,       // The input is not a DEFLATE stream.
    kTruncated,     // The input ended before the stream did.
    kSourceFailed,  // The source reported an error.
    kNoWindow,      // The window it was given is too small.
  };

  Inflater(ByteSource& source, std::span<std::uint8_t> window) noexcept;

  Inflater(const Inflater&) = delete;
  Inflater& operator=(const Inflater&) = delete;
  ~Inflater() = default;

  // Starts again on another stream, read from `source`.
  void Reset(ByteSource& source) noexcept;

  // The next plain bytes. Fills `dst` unless the stream ends first. Returns
  // how many bytes were read, 0 at the end of the stream, and kError if the
  // stream cannot be followed any further (CurrentStatus() says why). Bytes
  // that came before the fault are returned first: the error comes from
  // the call after.
  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

  Status CurrentStatus() const noexcept { return status_; }

  // Plain bytes produced so far.
  std::uint32_t BytesOut() const noexcept { return bytesOut_; }

 private:
  // One prefix code: which bit patterns stand for which symbols.
  struct Code {
    // How many symbols have a code of each length, 1 to 15 bits.
    std::array<std::uint16_t, 16> counts{};
    // The symbols, in the order of their codes, and how many have one.
    std::span<std::uint16_t> symbols;
    std::uint16_t symbolCount = 0;
    // Looked up by the next `fastBits` bits of input: the symbol those bits
    // begin with and how long its code is, or 0 if the code is longer than
    // that. Empty for a code that has no such table.
    std::span<std::uint16_t> fast;
    std::uint8_t fastBits = 0;
  };

  enum class Stage : std::uint8_t { kBlockStart, kStored, kCompressed };
  enum class Built : std::uint8_t { kComplete, kIncomplete, kOverSubscribed };

  // The input, a bit at a time.
  void Fill() noexcept;
  bool Refill() noexcept;
  void Drop(unsigned count) noexcept;
  bool Bits(unsigned count, std::uint32_t& value) noexcept;

  static Built Build(Code& code, std::span<const std::uint8_t> lengths) noexcept;
  // The next symbol of `code`, or -1 if the input does not hold one.
  int Decode(const Code& code) noexcept;

  void StartBlock() noexcept;
  bool StartStored() noexcept;
  void UseFixedCodes() noexcept;
  bool ReadCodes() noexcept;
  std::size_t CopyStored(std::span<std::uint8_t> dst) noexcept;
  std::size_t Decompress(std::span<std::uint8_t> dst) noexcept;
  void Fail(Status status) noexcept;

  ByteSource* source_;
  std::span<std::uint8_t> window_;
  Status status_ = Status::kRunning;
  Stage stage_ = Stage::kBlockStart;
  bool lastBlock_ = false;    // The block being read is the stream's last.
  bool fixedBuilt_ = false;   // The codes now built are the fixed ones.
  bool sourceEnded_ = false;  // The source has no more to give.

  std::uint32_t bitBuffer_ = 0;  // Input not yet used, the next bit lowest.
  unsigned bitCount_ = 0;        // How many bits of it are real.
  std::size_t inputPosition_ = 0;
  std::size_t inputEnd_ = 0;

  std::size_t windowPosition_ = 0;  // Where the next byte of output goes.
  std::uint32_t history_ = 0;       // Bytes of output the window holds, up to its size.
  std::uint32_t bytesOut_ = 0;
  std::uint32_t storedLeft_ = 0;     // Bytes of a stored block still to copy.
  std::uint32_t matchLeft_ = 0;      // Bytes of a repeat still to copy,
  std::uint32_t matchDistance_ = 0;  // and how far back they are.

  Code literal_;
  Code distance_;
  Code codeLength_;
  std::array<std::uint16_t, 288> literalSymbols_{};
  std::array<std::uint16_t, 512> literalFast_{};
  std::array<std::uint16_t, 32> distanceSymbols_{};
  std::array<std::uint16_t, 64> distanceFast_{};
  std::array<std::uint16_t, 19> codeLengthSymbols_{};
  std::array<std::uint8_t, 320> lengths_{};  // Code lengths, while a block's codes are being read.
  std::array<std::uint8_t, 256> input_{};
};

}  // namespace bn::core
