#include "draw/jpeg.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "jpeg_builder.h"

// The reader is given JPEG files made here from the coefficients of their
// blocks, of every kind it reads, and what it gives out is set beside what
// those coefficients come to worked out in real numbers (jpeg_builder.h).
// Then files of the kinds it refuses, and files spoiled in each way it looks
// for.

namespace {

using bn::draw::JpegHeader;
using bn::draw::JpegReader;
using bn::draw::JpegReduction;
using bn::draw::JpegStatus;
using bn::draw::RowSink;
using bn::test::JpegComponent;
using bn::test::JpegExpected;
using bn::test::JpegFile;
using bn::test::JpegOptions;
using bn::test::JpegPicture;
using bn::test::JpegSegment;

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
class Rows final : public RowSink {
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
  JpegStatus status = JpegStatus::kOk;
  JpegHeader header;
  bool headerKnown = false;
  JpegReduction reduction = JpegReduction::kNone;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t rows = 0;
  Bytes pixels;
  std::uint32_t segments = 0;
  std::uint32_t scans = 0;
  std::uint32_t junk = 0;
  std::size_t taken = 0;
};

// Reads `file` a step at a time, from a source that gives `piece` bytes at
// a call, with `rowBytes` of room for rows (by default, what the picture
// needs). Holds every step to the reader's bound, and to a row at the most.
Reading ReadJpeg(std::span<const std::uint8_t> file, JpegReduction reduction = JpegReduction::kNone,
                 std::size_t piece = 4096, std::size_t rowBytes = SIZE_MAX, std::size_t failAt = SIZE_MAX) {
  if (rowBytes == SIZE_MAX) {
    JpegReader peek({});
    bn::core::MemoryByteSource source(file);
    peek.Read(source, nullptr);
    rowBytes = peek.HeaderKnown() ? static_cast<std::size_t>(JpegReader::RowBytesFor(peek.Header(), reduction)) : 0;
  }
  Bytes rows(rowBytes, 0xA5);
  JpegReader reader(rows);
  PieceSource source(file, piece);
  source.FailAt(failAt);
  Rows sink;
  reader.Begin(source, &sink, reduction);
  for (;;) {
    if (reader.HeaderKnown()) {
      sink.width = reader.Width();
    }
    const std::size_t taken = source.Taken();
    const std::uint32_t given = sink.count;
    const bool more = reader.Step();
    EXPECT_LE(source.Taken() - taken, JpegReader::MostInputPerStep());
    EXPECT_LE(sink.count - given, 1U);
    if (!more) {
      break;
    }
  }
  Reading reading;
  reading.status = reader.Status();
  reading.header = reader.Header();
  reading.headerKnown = reader.HeaderKnown();
  reading.reduction = reader.Reduction();
  reading.width = reader.Width();
  reading.height = reader.Height();
  reading.rows = reader.RowsGiven();
  reading.pixels = sink.pixels;
  reading.segments = reader.SegmentsPassedOver();
  reading.scans = reader.ScansPassedOver();
  reading.junk = reader.JunkBytes();
  reading.taken = source.Taken();
  EXPECT_EQ(sink.count, reading.rows);
  // Once it has stopped, it stays stopped.
  EXPECT_FALSE(reader.Step());
  EXPECT_EQ(reader.Status(), reading.status);
  return reading;
}

// Each pixel within `within` of what the picture's coefficients come to,
// held to 0 to 255.
void ExpectPixels(const Bytes& pixels, const std::vector<double>& expected, double within, const char* what) {
  ASSERT_EQ(pixels.size(), expected.size()) << what;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const double wanted = std::clamp(expected[i], 0.0, 255.0);
    ASSERT_LE(std::abs(pixels[i] - wanted), within) << what << ": pixel " << i;
  }
}

// A picture of the components and size given, every block's coefficients
// made up at random: averages that keep it mostly within the grays, and a
// few of the other frequencies, more of them the lower.
JpegPicture RandomPicture(std::mt19937& random, std::uint32_t width, std::uint32_t height,
                          std::vector<JpegComponent> components) {
  JpegPicture picture;
  picture.width = width;
  picture.height = height;
  picture.components = std::move(components);
  picture.Make();
  for (auto& table : picture.steps) {
    for (std::size_t i = 0; i < 64; ++i) {
      table[i] = static_cast<std::uint16_t>(2 + ((i * 7) % 11));
    }
  }
  std::uniform_int_distribution<int> average(-50, 50);
  std::uniform_int_distribution<int> other(-12, 12);
  std::uniform_int_distribution<int> which(0, 63);
  for (auto& blocks : picture.blocks) {
    for (auto& block : blocks) {
      block[0] = static_cast<std::int16_t>(average(random));
      for (int i = 0; i < 10; ++i) {
        const int at = which(random) % (1 + which(random));
        if (at != 0) {
          block[static_cast<std::size_t>(at)] = static_cast<std::int16_t>(other(random));
        }
      }
    }
  }
  return picture;
}

std::vector<JpegComponent> Gray() { return {JpegComponent{1, 1, 1, 0}}; }
std::vector<JpegComponent> Colour(std::uint8_t across, std::uint8_t down) {
  return {JpegComponent{1, across, down, 0}, JpegComponent{2, 1, 1, 1}, JpegComponent{3, 1, 1, 1}};
}

constexpr JpegReduction kEveryReduction[] = {JpegReduction::kNone, JpegReduction::kHalf, JpegReduction::kQuarter,
                                             JpegReduction::kEighth};

// What the picture should come to at a reduction, and how near: an eighth
// is the average alone, worked out exactly.
void ExpectPicture(const Reading& reading, const JpegPicture& picture, JpegReduction reduction, unsigned shift,
                   const char* what) {
  const auto by = static_cast<std::uint32_t>(reduction);
  EXPECT_EQ(reading.status, JpegStatus::kOk) << what;
  EXPECT_EQ(reading.width, (picture.width + by - 1) / by) << what;
  EXPECT_EQ(reading.rows, (picture.height + by - 1) / by) << what;
  ExpectPixels(reading.pixels, JpegExpected(picture, by, shift), by == 8 ? 0.5 + 1e-9 : 1.0, what);
}

