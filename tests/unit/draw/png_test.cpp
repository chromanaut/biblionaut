#include "draw/png.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "draw/gray.h"
#include "png_builder.h"

// The reader is given PNG files made here with zlib, of every kind of
// picture, and what it gives out is set beside the gray each pixel should
// come to, worked out from the picture's samples a pixel at a time
// (png_builder.h). Then files spoiled in each way the reader looks for.

namespace {

using bn::draw::PngColour;
using bn::draw::PngHeader;
using bn::draw::PngReader;
using bn::draw::PngRowSink;
using bn::draw::PngStatus;
using bn::test::PngExtras;
using bn::test::PngFile;
using bn::test::PngPicture;

using Bytes = std::vector<std::uint8_t>;

std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// A source that gives at most so many bytes at a call and counts what it
// has given.
class PieceSource final : public bn::core::ByteSource {
 public:
  PieceSource(std::span<const std::uint8_t> bytes, std::size_t piece) : bytes_(bytes), piece_(piece) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    if (failAt_ <= taken_) {
      return kError;
    }
    const std::size_t count = std::min({dst.size(), piece_, bytes_.size() - taken_, failAt_ - taken_});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(taken_), count, dst.begin());
    taken_ += count;
    return static_cast<std::ptrdiff_t>(count);
  }

  std::size_t Taken() const { return taken_; }
  // The source fails once this many bytes have been given.
  void FailAt(std::size_t at) { failAt_ = at; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t piece_;
  std::size_t taken_ = 0;
  std::size_t failAt_ = SIZE_MAX;
};

// Keeps every row it is given, and checks they come in order and whole.
class Rows final : public PngRowSink {
 public:
  void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override {
    EXPECT_EQ(y, count);
    EXPECT_EQ(gray.size(), width);
    pixels.insert(pixels.end(), gray.begin(), gray.end());
    ++count;
  }

  std::uint32_t width = 0;
  std::uint32_t count = 0;
  Bytes pixels;
};

// What reading a file came to.
struct Reading {
  PngStatus status = PngStatus::kOk;
  PngHeader header;
  bool headerKnown = false;
  std::uint32_t rows = 0;
  Bytes pixels;
  std::size_t mostTakenInAStep = 0;
  std::uint32_t passedOver = 0;
  std::uint32_t badPassedOver = 0;
  bool transparent = false;
};

// Reads `file` a step at a time, from a source that gives `piece` bytes at
// a call, with `rowBytes` of room for rows (by default, what the picture
// needs). Holds every step to the reader's bound.
Reading ReadPng(std::span<const std::uint8_t> file, std::size_t piece = 4096, std::size_t rowBytes = 0) {
  static Bytes window(PngReader::kWindowBytes);
  // The width, for the sink, and the room for rows, from the file's own
  // header if it has one.
  std::uint32_t width = 0;
  if (rowBytes == 0) {
    PngReader peek({}, {});
    bn::core::MemoryByteSource source(file);
    peek.Read(source, nullptr);
    rowBytes = peek.HeaderKnown() ? static_cast<std::size_t>(PngReader::RowBytesFor(peek.Header())) : 0;
  }
  Bytes rows(rowBytes, 0xA5);
  PngReader reader(window, rows);
  PieceSource source(file, piece);
  Rows sink;
  reader.Begin(source, &sink);
  Reading reading;
  for (;;) {
    if (reader.HeaderKnown()) {
      width = reader.Header().width;
      sink.width = width;
    }
    const std::size_t before = source.Taken();
    const bool more = reader.Step();
    reading.mostTakenInAStep = std::max(reading.mostTakenInAStep, source.Taken() - before);
    EXPECT_LE(source.Taken() - before, PngReader::MostInputPerStep());
    if (!more) {
      break;
    }
  }
  reading.status = reader.Status();
  reading.header = reader.Header();
  reading.headerKnown = reader.HeaderKnown();
  reading.rows = reader.RowsGiven();
  reading.pixels = sink.pixels;
  reading.passedOver = reader.ChunksPassedOver();
  reading.badPassedOver = reader.BadChecksumsPassedOver();
  reading.transparent = reader.Transparent();
  EXPECT_EQ(sink.count, reading.rows);
  // Once it has stopped, it stays stopped.
  EXPECT_FALSE(reader.Step());
  EXPECT_EQ(reader.Status(), reading.status);
  return reading;
}

// A picture of the kind given, with samples made up at random.
PngPicture RandomPicture(std::mt19937& random, std::uint32_t width, std::uint32_t height, std::uint8_t depth,
                         std::uint8_t colour) {
  PngPicture picture;
  picture.width = width;
  picture.height = height;
  picture.depth = depth;
  picture.colour = colour;
  picture.rows.resize(picture.Stride() * height);
  std::uniform_int_distribution<int> byte(0, 255);
  for (auto& value : picture.rows) {
    value = static_cast<std::uint8_t>(byte(random));
  }
  // The bits past the last pixel of a row are nothing in particular, and
  // are left as they are: the reader must not look at them.
  return picture;
}

