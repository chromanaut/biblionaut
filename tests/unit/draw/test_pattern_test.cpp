#include "draw/test_pattern.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using bn::draw::TestPattern;

constexpr std::uint16_t kWidth = 800;
constexpr std::uint16_t kHeight = 480;

// A whole picture, rendered in one go.
struct Picture {
  Picture(std::uint16_t w, std::uint16_t h, std::uint8_t step)
      : width(w), height(h), rowBytes((static_cast<std::size_t>(w) + 7) / 8), bits(rowBytes * h) {
    TestPattern(w, h, step).RenderRows(0, bits);
  }

  bool Black(int x, int y) const {
    const unsigned byte = bits[(static_cast<std::size_t>(y) * rowBytes) + (static_cast<std::size_t>(x) / 8)];
    return ((byte >> (7U - (static_cast<unsigned>(x) % 8U))) & 1U) == 0;
  }

  std::uint16_t width;
  std::uint16_t height;
  std::size_t rowBytes;
  std::vector<std::uint8_t> bits;
};

TEST(TestPattern, ComesOutTheSameWhateverSizeOfBandItIsRenderedIn) {
  const Picture whole(kWidth, kHeight, 2);
  const TestPattern pattern(kWidth, kHeight, 2);
  ASSERT_EQ(pattern.RowBytes(), 100u);

  for (const std::size_t bandRows : {std::size_t{80}, std::size_t{1}, std::size_t{7}, std::size_t{100}}) {
    std::vector<std::uint8_t> joined;
    for (std::size_t row = 0; row < kHeight; row += bandRows) {
      const std::size_t rows = std::min<std::size_t>(bandRows, kHeight - row);
      std::vector<std::uint8_t> band(rows * pattern.RowBytes(), 0x55);
      pattern.RenderRows(static_cast<std::uint16_t>(row), band);
      joined.insert(joined.end(), band.begin(), band.end());
    }
    EXPECT_EQ(joined, whole.bits) << "bands of " << bandRows << " rows";
  }
}

TEST(TestPattern, HasAFrameAroundTheVeryEdge) {
  const Picture picture(kWidth, kHeight, 1);
  for (const int x : {0, 1, 400, 798, 799}) {
    EXPECT_TRUE(picture.Black(x, 0)) << x;
    EXPECT_TRUE(picture.Black(x, 1)) << x;
    EXPECT_TRUE(picture.Black(x, 478)) << x;
    EXPECT_TRUE(picture.Black(x, 479)) << x;
  }
  for (const int y : {0, 1, 240, 478, 479}) {
    EXPECT_TRUE(picture.Black(0, y)) << y;
    EXPECT_TRUE(picture.Black(1, y)) << y;
    EXPECT_TRUE(picture.Black(798, y)) << y;
    EXPECT_TRUE(picture.Black(799, y)) << y;
  }
  // And it is only two pixels thick.
  EXPECT_FALSE(picture.Black(600, 2));
  EXPECT_FALSE(picture.Black(200, 477));
  EXPECT_FALSE(picture.Black(3, 100));
  EXPECT_FALSE(picture.Black(797, 100));
}

TEST(TestPattern, HasAnUnbrokenDiagonalFromTheFirstPixelToTheLast) {
  const Picture picture(kWidth, kHeight, 1);
  for (int y = 0; y < kHeight; ++y) {
    const int x = (y * (kWidth - 1)) / (kHeight - 1);
    EXPECT_TRUE(picture.Black(x, y)) << y;
    if (y + 1 < kHeight) {
      // Each row's piece reaches the column where the next row's begins.
      const int next = ((y + 1) * (kWidth - 1)) / (kHeight - 1);
      EXPECT_TRUE(picture.Black(next, y)) << y;
    }
  }
  // Away from the line there is nothing: it is a line, not a wedge.
  EXPECT_FALSE(picture.Black(400 + 12, 240));
  EXPECT_FALSE(picture.Black(400 - 12, 240));
}

TEST(TestPattern, HasMarksThatTellTheCornersApart) {
  const Picture picture(kWidth, kHeight, 1);
  // The F: stem, top bar, middle bar, and the gaps that make it an F.
  EXPECT_TRUE(picture.Black(40, 180));
  EXPECT_TRUE(picture.Black(90, 100));
  EXPECT_TRUE(picture.Black(70, 144));
  EXPECT_FALSE(picture.Black(70, 124));
  EXPECT_FALSE(picture.Black(90, 144));
  EXPECT_FALSE(picture.Black(60, 180));
  // A solid square in the corner at the end of the first row.
  EXPECT_TRUE(picture.Black(kWidth - 52, 52));
  // A hollow one in the corner at the end of the first column.
  EXPECT_TRUE(picture.Black(33, kHeight - 71));
  EXPECT_FALSE(picture.Black(52, kHeight - 52));

  // So the picture is not its own mirror image, either way round.
  int differencesAcross = 0;
  int differencesDown = 0;
  for (int y = 0; y < kHeight; y += 3) {
    for (int x = 0; x < kWidth; x += 3) {
      differencesAcross += picture.Black(x, y) != picture.Black(kWidth - 1 - x, y) ? 1 : 0;
      differencesDown += picture.Black(x, y) != picture.Black(x, kHeight - 1 - y) ? 1 : 0;
    }
  }
  EXPECT_GT(differencesAcross, 1000);
  EXPECT_GT(differencesDown, 1000);
}

