// The fuzz target for the PNG reader (draw/png.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes the source gives at a call
//            bits 3 and 4: how much room there is for rows: what the
//              picture needs, a byte less, 64 bytes, or a lot
//            bit 5: the file is taken as it is, its checksums and all;
//              otherwise each whole chunk's checksum is put right first
//            bit 6: the data is plain: what the IDAT chunks hold is taken
//              as the rows, filter bytes and all, and compressed here
//              before the reader sees it (only when checksums are put
//              right)
//   byte 1   bits 0 to 2: when the data is compressed here, how long its
//              IDAT chunks are
//            bits 3 and 4: how many empty IDAT chunks come between two of
//              them (3 is one more than the reader passes over)
//            bits 5 and 6: zlib's level: 0, 1, 6 or 9
//   the rest a PNG file, or what passes for one
//
// Nearly any change to a real file would be stopped at a chunk's checksum,
// or at the first byte of its compressed data, and the reader behind them
// never reached: putting the checksums right and compressing the data here
// is how the engine's changes get to the header's fields, the palette,
// transparency and the filters.
//
// The file is read four ways: for its header alone; with the room and
// pieces byte 0 chooses; with all the room it needs, in pieces of 4 KB;
// and plainly, all at once, with zlib (tests/support/png_reference.h).
// What has to hold, whatever the bytes are:
//
//   - no step takes more from its source than MostInputPerStep(), and the
//     header alone takes the signature and the header chunk and no more;
//   - rows come in order, each as wide as the picture, no more of them than
//     it is high, and a picture is only whole with all of them;
//   - it says it has no room only when the room is less than RowBytesFor()
//     the header, and that a picture is interlaced only when it is;
//   - once it has stopped, it stays stopped;
//   - the header alone says what the whole reading says of the header;
//   - with the room it needs, it ends as it ends in pieces of 4 KB, with the
//     same rows;
//   - the rows are the plain reading's as far as both go, and where the
//     plain reading knows where it stops, the reader stops there too, the
//     same way: but for data that zlib takes and the inflater refuses as
//     bad, which is what it does with a long run of blocks that produce
//     nothing.

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "draw/png.h"
#include "fuzz_support.h"
#include "png_builder.h"
#include "png_reference.h"