Bytes Be16(std::initializer_list<std::uint16_t> values) {
  Bytes out;
  for (const std::uint16_t value : values) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
  }
  return out;
}

struct Kind {
  std::uint8_t colour;
  std::uint8_t depth;
};

constexpr std::array<Kind, 15> kEveryKind = {{
    {0, 1},
    {0, 2},
    {0, 4},
    {0, 8},
    {0, 16},  // gray
    {2, 8},
    {2, 16},  // colour
    {3, 1},
    {3, 2},
    {3, 4},
    {3, 8},  // palette
    {4, 8},
    {4, 16},  // gray with alpha
    {6, 8},
    {6, 16},  // colour with alpha
}};

constexpr std::array<std::uint8_t, 7> kEveryFilter = {0, 1, 2, 3, 4, 4, 1};

// A palette of `entries` colours, made up at random, as the chunk has it
// and as the extras keep it.
Bytes RandomPalette(std::mt19937& random, std::size_t entries, PngExtras& extras) {
  Bytes chunk;
  std::uniform_int_distribution<int> byte(0, 255);
  for (std::size_t entry = 0; entry < entries; ++entry) {
    const std::array<std::uint8_t, 3> colour = {static_cast<std::uint8_t>(byte(random)),
                                                static_cast<std::uint8_t>(byte(random)),
                                                static_cast<std::uint8_t>(byte(random))};
    extras.palette.push_back(colour);
    chunk.insert(chunk.end(), colour.begin(), colour.end());
  }
  return chunk;
}

TEST(Gray, OverWhiteIsTheNearestGrayForEveryGrayAndAlpha) {
  for (unsigned gray = 0; gray < 256; ++gray) {
    for (unsigned alpha = 0; alpha < 256; ++alpha) {
      ASSERT_EQ(bn::draw::OverWhite(static_cast<std::uint8_t>(gray), static_cast<std::uint8_t>(alpha)),
                bn::test::PlainOverWhite(gray, alpha))
          << gray << " " << alpha;
    }
  }
  EXPECT_EQ(bn::draw::OverWhite(0, 255), 0);
  EXPECT_EQ(bn::draw::OverWhite(0, 0), 255);
  EXPECT_EQ(bn::draw::OverWhite(0, 128), 127);
}

TEST(Png, ReadsTheHeaderAloneWithNoMemory) {
  const Bytes file = PngFile().Header(640, 480, 8, 2).Add("tEXt", {'a'}).End().Bytes();
  PngReader reader({}, {});
  PieceSource source(file, 3);
  EXPECT_EQ(reader.Read(source, nullptr), PngStatus::kOk);
  ASSERT_TRUE(reader.HeaderKnown());
  EXPECT_EQ(reader.Header().width, 640U);
  EXPECT_EQ(reader.Header().height, 480U);
  EXPECT_EQ(reader.Header().depth, 8);
  EXPECT_EQ(reader.Header().colour, PngColour::kRgb);
  EXPECT_FALSE(reader.Header().interlaced);
  // The signature, the header's start, its 13 bytes and its checksum, and
  // nothing after.
  EXPECT_EQ(source.Taken(), 8U + 8U + 13U + 4U);
  EXPECT_EQ(reader.RowsGiven(), 0U);
}

TEST(Png, GivesEveryKindOfPictureAsTheGrayItLooks) {
  std::mt19937 random = Repeatable(1);
  for (const Kind kind : kEveryKind) {
    for (const std::uint32_t width : {1U, 2U, 3U, 7U, 8U, 9U, 31U, 64U}) {
      SCOPED_TRACE(testing::Message() << "colour " << int{kind.colour} << " depth " << int{kind.depth} << " width "
                                      << width);
      const PngPicture picture = RandomPicture(random, width, 9, kind.depth, kind.colour);
      PngExtras extras;
      std::vector<std::pair<std::string_view, Bytes>> before;
      if (kind.colour == 3) {
        before.emplace_back("PLTE", RandomPalette(random, std::size_t{1} << kind.depth, extras));
      }
      const Bytes file = bn::test::PngFor(picture, kEveryFilter, before);
      const Reading reading = ReadPng(file);
      ASSERT_EQ(reading.status, PngStatus::kOk) << PngStatusName(reading.status);
      EXPECT_EQ(reading.rows, 9U);
      EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, extras));
      EXPECT_EQ(reading.transparent, kind.colour == 4 || kind.colour == 6);
    }
  }
}