Bytes Header(const JpegPicture& picture, std::uint8_t frame = 0xC0) {
  JpegOptions options;
  options.frame = frame;
  return JpegFile(picture, options);
}

TEST(Jpeg, ReadsTheHeaderAloneWithNoMemory) {
  auto random = Repeatable(1);
  const JpegPicture picture = RandomPicture(random, 37, 21, Colour(2, 2));
  const Bytes file = JpegFile(picture);
  JpegReader reader({});
  bn::core::MemoryByteSource source(file);
  EXPECT_EQ(reader.Read(source, nullptr), JpegStatus::kOk);
  ASSERT_TRUE(reader.HeaderKnown());
  const JpegHeader& header = reader.Header();
  EXPECT_EQ(header.width, 37U);
  EXPECT_EQ(header.height, 21U);
  EXPECT_EQ(header.components, 3);
  EXPECT_FALSE(header.progressive);
  EXPECT_EQ(header.grayAcross, 2);
  EXPECT_EQ(header.grayDown, 2);
  EXPECT_EQ(header.colourAcross, 1);
  EXPECT_EQ(header.colourDown, 1);
  EXPECT_STREQ(bn::draw::JpegKindName(header), "4:2:0");
  EXPECT_EQ(reader.RowsGiven(), 0U);
}

TEST(Jpeg, NamesEachKindOfPicture) {
  JpegHeader header;
  header.components = 1;
  EXPECT_STREQ(bn::draw::JpegKindName(header), "gray");
  header.progressive = true;
  EXPECT_STREQ(bn::draw::JpegKindName(header), "progressive gray");
  header.components = 3;
  header.colourAcross = 1;
  header.colourDown = 1;
  const struct {
    std::uint8_t across;
    std::uint8_t down;
    const char* name;
  } kinds[] = {{1, 1, "progressive 4:4:4"}, {2, 1, "progressive 4:2:2"}, {2, 2, "progressive 4:2:0"},
               {1, 2, "progressive 4:4:0"}, {4, 1, "progressive 4:1:1"}, {3, 1, "progressive ycbcr"}};
  for (const auto& kind : kinds) {
    header.grayAcross = kind.across;
    header.grayDown = kind.down;
    EXPECT_STREQ(bn::draw::JpegKindName(header), kind.name);
  }
  header.progressive = false;
  header.grayAcross = 2;
  header.grayDown = 1;
  EXPECT_STREQ(bn::draw::JpegKindName(header), "4:2:2");
  header.components = 4;
  EXPECT_STREQ(bn::draw::JpegKindName(header), "cmyk");
}

TEST(Jpeg, GivesAGrayPictureAsItsPixels) {
  auto random = Repeatable(2);
  for (const std::uint32_t side : {1U, 7U, 8U, 9U, 31U}) {
    const JpegPicture picture = RandomPicture(random, side, side + 3, Gray());
    const Reading reading = ReadJpeg(JpegFile(picture));
    EXPECT_STREQ(bn::draw::JpegKindName(reading.header), "gray");
    ExpectPicture(reading, picture, JpegReduction::kNone, 0, "gray");
  }
}

TEST(Jpeg, GivesAColourPicturesGrayHoweverItIsSampled) {
  auto random = Repeatable(3);
  const std::pair<std::uint8_t, std::uint8_t> samplings[] = {{1, 1}, {2, 1}, {2, 2}, {1, 2}, {4, 1}, {1, 4}};
  for (const auto& [across, down] : samplings) {
    const JpegPicture picture = RandomPicture(random, 45, 29, Colour(across, down));
    for (const JpegReduction reduction : kEveryReduction) {
      ExpectPicture(ReadJpeg(JpegFile(picture), reduction), picture, reduction, 0, "colour");
    }
  }
}

// The table that turns a block into `side` pixels a side, made the way the
// reader's source says its tables are: what frequency u gives a pixel,
// C(u)/2 * cos((2x + 1)u pi / 16), averaged over the pixels of the whole
// block that the pixel stands for, times 4096 and rounded.
std::array<std::array<std::int32_t, 8>, 8> TransformTable(std::size_t side) {
  std::array<std::array<std::int32_t, 8>, 8> table{};
  const std::size_t each = 8 / side;
  for (std::size_t u = 0; u < 8; ++u) {
    for (std::size_t x = 0; x < side; ++x) {
      double sum = 0;
      for (std::size_t k = 0; k < each; ++k) {
        const double scale = u == 0 ? std::numbers::sqrt2 / 4 : 0.5;
        sum += scale * std::cos(((2.0 * static_cast<double>((x * each) + k)) + 1) * static_cast<double>(u) *
                                std::numbers::pi / 16);
      }
      table[u][x] = static_cast<std::int32_t>(std::lround(4096.0 * sum / static_cast<double>(each)));
    }
  }
  return table;
}

// What a picture's first component comes to with each block `side` pixels a
// side, by multiplying every coefficient by every entry of that table,
// down and then across, in whole numbers rounded as the reader's source
// rounds them: 9 bits dropped after the first product, 15 after the second.
Bytes PixelsByTheWholeProduct(const JpegPicture& picture, std::size_t side) {
  // A sum with so many bits dropped from it, rounded down whatever its sign.
  const auto dropped = [](std::int64_t sum, std::int64_t one) {
    return sum >= 0 ? sum / one : -((-sum + one - 1) / one);
  };
  const std::array<std::array<std::int32_t, 8>, 8> table = TransformTable(side);
  const std::size_t by = 8 / side;
  const std::size_t wide = (picture.width + by - 1) / by;
  const std::size_t rows = (picture.height + by - 1) / by;
  const std::size_t across = picture.BlocksAcross(0);
  const auto& steps = picture.steps.at(picture.components.at(0).table);
  Bytes pixels(wide * rows);
  for (std::size_t at = 0; at < picture.blocks.at(0).size(); ++at) {
    const bn::test::JpegBlock& block = picture.blocks[0][at];
    std::array<std::array<std::int64_t, 8>, 8> between{};
    for (std::size_t u = 0; u < 8; ++u) {
      for (std::size_t y = 0; y < side; ++y) {
        std::int64_t sum = 0;
        for (std::size_t v = 0; v < 8; ++v) {
          sum += std::int64_t{table[v][y]} * block[(v * 8) + u] * steps[(v * 8) + u];
        }
        between[y][u] = dropped(sum + 256, 512);
      }
    }
    for (std::size_t y = 0; y < side; ++y) {
      for (std::size_t x = 0; x < side; ++x) {
        std::int64_t sum = 0;
        for (std::size_t u = 0; u < 8; ++u) {
          sum += std::int64_t{table[u][x]} * between[y][u];
        }
        const std::size_t px = ((at % across) * side) + x;
        const std::size_t py = ((at / across) * side) + y;
        if (px < wide && py < rows) {
          pixels[(py * wide) + px] =
              static_cast<std::uint8_t>(std::clamp<std::int64_t>(dropped(sum + 16384, 32768) + 128, 0, 255));
        }
      }
    }
  }
  return pixels;
}