TEST(TestPattern, HasAStripThatChangesEveryEightyRows) {
  const Picture picture(kWidth, kHeight, 1);
  for (int band = 0; band < 6; ++band) {
    const bool black = band % 2 == 0;
    EXPECT_EQ(picture.Black(8, (band * 80) + 3), black) << band;
    EXPECT_EQ(picture.Black(13, (band * 80) + 76), black) << band;
  }
  EXPECT_FALSE(picture.Black(5, 40));
  EXPECT_FALSE(picture.Black(14, 40));
}

TEST(TestPattern, HasPatchesOfSinglePixelDetail) {
  const Picture picture(kWidth, kHeight, 1);
  // Vertical lines one pixel wide.
  EXPECT_TRUE(picture.Black(250, 60));
  EXPECT_FALSE(picture.Black(251, 60));
  EXPECT_TRUE(picture.Black(252, 61));
  // Horizontal lines one pixel high.
  EXPECT_TRUE(picture.Black(380, 40));
  EXPECT_FALSE(picture.Black(380, 41));
  EXPECT_TRUE(picture.Black(381, 42));
  // Checks of one pixel.
  EXPECT_TRUE(picture.Black(474, 40));
  EXPECT_FALSE(picture.Black(475, 40));
  EXPECT_FALSE(picture.Black(474, 41));
  EXPECT_TRUE(picture.Black(475, 41));
  // A quarter of the pixels black, and three quarters.
  int quarter = 0;
  int threeQuarters = 0;
  for (int dy = 0; dy < 96; ++dy) {
    for (int dx = 0; dx < 96; ++dx) {
      quarter += picture.Black(440 + dx, 152 + dy) ? 1 : 0;
      threeQuarters += picture.Black(552 + dx, 152 + dy) ? 1 : 0;
    }
  }
  EXPECT_EQ(quarter, 96 * 96 / 4);
  EXPECT_EQ(threeQuarters, 96 * 96 * 3 / 4);
}

TEST(TestPattern, ShowsItsStepAndChangesNothingElse) {
  const Picture first(kWidth, kHeight, 1);
  for (std::uint8_t step = 1; step <= TestPattern::kStepCount; ++step) {
    const Picture picture(kWidth, kHeight, step);
    // As many boxes filled as the step number.
    for (int box = 0; box < TestPattern::kStepCount; ++box) {
      const int x = 60 + (72 * box);
      EXPECT_EQ(picture.Black(x + 24, 324), box < step) << "step " << int{step} << " box " << box;
      EXPECT_TRUE(picture.Black(x + 1, 301));  // Its outline is always there.
    }
    // The block's stripes, 8 pixels wide, lie flat on odd steps and stand
    // up on even ones. Two points one above the other differ when they lie
    // flat; two points side by side differ when they stand.
    const bool above = picture.Black(150, 372 + 4);
    const bool below = picture.Black(150, 372 + 12);
    const bool left = picture.Black(100 + 4, 400);
    const bool right = picture.Black(100 + 12, 400);
    if (step % 2 == 1) {
      EXPECT_TRUE(above && !below) << "step " << int{step};
      EXPECT_EQ(left, right) << "step " << int{step};
    } else {
      EXPECT_TRUE(left && !right) << "step " << int{step};
      EXPECT_EQ(above, below) << "step " << int{step};
    }

    // Everything above the boxes is the same in every step.
    const std::size_t unchanged = 300 * first.rowBytes;
    EXPECT_TRUE(std::equal(first.bits.begin(), first.bits.begin() + static_cast<std::ptrdiff_t>(unchanged),
                           picture.bits.begin()))
        << "step " << int{step};
  }
  // A step number out of range fills nothing more than the boxes there are.
  const Picture none(kWidth, kHeight, 0);
  EXPECT_FALSE(none.Black(84, 324));
}

TEST(TestPattern, KeepsToAnySizeOfPicture) {
  // A width that is not a whole number of bytes: the spare bits stay white.
  const Picture small(37, 20, 1);
  ASSERT_EQ(small.rowBytes, 5u);
  EXPECT_TRUE(small.Black(36, 10));  // The frame's right edge.
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(small.bits[(static_cast<std::size_t>(y) * 5) + 4] & 0x07U, 0x07U) << y;
  }

  // Rows asked for past the bottom come out white.
  const TestPattern pattern(37, 20, 1);
  constexpr std::size_t kTenRows = 50;  // Rows 15 to 24, of which the picture has 15 to 19.
  constexpr std::ptrdiff_t kFiveRows = 25;
  std::vector<std::uint8_t> band(kTenRows, 0x00);
  pattern.RenderRows(15, band);
  EXPECT_TRUE(std::equal(band.begin(), band.begin() + kFiveRows, small.bits.begin() + (3 * kFiveRows)));
  EXPECT_TRUE(std::all_of(band.begin() + kFiveRows, band.end(), [](std::uint8_t byte) { return byte == 0xFF; }));

  std::vector<std::uint8_t> beyond(10, 0x00);
  pattern.RenderRows(400, beyond);
  EXPECT_TRUE(std::all_of(beyond.begin(), beyond.end(), [](std::uint8_t byte) { return byte == 0xFF; }));

  // The other supported panel is a different shape; nothing lands outside it.
  const Picture x3(792, 528, 4);
  EXPECT_EQ(x3.rowBytes, 99u);
  EXPECT_TRUE(x3.Black(791, 527));

  // Nothing at all is a picture of nothing.
  std::vector<std::uint8_t> nothing;
  TestPattern(0, 0, 1).RenderRows(0, nothing);
  TestPattern(800, 480, 1).RenderRows(0, nothing);
}

}  // namespace
