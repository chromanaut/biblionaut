#include "draw/dither.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace {

using bn::draw::Ditherer;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// `picture`, `width` wide, in `levels` levels: given row by row.
std::vector<std::uint8_t> Dithered(std::vector<std::uint8_t> picture, std::size_t width, std::uint32_t levels) {
  std::vector<std::int16_t> errors(Ditherer::ErrorsFor(width));
  Ditherer ditherer;
  EXPECT_TRUE(ditherer.Begin(width, levels, errors));
  for (std::size_t at = 0; at < picture.size(); at += width) {
    EXPECT_TRUE(ditherer.Row(std::span(picture).subspan(at, width)));
  }
  return picture;
}

double Mean(const std::vector<std::uint8_t>& picture) {
  double sum = 0;
  for (const std::uint8_t pixel : picture) {
    sum += pixel;
  }
  return sum / static_cast<double>(picture.size());
}

TEST(Dither, EveryPixelBecomesOneOfTheLevels) {
  std::mt19937 random = Repeatable(3);
  std::vector<std::uint8_t> picture(3072);  // 64 by 48.
  for (std::uint8_t& pixel : picture) {
    pixel = static_cast<std::uint8_t>(random());
  }
  const std::vector<std::uint8_t> two = Dithered(picture, 64, 2);
  EXPECT_EQ(std::set<std::uint8_t>(two.begin(), two.end()), (std::set<std::uint8_t>{0, 255}));
  const std::vector<std::uint8_t> four = Dithered(picture, 64, 4);
  EXPECT_EQ(std::set<std::uint8_t>(four.begin(), four.end()), (std::set<std::uint8_t>{0, 85, 170, 255}));
}

TEST(Dither, APatchComesOutAsLightAsItsGraysWere) {
  for (const std::uint32_t levels : {2U, 4U, 16U}) {
    for (const std::uint8_t gray : {std::uint8_t{20}, std::uint8_t{100}, std::uint8_t{128}, std::uint8_t{230}}) {
      const std::vector<std::uint8_t> patch = Dithered(std::vector<std::uint8_t>(6400, gray), 80, levels);
      EXPECT_NEAR(Mean(patch), gray, 2.0) << levels << " " << int{gray};
    }
  }
  // A ramp from black to white, across: each column, top to bottom, comes
  // out about as light as it was.
  std::vector<std::uint8_t> ramp(16384);  // 256 by 64.
  for (std::size_t at = 0; at < ramp.size(); ++at) {
    ramp[at] = static_cast<std::uint8_t>(at % 256);
  }
  const std::vector<std::uint8_t> dithered = Dithered(ramp, 256, 2);
  for (std::size_t from = 0; from < 256; from += 32) {
    double sum = 0;
    for (std::size_t y = 0; y < 64; ++y) {
      for (std::size_t x = from; x < from + 32; ++x) {
        sum += dithered[(y * 256) + x];
      }
    }
    EXPECT_NEAR(sum / (32.0 * 64), static_cast<double>(from) + 15.5, 6.0) << from;
  }
}

TEST(Dither, WhatIsALevelAlreadyStaysAsItIs) {
  std::vector<std::uint8_t> picture(1024, 255);  // 32 by 32.
  std::fill(picture.begin(), picture.begin() + 512, std::uint8_t{0});
  EXPECT_EQ(Dithered(picture, 32, 2), picture);
  std::vector<std::uint8_t> grays(1024);
  for (std::size_t at = 0; at < grays.size(); ++at) {
    grays[at] = std::uint8_t{85} * static_cast<std::uint8_t>(at % 4);
  }
  EXPECT_EQ(Dithered(grays, 32, 4), grays);
  // With all 256 levels nothing changes at all.
  std::mt19937 random = Repeatable(5);
  for (std::uint8_t& pixel : grays) {
    pixel = static_cast<std::uint8_t>(random());
  }
  EXPECT_EQ(Dithered(grays, 32, 256), grays);
}

TEST(Dither, WhatIsCarriedGoesOnFromOneCallToTheNext) {
  // Rows given one at a time come out as the same rows given in strips of
  // any size: what is carried down is kept from call to call.
  std::mt19937 random = Repeatable(9);
  std::vector<std::uint8_t> picture(1200);  // 40 by 30.
  for (std::uint8_t& pixel : picture) {
    pixel = static_cast<std::uint8_t>(random() % 256);
  }
  const std::vector<std::uint8_t> whole = Dithered(picture, 40, 2);
  // A ditherer begun again forgets: the first row after is as the first of a picture.
  std::vector<std::int16_t> errors(Ditherer::ErrorsFor(40));
  Ditherer ditherer;
  ASSERT_TRUE(ditherer.Begin(40, 2, errors));
  std::vector<std::uint8_t> again = picture;
  ASSERT_TRUE(ditherer.Row(std::span(again).first(40)));
  ASSERT_TRUE(ditherer.Begin(40, 2, errors));
  std::vector<std::uint8_t> fresh = picture;
  ASSERT_TRUE(ditherer.Row(std::span(fresh).first(40)));
  EXPECT_TRUE(std::equal(again.begin(), again.begin() + 40, fresh.begin()));
  EXPECT_TRUE(std::equal(whole.begin(), whole.begin() + 40, fresh.begin()));
}