TEST(Jpeg, GivesThePixelsMultiplyingTheWholeTableOutGives) {
  // The reader makes each product once for a pixel and the one as far from
  // the other end of its row or column, and stops at the last coefficient
  // a block has. Neither may show: every pixel is the number the whole
  // product comes to, not within a gray of it, at the whole size, a half
  // and a quarter, for gray and for colour, with sides that are no whole
  // number of blocks.
  EXPECT_EQ(TransformTable(8)[1][0], 2009);
  EXPECT_EQ(TransformTable(8)[7][3], -2009);
  EXPECT_EQ(TransformTable(4)[1][0], 1856);
  EXPECT_EQ(TransformTable(4)[7][0], -369);
  EXPECT_EQ(TransformTable(2)[1][0], 1312);
  EXPECT_EQ(TransformTable(2)[7][1], 261);
  auto random = Repeatable(61);
  std::size_t pixelsInAll = 0;
  // Sides that are a block, less, more, and no whole number of them: the
  // same on every machine, which what a distribution takes from the
  // numbers it is given is not.
  constexpr std::array<std::uint32_t, 12> kWidths = {1, 7, 8, 9, 16, 31, 33, 45, 50, 64, 67, 70};
  constexpr std::array<std::uint32_t, 12> kHeights = {3, 8, 23, 17, 16, 9, 40, 29, 5, 48, 31, 50};
  for (std::size_t round = 0; round < kWidths.size(); ++round) {
    std::vector<JpegComponent> components = Gray();
    if (round % 3 != 0) {
      components = Colour(2, round % 3 == 1 ? 2 : 1);
    }
    const JpegPicture picture = RandomPicture(random, kWidths[round], kHeights[round], components);
    const std::vector<std::uint8_t> file = JpegFile(picture);
    for (const JpegReduction reduction : {JpegReduction::kNone, JpegReduction::kHalf, JpegReduction::kQuarter}) {
      const Reading reading = ReadJpeg(file, reduction);
      ASSERT_EQ(reading.status, JpegStatus::kOk) << round;
      const Bytes expected = PixelsByTheWholeProduct(picture, 8 / static_cast<std::size_t>(reduction));
      ASSERT_EQ(reading.pixels.size(), expected.size()) << round;
      for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(reading.pixels[i], expected[i])
            << "round " << round << ", reduced by " << static_cast<int>(reduction) << ", pixel " << i;
      }
      pixelsInAll += expected.size();
    }
  }
  EXPECT_GT(pixelsInAll, 10000U);

  // A sum that lies exactly on the half that rounding turns on comes to
  // one pixel in 32,768 by chance, so a block is looked for that has one
  // in the far half of its rows, where a pixel is the two sums taken one
  // from the other: an average and one frequency across, the same down
  // every column, so that each row of pixels is the same.
  const std::array<std::array<std::int32_t, 8>, 8> table = TransformTable(8);
  const auto dropped = [](std::int64_t sum, std::int64_t one) {
    return sum >= 0 ? sum / one : -((-sum + one - 1) / one);
  };
  int onTheHalf = 0;
  for (int average = -300; average <= 300 && onTheHalf < 3; ++average) {
    for (int across = -40; across <= 40 && onTheHalf < 3; ++across) {
      const std::int64_t first = dropped((std::int64_t{table[0][0]} * average) + 256, 512);
      const std::int64_t second = dropped((std::int64_t{table[0][0]} * across) + 256, 512);
      bool here = false;
      for (std::size_t x = 4; x < 8; ++x) {
        const std::int64_t sum = (table[0][x] * first) + (table[1][x] * second);
        here = here || (((sum + 16384) % 32768 == 0) && std::abs(sum) < 3276800);
      }
      if (!here) {
        continue;
      }
      ++onTheHalf;
      JpegPicture edge;
      edge.width = 8;
      edge.height = 8;
      edge.components = Gray();
      edge.Make();
      edge.blocks[0][0][0] = static_cast<std::int16_t>(average);
      edge.blocks[0][0][1] = static_cast<std::int16_t>(across);
      const Reading reading = ReadJpeg(JpegFile(edge));
      ASSERT_EQ(reading.status, JpegStatus::kOk);
      ASSERT_TRUE(reading.pixels == PixelsByTheWholeProduct(edge, 8)) << average << " and " << across;
    }
  }
  EXPECT_EQ(onTheHalf, 3);
}

TEST(Jpeg, ReadsAPictureSmallerAtEachReduction) {
  auto random = Repeatable(4);
  for (const std::uint32_t width : {1U, 5U, 16U, 50U}) {
    const JpegPicture picture = RandomPicture(random, width, 23, Gray());
    for (const JpegReduction reduction : kEveryReduction) {
      ExpectPicture(ReadJpeg(JpegFile(picture), reduction), picture, reduction, 0, "smaller");
    }
  }
}