TEST(Png, UndoesEachFilterOnItsOwn) {
  std::mt19937 random = Repeatable(2);
  for (const Kind kind : kEveryKind) {
    for (std::uint8_t filter = 0; filter <= 4; ++filter) {
      const PngPicture picture = RandomPicture(random, 13, 5, kind.depth, kind.colour);
      PngExtras extras;
      std::vector<std::pair<std::string_view, Bytes>> before;
      if (kind.colour == 3) {
        before.emplace_back("PLTE", RandomPalette(random, 256, extras));
      }
      const std::array<std::uint8_t, 1> filters = {filter};
      const Reading reading = ReadPng(bn::test::PngFor(picture, filters, before));
      ASSERT_EQ(reading.status, PngStatus::kOk);
      EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, extras))
          << "colour " << int{kind.colour} << " depth " << int{kind.depth} << " filter " << int{filter};
    }
  }
}

TEST(Png, StretchesFewBitsOfGraySoTheLargestIsWhite) {
  PngPicture picture;
  picture.width = 4;
  picture.height = 1;
  picture.depth = 2;
  picture.colour = 0;
  picture.rows = {0b00011011};  // 0, 1, 2, 3
  const std::array<std::uint8_t, 1> none = {0};
  const Reading reading = ReadPng(bn::test::PngFor(picture, none));
  EXPECT_EQ(reading.pixels, (Bytes{0, 85, 170, 255}));
}

TEST(Png, TakesSixteenBitSamplesByTheirHighByte) {
  PngPicture picture;
  picture.width = 2;
  picture.height = 1;
  picture.depth = 16;
  picture.colour = 0;
  picture.rows = {0x12, 0xFF, 0xAB, 0x00};
  const std::array<std::uint8_t, 1> none = {0};
  EXPECT_EQ(ReadPng(bn::test::PngFor(picture, none)).pixels, (Bytes{0x12, 0xAB}));
}

TEST(Png, PutsWhatIsNotSolidOverWhite) {
  PngPicture picture;
  picture.width = 3;
  picture.height = 1;
  picture.depth = 8;
  picture.colour = 4;
  picture.rows = {0, 255, 0, 0, 100, 128};  // Black, solid; black, not there; 100 at half.
  const std::array<std::uint8_t, 1> none = {0};
  const Reading reading = ReadPng(bn::test::PngFor(picture, none));
  EXPECT_EQ(reading.pixels, (Bytes{0, 255, bn::test::PlainOverWhite(100, 128)}));
  EXPECT_TRUE(reading.transparent);
}

TEST(Png, TakesAGrayOrAColourThatStandsForNone) {
  std::mt19937 random = Repeatable(3);
  for (const Kind kind : kEveryKind) {
    if (kind.colour != 0 && kind.colour != 2) {
      continue;
    }
    PngPicture picture = RandomPicture(random, 16, 4, kind.depth, kind.colour);
    // The key is the first pixel's samples, so that some pixel has it.
    PngExtras extras;
    extras.hasKey = true;
    Bytes chunk;
    for (std::size_t sample = 0; sample < picture.Samples(); ++sample) {
      extras.key[sample] = picture.Sample(0, sample);
      const Bytes value = Be16({static_cast<std::uint16_t>(extras.key[sample])});
      chunk.insert(chunk.end(), value.begin(), value.end());
    }
    const Reading reading = ReadPng(bn::test::PngFor(picture, kEveryFilter, {{"tRNS", chunk}}));
    ASSERT_EQ(reading.status, PngStatus::kOk);
    EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, extras)) << int{kind.colour} << " " << int{kind.depth};
    EXPECT_EQ(reading.pixels[0], 255);
    EXPECT_TRUE(reading.transparent);
  }
}

TEST(Png, ComparesAKeyOfSixteenBitsWhole) {
  // Two pixels whose high bytes are the key's: only the one that is the
  // key in full is none.
  PngPicture picture;
  picture.width = 2;
  picture.height = 1;
  picture.depth = 16;
  picture.colour = 0;
  picture.rows = {0x40, 0x01, 0x40, 0x02};
  const std::array<std::uint8_t, 1> none = {0};
  const Reading reading = ReadPng(bn::test::PngFor(picture, none, {{"tRNS", Be16({0x4002})}}));
  EXPECT_EQ(reading.pixels, (Bytes{0x40, 255}));
}

TEST(Png, GivesAPalettesEntriesTheirAlphas) {
  std::mt19937 random = Repeatable(4);
  for (const std::uint8_t depth : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{8}}) {
    const PngPicture picture = RandomPicture(random, 20, 6, depth, 3);
    PngExtras extras;
    const Bytes palette = RandomPalette(random, std::size_t{1} << depth, extras);
    // Alphas for some of the entries, not all.
    extras.alphas.resize(std::max<std::size_t>(1, extras.palette.size() / 2));
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& alpha : extras.alphas) {
      alpha = static_cast<std::uint8_t>(byte(random));
    }
    const Reading reading =
        ReadPng(bn::test::PngFor(picture, kEveryFilter, {{"PLTE", palette}, {"tRNS", extras.alphas}}));
    ASSERT_EQ(reading.status, PngStatus::kOk);
    EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, extras)) << int{depth};
    EXPECT_TRUE(reading.transparent);
  }
}

