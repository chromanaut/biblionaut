#include "draw/scale.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "core/byte_source.h"
#include "draw/png.h"
#include "draw/row_sink.h"
#include "png_builder.h"

// What the scaler makes is set beside a plain working-out of the same
// picture in floating point: each new pixel the average of the old ones it
// covers, by how much of each it covers, or a blend of the two old ones
// nearest its middle, each direction by itself. The scaler works in whole
// numbers, a 256th of a gray at a time, so it may be one gray from the plain
// working-out, and no more.

namespace {

using bn::draw::RowSink;
using bn::draw::Scaler;
using bn::draw::ScaleStatus;

using Bytes = std::vector<std::uint8_t>;

std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// A picture of gray, row after row.
struct Picture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  Bytes gray;

  std::uint8_t At(std::uint32_t x, std::uint32_t y) const { return gray[(std::size_t{y} * width) + x]; }
};

Picture RandomPicture(std::mt19937& random, std::uint32_t width, std::uint32_t height) {
  Picture picture{width, height, Bytes(std::size_t{width} * height)};
  std::uniform_int_distribution<int> byte(0, 255);
  for (auto& value : picture.gray) {
    value = static_cast<std::uint8_t>(byte(random));
  }
  return picture;
}

// Keeps the rows it is given, and checks they come in order and whole.
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

// `picture` scaled to `width` by `height`: its rows given one at a time,
// and the new rows taken out after each, as a reader would.
Picture Scale(const Picture& picture, std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint32_t> sums(Scaler::SumsFor(width), 0xA5A5A5A5U);
  Bytes row(width, 0xA5);
  Scaler scaler(sums, row);
  Rows rows;
  rows.width = width;
  EXPECT_EQ(scaler.Begin(picture.width, picture.height, width, height, rows), ScaleStatus::kOk);
  for (std::uint32_t y = 0; y < picture.height; ++y) {
    const std::uint32_t before = rows.count;
    scaler.Row(y, std::span(picture.gray).subspan(std::size_t{y} * picture.width, picture.width));
    EXPECT_EQ(rows.count, before) << "a row was given out while one was being taken";
    while (true) {
      const std::uint32_t given = rows.count;
      if (!scaler.Step()) {
        EXPECT_EQ(rows.count, given);
        break;
      }
      EXPECT_EQ(rows.count, given + 1) << "a step gave out other than one row";
    }
  }
  EXPECT_EQ(scaler.Status(), ScaleStatus::kOk);
  EXPECT_TRUE(scaler.Finished());
  EXPECT_EQ(rows.count, height);
  return Picture{width, height, rows.pixels};
}

// One direction of the plain working-out: `old` values made `size` long.
std::vector<double> PlainLine(const std::vector<double>& old, std::uint32_t size) {
  const auto count = static_cast<std::uint32_t>(old.size());
  std::vector<double> out(size);
  for (std::uint32_t at = 0; at < size; ++at) {
    if (size == count) {
      out[at] = old[at];
    } else if (size < count) {
      // The new one covers [at * count / size, (at + 1) * count / size) of the old.
      const double start = static_cast<double>(at) * count / size;
      const double end = static_cast<double>(at + 1) * count / size;
      double sum = 0;
      for (auto i = static_cast<std::uint32_t>(std::floor(start)); i < count && i < end; ++i) {
        sum += old[i] * (std::min<double>(i + 1, end) - std::max<double>(i, start));
      }
      out[at] = sum / (end - start);
    } else {
      const double middle = ((at + 0.5) * count / size) - 0.5;
      if (middle <= 0) {
        out[at] = old[0];
      } else if (middle >= count - 1) {
        out[at] = old[count - 1];
      } else {
        const auto before = static_cast<std::uint32_t>(std::floor(middle));
        const double weight = middle - before;
        out[at] = (old[before] * (1 - weight)) + (old[before + 1] * weight);
      }
    }
  }
  return out;
}

std::vector<double> PlainScale(const Picture& picture, std::uint32_t width, std::uint32_t height) {
  // Across first, then down.
  std::vector<std::vector<double>> across;
  for (std::uint32_t y = 0; y < picture.height; ++y) {
    std::vector<double> row(picture.width);
    for (std::uint32_t x = 0; x < picture.width; ++x) {
      row[x] = picture.At(x, y);
    }
    across.push_back(PlainLine(row, width));
  }
  std::vector<double> out(std::size_t{width} * height);
  for (std::uint32_t x = 0; x < width; ++x) {
    std::vector<double> column(picture.height);
    for (std::uint32_t y = 0; y < picture.height; ++y) {
      column[y] = across[y][x];
    }
    const std::vector<double> down = PlainLine(column, height);
    for (std::uint32_t y = 0; y < height; ++y) {
      out[(std::size_t{y} * width) + x] = down[y];
    }
  }
  return out;
}