TEST(Jpeg, AveragesThePixelsOfABlockWhenItReadsItSmaller) {
  // A block of black and white columns, one pixel wide, which read at a half
  // is an even gray, not an edge that rings.
  JpegPicture picture;
  picture.width = 8;
  picture.height = 8;
  picture.Make();
  std::array<double, 8> column{};
  for (std::size_t x = 0; x < 8; ++x) {
    column[x] = x % 2 == 0 ? -128 : 127;
  }
  // The forward transform of those columns, by T.81 A.3.3.
  for (std::size_t u = 0; u < 8; ++u) {
    double sum = 0;
    for (std::size_t x = 0; x < 8; ++x) {
      sum += column[x] * std::cos(static_cast<double>(((2 * x) + 1) * u) * std::numbers::pi / 16);
    }
    const double cu = u == 0 ? std::numbers::sqrt2 / 2 : 1.0;
    picture.Block(0, 0, 0)[u] = static_cast<std::int16_t>(std::lround(cu * sum * std::numbers::sqrt2 / 2 / 2 * 2));
  }
  const Reading half = ReadJpeg(JpegFile(picture), JpegReduction::kHalf);
  ASSERT_EQ(half.status, JpegStatus::kOk);
  ASSERT_EQ(half.pixels.size(), 16U);
  for (const std::uint8_t pixel : half.pixels) {
    EXPECT_NEAR(pixel, 128, 2);
  }
  ExpectPicture(half, picture, JpegReduction::kHalf, 0, "columns");
}

TEST(Jpeg, ReadsAProgressivePictureFromItsFirstScanAtAnEighth) {
  auto random = Repeatable(5);
  for (const unsigned shift : {0U, 1U, 3U}) {
    for (const bool alone : {false, true}) {
      const JpegPicture picture = RandomPicture(random, 53, 35, Colour(2, 2));
      JpegOptions options;
      options.frame = 0xC2;
      options.shift = static_cast<std::uint8_t>(shift);
      options.grayAverageAlone = alone;
      // Whatever is asked, an eighth.
      const Reading reading = ReadJpeg(JpegFile(picture, options), JpegReduction::kNone);
      EXPECT_TRUE(reading.header.progressive);
      EXPECT_EQ(reading.reduction, JpegReduction::kEighth);
      ExpectPicture(reading, picture, JpegReduction::kEighth, shift, "progressive");
      EXPECT_EQ(reading.scans, 0U);
    }
  }
}

TEST(Jpeg, PassesOverAProgressivePicturesScansBeforeTheGraysAverages) {
  auto random = Repeatable(6);
  const JpegPicture picture = RandomPicture(random, 40, 24, Colour(1, 1));
  JpegOptions options;
  options.frame = 0xC2;
  options.colourAveragesFirst = true;
  const Reading reading = ReadJpeg(JpegFile(picture, options));
  ExpectPicture(reading, picture, JpegReduction::kEighth, 0, "colour first");
  EXPECT_EQ(reading.scans, 1U);
}

TEST(Jpeg, ReadsAProgressiveGrayPicture) {
  auto random = Repeatable(7);
  const JpegPicture picture = RandomPicture(random, 70, 9, Gray());
  JpegOptions options;
  options.frame = 0xC2;
  options.shift = 2;
  const Reading reading = ReadJpeg(JpegFile(picture, options), JpegReduction::kHalf);
  EXPECT_STREQ(bn::draw::JpegKindName(reading.header), "progressive gray");
  ExpectPicture(reading, picture, JpegReduction::kEighth, 2, "progressive gray");
}

TEST(Jpeg, ReadsAPictureInSeveralScansFromTheGraysOwn) {
  auto random = Repeatable(8);
  for (const bool colourFirst : {false, true}) {
    const JpegPicture picture = RandomPicture(random, 30, 19, Colour(2, 1));
    JpegOptions options;
    options.separateScans = true;
    options.colourFirst = colourFirst;
    const Reading reading = ReadJpeg(JpegFile(picture, options), JpegReduction::kHalf);
    ExpectPicture(reading, picture, JpegReduction::kHalf, 0, "several scans");
    EXPECT_EQ(reading.scans, colourFirst ? 2U : 0U);
  }
}

TEST(Jpeg, FollowsRestartMarkers) {
  auto random = Repeatable(9);
  const JpegPicture picture = RandomPicture(random, 61, 33, Colour(2, 2));
  // Units across: 4. A restart inside a row of units, at its end, and past it.
  for (const std::uint16_t interval :
       {std::uint16_t{1}, std::uint16_t{3}, std::uint16_t{4}, std::uint16_t{5}, std::uint16_t{100}}) {
    for (const std::size_t fill : {std::size_t{0}, std::size_t{3}}) {
      JpegOptions options;
      options.restartInterval = interval;
      options.fill = fill;
      ExpectPicture(ReadJpeg(JpegFile(picture, options)), picture, JpegReduction::kNone, 0, "restarts");
    }
  }
  JpegOptions options;
  options.frame = 0xC2;
  options.restartInterval = 2;
  ExpectPicture(ReadJpeg(JpegFile(picture, options)), picture, JpegReduction::kEighth, 0, "progressive restarts");
}

TEST(Jpeg, RefusesARestartMarkerOutOfTurnOrMissing) {
  auto random = Repeatable(10);
  const JpegPicture picture = RandomPicture(random, 16, 32, Gray());
  JpegOptions options;
  options.restartInterval = 2;  // Two units a row: a restart at each row's start.
  const Bytes file = JpegFile(picture, options);
  const Reading whole = ReadJpeg(file);
  ASSERT_EQ(whole.status, JpegStatus::kOk);
  // The second restart marker, 0xFFD1, made 0xFFD5, and taken out.
  Bytes wrong = file;
  Bytes missing = file;
  for (std::size_t at = file.size() - 2; at > 0; --at) {
    if (file[at] == 0xFF && file[at + 1] == 0xD1) {
      wrong[at + 1] = 0xD5;
      missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(at),
                    missing.begin() + static_cast<std::ptrdiff_t>(at) + 2);
      break;
    }
  }
  for (const Bytes& spoiled : {wrong, missing}) {
    const Reading reading = ReadJpeg(spoiled);
    EXPECT_EQ(reading.status, JpegStatus::kBadData);
    // The two rows of blocks before it were given.
    EXPECT_EQ(reading.rows, 16U);
    EXPECT_TRUE(std::equal(reading.pixels.begin(), reading.pixels.end(), whole.pixels.begin()));
  }
}

