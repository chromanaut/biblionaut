#include "reader/gray_rows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

using bn::reader::KeptWhite;
using bn::reader::LevelOf;
using bn::reader::PackLevels;
using bn::reader::PackWhite;
using bn::reader::UnpackWhite;

// What the panel and the cache are to be given, worked out a pixel at a
// time with nothing passed over.
struct Plain {
  std::vector<std::uint8_t> white;
  bool grays = false;
};

Plain PlainWhite(const std::vector<std::uint8_t>& grays) {
  Plain plain;
  plain.white.assign((grays.size() + 7) / 8, 0);
  for (std::size_t x = 0; x < grays.size(); ++x) {
    if (grays[x] == 255) {
      plain.white[x / 8] = static_cast<std::uint8_t>(plain.white[x / 8] | (0x80U >> (x % 8)));
    } else if (grays[x] != 0) {
      plain.grays = true;
    }
  }
  return plain;
}

std::vector<std::uint8_t> PlainLevels(const std::vector<std::uint8_t>& grays, unsigned bits, unsigned top) {
  std::vector<std::uint8_t> kept(((grays.size() * bits) + 7) / 8, 0);
  for (std::size_t x = 0; x < grays.size(); ++x) {
    const std::size_t bit = x * bits;
    kept[bit / 8] = static_cast<std::uint8_t>(kept[bit / 8] | (LevelOf(grays[x], top) << (8U - bits - (bit % 8))));
  }
  return kept;
}

Plain PlainUnpacked(const std::vector<std::uint8_t>& kept, std::size_t width, unsigned bits, unsigned top) {
  Plain plain;
  plain.white.assign((width + 7) / 8, 0);
  for (std::size_t x = 0; x < width; ++x) {
    const std::size_t bit = x * bits;
    const unsigned level = (static_cast<unsigned>(kept[bit / 8]) >> (8U - bits - (bit % 8))) & ((1U << bits) - 1U);
    if (level == top) {
      plain.white[x / 8] = static_cast<std::uint8_t>(plain.white[x / 8] | (0x80U >> (x % 8)));
    } else if (level != 0) {
      plain.grays = true;
    }
  }
  return plain;
}

// The gray of level `level` of `top` + 1, as the ditherer leaves a pixel.
std::uint8_t GrayOf(unsigned level, unsigned top) { return static_cast<std::uint8_t>((level * 255U) / top); }

// Rows of the kinds a page has: all of one level; white with a little on
// it; runs of white, of black and of anything; every pixel its own.
enum class Kind : std::uint8_t { kWhite, kBlack, kSparse, kRuns, kNoise, kAnyGray };
constexpr Kind kKinds[] = {Kind::kWhite, Kind::kBlack, Kind::kSparse, Kind::kRuns, Kind::kNoise, Kind::kAnyGray};

std::vector<std::uint8_t> RowOf(Kind kind, std::size_t width, unsigned top, std::mt19937& random) {
  std::vector<std::uint8_t> row(width, 255);
  for (std::size_t x = 0; x < width;) {
    const std::uint32_t level = static_cast<std::uint32_t>(random()) % (top + 1);
    const std::size_t run = 1 + (static_cast<std::uint32_t>(random()) % 23);
    const std::uint32_t chance = static_cast<std::uint32_t>(random()) % 100;
    const std::uint32_t gray = static_cast<std::uint32_t>(random()) % 256;
    switch (kind) {
      case Kind::kWhite:
        ++x;
        break;
      case Kind::kBlack:
        row[x++] = 0;
        break;
      case Kind::kSparse:
        // One pixel in twenty is something else.
        row[x++] = chance < 5 ? GrayOf(level, top) : 255;
        break;
      case Kind::kRuns: {
        // A run of white, of black, or of pixels of one level.
        const std::size_t end = std::min(width, x + run);
        for (; x < end; ++x) {
          row[x] = chance < 40 ? 255 : chance < 80 ? 0 : GrayOf(level, top);
        }
        break;
      }
      case Kind::kNoise:
        row[x++] = GrayOf(level, top);
        break;
      case Kind::kAnyGray:
        // Not a level's gray at all, which a row the ditherer made never
        // has: what comes out is still what each pixel gives.
        row[x++] = static_cast<std::uint8_t>(gray);
        break;
    }
  }
  return row;
}