namespace {

using bn::draw::PngHeader;
using bn::draw::PngReader;
using bn::draw::PngStatus;

constexpr std::array<std::size_t, 8> kSourcePieces = {1, 2, 3, 17, 100, 1024, 4096, 65536};
constexpr std::array<std::size_t, 8> kDataPieces = {1, 2, 7, 64, 300, 1000, 8192, 65536};
constexpr std::array<int, 4> kLevels = {0, 1, 6, 9};
// A reading leaves off once it has given this much gray: a few KB of data
// can stand for megabytes of rows, and what the engine is after is in the
// file, not in how long it can be made to go on.
constexpr std::size_t kMostGray = 1048576;
// The most room for rows that is given.
constexpr std::uint64_t kMostRoom = 4194304;

std::uint32_t Be32(std::span<const std::uint8_t> bytes, std::size_t at) {
  return (std::uint32_t{bytes[at]} << 24U) | (std::uint32_t{bytes[at + 1]} << 16U) |
         (std::uint32_t{bytes[at + 2]} << 8U) | bytes[at + 3];
}

void PutChunk(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> name, std::span<const std::uint8_t> body) {
  bn::test::PutBe32(out, static_cast<std::uint32_t>(body.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), body.begin(), body.end());
  bn::test::PutBe32(out,
                    static_cast<std::uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

// The file as the reader is to see it: with every whole chunk's checksum
// put right and, if `plainData`, the IDAT chunks' bytes compressed.
std::vector<std::uint8_t> Prepare(std::span<const std::uint8_t> file, bool plainData, std::size_t piece, unsigned empty,
                                  int level) {
  std::vector<std::uint8_t> out(file.begin(),
                                file.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(8, file.size())));
  std::size_t pos = out.size();
  static constexpr std::array<std::uint8_t, 4> kData = {'I', 'D', 'A', 'T'};
  while (pos + 12 <= file.size()) {
    const std::uint32_t length = Be32(file, pos);
    if (length > file.size() - pos - 12) {
      break;
    }
    const std::span<const std::uint8_t> name = file.subspan(pos + 4, 4);
    if (plainData && std::equal(name.begin(), name.end(), kData.begin())) {
      std::vector<std::uint8_t> rows;
      while (pos + 12 <= file.size() && Be32(file, pos) <= file.size() - pos - 12 &&
             std::equal(kData.begin(), kData.end(), file.begin() + static_cast<std::ptrdiff_t>(pos + 4))) {
        const std::uint32_t size = Be32(file, pos);
        rows.insert(rows.end(), file.begin() + static_cast<std::ptrdiff_t>(pos + 8),
                    file.begin() + static_cast<std::ptrdiff_t>(pos + 8 + size));
        pos += 12 + size;
      }
      const std::vector<std::uint8_t> stream = bn::test::ZlibStream(rows, level);
      for (std::size_t at = 0; at < stream.size(); at += piece) {
        if (at > 0) {
          for (unsigned i = 0; i < empty; ++i) {
            PutChunk(out, kData, {});
          }
        }
        PutChunk(out, kData, std::span(stream).subspan(at, std::min(piece, stream.size() - at)));
      }
      continue;
    }
    PutChunk(out, name, file.subspan(pos + 8, length));
    pos += 12 + length;
  }
  out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(pos), file.end());
  return out;
}

// Keeps the rows, and holds them to what the reader promises of them.
class Rows final : public bn::draw::RowSink {
 public:
  explicit Rows(const PngReader& reader) : reader_(reader) {}

  void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override {
    BN_FUZZ_CHECK(reader_.HeaderKnown(), "a row was given before the header was known");
    BN_FUZZ_CHECK(y == count, "a row was given out of its order");
    BN_FUZZ_CHECK(y < reader_.Header().height, "more rows were given than the picture has");
    BN_FUZZ_CHECK(gray.size() == reader_.Header().width, "a row was given that is not as wide as the picture");
    pixels.insert(pixels.end(), gray.begin(), gray.end());
    ++count;
  }

  std::uint32_t count = 0;
  std::vector<std::uint8_t> pixels;