TEST(Jpeg, PassesOverSegmentsAndWhatIsBetweenThem) {
  auto random = Repeatable(11);
  const JpegPicture picture = RandomPicture(random, 20, 20, Colour(1, 1));
  JpegOptions options;
  options.before = {JpegSegment(0xE0, {'J', 'F', 'I', 'F', 0, 1, 2, 0, 0, 1, 0, 1, 0, 0}),
                    JpegSegment(0xE1, Bytes(3000, 0x42)),
                    JpegSegment(0xFE, {'a', ' ', 'n', 'o', 't', 'e'}),
                    {0x12, 0x34, 0xFF, 0x00},
                    {0xFF, 0xFF, 0xFF},
                    {0xFF, 0x01}};
  options.beforeScan = JpegSegment(0xED, {1, 2, 3});
  const Reading reading = ReadJpeg(JpegFile(picture, options));
  ExpectPicture(reading, picture, JpegReduction::kNone, 0, "segments");
  // Three segments, the standalone marker and the one before the scan.
  EXPECT_EQ(reading.segments, 5U);
  EXPECT_EQ(reading.junk, 4U);
}

TEST(Jpeg, TakesQuantizationTablesOfSixteenBits) {
  auto random = Repeatable(12);
  JpegPicture picture = RandomPicture(random, 24, 16, Gray());
  for (std::size_t i = 0; i < 64; ++i) {
    picture.steps[0][i] = static_cast<std::uint16_t>(300 + i);
  }
  for (auto& block : picture.blocks[0]) {
    block.fill(0);
    block[0] = 1;
    block[1] = -1;
  }
  JpegOptions options;
  options.frame = 0xC1;
  options.sixteenBitSteps = true;
  ExpectPicture(ReadJpeg(JpegFile(picture, options)), picture, JpegReduction::kNone, 0, "sixteen bits");
}

TEST(Jpeg, RefusesTheKindsItDoesNotReadOnceItHasTheirHeader) {
  auto random = Repeatable(13);
  const JpegPicture colour = RandomPicture(random, 16, 16, Colour(1, 1));
  const auto refused = [](const Bytes& file, const char* what) {
    const Reading reading = ReadJpeg(file);
    EXPECT_EQ(reading.status, JpegStatus::kUnsupported) << what;
    EXPECT_TRUE(reading.headerKnown) << what;
    EXPECT_EQ(reading.rows, 0U) << what;
  };
  for (const std::uint8_t frame : Bytes{0xC3, 0xC5, 0xC9, 0xCA, 0xCB}) {
    refused(Header(colour, frame), "frame");
  }
  JpegOptions twelve;
  twelve.precision = 12;
  refused(JpegFile(colour, twelve), "12 bits");
  JpegOptions adobe;
  adobe.before = {JpegSegment(0xEE, {'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, 0})};
  refused(JpegFile(colour, adobe), "rgb by Adobe's marker");
  JpegPicture named = colour;
  named.components[0].id = 'R';
  named.components[1].id = 'G';
  named.components[2].id = 'B';
  refused(JpegFile(named), "rgb by name");
  JpegPicture cmyk = RandomPicture(random, 16, 16, {{1, 1, 1, 0}, {2, 1, 1, 0}, {3, 1, 1, 0}, {4, 1, 1, 0}});
  refused(JpegFile(cmyk), "cmyk");
  JpegPicture less = RandomPicture(random, 16, 16, {{1, 1, 1, 0}, {2, 2, 1, 1}, {3, 1, 1, 1}});
  refused(JpegFile(less), "gray sampled less");
  JpegPicture later = colour;
  later.height = 0;
  refused(JpegFile(later), "height later");
  // And the same with only the header asked for.
  JpegReader reader({});
  const Bytes cmykFile = JpegFile(cmyk);
  bn::core::MemoryByteSource source(cmykFile);
  EXPECT_EQ(reader.Read(source, nullptr), JpegStatus::kUnsupported);
  EXPECT_TRUE(reader.HeaderKnown());
  EXPECT_EQ(reader.Header().components, 4);
}

TEST(Jpeg, RefusesWhatIsNotAJpeg) {
  for (const Bytes& file : {Bytes{}, Bytes{0xFF}, Bytes{0xFF, 0xD9}, Bytes{0x89, 'P', 'N', 'G'}}) {
    EXPECT_EQ(ReadJpeg(file).status, JpegStatus::kNotJpeg);
  }
}

// Where the first segment with `marker` begins in `file`, by going from one
// segment to the next.
std::size_t SegmentAt(const Bytes& file, std::uint8_t marker) {
  for (std::size_t at = 2; at + 4 <= file.size();) {
    if (file[at + 1] == marker) {
      return at;
    }
    at += 2 + ((std::size_t{file[at + 2]} << 8U) | file[at + 3]);
  }
  ADD_FAILURE() << "no such segment";
  return file.size();
}

// The file with the segment that begins with `marker` (the first of them)
// made into `segment`.
Bytes WithSegment(const Bytes& file, std::uint8_t marker, const Bytes& segment) {
  for (std::size_t at = 2; at + 4 <= file.size();) {
    const std::size_t length = (std::size_t{file[at + 2]} << 8U) | file[at + 3];
    if (file[at + 1] == marker) {
      Bytes out(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(at));
      out.insert(out.end(), segment.begin(), segment.end());
      out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(at + 2 + length), file.end());
      return out;
    }
    at += 2 + length;
  }
  ADD_FAILURE() << "no such segment";
  return file;
}

TEST(Jpeg, RefusesAFrameHeaderJpegDoesNotAllow) {
  auto random = Repeatable(14);
  const Bytes file = JpegFile(RandomPicture(random, 16, 16, Colour(1, 1)));
  const auto frame = [&](const Bytes& body) { return WithSegment(file, 0xC0, JpegSegment(0xC0, body)); };
  const Bytes good = {8, 0, 16, 0, 16, 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1};
  EXPECT_EQ(ReadJpeg(frame(good)).status, JpegStatus::kOk);
  const auto bad = [&](std::size_t at, std::uint8_t value) {
    Bytes body = good;
    body[at] = value;
    return frame(body);
  };
  Bytes noWidth = good;
  noWidth[3] = 0;
  noWidth[4] = 0;
  EXPECT_EQ(ReadJpeg(frame(noWidth)).status, JpegStatus::kBadHeader) << "no width";
  EXPECT_EQ(ReadJpeg(bad(7, 0x01)).status, JpegStatus::kBadHeader) << "sampled 0 across";
  EXPECT_EQ(ReadJpeg(bad(7, 0x51)).status, JpegStatus::kBadHeader) << "sampled 5 across";
  EXPECT_EQ(ReadJpeg(bad(8, 4)).status, JpegStatus::kBadHeader) << "table 4";
  EXPECT_EQ(ReadJpeg(bad(9, 1)).status, JpegStatus::kBadHeader) << "a component twice";
  EXPECT_EQ(ReadJpeg(bad(0, 9)).status, JpegStatus::kBadHeader) << "9 bits";
  EXPECT_EQ(ReadJpeg(bad(5, 0)).status, JpegStatus::kBadHeader) << "no components";
  EXPECT_EQ(ReadJpeg(bad(5, 2)).status, JpegStatus::kBadHeader) << "a length that is not the components'";
  // A second frame header.
  Bytes twice = file;
  const Bytes second = JpegSegment(0xC0, good);
  twice.insert(twice.begin() + static_cast<std::ptrdiff_t>(SegmentAt(twice, 0xDA)), second.begin(), second.end());
  EXPECT_EQ(ReadJpeg(twice).status, JpegStatus::kBadHeader);
}

TEST(Jpeg, RefusesAScanOrTheEndBeforeTheFrameHeader) {
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xD9}).status, JpegStatus::kBadHeader);
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 63, 0}).status, JpegStatus::kBadHeader);
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xFE, 0, 3, 'x'}).status, JpegStatus::kBadHeader);
}

