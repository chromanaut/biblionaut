// The fuzz target for the JPEG reader (draw/jpeg.h).
//
// What it is given:
//
//   byte 0   how many bytes the source gives at a call: 0 for 4096,
//            otherwise that many
//   byte 1   bits 0 and 1: the reduction asked, 1, 2, 4 or 8
//            bit 2: only the header is read
//            bit 3: the room for rows is a byte less than RowBytesFor()
//              says
//   the rest a JPEG file, or what passes for one
//
// The file is read for its header alone; with the pieces and room the
// first two bytes choose; with all the room it needs, in pieces of 4 KB;
// and at an eighth, in pieces of 4 KB. What has to hold, whatever the
// bytes are:
//
//   - no step takes more from its source than MostInputPerStep(), nor gives
//     more than one row;
//   - rows come in order, each as wide as the picture is read, no more of
//     them than it is high, and a picture is only whole with all of them;
//   - it says it has no room only when the room is less than RowBytesFor()
//     the header at the reduction taken, which is an eighth for a
//     progressive picture, and the picture's sides at it are its own
//     divided by the reduction, rounded up;
//   - once it has stopped, it stays stopped;
//   - the header alone says what the whole reading says of the header, and
//     a header the reader takes is one it can say how much room it needs
//     for;
//   - however the file is given, it ends the same way with the same rows;
//   - read at any reduction it ends as it does at an eighth: how far the
//     coded data can be read does not depend on the size it is made.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "draw/jpeg.h"
#include "fuzz_support.h"

namespace {

using bn::draw::JpegHeader;
using bn::draw::JpegReader;
using bn::draw::JpegReduction;
using bn::draw::JpegStatus;

// A reading leaves off once it has given this much gray: a few KB of coded
// data can stand for megabytes of rows, and what the engine is after is in
// the file, not in how long it can be made to go on.
constexpr std::size_t kMostGray = 1048576;
// The most room for rows that is given.
constexpr std::uint64_t kMostRoom = 4194304;
constexpr std::array<JpegReduction, 4> kReductions = {JpegReduction::kNone, JpegReduction::kHalf,
                                                      JpegReduction::kQuarter, JpegReduction::kEighth};

class Rows final : public bn::draw::RowSink {
 public:
  explicit Rows(const JpegReader& reader) : reader_(reader) {}

  void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override {
    BN_FUZZ_CHECK(reader_.HeaderKnown(), "a row was given before the header was known");
    BN_FUZZ_CHECK(y == count, "a row was given out of its order");
    BN_FUZZ_CHECK(y < reader_.Height(), "more rows were given than the picture has");
    BN_FUZZ_CHECK(gray.size() == reader_.Width(), "a row was given that is not as wide as the picture is read");
    pixels.insert(pixels.end(), gray.begin(), gray.end());
    ++count;
  }

  std::uint32_t count = 0;
  std::vector<std::uint8_t> pixels;