// The most any pixel is from the plain working-out, in grays.
double FurthestFromPlain(const Picture& scaled, const std::vector<double>& plain) {
  double furthest = 0;
  for (std::size_t at = 0; at < plain.size(); ++at) {
    furthest = std::max(furthest, std::abs(scaled.gray[at] - plain[at]));
  }
  return furthest;
}

TEST(Scaler, CopiesAPictureOfTheSameSizeAsItIs) {
  std::mt19937 random = Repeatable(1);
  const Picture picture = RandomPicture(random, 37, 23);
  EXPECT_EQ(Scale(picture, 37, 23).gray, picture.gray);
}

TEST(Scaler, KeepsAPictureOfOneGrayThatGrayAtAnySize) {
  for (const std::uint8_t gray :
       {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{128}, std::uint8_t{254}, std::uint8_t{255}}) {
    const Picture picture{13, 7, Bytes(91, gray)};
    for (const auto& [width, height] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {1, 1}, {5, 3}, {13, 7}, {14, 8}, {100, 2}, {2, 100}, {480, 800}}) {
      const Picture scaled = Scale(picture, width, height);
      EXPECT_TRUE(std::all_of(scaled.gray.begin(), scaled.gray.end(), [&](std::uint8_t v) { return v == gray; }))
          << int{gray} << " at " << width << "x" << height;
    }
  }
}

TEST(Scaler, MakesAPixelSmallerTheAverageOfWhatItCovers) {
  // Halved: each new pixel is the average of a square of four.
  const Picture square{4, 2, {0, 100, 200, 255, 50, 150, 40, 60}};
  EXPECT_EQ(Scale(square, 2, 1).gray, (Bytes{75, 139}));  // 300 / 4 and 555 / 4, rounded.
  // Three into two: the first new pixel covers the first old one and half
  // of the second.
  const Picture three{3, 1, {0, 255, 30}};
  EXPECT_EQ(Scale(three, 2, 1).gray, (Bytes{85, 105}));  // 127.5 / 1.5 and 157.5 / 1.5.
}

TEST(Scaler, KeepsALineOnePixelWideWhenMadeSmaller) {
  // A black line down a white picture, a tenth as wide: still there, and
  // the only thing that is not white.
  Picture picture{100, 20, Bytes(2000, 255)};
  for (std::uint32_t y = 0; y < 20; ++y) {
    picture.gray[(std::size_t{y} * 100) + 57] = 0;
  }
  const Picture scaled = Scale(picture, 10, 2);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 10; ++x) {
      EXPECT_EQ(scaled.At(x, y), x == 5 ? 230 : 255) << x << " " << y;  // 255 * 9 / 10, rounded.
    }
  }
}

TEST(Scaler, MakesAPixelLargerABlendOfTheTwoNearest) {
  // Two made four: the new middles lie at -1/4, 1/4, 3/4 and 5/4 of the old.
  const Picture two{2, 1, {0, 255}};
  EXPECT_EQ(Scale(two, 4, 1).gray, (Bytes{0, 64, 191, 255}));
  // The same down a column.
  const Picture column{1, 2, {0, 255}};
  EXPECT_EQ(Scale(column, 1, 4).gray, (Bytes{0, 64, 191, 255}));
}

TEST(Scaler, ComesWithinAGrayOfThePlainWorkingOut) {
  std::mt19937 random = Repeatable(2);
  std::uniform_int_distribution<std::uint32_t> side(1, 60);
  double furthest = 0;
  for (int trial = 0; trial < 300; ++trial) {
    const Picture picture = RandomPicture(random, side(random), side(random));
    const std::uint32_t width = side(random);
    const std::uint32_t height = side(random);
    const Picture scaled = Scale(picture, width, height);
    const double distance = FurthestFromPlain(scaled, PlainScale(picture, width, height));
    EXPECT_LE(distance, 1.0) << picture.width << "x" << picture.height << " to " << width << "x" << height;
    furthest = std::max(furthest, distance);
  }
  // And not by luck: the rounding is seen to come near the half gray.
  EXPECT_GT(furthest, 0.5);
}