// A picture in `levels` levels worked out the plain way, a pixel at a time
// with every sum written out: what the ditherer has to come to whatever it
// looks up or passes over.
std::vector<std::uint8_t> DitheredThePlainWay(std::vector<std::uint8_t> picture, std::size_t width,
                                              std::uint32_t levels) {
  const auto steps = static_cast<std::int32_t>(levels - 1);
  std::vector<std::int32_t> here(width + 2, 0);
  std::vector<std::int32_t> below(width + 2, 0);
  for (std::size_t top = 0; top < picture.size(); top += width) {
    for (std::size_t x = 0; x < width; ++x) {
      const std::int32_t wanted = (std::int32_t{picture[top + x]} * 16) + here[x + 1];
      const std::int32_t clamped = std::clamp<std::int32_t>(wanted, 0, 255 * 16);
      const std::int32_t level = ((clamped * steps) + (255 * 8)) / (255 * 16);
      const std::int32_t gray = ((level * 255) + (steps / 2)) / steps;
      picture[top + x] = static_cast<std::uint8_t>(gray);
      const std::int32_t left = std::clamp<std::int32_t>(wanted - (gray * 16), -255 * 16, 255 * 16);
      const std::int32_t right = (left * 7) / 16;
      const std::int32_t belowLeft = (left * 3) / 16;
      const std::int32_t straightBelow = (left * 5) / 16;
      // Carried in 16 bits, as the ditherer carries it.
      here[x + 2] = static_cast<std::int16_t>(here[x + 2] + right);
      below[x] = static_cast<std::int16_t>(below[x] + belowLeft);
      below[x + 1] = static_cast<std::int16_t>(below[x + 1] + straightBelow);
      below[x + 2] = static_cast<std::int16_t>(below[x + 2] + (left - right - belowLeft - straightBelow));
    }
    here = below;
    std::fill(below.begin(), below.end(), 0);
  }
  return picture;
}

TEST(Dither, ComesToWhatThePlainWayDoesForEveryKindOfPicture) {
  // The ditherer looks each level's gray up, and passes over a pixel that
  // is white or black with nothing carried to it. Neither may show:
  // pictures of noise, of mostly paper with a little ink (the pixels it
  // passes over, and those beside them that it may not), of mostly ink, of
  // nothing but the levels themselves, and of a ramp, in levels few and
  // many and at widths from one pixel up.
  std::mt19937 random = Repeatable(37);
  for (const std::uint32_t levels : {2U, 3U, 4U, 5U, 16U, 255U, 256U}) {
    for (const std::size_t width :
         {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, std::size_t{481}}) {
      const std::size_t rows = 23;
      for (int kind = 0; kind < 5; ++kind) {
        std::vector<std::uint8_t> picture(width * rows);
        for (std::size_t at = 0; at < picture.size(); ++at) {
          const auto chance = static_cast<std::uint32_t>(random());
          switch (kind) {
            case 0:  // Noise.
              picture[at] = static_cast<std::uint8_t>(chance);
              break;
            case 1:  // Paper, with a little ink and a little gray.
              picture[at] = chance % 16 == 0   ? std::uint8_t{0}
                            : chance % 16 == 1 ? static_cast<std::uint8_t>(chance >> 8U)
                                               : std::uint8_t{255};
              break;
            case 2:  // Ink, with a little paper and a little gray.
              picture[at] = chance % 16 == 0   ? std::uint8_t{255}
                            : chance % 16 == 1 ? static_cast<std::uint8_t>(chance >> 8U)
                                               : std::uint8_t{0};
              break;
            case 3:  // The levels themselves, in any order.
              picture[at] = static_cast<std::uint8_t>((((chance % levels) * 255) + ((levels - 1) / 2)) / (levels - 1));
              break;
            default:  // A ramp across.
              picture[at] = static_cast<std::uint8_t>(((at % width) * 255) / std::max<std::size_t>(width - 1, 1));
              break;
          }
        }
        ASSERT_TRUE(Dithered(picture, width, levels) == DitheredThePlainWay(picture, width, levels))
            << levels << " levels, " << width << " wide, kind " << kind;
      }
    }
  }
}

TEST(Dither, RefusesWhatItCannotDo) {
  std::vector<std::int16_t> errors(Ditherer::ErrorsFor(10));
  Ditherer ditherer;
  EXPECT_FALSE(ditherer.Begin(10, 1, errors));
  EXPECT_FALSE(ditherer.Begin(10, 257, errors));
  EXPECT_FALSE(ditherer.Begin(0, 2, errors));
  EXPECT_FALSE(ditherer.Begin(11, 2, errors));
  std::vector<std::uint8_t> row(10, 128);
  EXPECT_FALSE(ditherer.Row(row));
  ASSERT_TRUE(ditherer.Begin(10, 2, errors));
  std::vector<std::uint8_t> wrong(9, 128);
  EXPECT_FALSE(ditherer.Row(wrong));
  EXPECT_EQ(wrong, std::vector<std::uint8_t>(9, 128));
}

}  // namespace