 private:
  const JpegReader& reader_;
};

struct Reading {
  JpegStatus status = JpegStatus::kOk;
  bool headerKnown = false;
  JpegHeader header;
  std::uint32_t rows = 0;
  std::vector<std::uint8_t> gray;
  bool leftOff = false;
};

bool SameHeader(const JpegHeader& a, const JpegHeader& b) {
  return a.width == b.width && a.height == b.height && a.components == b.components && a.progressive == b.progressive &&
         a.grayAcross == b.grayAcross && a.grayDown == b.grayDown && a.colourAcross == b.colourAcross &&
         a.colourDown == b.colourDown;
}

Reading ReadWith(std::span<const std::uint8_t> file, std::size_t piece, JpegReduction reduction, std::uint64_t room) {
  std::vector<std::uint8_t> rows(static_cast<std::size_t>(room), 0xA5);
  JpegReader reader(rows);
  bn::fuzz::PiecewiseSource source(file, piece);
  Rows sink(reader);
  reader.Begin(source, &sink, reduction);
  Reading reading;
  for (;;) {
    const std::size_t before = source.Taken();
    const std::uint32_t given = sink.count;
    const bool more = reader.Step();
    BN_FUZZ_CHECK(source.Taken() - before <= JpegReader::MostInputPerStep(),
                  "a step took more from its source than a step may");
    BN_FUZZ_CHECK(sink.count - given <= 1, "a step gave more than one row");
    if (!more) {
      break;
    }
    if (sink.pixels.size() >= kMostGray) {
      reading.leftOff = true;
      break;
    }
  }
  reading.status = reader.Status();
  reading.headerKnown = reader.HeaderKnown();
  reading.header = reader.Header();
  reading.rows = reader.RowsGiven();
  reading.gray = std::move(sink.pixels);
  BN_FUZZ_CHECK(reading.rows == sink.count, "the count of rows given is not the rows given");
  if (reading.leftOff) {
    return reading;
  }
  BN_FUZZ_CHECK(!reader.Step() && reader.Status() == reading.status, "a reading that had stopped went on");
  if (reading.headerKnown && reading.status != JpegStatus::kUnsupported) {
    const JpegHeader& header = reading.header;
    const JpegReduction taken = JpegReader::ReductionTaken(header, reduction);
    const auto by = static_cast<std::uint32_t>(taken);
    BN_FUZZ_CHECK(reader.Reduction() == taken && (taken == reduction || header.progressive),
                  "a picture was read at a reduction other than the one asked, and not for being progressive");
    BN_FUZZ_CHECK(reader.Width() == (header.width + by - 1) / by && reader.Height() == (header.height + by - 1) / by,
                  "a picture's sides as read are not its own divided by the reduction");
    const std::uint64_t needed = JpegReader::RowBytesFor(header, reduction);
    BN_FUZZ_CHECK(needed > 0, "a header was taken that the reader cannot say the room for");
    BN_FUZZ_CHECK(reading.status != JpegStatus::kOk || reading.rows == reader.Height(),
                  "a picture was called whole without all its rows");
    BN_FUZZ_CHECK(reading.status != JpegStatus::kNoRoom || needed > room, "no room was said where there was room");
  } else {
    BN_FUZZ_CHECK(reading.rows == 0 && reading.status != JpegStatus::kOk && reading.status != JpegStatus::kNoRoom,
                  "a picture without a header it reads was read");
  }
  return reading;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned first = input.Byte();
  const unsigned second = input.Byte();
  const std::span<const std::uint8_t> file = input.Rest();
  const std::size_t piece = first == 0 ? 4096 : first;
  const JpegReduction reduction = kReductions[second & 3U];
  const bool headerOnly = (second & 4U) != 0;
  const bool short1 = (second & 8U) != 0;

  JpegReader peek({});
  bn::fuzz::PiecewiseSource peekSource(file, piece);
  const JpegStatus headerStatus = peek.Read(peekSource, nullptr);
  BN_FUZZ_CHECK(headerStatus != JpegStatus::kNoRoom, "reading the header alone wanted room");
  BN_FUZZ_CHECK(headerStatus != JpegStatus::kOk || peek.HeaderKnown(), "reading the header alone ended without it");
  BN_FUZZ_CHECK(peek.RowsGiven() == 0, "reading the header alone gave rows");
  if (headerOnly) {
    return 0;
  }
  const std::uint64_t needed = peek.HeaderKnown() ? JpegReader::RowBytesFor(peek.Header(), reduction) : 0;
  if (needed > kMostRoom) {
    return 0;
  }
  const std::uint64_t room = short1 && needed > 0 ? needed - 1 : needed;

  const Reading chosen = ReadWith(file, piece, reduction, room);
  BN_FUZZ_CHECK(chosen.headerKnown == peek.HeaderKnown() || headerStatus != JpegStatus::kOk,
                "the header alone and the whole reading disagree on whether there is a header");
  if (chosen.headerKnown && peek.HeaderKnown()) {
    BN_FUZZ_CHECK(SameHeader(chosen.header, peek.Header()), "the header alone is not the whole reading's header");
  }
  if (headerStatus != JpegStatus::kOk) {
    BN_FUZZ_CHECK(chosen.status == headerStatus, "the header alone failed one way and the whole reading another");
  }
  const Reading whole = ReadWith(file, 4096, reduction, needed);
  if (!short1 && !chosen.leftOff && !whole.leftOff) {
    BN_FUZZ_CHECK(chosen.status == whole.status && chosen.gray == whole.gray,
                  "the file given in other pieces was read differently");
  }
  if (!whole.leftOff && whole.status != JpegStatus::kNoRoom) {
    const std::uint64_t eighthRoom =
        peek.HeaderKnown() ? JpegReader::RowBytesFor(peek.Header(), JpegReduction::kEighth) : 0;
    const Reading eighth = ReadWith(file, 4096, JpegReduction::kEighth, eighthRoom);
    BN_FUZZ_CHECK(eighth.leftOff || eighth.status == whole.status,
                  "read at an eighth it ended otherwise than at the reduction asked");
  }
  return 0;
}