TEST(Jpeg, RefusesMarkersThatAreNotAsJpegAllows) {
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xFE, 0, 1}).status, JpegStatus::kBadMarker);
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xD8}).status, JpegStatus::kBadMarker);
  EXPECT_EQ(ReadJpeg(Bytes{0xFF, 0xD8, 0xFF, 0xDD, 0, 3, 0}).status, JpegStatus::kBadMarker);
}

TEST(Jpeg, RefusesTablesJpegDoesNotAllow) {
  auto random = Repeatable(15);
  const Bytes file = JpegFile(RandomPicture(random, 16, 16, Gray()));
  const auto huffman = [&](const Bytes& body) { return WithSegment(file, 0xC4, JpegSegment(0xC4, body)); };
  Bytes counts(17, 0);
  counts[0] = 0x20;  // A third kind of table.
  EXPECT_EQ(ReadJpeg(huffman(counts)).status, JpegStatus::kBadTable);
  counts[0] = 0x04;  // A fifth table.
  EXPECT_EQ(ReadJpeg(huffman(counts)).status, JpegStatus::kBadTable);
  counts[0] = 0x00;
  counts[1] = 3;  // Three codes of one bit.
  counts.insert(counts.end(), {0, 1, 2});
  EXPECT_EQ(ReadJpeg(huffman(counts)).status, JpegStatus::kBadTable);
  Bytes allOnes(17, 0);
  allOnes[1] = 2;  // Codes 0 and 1: the second all ones.
  allOnes.insert(allOnes.end(), {0, 1});
  EXPECT_EQ(ReadJpeg(huffman(allOnes)).status, JpegStatus::kBadTable);
  Bytes tooMany(17, 0);
  tooMany[16] = 255;
  tooMany[15] = 2;  // 257 values.
  EXPECT_EQ(ReadJpeg(huffman(tooMany)).status, JpegStatus::kBadTable);
  const Bytes short1(10, 0);
  EXPECT_EQ(ReadJpeg(huffman(short1)).status, JpegStatus::kBadTable);
  const auto steps = [&](const Bytes& body) { return WithSegment(file, 0xDB, JpegSegment(0xDB, body)); };
  Bytes good(65, 1);
  good[0] = 0x00;
  EXPECT_EQ(ReadJpeg(steps(good)).status, JpegStatus::kOk);
  Bytes wrongPrecision(65, 1);
  wrongPrecision[0] = 0x20;
  EXPECT_EQ(ReadJpeg(steps(wrongPrecision)).status, JpegStatus::kBadTable);
  Bytes fifth(65, 1);
  fifth[0] = 0x04;
  EXPECT_EQ(ReadJpeg(steps(fifth)).status, JpegStatus::kBadTable);
  EXPECT_EQ(ReadJpeg(steps(Bytes(40, 0))).status, JpegStatus::kBadTable);
}

TEST(Jpeg, RefusesAScanJpegDoesNotAllow) {
  auto random = Repeatable(16);
  const JpegPicture picture = RandomPicture(random, 16, 16, Colour(1, 1));
  const Bytes file = JpegFile(picture);
  const auto scan = [&](const Bytes& body) { return WithSegment(file, 0xDA, JpegSegment(0xDA, body)); };
  EXPECT_EQ(ReadJpeg(scan({3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0})).status, JpegStatus::kOk);
  EXPECT_EQ(ReadJpeg(scan({0, 0, 63, 0})).status, JpegStatus::kBadScan) << "no components";
  EXPECT_EQ(ReadJpeg(scan({5, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 0, 63, 0})).status, JpegStatus::kBadScan) << "five";
  EXPECT_EQ(ReadJpeg(scan({1, 9, 0x00, 0, 63, 0})).status, JpegStatus::kBadScan) << "no such component";
  EXPECT_EQ(ReadJpeg(scan({2, 1, 0x00, 1, 0x00, 0, 63, 0})).status, JpegStatus::kBadScan) << "one twice";
  EXPECT_EQ(ReadJpeg(scan({1, 1, 0x40, 0, 63, 0})).status, JpegStatus::kBadScan) << "a fifth table";
  EXPECT_EQ(ReadJpeg(scan({1, 1, 0x22, 0, 63, 0})).status, JpegStatus::kBadScan) << "a table not given";
  EXPECT_EQ(ReadJpeg(scan({1, 1, 0x00, 0, 63})).status, JpegStatus::kBadScan) << "too short";
  // More blocks in a unit than ten.
  const JpegPicture many = RandomPicture(random, 16, 16, {{1, 4, 2, 0}, {2, 2, 1, 1}, {3, 1, 1, 1}});
  EXPECT_EQ(ReadJpeg(JpegFile(many)).status, JpegStatus::kBadScan);
  // A progressive scan of averages with other frequencies in it.
  JpegOptions progressive;
  progressive.frame = 0xC2;
  const Bytes first = JpegFile(picture, progressive);
  EXPECT_EQ(ReadJpeg(WithSegment(first, 0xDA, JpegSegment(0xDA, {3, 1, 0, 2, 0, 3, 0, 0, 5, 0}))).status,
            JpegStatus::kBadScan);
  // Without a table of quantization for the gray.
  EXPECT_EQ(ReadJpeg(WithSegment(file, 0xDB, Bytes{})).status, JpegStatus::kBadScan);
}

