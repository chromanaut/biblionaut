// The fuzz target for the inflater (core/inflate.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes are asked for at a call
//            bits 3 to 5: how many bytes the source gives at a call
//   the rest a DEFLATE stream, or what passes for one
//
// The inflater reads the stream twice, once in pieces of 1 KB from a
// source that gives all it is asked for, and once in the pieces byte 0
// chooses; and zlib reads it. What has to hold, whatever the bytes are:
//
//   - no call takes more from the source than MostInputFor() says a call
//     of its size may;
//   - a call gives no more than it was asked for, and gives nothing only
//     when the stream has ended as it should;
//   - once the stream has ended or failed, it stays so;
//   - the two readings give the same bytes and end the same way, however
//     the stream and the output were cut up (but for how a reading ended
//     that was left off for having gone on long enough: see kMostOutput);
//   - what comes out is what zlib makes of the same stream, as far as
//     both go; a stream is only called whole if zlib calls it whole; and
//     one that zlib calls whole is either called whole or refused as bad
//     data, which is what the inflater does with a long run of blocks
//     that produce nothing, and zlib does not.

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/inflate.h"
#include "fuzz_support.h"

namespace {

using bn::core::Inflater;
using Status = bn::core::Inflater::Status;

// A stream of a few KB can stand for hundreds of times as much. Past this
// much output a reading is left off: what the engine is after is in the
// stream, not in how long it can be made to go on. Two readings of one
// stream are left off a little apart, each at the end of a call, so one
// may have come to a fault that lies just past this and the other not:
// how a reading ended is only looked at if it was not left off.
constexpr std::size_t kMostOutput = 1048576;
// In the reading by pieces, how many calls are made with the chosen size
// before the rest are made with a large one. A megabyte a byte at a time
// would be most of the engine's time.
constexpr std::size_t kCallsInSmallPieces = 4096;
constexpr std::size_t kLargePiece = 4096;

constexpr std::array<std::size_t, 8> kOutputPieces = {1, 2, 3, 7, 64, 257, 1024, 4096};
constexpr std::array<std::size_t, 8> kSourcePieces = {1, 2, 3, 17, 100, 255, 256, 256};

struct Reading {
  std::vector<std::uint8_t> out;
  Status status = Status::kRunning;
  bool leftOff = false;  // It reached kMostOutput and was not read to its end.
};

Reading Inflate(std::span<const std::uint8_t> stream, std::size_t outputPiece, std::size_t sourcePiece) {
  bn::fuzz::PiecewiseSource source(stream, sourcePiece);
  // The window starts each reading full of something that is not output:
  // nothing already in it may ever come out.
  static std::vector<std::uint8_t> window(Inflater::kWindowBytes);
  std::fill(window.begin(), window.end(), std::uint8_t{0xA5});
  Inflater inflater(source, window);

  Reading reading;
  std::vector<std::uint8_t> piece(std::max(outputPiece, kLargePiece));
  std::size_t calls = 0;
  for (;;) {
    const std::size_t wanted = calls < kCallsInSmallPieces ? outputPiece : kLargePiece;
    ++calls;
    const std::size_t takenBefore = source.Taken();
    const std::ptrdiff_t read = inflater.Read(std::span(piece).first(wanted));
    BN_FUZZ_CHECK(source.Taken() - takenBefore <= Inflater::MostInputFor(wanted),
                  "a call took more from its source than a call of its size may");
    if (read <= 0) {
      BN_FUZZ_CHECK(read == 0 || read == bn::core::ByteSource::kError, "a call gave back a count below zero");
      if (read == 0) {
        BN_FUZZ_CHECK(inflater.CurrentStatus() == Status::kDone, "a call gave nothing though the stream had not ended");
      } else {
        BN_FUZZ_CHECK(inflater.CurrentStatus() == Status::kBadData || inflater.CurrentStatus() == Status::kTruncated,
                      "a stream from a source that works failed with something other than bad data or truncated");
      }
      // And it stays ended.
      const Status ended = inflater.CurrentStatus();
      BN_FUZZ_CHECK(inflater.Read(std::span(piece).first(wanted)) == read && inflater.CurrentStatus() == ended,
                    "a stream that had ended went on");
      break;
    }
    BN_FUZZ_CHECK(static_cast<std::size_t>(read) <= wanted, "a call gave more than it was asked for");
    reading.out.insert(reading.out.end(), piece.begin(), piece.begin() + read);
    if (reading.out.size() >= kMostOutput) {
      reading.leftOff = true;
      break;
    }
  }
  reading.status = inflater.CurrentStatus();
  BN_FUZZ_CHECK(inflater.BytesOut() == reading.out.size(), "the count of bytes produced is not what was produced");
  return reading;
}

// What zlib makes of the same stream.
struct Reference {
  enum class End : std::uint8_t { kWhole, kBad, kCutShort, kLeftOff, kUnknown };
  std::vector<std::uint8_t> out;
  End end = End::kUnknown;
};

Reference InflateWithZlib(std::span<const std::uint8_t> stream) {
  Reference reference;
  z_stream z = {};
  // Fifteen bits of window, and the minus sign for a stream with nothing
  // around it.
  if (inflateInit2(&z, -15) != Z_OK) {
    return reference;
  }
  std::vector<std::uint8_t> input(stream.begin(), stream.end());  // zlib's interface is not const about its input.
  z.next_in = input.data();
  z.avail_in = static_cast<uInt>(input.size());
  static std::vector<std::uint8_t> chunk(65536);
  for (;;) {
    z.next_out = chunk.data();
    z.avail_out = static_cast<uInt>(chunk.size());
    const int result = inflate(&z, Z_NO_FLUSH);
    reference.out.insert(reference.out.end(), chunk.begin(),
                         chunk.begin() + static_cast<std::ptrdiff_t>(chunk.size() - z.avail_out));
    if (result == Z_STREAM_END) {
      reference.end = Reference::End::kWhole;
    } else if (result == Z_DATA_ERROR) {
      reference.end = Reference::End::kBad;
    } else if (result == Z_BUF_ERROR) {
      reference.end = Reference::End::kCutShort;  // It can do no more without more input, and there is none.
    } else if (result != Z_OK) {
      reference.end = Reference::End::kUnknown;
    } else if (reference.out.size() > kMostOutput + kLargePiece) {
      reference.end = Reference::End::kLeftOff;
    } else {
      continue;
    }
    break;
  }
  inflateEnd(&z);
  return reference;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> stream = input.Rest();

  const Reading plain = Inflate(stream, 1024, 256);
  const Reading pieces = Inflate(stream, kOutputPieces[choices & 7U], kSourcePieces[(choices >> 3U) & 7U]);
  BN_FUZZ_CHECK(pieces.leftOff == plain.leftOff, "a stream was left off read in large pieces and not in small ones");
  BN_FUZZ_CHECK(plain.leftOff || pieces.status == plain.status,
                "a stream ended one way read in large pieces and another way read in small ones");
  // Left off, the two have gone past the limit by different amounts.
  const std::size_t both = std::min(plain.out.size(), pieces.out.size());
  BN_FUZZ_CHECK(
      (plain.leftOff || plain.out.size() == pieces.out.size()) &&
          std::equal(plain.out.begin(), plain.out.begin() + static_cast<std::ptrdiff_t>(both), pieces.out.begin()),
      "a stream gave different bytes read in large pieces and in small ones");

  const Reference reference = InflateWithZlib(stream);
  const std::size_t common = std::min(plain.out.size(), reference.out.size());
  BN_FUZZ_CHECK(
      std::equal(plain.out.begin(), plain.out.begin() + static_cast<std::ptrdiff_t>(common), reference.out.begin()),
      "the bytes that came out are not the bytes zlib gets from the same stream");
  if (plain.leftOff || reference.end == Reference::End::kLeftOff || reference.end == Reference::End::kUnknown) {
    return 0;
  }
  if (plain.status == Status::kDone) {
    BN_FUZZ_CHECK(reference.end == Reference::End::kWhole && reference.out.size() == plain.out.size(),
                  "a stream was called whole that zlib does not call whole, or not at that length");
  }
  if (reference.end == Reference::End::kWhole) {
    BN_FUZZ_CHECK(plain.status == Status::kDone || plain.status == Status::kBadData,
                  "a stream that zlib calls whole was called cut short");
  }
  return 0;
}