TEST(Png, DrawsAnIndexPastThePalettesEndBlack) {
  PngPicture picture;
  picture.width = 3;
  picture.height = 1;
  picture.depth = 8;
  picture.colour = 3;
  picture.rows = {0, 1, 200};
  const std::array<std::uint8_t, 1> none = {0};
  const Reading reading = ReadPng(bn::test::PngFor(picture, none, {{"PLTE", Bytes{255, 255, 255, 10, 10, 10}}}));
  ASSERT_EQ(reading.status, PngStatus::kOk);
  EXPECT_EQ(reading.pixels, (Bytes{255, 10, 0}));
}

TEST(Png, PassesOverTransparencyItCannotUse) {
  std::mt19937 random = Repeatable(5);
  PngPicture picture = RandomPicture(random, 5, 2, 8, 0);
  picture.rows[0] = 7;
  const std::array<std::uint8_t, 1> none = {0};
  const PngExtras plain;
  // The wrong length for a gray picture; and a second after a good one.
  Reading reading = ReadPng(bn::test::PngFor(picture, none, {{"tRNS", Bytes{0, 7, 0}}}));
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, plain));
  EXPECT_EQ(reading.passedOver, 1U);
  EXPECT_FALSE(reading.transparent);
  PngExtras keyed;
  keyed.hasKey = true;
  keyed.key[0] = 7;
  reading = ReadPng(bn::test::PngFor(picture, none, {{"tRNS", Be16({7})}, {"tRNS", Be16({9})}}));
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, keyed));
  EXPECT_EQ(reading.passedOver, 1U);
  // Alphas for a palette that has not come yet, and more alphas than it
  // has entries.
  PngPicture indexed = RandomPicture(random, 5, 2, 1, 3);
  PngExtras extras;
  const Bytes palette = RandomPalette(random, 2, extras);
  reading = ReadPng(bn::test::PngFor(indexed, none, {{"tRNS", Bytes{0}}, {"PLTE", palette}, {"tRNS", Bytes{0, 0, 0}}}));
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(indexed, extras));
  EXPECT_EQ(reading.passedOver, 2U);
  EXPECT_FALSE(reading.transparent);
  // Alphas for an alpha picture.
  const PngPicture alpha = RandomPicture(random, 5, 2, 8, 4);
  reading = ReadPng(bn::test::PngFor(alpha, none, {{"tRNS", Be16({0})}}));
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(alpha, plain));
  EXPECT_EQ(reading.passedOver, 1U);
}

TEST(Png, PassesOverAPaletteThatAPictureOfAnotherKindHas) {
  std::mt19937 random = Repeatable(6);
  for (const std::uint8_t colour : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{6}}) {
    const PngPicture picture = RandomPicture(random, 6, 3, 8, colour);
    PngExtras unused;
    const Bytes palette = RandomPalette(random, 3, unused);
    const std::array<std::uint8_t, 1> none = {0};
    const Reading reading = ReadPng(bn::test::PngFor(picture, none, {{"PLTE", palette}}));
    ASSERT_EQ(reading.status, PngStatus::kOk);
    EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, PngExtras{}));
    EXPECT_EQ(reading.passedOver, 1U);
  }
}

TEST(Png, GivesTheSameRowsHoweverTheFileIsCutUp) {
  std::mt19937 random = Repeatable(7);
  const PngPicture picture = RandomPicture(random, 50, 40, 8, 6);
  const Bytes filtered = bn::test::FilterPngRows(picture, kEveryFilter);
  const Bytes stream = bn::test::ZlibStream(filtered, 1);
  const Bytes expected = bn::test::PlainGray(picture, PngExtras{});
  for (const std::size_t chunk : {1UL, 2UL, 3UL, 100UL, 1024UL, 8192UL}) {
    for (const unsigned empty : {0U, 1U, 2U}) {
      PngFile file;
      file.Header(50, 40, 8, 6).Add("tEXt", {'h', 'i'}).Data(stream, chunk, empty).End();
      for (const std::size_t piece : {1UL, 5UL, 4096UL}) {
        const Reading reading = ReadPng(file.Bytes(), piece);
        ASSERT_EQ(reading.status, PngStatus::kOk) << chunk << " " << empty << " " << piece;
        EXPECT_EQ(reading.pixels, expected) << chunk << " " << empty << " " << piece;
      }
    }
  }
}