TEST(Jpeg, RefusesCodedDataItsTablesDoNotAllow) {
  // A gray picture whose only scan's data begins with sixteen 1 bits: no
  // code of the standard table for averages.
  auto random = Repeatable(17);
  const Bytes file = JpegFile(RandomPicture(random, 8, 8, Gray()));
  // The scan's header: its marker, length, one component and three bytes.
  const std::size_t start = SegmentAt(file, 0xDA) + 2 + 2 + 1 + 2 + 3;
  Bytes spoiled(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(start));
  spoiled.insert(spoiled.end(), {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xD9});
  const Reading reading = ReadJpeg(spoiled);
  EXPECT_EQ(reading.status, JpegStatus::kBadData);
  EXPECT_EQ(reading.rows, 0U);
}

TEST(Jpeg, SaysWhereThePictureEndsTooSoon) {
  auto random = Repeatable(18);
  const JpegPicture picture = RandomPicture(random, 24, 40, Colour(2, 2));
  JpegOptions options;
  options.restartInterval = 1;
  const Bytes file = JpegFile(picture, options);
  const Reading whole = ReadJpeg(file);
  ASSERT_EQ(whole.status, JpegStatus::kOk);
  for (std::size_t length = 0; length < file.size(); ++length) {
    const Reading reading = ReadJpeg(std::span(file).first(length));
    if (length < 2) {
      EXPECT_EQ(reading.status, JpegStatus::kNotJpeg);
    } else if (reading.status != JpegStatus::kOk) {
      EXPECT_TRUE(reading.status == JpegStatus::kTruncated || reading.status == JpegStatus::kBadHeader) << length;
    }
    // The rows it gave are the whole picture's, as far as they go.
    EXPECT_LE(reading.rows, whole.rows);
    EXPECT_TRUE(std::equal(reading.pixels.begin(), reading.pixels.end(), whole.pixels.begin())) << length;
  }
  // Without its end marker it is whole.
  EXPECT_EQ(ReadJpeg(std::span(file).first(file.size() - 2)).status, JpegStatus::kOk);
}

TEST(Jpeg, GivesTheSameRowsHoweverTheFileIsCutUp) {
  auto random = Repeatable(19);
  const JpegPicture picture = RandomPicture(random, 33, 17, Colour(2, 1));
  JpegOptions options;
  options.restartInterval = 3;
  options.before = {JpegSegment(0xE1, Bytes(2500, 0xFF))};
  const Bytes file = JpegFile(picture, options);
  const Reading whole = ReadJpeg(file);
  ASSERT_EQ(whole.status, JpegStatus::kOk);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{511}, std::size_t{513}}) {
    const Reading reading = ReadJpeg(file, JpegReduction::kNone, piece);
    EXPECT_EQ(reading.status, JpegStatus::kOk);
    EXPECT_EQ(reading.pixels, whole.pixels) << piece;
  }
}

// How many steps reading `file` takes, to its end.
std::size_t StepsFor(const Bytes& file) {
  Bytes rows(65536);
  JpegReader reader(rows);
  bn::core::MemoryByteSource source(file);
  Rows sink;
  reader.Begin(source, &sink);
  std::size_t steps = 1;
  for (; reader.Step(); ++steps) {
    sink.width = reader.Width();
  }
  EXPECT_EQ(reader.Status(), JpegStatus::kOk);
  return steps;
}

TEST(Jpeg, DecodesNoMoreUnitsOfBlocksAStepThanItSays) {
  // Two gray pictures a row of blocks high, the same but for their width:
  // 17 blocks across and 1. The wider takes as many more steps as it takes
  // to decode 16 more blocks at kMcusPerStep a step.
  auto random = Repeatable(24);
  const Bytes wide = JpegFile(RandomPicture(random, 17 * 8, 8, Gray()));
  const Bytes narrow = JpegFile(RandomPicture(random, 8, 8, Gray()));
  const std::size_t more = ((17 + JpegReader::kMcusPerStep - 1) / JpegReader::kMcusPerStep) - 1;
  EXPECT_EQ(StepsFor(wide) - StepsFor(narrow), more);
}

TEST(Jpeg, HoldsAStepToItsBoundWhateverTheData) {
  // The longest codes and the most bits there are, in units of ten blocks:
  // ReadJpeg checks each step against MostInputPerStep().
  JpegPicture picture;
  picture.width = 64;
  picture.height = 16;
  picture.components = {{1, 4, 2, 0}, {2, 1, 1, 1}, {3, 1, 1, 1}};
  picture.Make();
  for (auto& blocks : picture.blocks) {
    for (auto& block : blocks) {
      for (std::size_t i = 0; i < 64; ++i) {
        block[i] = static_cast<std::int16_t>(i % 2 == 0 ? 1023 : -1023);
      }
    }
  }
  const Reading reading = ReadJpeg(JpegFile(picture));
  EXPECT_EQ(reading.status, JpegStatus::kOk);
}

TEST(Jpeg, KeepsItsSumsInRangeWhateverTheCoefficients) {
  JpegPicture picture;
  picture.width = 16;
  picture.height = 8;
  picture.Make(65535);
  for (auto& block : picture.blocks[0]) {
    for (std::size_t i = 0; i < 64; ++i) {
      block[i] = static_cast<std::int16_t>(i == 0 ? 2047 : (i % 3 == 0 ? -1023 : 1023));
    }
  }
  JpegOptions options;
  options.frame = 0xC1;
  options.sixteenBitSteps = true;
  for (const JpegReduction reduction : kEveryReduction) {
    const Reading reading = ReadJpeg(JpegFile(picture, options), reduction);
    EXPECT_EQ(reading.status, JpegStatus::kOk);
  }
}