constexpr std::size_t kWidths[] = {0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 100, 479, 480, 800};

TEST(GrayRows, PacksARowInBlackAndWhiteAsLookingAtEachPixelWould) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same rows every run, so that a failure can be found again.
  std::mt19937 random(20261127);
  for (const Kind kind : kKinds) {
    for (const std::size_t width : kWidths) {
      for (unsigned round = 0; round < 8; ++round) {
        const std::vector<std::uint8_t> row = RowOf(kind, width, 3, random);
        const Plain plain = PlainWhite(row);
        std::vector<std::uint8_t> bits(plain.white.size(), 0xA5);
        const bool grays = PackWhite(row, bits);
        const std::string where =
            "kind " + std::to_string(static_cast<int>(kind)) + ", " + std::to_string(width) + " wide";
        EXPECT_EQ(bits, plain.white) << where;
        EXPECT_EQ(grays, plain.grays) << where;
      }
    }
  }
}

TEST(GrayRows, FindsOnePixelThatIsNotWhiteWhereverItIs) {
  for (const unsigned gray : {0U, 85U, 170U, 254U, 1U}) {
    for (std::size_t at = 0; at < 41; ++at) {
      std::vector<std::uint8_t> row(41, 255);
      row[at] = static_cast<std::uint8_t>(gray);
      const Plain plain = PlainWhite(row);
      std::vector<std::uint8_t> bits(plain.white.size(), 0xFF);
      EXPECT_EQ(PackWhite(row, bits), gray != 0) << gray << " at " << at;
      EXPECT_EQ(bits, plain.white) << gray << " at " << at;
      // And the same pixel on black.
      std::vector<std::uint8_t> dark(41, 0);
      dark[at] = static_cast<std::uint8_t>(gray == 0 ? 255 : gray);
      const Plain plainDark = PlainWhite(dark);
      EXPECT_EQ(PackWhite(dark, bits), gray != 0) << gray << " on black at " << at;
      EXPECT_EQ(bits, plainDark.white) << gray << " on black at " << at;
    }
  }
}

TEST(GrayRows, ClearsWhatIsAfterTheRowInBlackAndWhite) {
  const std::vector<std::uint8_t> row(19, 255);
  std::vector<std::uint8_t> bits(6, 0xFF);
  EXPECT_FALSE(PackWhite(row, bits));
  EXPECT_EQ(bits, (std::vector<std::uint8_t>{0xFF, 0xFF, 0xE0, 0x00, 0x00, 0x00}));
  // A row that is whole bytes, into more than it needs.
  const std::vector<std::uint8_t> whole(16, 255);
  bits.assign(4, 0xFF);
  EXPECT_FALSE(PackWhite(whole, bits));
  EXPECT_EQ(bits, (std::vector<std::uint8_t>{0xFF, 0xFF, 0x00, 0x00}));
}

TEST(GrayRows, PacksOnlyWhatThereIsRoomFor) {
  std::vector<std::uint8_t> row(40, 255);
  row[20] = 85;
  std::vector<std::uint8_t> bits(2, 0);
  EXPECT_FALSE(PackWhite(row, bits)) << "the gray is after what there was room for";
  EXPECT_EQ(bits, (std::vector<std::uint8_t>{0xFF, 0xFF}));
  std::vector<std::uint8_t> kept(3, 0);
  PackLevels(row, 2, 3, kept);
  EXPECT_EQ(kept, (std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF}));
  PackLevels(row, 0, 3, kept);
  EXPECT_EQ(kept, (std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF})) << "nothing can be kept at no bits a pixel";
}

struct Depth {
  unsigned bits;
  unsigned top;
};
// Every number of bits the cache keeps a pixel in, with levels that fill
// it and levels that do not.
constexpr Depth kDepths[] = {{1, 1}, {2, 3}, {2, 2}, {4, 15}, {4, 4}, {4, 7}, {8, 255}, {8, 16}};

