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