 private:
  const PngReader& reader_;
};

struct Reading {
  PngStatus status = PngStatus::kOk;
  bool headerKnown = false;
  PngHeader header;
  std::uint32_t rows = 0;
  std::vector<std::uint8_t> gray;
  bool leftOff = false;
};

bool SameHeader(const PngHeader& a, const PngHeader& b) {
  return a.width == b.width && a.height == b.height && a.depth == b.depth && a.colour == b.colour &&
         a.interlaced == b.interlaced;
}

Reading ReadWith(std::span<const std::uint8_t> file, std::size_t piece, std::uint64_t room) {
  static std::vector<std::uint8_t> window(PngReader::kWindowBytes);
  std::vector<std::uint8_t> rows(static_cast<std::size_t>(room), 0xA5);
  PngReader reader(window, rows);
  bn::fuzz::PiecewiseSource source(file, piece);
  Rows sink(reader);
  reader.Begin(source, &sink);
  Reading reading;
  for (;;) {
    const std::size_t before = source.Taken();
    const bool more = reader.Step();
    BN_FUZZ_CHECK(source.Taken() - before <= PngReader::MostInputPerStep(),
                  "a step took more from its source than a step may");
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
  if (reading.headerKnown) {
    const std::uint64_t needed = PngReader::RowBytesFor(reading.header);
    BN_FUZZ_CHECK(needed > 0, "a header was taken that PNG does not allow");
    BN_FUZZ_CHECK(reading.status != PngStatus::kOk || reading.rows == reading.header.height,
                  "a picture was called whole without all its rows");
    BN_FUZZ_CHECK(reading.status != PngStatus::kNoRoom || needed > room, "no room was said where there was room");
    BN_FUZZ_CHECK(reading.status != PngStatus::kInterlaced || reading.header.interlaced,
                  "a picture was called interlaced that is not");
  } else {
    BN_FUZZ_CHECK(reading.rows == 0 && reading.status != PngStatus::kOk, "a picture without a header was read");
  }
  return reading;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned first = input.Byte();
  const unsigned second = input.Byte();
  const std::span<const std::uint8_t> given = input.Rest();
  const bool asIs = (first & 0x20U) != 0;
  const std::vector<std::uint8_t> file = asIs ? std::vector<std::uint8_t>(given.begin(), given.end())
                                              : Prepare(given, (first & 0x40U) != 0, kDataPieces[second & 7U],
                                                        (second >> 3U) & 3U, kLevels[(second >> 5U) & 3U]);

  // The header alone, with no memory at all.
  PngReader peek({}, {});
  bn::fuzz::PiecewiseSource peekSource(file, kSourcePieces[first & 7U]);
  const PngStatus headerStatus = peek.Read(peekSource, nullptr);
  BN_FUZZ_CHECK(headerStatus != PngStatus::kOk || (peek.HeaderKnown() && peekSource.Taken() == 8 + 12 + 13),
                "reading the header alone did not read the header, or read more");
  BN_FUZZ_CHECK(headerStatus != PngStatus::kNoRoom && headerStatus != PngStatus::kInterlaced,
                "reading the header alone wanted room, or minded interlacing");
  const std::uint64_t needed = peek.HeaderKnown() ? PngReader::RowBytesFor(peek.Header()) : 0;
  const std::uint64_t plenty = std::min(needed, kMostRoom);

  // With the room and the pieces chosen.
  std::uint64_t room = 0;
  switch ((first >> 3U) & 3U) {
    case 0:
      room = plenty;
      break;
    case 1:
      room = plenty > 0 ? plenty - 1 : 0;
      break;
    case 2:
      room = 64;
      break;
    default:
      room = kMostRoom;
      break;
  }
  const Reading chosen = ReadWith(file, kSourcePieces[first & 7U], room);
  BN_FUZZ_CHECK(
      chosen.headerKnown == peek.HeaderKnown() && (!chosen.headerKnown || SameHeader(chosen.header, peek.Header())),
      "the header alone said another thing than the whole reading");
  if (headerStatus != PngStatus::kOk) {
    BN_FUZZ_CHECK(chosen.status == headerStatus, "a header the header alone refused was refused another way");
  }

  // With the room it needs, in pieces of 4 KB.
  const Reading whole = ReadWith(file, 4096, plenty);
  if (!chosen.leftOff && !whole.leftOff && chosen.status != PngStatus::kNoRoom) {
    BN_FUZZ_CHECK(chosen.status == whole.status && chosen.gray == whole.gray,
                  "the reading went otherwise in other pieces, or with more room");
  }

  // And plainly.
  const bn::test::PngReference plain = bn::test::ReadPngPlainly(file, plenty, kMostGray * 16);
  const std::size_t common = std::min(whole.gray.size(), plain.gray.size());
  BN_FUZZ_CHECK(
      std::equal(whole.gray.begin(), whole.gray.begin() + static_cast<std::ptrdiff_t>(common), plain.gray.begin()),
      "the rows are not the plain reading's");
  if (whole.leftOff || plain.leftOff) {
    return 0;
  }
  BN_FUZZ_CHECK(whole.headerKnown == plain.headerKnown, "the plain reading read the header otherwise");
  if (plain.exact) {
    const bool same = whole.status == plain.status && whole.rows == plain.rows;
    const bool refusedAsBad = whole.status == PngStatus::kBadData &&
                              (plain.status == PngStatus::kOk || plain.status == PngStatus::kTruncated) &&
                              whole.rows <= plain.rows;
    BN_FUZZ_CHECK(same || refusedAsBad, "the reading ended otherwise than the plain reading");
  }
  return 0;
}