TEST(Jpeg, SaysWhenItHasNoRoomForThePicture) {
  auto random = Repeatable(20);
  const JpegPicture picture = RandomPicture(random, 40, 20, Colour(2, 2));
  const Bytes file = JpegFile(picture);
  for (const JpegReduction reduction : kEveryReduction) {
    JpegReader peek({});
    bn::core::MemoryByteSource source(file);
    peek.Read(source, nullptr);
    const auto room = static_cast<std::size_t>(JpegReader::RowBytesFor(peek.Header(), reduction));
    // Three units across of 16 pixels, and two rows of blocks: at the whole
    // size 48 by 16.
    EXPECT_EQ(room, std::size_t{48} * 16 / (static_cast<std::size_t>(reduction) * static_cast<std::size_t>(reduction)));
    EXPECT_EQ(ReadJpeg(file, reduction, 4096, room).status, JpegStatus::kOk);
    const Reading reading = ReadJpeg(file, reduction, 4096, room - 1);
    EXPECT_EQ(reading.status, JpegStatus::kNoRoom);
    EXPECT_TRUE(reading.headerKnown);
  }
}

TEST(Jpeg, SaysWhenItsSourceFails) {
  // Wherever it fails: in a segment, between them, in the coded data, and at
  // a restart marker.
  auto random = Repeatable(21);
  JpegOptions options;
  options.restartInterval = 1;
  options.before = {JpegSegment(0xFE, {'a', 'b'}), {0x12, 0x34}};
  const Bytes file = JpegFile(RandomPicture(random, 16, 16, Colour(1, 1)), options);
  // A source that fails after what the reading takes is never asked again.
  const std::size_t taken = ReadJpeg(file, JpegReduction::kNone, 1).taken;
  for (std::size_t at = 0; at <= file.size(); ++at) {
    const Reading reading = ReadJpeg(file, JpegReduction::kNone, 1, SIZE_MAX, at);
    EXPECT_EQ(reading.status, at < taken ? JpegStatus::kSourceFailed : JpegStatus::kOk) << at;
  }
}

TEST(Jpeg, ReadsOnePictureAfterAnother) {
  auto random = Repeatable(22);
  const JpegPicture first = RandomPicture(random, 20, 12, Colour(2, 2));
  const JpegPicture second = RandomPicture(random, 9, 30, Gray());
  const Bytes firstFile = JpegFile(first);
  const Bytes secondFile = JpegFile(second);
  Bytes rows(4096);
  JpegReader reader(rows);
  for (const auto* file : {&firstFile, &secondFile, &firstFile}) {
    bn::core::MemoryByteSource source(*file);
    Rows sink;
    sink.width = file == &secondFile ? 9 : 20;
    EXPECT_EQ(reader.Read(source, &sink), JpegStatus::kOk);
    EXPECT_EQ(sink.count, file == &secondFile ? 30U : 12U);
  }
  // A picture's tables are its own: a second without them is refused.
  const Bytes withoutTables =
      WithSegment(WithSegment(WithSegment(WithSegment(secondFile, 0xC4, {}), 0xC4, {}), 0xC4, {}), 0xC4, {});
  bn::core::MemoryByteSource source(withoutTables);
  Rows sink;
  sink.width = 9;
  EXPECT_EQ(reader.Read(source, &sink), JpegStatus::kBadScan);
}

TEST(Jpeg, StopsAtItsLastRow) {
  auto random = Repeatable(23);
  const JpegPicture picture = RandomPicture(random, 16, 16, Gray());
  JpegOptions options;
  options.end = false;
  Bytes file = JpegFile(picture, options);
  const std::size_t length = file.size();
  // What comes after the last row is not read.
  file.insert(file.end(), 10000, 0x55);
  const Reading reading = ReadJpeg(file, JpegReduction::kNone, 1);
  EXPECT_EQ(reading.status, JpegStatus::kOk);
  EXPECT_LE(reading.taken, length + 4);
}

TEST(Jpeg, ChoosesTheMostItCanReadSmallerAndStillBeAsLargeAsShown) {
  JpegHeader header;
  header.width = 1600;
  header.height = 2400;
  header.components = 3;
  header.grayAcross = 2;
  header.grayDown = 2;
  header.colourAcross = 1;
  header.colourDown = 1;
  EXPECT_EQ(JpegReader::ReductionFor(header, 480, 720), JpegReduction::kHalf);
  EXPECT_EQ(JpegReader::ReductionFor(header, 400, 600), JpegReduction::kQuarter);
  EXPECT_EQ(JpegReader::ReductionFor(header, 200, 300), JpegReduction::kEighth);
  EXPECT_EQ(JpegReader::ReductionFor(header, 199, 300), JpegReduction::kEighth);
  EXPECT_EQ(JpegReader::ReductionFor(header, 201, 300), JpegReduction::kQuarter);
  EXPECT_EQ(JpegReader::ReductionFor(header, 1601, 10), JpegReduction::kNone);
  EXPECT_EQ(JpegReader::WidthAt(header, JpegReduction::kEighth), 200U);
  header.width = 1601;
  EXPECT_EQ(JpegReader::WidthAt(header, JpegReduction::kEighth), 201U);
  header.progressive = true;
  EXPECT_EQ(JpegReader::ReductionFor(header, 1600, 2400), JpegReduction::kEighth);
  EXPECT_EQ(JpegReader::ReductionTaken(header, JpegReduction::kNone), JpegReduction::kEighth);
  EXPECT_EQ(JpegReader::HeightAt(header, JpegReduction::kNone), 300U);
}

TEST(Jpeg, NeedsNoRoomForAHeaderItDoesNotRead) {
  JpegHeader header;
  EXPECT_EQ(JpegReader::RowBytesFor(header, JpegReduction::kNone), 0U);
  header.width = 16;
  header.height = 16;
  header.components = 4;
  header.grayAcross = 1;
  header.grayDown = 1;
  EXPECT_EQ(JpegReader::RowBytesFor(header, JpegReduction::kNone), 0U);
  header.components = 1;
  EXPECT_EQ(JpegReader::RowBytesFor(header, JpegReduction::kNone), 128U);
}

}  // namespace