TEST(Png, RefusesMoreEmptyDataChunksInARowThanAnEncoderWouldWrite) {
  std::mt19937 random = Repeatable(8);
  const PngPicture picture = RandomPicture(random, 20, 20, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes stream = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none));
  PngFile file;
  file.Header(20, 20, 8, 0).Data(stream, 10, PngReader::kMaxEmptyChunksInARow + 1).End();
  EXPECT_EQ(ReadPng(file.Bytes()).status, PngStatus::kBadData);
  // An empty chunk first, then data.
  PngFile first;
  first.Header(20, 20, 8, 0).Add("IDAT", std::span<const std::uint8_t>()).Data(stream).End();
  EXPECT_EQ(ReadPng(first.Bytes()).status, PngStatus::kOk);
}

TEST(Png, HoldsAStepToItsBoundWhenTheDataIsInChunksOfOneByte) {
  // The most a step can be made to take: data a byte to a chunk, with the
  // most empty chunks between, and a picture whose rows are long enough
  // for a step to ask the inflater for all it may.
  std::mt19937 random = Repeatable(9);
  const PngPicture picture = RandomPicture(random, 3000, 3, 8, 2);
  const std::array<std::uint8_t, 1> none = {0};
  // Stored, not compressed: a byte of data is a byte of the row.
  const Bytes stream = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none), 0);
  PngFile file;
  file.Header(3000, 3, 8, 2).Data(stream, 1, PngReader::kMaxEmptyChunksInARow).End();
  const Reading reading = ReadPng(file.Bytes(), 4096);
  ASSERT_EQ(reading.status, PngStatus::kOk);
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, PngExtras{}));
  // Within the bound, and not far within it: a step asks for 1 KB, and a
  // KB of stored data costs 37 bytes a byte here.
  EXPECT_LE(reading.mostTakenInAStep, PngReader::MostInputPerStep());
  EXPECT_GT(reading.mostTakenInAStep, PngReader::kRowBytesPerStep * 36);
}

TEST(Png, ReadsALongChunkThatIsPassedOverAPieceAtATime) {
  std::mt19937 random = Repeatable(10);
  const PngPicture picture = RandomPicture(random, 8, 8, 8, 0);
  const Bytes text(100000, 'x');
  const std::array<std::uint8_t, 1> none = {0};
  const Reading reading = ReadPng(bn::test::PngFor(picture, none, {{"zTXt", text}}));
  ASSERT_EQ(reading.status, PngStatus::kOk);
  EXPECT_LE(reading.mostTakenInAStep, PngReader::kChunkBytes + 4);
  EXPECT_EQ(reading.passedOver, 1U);
}

TEST(Png, RefusesInterlacedRowsAndReadsTheirHeader) {
  const Bytes file = PngFile().Header(10, 10, 8, 0, 1).Data(bn::test::ZlibStream(Bytes(200, 0))).End().Bytes();
  const Reading reading = ReadPng(file, 4096, 4096);
  EXPECT_EQ(reading.status, PngStatus::kInterlaced);
  EXPECT_TRUE(reading.headerKnown);
  EXPECT_TRUE(reading.header.interlaced);
  EXPECT_EQ(reading.rows, 0U);
  // Without rows asked for, its header is all there is to read.
  PngReader reader({}, {});
  bn::core::MemoryByteSource source(file);
  EXPECT_EQ(reader.Read(source, nullptr), PngStatus::kOk);
  EXPECT_TRUE(reader.Header().interlaced);
}

TEST(Png, RefusesWhatIsNotAPng) {
  EXPECT_EQ(ReadPng(Bytes{}, 4096, 1).status, PngStatus::kNotPng);
  EXPECT_EQ(ReadPng(Bytes{0x89, 'P', 'N', 'G'}, 4096, 1).status, PngStatus::kNotPng);
  // Any byte of the signature changed: among them the line ends, which a
  // copy made as text would change.
  for (std::size_t at = 0; at < 8; ++at) {
    Bytes file = PngFile().Header(1, 1, 8, 0).Bytes();
    file[at] = static_cast<std::uint8_t>(file[at] ^ 0x20U);
    EXPECT_EQ(ReadPng(file, 4096, 1).status, PngStatus::kNotPng) << at;
  }
}