// One direction of the working-out in exact fractions: for each new pixel,
// the old ones it draws on and how much of each, over one denominator.
struct ExactLine {
  std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>> parts;
  std::uint64_t denominator = 1;
};

ExactLine Exact(std::uint32_t count, std::uint32_t size) {
  ExactLine line;
  line.parts.resize(size);
  if (size == count) {
    for (std::uint32_t at = 0; at < size; ++at) {
      line.parts[at] = {{at, 1}};
    }
  } else if (size < count) {
    // A new pixel covers [at * count, (at + 1) * count) and an old one
    // [i * size, (i + 1) * size).
    line.denominator = count;
    for (std::uint32_t at = 0; at < size; ++at) {
      const std::uint64_t start = std::uint64_t{at} * count;
      const std::uint64_t end = start + count;
      for (std::uint64_t i = start / size; i < count && i * size < end; ++i) {
        const std::uint64_t overlap = std::min(end, (i + 1) * size) - std::max(start, i * size);
        line.parts[at].emplace_back(static_cast<std::uint32_t>(i), overlap);
      }
    }
  } else {
    // The middle at ((2 at + 1) count - size) / (2 size).
    line.denominator = 2ULL * size;
    for (std::uint32_t at = 0; at < size; ++at) {
      const std::int64_t middle = ((2LL * at + 1) * count) - size;
      if (middle <= 0) {
        line.parts[at] = {{0, line.denominator}};
      } else if (middle >= static_cast<std::int64_t>((count - 1) * line.denominator)) {
        line.parts[at] = {{count - 1, line.denominator}};
      } else {
        const auto before = static_cast<std::uint32_t>(middle / static_cast<std::int64_t>(line.denominator));
        const auto rest = static_cast<std::uint64_t>(middle) % line.denominator;
        line.parts[at] = {{before, line.denominator - rest}, {before + 1, rest}};
      }
    }
  }
  return line;
}

// The whole numbers the scaler's header says it keeps, worked out from the
// exact fractions: a pixel made the new width rounded to 256ths of a gray,
// a blend's weight rounded to 4096ths of the way, each a half up.
std::uint64_t RoundedHalfUp(std::uint64_t numerator, std::uint64_t denominator) {
  return ((2 * numerator) + denominator) / (2 * denominator);
}

// What one direction makes of `values` (in `unit`ths of a gray) at new
// pixel `at`, in `unit`ths of a gray times `scale`, before it is rounded.
struct Made {
  std::uint64_t numerator;
  std::uint64_t denominator;
};

Made MadeAt(const ExactLine& line, std::uint32_t count, std::uint32_t size, std::uint32_t at,
            const std::vector<std::uint64_t>& values) {
  const auto& parts = line.parts[at];
  if (size == count || parts.size() == 1) {
    return {values[parts[0].first], 1};
  }
  if (size < count) {
    std::uint64_t sum = 0;
    for (const auto& [i, overlap] : parts) {
      sum += values[i] * overlap;
    }
    return {sum, line.denominator};
  }
  // A blend: its weight is rounded to 4096ths first.
  const std::uint64_t weight = RoundedHalfUp(parts[1].second * 4096, line.denominator);
  return {(values[parts[0].first] * (4096 - weight)) + (values[parts[1].first] * weight), 4096};
}