TEST(GrayRows, PacksARowsLevelsAsLookingAtEachPixelWould) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same rows every run, so that a failure can be found again.
  std::mt19937 random(11272026);
  for (const Depth depth : kDepths) {
    for (const Kind kind : kKinds) {
      for (const std::size_t width : kWidths) {
        for (unsigned round = 0; round < 4; ++round) {
          const std::vector<std::uint8_t> row = RowOf(kind, width, depth.top, random);
          const std::vector<std::uint8_t> plain = PlainLevels(row, depth.bits, depth.top);
          std::vector<std::uint8_t> kept(plain.size(), 0x5A);
          PackLevels(row, depth.bits, depth.top, kept);
          EXPECT_EQ(kept, plain) << depth.bits << " bits, top " << depth.top << ", kind " << static_cast<int>(kind)
                                 << ", " << width << " wide";
        }
      }
    }
  }
}

TEST(GrayRows, ClearsWhatIsAfterTheRowsLevels) {
  const std::vector<std::uint8_t> row(5, 255);
  std::vector<std::uint8_t> kept(4, 0xFF);
  PackLevels(row, 2, 3, kept);
  EXPECT_EQ(kept, (std::vector<std::uint8_t>{0xFF, 0xC0, 0x00, 0x00}));
}

TEST(GrayRows, SaysWhatAByteOfWhiteIsAsItIsKept) {
  EXPECT_EQ(KeptWhite(1, 1), 0xFF);
  EXPECT_EQ(KeptWhite(2, 3), 0xFF);
  EXPECT_EQ(KeptWhite(2, 2), 0xAA);
  EXPECT_EQ(KeptWhite(4, 15), 0xFF);
  EXPECT_EQ(KeptWhite(4, 4), 0x44);
  EXPECT_EQ(KeptWhite(8, 255), 0xFF);
  EXPECT_EQ(KeptWhite(8, 16), 0x10);
  EXPECT_EQ(KeptWhite(0, 3), 0x00);
}

TEST(GrayRows, UnpacksAKeptRowInBlackAndWhiteAsLookingAtEachPixelWould) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same rows every run, so that a failure can be found again.
  std::mt19937 random(27112026);
  for (const Depth depth : kDepths) {
    for (const Kind kind : kKinds) {
      if (kind == Kind::kAnyGray) {
        continue;  // A kept row holds levels.
      }
      for (const std::size_t width : kWidths) {
        for (unsigned round = 0; round < 4; ++round) {
          const std::vector<std::uint8_t> row = RowOf(kind, width, depth.top, random);
          const std::vector<std::uint8_t> kept = PlainLevels(row, depth.bits, depth.top);
          const Plain plain = PlainUnpacked(kept, width, depth.bits, depth.top);
          std::vector<std::uint8_t> white(plain.white.size(), 0x3C);
          const bool grays = UnpackWhite(kept, width, depth.bits, depth.top, white);
          const std::string where = std::to_string(depth.bits) + " bits, top " + std::to_string(depth.top) + ", kind " +
                                    std::to_string(static_cast<int>(kind)) + ", " + std::to_string(width) + " wide";
          EXPECT_EQ(white, plain.white) << where;
          EXPECT_EQ(grays, plain.grays) << where;
          // And it is the row in black and white that the grays gave.
          const Plain packed = PlainWhite(row);
          EXPECT_EQ(white, packed.white) << where;
          EXPECT_EQ(grays, packed.grays) << where;
        }
      }
    }
  }
}

TEST(GrayRows, UnpacksLevelsABookCouldNotHaveMadeAsTheyAre) {
  // Levels above the top one, which no row that was packed has: each is a
  // gray, as it is to one who looks at each pixel.
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same rows every run, so that a failure can be found again.
  std::mt19937 random(5);
  for (unsigned round = 0; round < 200; ++round) {
    std::vector<std::uint8_t> kept(10);
    for (std::uint8_t& byte : kept) {
      const std::uint32_t any = static_cast<std::uint32_t>(random()) % 256;
      const std::uint32_t pick = static_cast<std::uint32_t>(random()) % 4;
      byte = static_cast<std::uint8_t>(pick == 0 ? 0xAA : pick == 1 ? 0x00 : pick == 2 ? 0xFF : any);
    }
    const Plain plain = PlainUnpacked(kept, 37, 2, 2);
    std::vector<std::uint8_t> white(plain.white.size(), 0);
    const bool grays = UnpackWhite(kept, 37, 2, 2, white);
    EXPECT_EQ(white, plain.white) << round;
    EXPECT_EQ(grays, plain.grays) << round;
  }
}

}  // namespace