TEST(Png, RefusesAHeaderPngDoesNotAllow) {
  const auto status = [](const Bytes& header) {
    return ReadPng(PngFile().Add("IHDR", header).End().Bytes(), 4096, 1).status;
  };
  using bn::test::PngHeaderBytes;
  EXPECT_EQ(status(PngHeaderBytes(0, 1, 8, 0)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 0, 8, 0)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(0x80000000U, 1, 8, 0)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 1, 3, 0)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 1, 4, 2)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 1, 16, 3)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 1, 8, 5)), PngStatus::kBadHeader);
  EXPECT_EQ(status(PngHeaderBytes(1, 1, 8, 0, 2)), PngStatus::kBadHeader);
  Bytes compressed = PngHeaderBytes(1, 1, 8, 0);
  compressed[10] = 1;
  EXPECT_EQ(status(compressed), PngStatus::kBadHeader);
  Bytes filtered = PngHeaderBytes(1, 1, 8, 0);
  filtered[11] = 1;
  EXPECT_EQ(status(filtered), PngStatus::kBadHeader);
  // A header of the wrong length, and a first chunk that is not one.
  EXPECT_EQ(status(Bytes(12, 1)), PngStatus::kBadHeader);
  EXPECT_EQ(ReadPng(PngFile().Add("tEXt", {'a'}).Header(1, 1, 8, 0).Bytes(), 4096, 1).status, PngStatus::kBadHeader);
  // The largest a side may be is allowed.
  EXPECT_EQ(status(PngHeaderBytes(0x7FFFFFFF, 1, 1, 0)), PngStatus::kNoRoom);
}

TEST(Png, RefusesAChunkThePictureNeedsWithAWrongChecksum) {
  std::mt19937 random = Repeatable(11);
  const PngPicture picture = RandomPicture(random, 30, 30, 8, 3);
  PngExtras extras;
  const Bytes palette = RandomPalette(random, 256, extras);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes stream = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none));
  const Bytes header = bn::test::PngHeaderBytes(30, 30, 8, 3);
  EXPECT_EQ(ReadPng(PngFile().Add("IHDR", header, true).Bytes(), 4096, 1).status, PngStatus::kBadChecksum);
  EXPECT_EQ(ReadPng(PngFile().Add("IHDR", header).Add("PLTE", palette, true).Data(stream).End().Bytes()).status,
            PngStatus::kBadChecksum);
  // The data's checksum is found wrong when the chunk has been read: the
  // rows made from it before then have been given.
  PngFile spoiled;
  spoiled.Add("IHDR", header).Add("PLTE", palette);
  spoiled.Add("IDAT", std::span(stream).first(stream.size() / 2), true);
  spoiled.Add("IDAT", std::span(stream).subspan(stream.size() / 2)).End();
  const Reading reading = ReadPng(spoiled.Bytes());
  EXPECT_EQ(reading.status, PngStatus::kBadChecksum);
  EXPECT_LT(reading.rows, 30U);
}

TEST(Png, PassesOverAChunkItCanDoWithoutThatHasAWrongChecksum) {
  std::mt19937 random = Repeatable(12);
  const PngPicture picture = RandomPicture(random, 30, 3, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  PngFile file;
  file.Header(30, 3, 8, 0).Add("tRNS", Be16({picture.rows[0]}), true).Add("tEXt", {'a', 'b'}, true);
  file.Data(bn::test::ZlibStream(bn::test::FilterPngRows(picture, none))).End();
  const Reading reading = ReadPng(file.Bytes());
  ASSERT_EQ(reading.status, PngStatus::kOk);
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, PngExtras{}));
  EXPECT_EQ(reading.passedOver, 2U);
  EXPECT_EQ(reading.badPassedOver, 2U);
  EXPECT_FALSE(reading.transparent);
}

TEST(Png, RefusesChunksThatAreNotAsPngAllows) {
  const Bytes stream = bn::test::ZlibStream(Bytes(2, 0));
  const auto status = [&](PngFile& file) { return ReadPng(file.Data(stream).End().Bytes(), 4096, 4096).status; };
  PngFile twoHeaders;
  twoHeaders.Header(1, 1, 8, 0).Header(1, 1, 8, 0);
  EXPECT_EQ(status(twoHeaders), PngStatus::kBadChunk);
  PngFile badName;
  badName.Header(1, 1, 8, 0).Add("tE1t", {'a'});
  EXPECT_EQ(status(badName), PngStatus::kBadChunk);
  PngFile twoPalettes;
  twoPalettes.Header(1, 1, 8, 3).Add("PLTE", {1, 2, 3}).Add("PLTE", {1, 2, 3});
  EXPECT_EQ(status(twoPalettes), PngStatus::kBadChunk);
  for (const std::size_t length : {0UL, 4UL, 771UL}) {
    PngFile wrong;
    wrong.Header(1, 1, 8, 3).Add("PLTE", Bytes(length, 9));
    EXPECT_EQ(status(wrong), PngStatus::kBadChunk) << length;
  }
  PngFile noPalette;
  noPalette.Header(1, 1, 8, 3);
  EXPECT_EQ(status(noPalette), PngStatus::kNoPalette);
  PngFile unknown;
  unknown.Header(1, 1, 8, 0).Add("ABCD", {1});
  EXPECT_EQ(status(unknown), PngStatus::kUnknownChunk);
  PngFile ancillary;
  ancillary.Header(1, 1, 8, 0).Add("abcd", {1}).Add("vpAg", {1, 2});
  EXPECT_EQ(status(ancillary), PngStatus::kOk);
  // A chunk longer than any may be.
  Bytes huge = PngFile().Header(1, 1, 8, 0).Bytes();
  bn::test::PutBe32(huge, 0x80000000U);
  huge.insert(huge.end(), {'t', 'E', 'X', 't'});
  EXPECT_EQ(ReadPng(huge, 4096, 4096).status, PngStatus::kBadChunk);
  // And a chunk of data after the first that says so: found when the
  // reader comes to it, which is before its last row.
  std::mt19937 random = Repeatable(19);
  const PngPicture picture = RandomPicture(random, 100, 20, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes rows = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none), 0);
  Bytes longData = PngFile().Header(100, 20, 8, 0).Add("IDAT", std::span(rows).first(rows.size() / 2)).Bytes();
  bn::test::PutBe32(longData, 0x80000000U);
  longData.insert(longData.end(), {'I', 'D', 'A', 'T'});
  const Reading reading = ReadPng(longData);
  EXPECT_EQ(reading.status, PngStatus::kBadChunk);
  EXPECT_LT(reading.rows, 20U);
}