TEST(Scaler, KeepsToTheWholeNumbersItsHeaderGives) {
  // Every new pixel is what the stated roundings come to, worked out
  // another way: from exact fractions, rounded where the header says.
  std::mt19937 random = Repeatable(5);
  std::uniform_int_distribution<std::uint32_t> side(1, 90);
  for (int trial = 0; trial < 400; ++trial) {
    const Picture picture = RandomPicture(random, side(random), side(random));
    const std::uint32_t width = side(random);
    const std::uint32_t height = side(random);
    const Picture scaled = Scale(picture, width, height);
    const ExactLine across = Exact(picture.width, width);
    const ExactLine downward = Exact(picture.height, height);
    // Across: each old row made the new width, in 256ths of a gray.
    std::vector<std::vector<std::uint64_t>> rows;
    for (std::uint32_t y = 0; y < picture.height; ++y) {
      std::vector<std::uint64_t> old(picture.width);
      for (std::uint32_t x = 0; x < picture.width; ++x) {
        old[x] = picture.At(x, y);
      }
      std::vector<std::uint64_t> row(width);
      for (std::uint32_t x = 0; x < width; ++x) {
        const Made made = MadeAt(across, picture.width, width, x, old);
        row[x] = RoundedHalfUp(made.numerator * 256, made.denominator);
      }
      rows.push_back(row);
    }
    // Down: each column made the new height, rounded to a gray.
    for (std::uint32_t x = 0; x < width; ++x) {
      std::vector<std::uint64_t> column(picture.height);
      for (std::uint32_t y = 0; y < picture.height; ++y) {
        column[y] = rows[y][x];
      }
      for (std::uint32_t y = 0; y < height; ++y) {
        const Made made = MadeAt(downward, picture.height, height, y, column);
        const std::uint64_t gray = RoundedHalfUp(made.numerator, made.denominator * 256);
        ASSERT_EQ(scaled.At(x, y), gray) << picture.width << "x" << picture.height << " to " << width << "x" << height
                                         << " at " << x << "," << y;
      }
    }
  }
}

TEST(Scaler, ScalesAPageSizedPictureWithinAGray) {
  // The sizes a book's pictures meet: a wide one onto the panel, and a
  // small one made larger, at odd ratios.
  std::mt19937 random = Repeatable(3);
  for (const auto& [from, to] :
       std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>>>{
           {{1400, 440}, {480, 151}}, {{85, 75}, {197, 174}}, {{600, 900}, {480, 720}}, {{333, 17}, {101, 61}}}) {
    const Picture picture = RandomPicture(random, from.first, from.second);
    const Picture scaled = Scale(picture, to.first, to.second);
    EXPECT_LE(FurthestFromPlain(scaled, PlainScale(picture, to.first, to.second)), 1.0)
        << from.first << "x" << from.second;
  }
}

TEST(Scaler, KeepsItsSumsWithinThirtyTwoBitsAtTheLargestSides) {
  // The widest old row made one pixel, all white: the sum is as large as
  // it gets.
  const Picture wide{Scaler::kMaxSide, 1, Bytes(Scaler::kMaxSide, 255)};
  EXPECT_EQ(Scale(wide, 1, 1).gray, (Bytes{255}));
  // The tallest made one row.
  const Picture tall{1, Scaler::kMaxSide, Bytes(Scaler::kMaxSide, 255)};
  EXPECT_EQ(Scale(tall, 1, 1).gray, (Bytes{255}));
  // And the other way: one pixel made the largest.
  const Picture one{1, 1, {200}};
  const Picture large = Scale(one, Scaler::kMaxSide, 2);
  EXPECT_TRUE(std::all_of(large.gray.begin(), large.gray.end(), [](std::uint8_t v) { return v == 200; }));
}