TEST(Png, TakesItsDataOnlyFromChunksOfDataOneAfterAnother) {
  // The data split by another chunk, which PNG does not allow: the data
  // ends where the other chunk begins, and nothing of that chunk is taken
  // for data.
  std::mt19937 random = Repeatable(20);
  const PngPicture picture = RandomPicture(random, 60, 30, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes stream = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none), 0);
  PngFile file;
  file.Header(60, 30, 8, 0).Add("IDAT", std::span(stream).first(stream.size() / 2));
  file.Add("tEXt", Bytes(500, 'x')).Add("IDAT", std::span(stream).subspan(stream.size() / 2)).End();
  const Reading reading = ReadPng(file.Bytes());
  EXPECT_EQ(reading.status, PngStatus::kTruncated);
  EXPECT_GT(reading.rows, 0U);
  EXPECT_LT(reading.rows, 30U);
  const Bytes expected = bn::test::PlainGray(picture, PngExtras{});
  EXPECT_TRUE(std::equal(reading.pixels.begin(), reading.pixels.end(), expected.begin()));
}

TEST(Png, SaysWhereThePictureEndsTooSoon) {
  std::mt19937 random = Repeatable(13);
  const PngPicture picture = RandomPicture(random, 40, 40, 8, 2);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes stream = bn::test::ZlibStream(bn::test::FilterPngRows(picture, none), 0);
  const Bytes whole = PngFile().Header(40, 40, 8, 2).Data(stream, 1000).End().Bytes();
  const Bytes expected = bn::test::PlainGray(picture, PngExtras{});
  // Cut anywhere: inside the header, between chunks, inside the data.
  for (std::size_t length = 0; length + 40 < whole.size(); length += 97) {
    const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
    const Reading reading = ReadPng(cut, 4096, 8192);
    const bool signature = length >= 8;
    EXPECT_EQ(reading.status, signature ? PngStatus::kTruncated : PngStatus::kNotPng) << length;
    EXPECT_LT(reading.rows, 40U);
    EXPECT_TRUE(std::equal(reading.pixels.begin(), reading.pixels.end(), expected.begin())) << length;
  }
  // Data that is a whole stream, but of too few rows.
  const Reading shorter = ReadPng(PngFile().Header(40, 41, 8, 2).Data(stream).End().Bytes());
  EXPECT_EQ(shorter.status, PngStatus::kTruncated);
  EXPECT_EQ(shorter.rows, 40U);
  // The end, before any data.
  EXPECT_EQ(ReadPng(PngFile().Header(1, 1, 8, 0).End().Bytes(), 4096, 4096).status, PngStatus::kTruncated);
  // All the rows, and no end after them: that is a whole picture.
  const Bytes noEnd = PngFile().Header(40, 40, 8, 2).Data(stream, 1000).Bytes();
  EXPECT_EQ(ReadPng(noEnd).status, PngStatus::kOk);
}

TEST(Png, RefusesDataThatIsNotPngs) {
  std::mt19937 random = Repeatable(14);
  const PngPicture picture = RandomPicture(random, 10, 10, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  Bytes filtered = bn::test::FilterPngRows(picture, none);
  const Bytes stream = bn::test::ZlibStream(filtered);
  // zlib's own two bytes: another method, a window too large, a check that
  // fails, a dictionary.
  for (const auto& [first, second] :
       std::vector<std::pair<std::uint8_t, std::uint8_t>>{{0x79, 0x9C}, {0x88, 0x98}, {0x78, 0x9D}, {0x78, 0xBB}}) {
    Bytes changed = stream;
    changed[0] = first;
    changed[1] = second;
    EXPECT_EQ(ReadPng(PngFile().Header(10, 10, 8, 0).Data(changed).End().Bytes()).status, PngStatus::kBadData)
        << int{first} << " " << int{second};
  }
  // A DEFLATE block of a kind there is not.
  Bytes broken = stream;
  broken[2] = 0x07;
  EXPECT_EQ(ReadPng(PngFile().Header(10, 10, 8, 0).Data(broken).End().Bytes()).status, PngStatus::kBadData);
  // A row with a filter PNG does not have: the rows before it are given.
  filtered[(picture.Stride() + 1) * 4] = 5;
  const Reading reading = ReadPng(PngFile().Header(10, 10, 8, 0).Data(bn::test::ZlibStream(filtered)).End().Bytes());
  EXPECT_EQ(reading.status, PngStatus::kBadData);
  EXPECT_EQ(reading.rows, 4U);
}

TEST(Png, SaysWhenItHasNoRoomForThePicture) {
  std::mt19937 random = Repeatable(15);
  const PngPicture picture = RandomPicture(random, 100, 2, 16, 6);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes file = bn::test::PngFor(picture, none);
  PngHeader header;
  header.width = 100;
  header.height = 2;
  header.depth = 16;
  header.colour = PngColour::kRgba;
  const std::uint64_t needed = PngReader::RowBytesFor(header);
  EXPECT_EQ(needed, 2U * 801U);
  EXPECT_EQ(ReadPng(file, 4096, needed - 1).status, PngStatus::kNoRoom);
  EXPECT_EQ(ReadPng(file, 4096, needed).status, PngStatus::kOk);
  // A row of fewer bytes than pixels needs room for its gray.
  header.depth = 1;
  header.colour = PngColour::kGray;
  EXPECT_EQ(PngReader::RowBytesFor(header), 200U);
  header.depth = 3;
  EXPECT_EQ(PngReader::RowBytesFor(header), 0U);
  // Without a whole window.
  Bytes small(PngReader::kWindowBytes - 1);
  Bytes rows(needed);
  PngReader reader(small, rows);
  bn::core::MemoryByteSource source(file);
  Rows sink;
  sink.width = 100;
  EXPECT_EQ(reader.Read(source, &sink), PngStatus::kNoRoom);
}

TEST(Png, SaysWhenItsSourceFails) {
  std::mt19937 random = Repeatable(16);
  const PngPicture picture = RandomPicture(random, 30, 30, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes file = bn::test::PngFor(picture, none, {}, 100);
  static Bytes window(PngReader::kWindowBytes);
  for (std::size_t at = 0; at + 40 < file.size(); at += 37) {
    Bytes rows(static_cast<std::size_t>(PngReader::RowBytesFor({30, 30, 8, PngColour::kGray, false})));
    PngReader reader(window, rows);
    PieceSource source(file, 64);
    source.FailAt(at);
    Rows sink;
    sink.width = 30;
    EXPECT_EQ(reader.Read(source, &sink), PngStatus::kSourceFailed) << at;
  }
}

TEST(Png, ReadsOnePictureAfterAnother) {
  std::mt19937 random = Repeatable(17);
  static Bytes window(PngReader::kWindowBytes);
  Bytes rows(65536);
  PngReader reader(window, rows);
  for (const Kind kind : kEveryKind) {
    const PngPicture picture = RandomPicture(random, 21, 4, kind.depth, kind.colour);
    PngExtras extras;
    std::vector<std::pair<std::string_view, Bytes>> before;
    if (kind.colour == 3) {
      before.emplace_back("PLTE", RandomPalette(random, 3, extras));
    }
    const Bytes file = bn::test::PngFor(picture, kEveryFilter, before);
    bn::core::MemoryByteSource source(file);
    Rows sink;
    sink.width = 21;
    ASSERT_EQ(reader.Read(source, &sink), PngStatus::kOk);
    EXPECT_EQ(sink.pixels, bn::test::PlainGray(picture, extras));
  }
}

TEST(Png, StopsAtItsLastRow) {
  // Whatever comes after the rows is not read: more data, a broken end.
  std::mt19937 random = Repeatable(18);
  const PngPicture picture = RandomPicture(random, 10, 3, 8, 0);
  const std::array<std::uint8_t, 1> none = {0};
  Bytes filtered = bn::test::FilterPngRows(picture, none);
  filtered.insert(filtered.end(), 500, 0x55);
  Bytes file = PngFile().Header(10, 3, 8, 0).Data(bn::test::ZlibStream(filtered)).Bytes();
  file.insert(file.end(), {0, 0, 0, 1, '!', '!', '!', '!'});
  const Reading reading = ReadPng(file);
  EXPECT_EQ(reading.status, PngStatus::kOk);
  EXPECT_EQ(reading.pixels, bn::test::PlainGray(picture, PngExtras{}));
}

}  // namespace