TEST(Scaler, RefusesASideOfNothingOrTooLong) {
  std::vector<std::uint32_t> sums(Scaler::SumsFor(70000));
  Bytes row(70000);
  Scaler scaler(sums, row);
  Rows rows;
  EXPECT_EQ(scaler.Begin(0, 1, 1, 1, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(1, 0, 1, 1, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(1, 1, 0, 1, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(1, 1, 1, 0, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(Scaler::kMaxSide + 1, 1, 1, 1, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(1, 1, Scaler::kMaxSide + 1, 1, rows), ScaleStatus::kBadSize);
  EXPECT_EQ(scaler.Begin(1, 1, 1, 1, rows), ScaleStatus::kOk);
  // Refused, it takes nothing and gives nothing.
  EXPECT_EQ(scaler.Begin(5, 5, 0, 5, rows), ScaleStatus::kBadSize);
  const std::array<std::uint8_t, 5> gray{};
  scaler.Row(0, gray);
  EXPECT_FALSE(scaler.Step());
  EXPECT_EQ(rows.count, 0U);
}

TEST(Scaler, SaysWhenItHasTooLittleMemoryForTheNewWidth) {
  std::vector<std::uint32_t> sums(Scaler::SumsFor(100) - 1);
  Bytes row(100);
  Scaler scaler(sums, row);
  Rows rows;
  EXPECT_EQ(scaler.Begin(10, 10, 100, 10, rows), ScaleStatus::kNoRoom);
  std::vector<std::uint32_t> enough(Scaler::SumsFor(100));
  Bytes shortRow(99);
  Scaler other(enough, shortRow);
  EXPECT_EQ(other.Begin(10, 10, 100, 10, rows), ScaleStatus::kNoRoom);
  EXPECT_EQ(other.Begin(10, 10, 99, 10, rows), ScaleStatus::kOk);
}

TEST(Scaler, RefusesARowOutOfTurn) {
  const auto refused = [](auto&& give) {
    std::vector<std::uint32_t> sums(Scaler::SumsFor(8));
    Bytes row(8);
    Scaler scaler(sums, row);
    Rows rows;
    rows.width = 8;
    EXPECT_EQ(scaler.Begin(4, 3, 8, 6, rows), ScaleStatus::kOk);
    give(scaler);
    EXPECT_EQ(scaler.Status(), ScaleStatus::kOutOfTurn);
    // And takes nothing more.
    const std::array<std::uint8_t, 4> gray{};
    scaler.Row(scaler.RowsTaken(), gray);
    EXPECT_FALSE(scaler.Step());
    EXPECT_FALSE(scaler.Finished());
  };
  const std::array<std::uint8_t, 4> gray{};
  const std::array<std::uint8_t, 5> wide{};
  refused([&](Scaler& scaler) { scaler.Row(1, gray); });  // Not the next.
  refused([&](Scaler& scaler) { scaler.Row(0, wide); });  // Not as wide.
  refused([&](Scaler& scaler) {                           // New rows still waiting to be given out.
    scaler.Row(0, gray);
    scaler.Row(1, gray);
  });
  refused([&](Scaler& scaler) {  // Past the last.
    for (std::uint32_t y = 0; y < 4; ++y) {
      scaler.Row(y, gray);
      while (scaler.Step()) {
      }
    }
  });
}

TEST(Scaler, TakesARowMadeSmallerOnlyOnceItsNewRowIsGiven) {
  // Made smaller, a whole new row waits too; the next old row would add
  // into the row after it.
  std::vector<std::uint32_t> sums(Scaler::SumsFor(2));
  Bytes row(2);
  Scaler scaler(sums, row);
  Rows rows;
  rows.width = 2;
  ASSERT_EQ(scaler.Begin(4, 4, 2, 2, rows), ScaleStatus::kOk);
  const std::array<std::uint8_t, 4> gray{};
  scaler.Row(0, gray);
  EXPECT_FALSE(scaler.Ready());
  scaler.Row(1, gray);
  EXPECT_TRUE(scaler.Ready());
  scaler.Row(2, gray);
  EXPECT_EQ(scaler.Status(), ScaleStatus::kOutOfTurn);
}

TEST(Scaler, ScalesTheRowsAPngReaderGives) {
  // A reader of pictures gives its rows straight to the scaler.
  std::mt19937 random = Repeatable(4);
  bn::test::PngPicture png;
  png.width = 120;
  png.height = 40;
  png.depth = 8;
  png.colour = 0;
  png.rows.resize(std::size_t{120} * 40);
  std::uniform_int_distribution<int> byte(0, 255);
  for (auto& value : png.rows) {
    value = static_cast<std::uint8_t>(byte(random));
  }
  const std::array<std::uint8_t, 1> none = {0};
  const Bytes file = bn::test::PngFor(png, none);
  std::vector<std::uint8_t> window(bn::draw::PngReader::kWindowBytes);
  Bytes pngRows(bn::draw::PngReader::RowBytesFor({120, 40, 8, bn::draw::PngColour::kGray, false}));
  bn::draw::PngReader reader(window, pngRows);
  std::vector<std::uint32_t> sums(Scaler::SumsFor(48));
  Bytes row(48);
  Scaler scaler(sums, row);
  Rows rows;
  rows.width = 48;
  ASSERT_EQ(scaler.Begin(120, 40, 48, 16, rows), ScaleStatus::kOk);
  bn::core::MemoryByteSource source(file);
  reader.Begin(source, &scaler);
  // The new rows a reader's step made ready are taken out after it, the
  // last step's too.
  bool more = true;
  while (more) {
    more = reader.Step();
    while (scaler.Step()) {
    }
  }
  EXPECT_EQ(reader.Status(), bn::draw::PngStatus::kOk);
  EXPECT_TRUE(scaler.Finished());
  const Picture picture{120, 40, png.rows};
  EXPECT_EQ(rows.pixels, Scale(picture, 48, 16).gray);
}

}  // namespace
