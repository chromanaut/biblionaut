#include "draw/fill.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <string_view>
#include <vector>

#include "draw/flatten.h"
#include "draw/svg_path.h"

// The filler adds up cover in whole numbers as lines go by. Here what it
// draws is set beside the cover each pixel should have, found another way:
// for a shape with no dents, by cutting the shape to the pixel and
// measuring what is left; for any shape, by trying many points in each
// pixel and counting how the outline goes round each.

namespace {

using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::Point;
using bn::draw::SvgPathReader;
using bn::draw::SvgPathStatus;

struct Real {
  double x = 0;
  double y = 0;
};

using Outline = std::vector<Real>;
using Shape = std::vector<Outline>;

constexpr std::uint8_t kWhite = 255;
constexpr std::uint8_t kBlack = 0;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

Point AsPoint(Real point) {
  return {.x = static_cast<Fixed>(std::lround(point.x * kFixedOne)),
          .y = static_cast<Fixed>(std::lround(point.y * kFixedOne))};
}

// A picture, white until something is drawn on it.
struct Picture {
  Picture(std::size_t w, std::size_t h) : width(w), height(h), gray(w * h, kWhite) {}

  std::uint8_t At(std::size_t x, std::size_t y) const { return gray[(y * width) + x]; }
  // How much of a pixel is black, from nothing to one, if black was drawn
  // on white.
  double Cover(std::size_t x, std::size_t y) const { return (255.0 - At(x, y)) / 255.0; }

  std::size_t width;
  std::size_t height;
  std::vector<std::uint8_t> gray;
};

void Outlined(Filler& filler, const Shape& shape, bool closing = true) {
  for (const Outline& outline : shape) {
    filler.MoveTo(AsPoint(outline.front()));
    for (std::size_t i = 1; i < outline.size(); ++i) {
      filler.LineTo(AsPoint(outline[i]));
    }
    if (closing) {
      filler.Close();
    }
  }
}

// Draws shapes on a picture, one over another, in strips of `stripRows`
// rows: the whole of every shape is gone through again for each strip, as
// it is when a picture is taller than the memory there is for it.
struct Layer {
  Shape shape;
  FillRule rule = FillRule::kNonZero;
  std::uint8_t gray = kBlack;
  std::uint8_t opacity = 255;
};

void Draw(Picture& picture, const std::vector<Layer>& layers, std::size_t stripRows) {
  std::vector<std::int16_t> cells(Filler::CellsFor(picture.width, stripRows));
  for (std::size_t top = 0; top < picture.height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, picture.height - top);
    Filler filler;
    ASSERT_TRUE(filler.Begin(std::span(picture.gray).subspan(top * picture.width, rows * picture.width), picture.width,
                             static_cast<std::int32_t>(top), cells));
    for (const Layer& layer : layers) {
      Outlined(filler, layer.shape);
      filler.Fill(layer.rule, layer.gray, layer.opacity);
    }
  }
}

Picture Drawn(std::size_t width, std::size_t height, const Shape& shape, FillRule rule = FillRule::kNonZero,
              std::size_t stripRows = 0) {
  Picture picture(width, height);
  Draw(picture, {{.shape = shape, .rule = rule}}, stripRows == 0 ? height : stripRows);
  return picture;
}

// ---- The cover a pixel should have ----

// What is left of a shape with no dents once it is cut to the pixel at
// (x, y), as a part of the pixel.
double CutToPixel(const Outline& outline, double x, double y) {
  Outline cut = outline;
  // Each edge of the pixel in turn keeps what is on its inner side.
  const auto keep = [&cut](auto inside, auto crossing) {
    Outline kept;
    for (std::size_t i = 0; i < cut.size(); ++i) {
      const Real a = cut[i];
      const Real b = cut[(i + 1) % cut.size()];
      if (inside(a)) {
        kept.push_back(a);
      }
      if (inside(a) != inside(b)) {
        kept.push_back(crossing(a, b));
      }
    }
    cut = kept;
  };
  const auto atX = [](double edge) {
    return [edge](Real a, Real b) { return Real{edge, a.y + ((b.y - a.y) * (edge - a.x) / (b.x - a.x))}; };
  };
  const auto atY = [](double edge) {
    return [edge](Real a, Real b) { return Real{a.x + ((b.x - a.x) * (edge - a.y) / (b.y - a.y)), edge}; };
  };
  keep([x](Real p) { return p.x >= x; }, atX(x));
  keep([x](Real p) { return p.x <= x + 1; }, atX(x + 1));
  keep([y](Real p) { return p.y >= y; }, atY(y));
  keep([y](Real p) { return p.y <= y + 1; }, atY(y + 1));
  double twice = 0;
  for (std::size_t i = 0; i < cut.size(); ++i) {
    const Real a = cut[i];
    const Real b = cut[(i + 1) % cut.size()];
    twice += (a.x * b.y) - (b.x * a.y);
  }
  return std::abs(twice) / 2;
}

// How many times a shape's outlines go round a point.
int TimesRound(const Shape& shape, Real point) {
  int times = 0;
  for (const Outline& outline : shape) {
    for (std::size_t i = 0; i < outline.size(); ++i) {
      const Real a = outline[i];
      const Real b = outline[(i + 1) % outline.size()];
      const double side = ((b.x - a.x) * (point.y - a.y)) - ((point.x - a.x) * (b.y - a.y));
      if (a.y <= point.y && b.y > point.y && side > 0) {
        ++times;
      } else if (a.y > point.y && b.y <= point.y && side < 0) {
        --times;
      }
    }
  }
  return times;
}

// What trying a grid of points in the pixel at (x, y) finds: how much of
// it is inside, and whether the outline goes round the whole of it the
// same number of times but for one step. That is so of a pixel with one
// edge of the shape through it, or a corner, and not of one where the
// outline crosses itself or two of its parts overlap.
struct Sampled {
  double cover = 0;
  bool oneStep = true;
};

Sampled Sample(const Shape& shape, FillRule rule, std::size_t x, std::size_t y, int grid = 12) {
  int inside = 0;
  int fewest = INT32_MAX;
  int most = INT32_MIN;
  for (int row = 0; row < grid; ++row) {
    for (int column = 0; column < grid; ++column) {
      const int times = TimesRound(
          shape, {static_cast<double>(x) + ((column + 0.5) / grid), static_cast<double>(y) + ((row + 0.5) / grid)});
      if (rule == FillRule::kNonZero ? times != 0 : times % 2 != 0) {
        ++inside;
      }
      fewest = std::min(fewest, times);
      most = std::max(most, times);
    }
  }
  return {.cover = static_cast<double>(inside) / (grid * grid), .oneStep = most - fewest <= 1};
}

// Corners on a grid of sixteenths of a pixel, which a place holds exactly.
Real OnGrid(Real point) { return {std::round(point.x * 16) / 16, std::round(point.y * 16) / 16}; }

// A shape with no dents: points round an ellipse, in order.
Outline NoDents(std::mt19937& random, double width, double height) {
  std::uniform_real_distribution<double> part(0.0, 1.0);
  const int corners = 3 + static_cast<int>(random() % 6);
  std::vector<double> angles(static_cast<std::size_t>(corners));
  for (double& angle : angles) {
    angle = part(random) * 2 * std::numbers::pi;
  }
  std::sort(angles.begin(), angles.end());
  const Real middle = {width * (0.2 + (0.6 * part(random))), height * (0.2 + (0.6 * part(random)))};
  const double across = width * (0.1 + (0.7 * part(random)));
  const double down = height * (0.1 + (0.7 * part(random)));
  Outline outline;
  for (const double angle : angles) {
    outline.push_back(OnGrid({middle.x + (across * std::cos(angle)), middle.y + (down * std::sin(angle))}));
  }
  return outline;
}

// Any shape at all: corners anywhere, in any order, so it crosses itself.
Outline Tangle(std::mt19937& random, double width, double height) {
  std::uniform_real_distribution<double> across(-0.2 * width, 1.2 * width);
  std::uniform_real_distribution<double> down(-0.2 * height, 1.2 * height);
  Outline outline;
  const int corners = 3 + static_cast<int>(random() % 7);
  for (int i = 0; i < corners; ++i) {
    outline.push_back(OnGrid({across(random), down(random)}));
  }
  return outline;
}

// ---- Shapes whose cover is known exactly ----

TEST(Fill, ARectangleCoversEachPixelByAsMuchAsLiesInIt) {
  // From 2.25 to 6.5 across and from 1.5 to 4.75 down.
  const Picture picture = Drawn(10, 8, {{{2.25, 1.5}, {6.5, 1.5}, {6.5, 4.75}, {2.25, 4.75}}});
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 10; ++x) {
      const auto left = static_cast<double>(x);
      const auto top = static_cast<double>(y);
      const double across = std::clamp(std::min(left + 1, 6.5) - std::max(left, 2.25), 0.0, 1.0);
      const double down = std::clamp(std::min(top + 1, 4.75) - std::max(top, 1.5), 0.0, 1.0);
      EXPECT_NEAR(picture.Cover(x, y), across * down, 0.003) << x << "," << y;
    }
  }
  // Wholly inside is wholly black, and wholly outside is untouched.
  EXPECT_EQ(picture.At(4, 3), kBlack);
  EXPECT_EQ(picture.At(0, 0), kWhite);
  EXPECT_EQ(picture.At(7, 3), kWhite);
}

TEST(Fill, ASlantingEdgeCoversHalfOfEachPixelItCutsCornerToCorner) {
  const Picture picture = Drawn(8, 8, {{{0, 0}, {8, 0}, {0, 8}}});
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      const double want = x + y < 7 ? 1.0 : (x + y == 7 ? 0.5 : 0.0);
      EXPECT_NEAR(picture.Cover(x, y), want, 0.003) << x << "," << y;
    }
  }
}

TEST(Fill, AShapeWithNoDentsCoversEachPixelByWhatLiesInIt) {
  std::mt19937 random = Repeatable(21);
  double worst = 0;
  for (int i = 0; i < 150; ++i) {
    const std::size_t width = 8 + (random() % 30);
    const std::size_t height = 8 + (random() % 30);
    Outline outline = NoDents(random, static_cast<double>(width), static_cast<double>(height));
    if (i % 2 == 0) {
      std::reverse(outline.begin(), outline.end());
    }
    const FillRule rule = (i % 3 == 0) ? FillRule::kEvenOdd : FillRule::kNonZero;
    const Picture picture = Drawn(width, height, {outline}, rule);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        const double want = CutToPixel(outline, static_cast<double>(x), static_cast<double>(y));
        const double apart = std::abs(picture.Cover(x, y) - want);
        ASSERT_LT(apart, 0.012) << "shape " << i << " at " << x << "," << y << ": " << picture.Cover(x, y) << " for "
                                << want;
        worst = std::max(worst, apart);
      }
    }
  }
  // Within three parts in 255, with every corner where a place can hold it.
  EXPECT_LT(worst, 0.012);
}

TEST(Fill, AShallowLineIsSharedOutAmongThePixelsItCrosses) {
  // An edge that crosses sixteen pixels in the height of one, and one
  // that crosses a pixel's width in the height of sixteen.
  for (const bool lying : {true, false}) {
    const Outline outline = lying ? Outline{{0, 0}, {16, 0}, {16, 1}} : Outline{{0, 0}, {1, 16}, {0, 16}};
    const Picture picture = lying ? Drawn(16, 1, {outline}) : Drawn(1, 16, {outline});
    double total = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      const double cover = lying ? picture.Cover(i, 0) : picture.Cover(0, i);
      // A sliver that widens evenly: a 32nd in the first pixel, three in
      // the next, and so on.
      EXPECT_NEAR(cover, ((2.0 * static_cast<double>(i)) + 1) / 32, 0.004) << lying << " " << i;
      total += cover;
    }
    EXPECT_NEAR(total, 8.0, 0.02);
  }
}

// ---- Which parts are inside ----

Outline FivePointedStar(double middle, double radius) {
  Outline star;
  for (int i = 0; i < 5; ++i) {
    // Every second corner of a pentagon, so the outline crosses itself.
    const double angle = (-0.5 + (0.8 * i)) * std::numbers::pi;
    star.push_back(OnGrid({middle + (radius * std::cos(angle)), middle + (radius * std::sin(angle))}));
  }
  return star;
}

TEST(Fill, TheMiddleOfAStarIsInsideByOneRuleAndOutsideByTheOther) {
  const Shape star = {FivePointedStar(20, 18)};
  const Picture nonZero = Drawn(40, 40, star, FillRule::kNonZero);
  const Picture evenOdd = Drawn(40, 40, star, FillRule::kEvenOdd);
  // The middle, which the outline goes round twice.
  EXPECT_EQ(nonZero.At(20, 20), kBlack);
  EXPECT_EQ(evenOdd.At(20, 20), kWhite);
  // A point of the star, which it goes round once.
  EXPECT_EQ(nonZero.At(20, 6), kBlack);
  EXPECT_EQ(evenOdd.At(20, 6), kBlack);
  // And a corner of the picture, which it does not go round.
  EXPECT_EQ(nonZero.At(1, 1), kWhite);
  EXPECT_EQ(evenOdd.At(1, 1), kWhite);
}

TEST(Fill, AnOutlineInsideAnotherIsAHoleIfItRunsTheOtherWay) {
  const Outline outer = {{2, 2}, {18, 2}, {18, 18}, {2, 18}};
  const Outline sameWay = {{6, 6}, {14, 6}, {14, 14}, {6, 14}};
  const Outline otherWay = {{6, 6}, {6, 14}, {14, 14}, {14, 6}};
  // By the first rule the way the inner one runs decides it.
  EXPECT_EQ(Drawn(20, 20, {outer, sameWay}, FillRule::kNonZero).At(10, 10), kBlack);
  EXPECT_EQ(Drawn(20, 20, {outer, otherWay}, FillRule::kNonZero).At(10, 10), kWhite);
  // By the second it is a hole either way.
  EXPECT_EQ(Drawn(20, 20, {outer, sameWay}, FillRule::kEvenOdd).At(10, 10), kWhite);
  EXPECT_EQ(Drawn(20, 20, {outer, otherWay}, FillRule::kEvenOdd).At(10, 10), kWhite);
  // The ring between them is filled whichever.
  EXPECT_EQ(Drawn(20, 20, {outer, otherWay}, FillRule::kNonZero).At(4, 10), kBlack);
  EXPECT_EQ(Drawn(20, 20, {outer, sameWay}, FillRule::kEvenOdd).At(4, 10), kBlack);
}

TEST(Fill, AnyShapeCoversWhatTryingPointsSaysItCovers) {
  std::mt19937 random = Repeatable(22);
  constexpr std::size_t kWidth = 24;
  constexpr std::size_t kHeight = 20;
  double total = 0;
  std::size_t pixels = 0;
  std::size_t crossed = 0;
  for (int i = 0; i < 60; ++i) {
    Shape shape = {Tangle(random, kWidth, kHeight)};
    if (i % 3 == 0) {
      shape.push_back(Tangle(random, kWidth, kHeight));
    }
    const FillRule rule = (i % 2 == 0) ? FillRule::kEvenOdd : FillRule::kNonZero;
    const Picture picture = Drawn(kWidth, kHeight, shape, rule);
    for (std::size_t y = 0; y < kHeight; ++y) {
      for (std::size_t x = 0; x < kWidth; ++x) {
        const Sampled sampled = Sample(shape, rule, x, y);
        if (!sampled.oneStep) {
          // Where the outline crosses itself within the pixel, the cover
          // is only near: see the limit in fill.h.
          ++crossed;
          continue;
        }
        const double apart = std::abs(picture.Cover(x, y) - sampled.cover);
        // Trying 144 points in a pixel is itself only so exact.
        ASSERT_LT(apart, 0.11) << "shape " << i << " at " << x << "," << y << ": drawn " << picture.Cover(x, y)
                               << ", by 144 points " << sampled.cover << ", by 10,000 "
                               << Sample(shape, rule, x, y, 100).cover;
        total += apart;
        ++pixels;
      }
    }
  }
  // Taken over all the pixels the two agree far more closely than either
  // is out in any one of them.
  EXPECT_LT(total / static_cast<double>(pixels), 0.004);
  // And the pixels left out are few, even of shapes as tangled as these.
  EXPECT_LT(crossed, pixels / 20);
  EXPECT_GT(crossed, 0u);
}

TEST(Fill, WhereAnOutlineCrossesItselfInAPixelItsCoverIsOnlyNear) {
  // The limit of adding cover up line by line: a cell knows how much of
  // its pixel the outline has gone round, counted once for each time
  // round, and not which parts of it. A square whose right edge runs down
  // the middle of a column of pixels:
  const Outline square = {{2, 2}, {6.5, 2}, {6.5, 8}, {2, 8}};
  EXPECT_NEAR(Drawn(12, 10, {square}).At(6, 5), 128, 1);
  // The same square twice over, as one shape, has gone twice round that
  // half of each of those pixels: which adds up to once round the whole.
  const Shape twice = {square, square};
  // By the first rule it should look as it did once, and its edge comes
  // out wholly covered.
  EXPECT_EQ(Drawn(12, 10, twice, FillRule::kNonZero).At(6, 5), kBlack);
  // By the second rule twice round is outside, so nothing should be drawn
  // at all. Its middle is rightly left alone, and its edge is not.
  EXPECT_EQ(Drawn(12, 10, twice, FillRule::kEvenOdd).At(4, 5), kWhite);
  EXPECT_EQ(Drawn(12, 10, twice, FillRule::kEvenOdd).At(6, 5), kBlack);
  // Where one part of an outline lies over another by a single step, as
  // two squares that overlap do, the pixels between them are right.
  const Shape lapped = {square, {{6, 2}, {10, 2}, {10, 8}, {6, 8}}};
  EXPECT_EQ(Drawn(12, 10, lapped, FillRule::kNonZero).At(6, 5), kBlack);
  EXPECT_NEAR(Drawn(12, 10, lapped, FillRule::kEvenOdd).At(6, 5), 128, 1);
}

// ---- Strips ----

TEST(Fill, ComesOutTheSameHoweverThePictureIsCutIntoStrips) {
  std::mt19937 random = Repeatable(23);
  constexpr std::size_t kWidth = 40;
  constexpr std::size_t kHeight = 37;
  for (int i = 0; i < 40; ++i) {
    std::vector<Layer> layers(3);
    for (Layer& layer : layers) {
      layer = {.shape = {Tangle(random, kWidth, kHeight)},
               .rule = (random() % 2 == 0) ? FillRule::kEvenOdd : FillRule::kNonZero,
               .gray = static_cast<std::uint8_t>(random() % 256),
               .opacity = static_cast<std::uint8_t>(random() % 2 == 0 ? 255 : random() % 256)};
    }
    Picture whole(kWidth, kHeight);
    Draw(whole, layers, kHeight);
    for (const std::size_t stripRows :
         {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{36}, std::size_t{64}}) {
      Picture strips(kWidth, kHeight);
      Draw(strips, layers, stripRows);
      ASSERT_EQ(strips.gray, whole.gray) << "picture " << i << " in strips of " << stripRows;
    }
  }
}

TEST(Fill, WhatLiesOutsideThePictureIsCutOffAndWhatIsInsideIsUnchanged) {
  // A scene three times the picture's size each way, drawn whole and then
  // through a window onto its middle.
  std::mt19937 random = Repeatable(24);
  constexpr std::size_t kWidth = 20;
  constexpr std::size_t kHeight = 16;
  for (int i = 0; i < 60; ++i) {
    const Outline outline = Tangle(random, 3.0 * kWidth, 3.0 * kHeight);
    const FillRule rule = (i % 2 == 0) ? FillRule::kEvenOdd : FillRule::kNonZero;
    const Picture scene = Drawn(3 * kWidth, 3 * kHeight, {outline}, rule);
    Outline moved;
    for (const Real point : outline) {
      moved.push_back({point.x - kWidth, point.y - kHeight});
    }
    const Picture window = Drawn(kWidth, kHeight, {moved}, rule);
    for (std::size_t y = 0; y < kHeight; ++y) {
      for (std::size_t x = 0; x < kWidth; ++x) {
        // Where a line is cut at the picture's edge it is placed again to
        // the nearest 256th of a pixel, so a pixel may differ by a part or
        // two in 255.
        ASSERT_NEAR(window.At(x, y), scene.At(x + kWidth, y + kHeight), 2) << "shape " << i << " at " << x << "," << y;
      }
    }
  }
}

TEST(Fill, WhatIsLeftOfThePictureCountsForItsHeightAndWhatIsRightForNothing) {
  // A rectangle that starts far to the left: its near edge is never seen,
  // and every pixel up to its far edge is inside it all the same.
  Picture picture = Drawn(12, 6, {{{-500, 1}, {5, 1}, {5, 4}, {-500, 4}}});
  for (std::size_t x = 0; x < 12; ++x) {
    EXPECT_EQ(picture.At(x, 2), x < 5 ? kBlack : kWhite) << x;
    EXPECT_EQ(picture.At(x, 0), kWhite) << x;
    EXPECT_EQ(picture.At(x, 5), kWhite) << x;
  }
  // One that runs off to the right: filled from its near edge to the
  // picture's.
  picture = Drawn(12, 6, {{{5, 1}, {900, 1}, {900, 4}, {5, 4}}});
  for (std::size_t x = 0; x < 12; ++x) {
    EXPECT_EQ(picture.At(x, 2), x < 5 ? kWhite : kBlack) << x;
  }
  // One that covers the picture and far more.
  picture = Drawn(12, 6, {{{-9000, -9000}, {9000, -9000}, {9000, 9000}, {-9000, 9000}}});
  EXPECT_EQ(std::count(picture.gray.begin(), picture.gray.end(), kBlack), 72);
  // And those that miss it altogether, on each side.
  for (const Real by : {Real{-100, 0}, Real{100, 0}, Real{0, -100}, Real{0, 100}}) {
    picture = Drawn(12, 6, {{{2 + by.x, 1 + by.y}, {9 + by.x, 1 + by.y}, {9 + by.x, 5 + by.y}, {2 + by.x, 5 + by.y}}});
    EXPECT_EQ(std::count(picture.gray.begin(), picture.gray.end(), kWhite), 72) << by.x << "," << by.y;
  }
}

// ---- Outlines, paint and layers ----

TEST(Fill, AnOutlineLeftOpenIsClosedBackToWhereItBegan) {
  std::vector<std::int16_t> cells(Filler::CellsFor(10, 10));
  Picture open(10, 10);
  Filler filler;
  ASSERT_TRUE(filler.Begin(open.gray, 10, 0, cells));
  // Two triangles, neither closed: the second move closes the first, and
  // filling closes the second.
  Outlined(filler, {{{1, 1}, {4, 1}, {1, 4}}, {{6, 6}, {9, 6}, {9, 9}}}, false);
  filler.Fill(FillRule::kNonZero, kBlack);
  const Picture closed = Drawn(10, 10, {{{1, 1}, {4, 1}, {1, 4}}, {{6, 6}, {9, 6}, {9, 9}}});
  EXPECT_EQ(open.gray, closed.gray);
  EXPECT_EQ(open.At(1, 1), kBlack);
  EXPECT_EQ(open.At(8, 6), kBlack);
}

TEST(Fill, LaysOneShapeOverAnotherAndLetsTheOneBelowShowThroughAThinOne) {
  const Shape left = {{{0, 0}, {6, 0}, {6, 4}, {0, 4}}};
  const Shape right = {{{4, 0}, {10, 0}, {10, 4}, {4, 4}}};
  Picture picture(10, 4);
  Draw(picture, {{.shape = left, .gray = 40}, {.shape = right, .gray = 200}}, 4);
  EXPECT_EQ(picture.At(1, 1), 40);
  EXPECT_EQ(picture.At(5, 1), 200);  // Where they overlap, the later one.
  EXPECT_EQ(picture.At(8, 1), 200);

  // Half as strong, it comes out half way between itself and what it lies
  // on: white on the right, the first shape where they overlap.
  picture = Picture(10, 4);
  Draw(picture, {{.shape = left, .gray = 40}, {.shape = right, .gray = 200, .opacity = 128}}, 4);
  EXPECT_EQ(picture.At(1, 1), 40);
  EXPECT_NEAR(picture.At(5, 1), 120, 1);
  EXPECT_NEAR(picture.At(8, 1), 227, 1);

  // With no strength at all it leaves no mark.
  picture = Picture(10, 4);
  Draw(picture, {{.shape = left, .gray = 40}, {.shape = right, .gray = 200, .opacity = 0}}, 4);
  EXPECT_EQ(picture.At(5, 1), 40);
  EXPECT_EQ(picture.At(8, 1), kWhite);
}

TEST(Fill, LeavesNothingBehindForTheNextShape) {
  std::mt19937 random = Repeatable(25);
  std::vector<std::int16_t> cells(Filler::CellsFor(30, 30));
  for (int i = 0; i < 100; ++i) {
    Picture picture(30, 30);
    Filler filler;
    ASSERT_TRUE(filler.Begin(picture.gray, 30, 0, cells));
    Outlined(filler, {Tangle(random, 30, 30)});
    filler.Fill(i % 2 == 0 ? FillRule::kEvenOdd : FillRule::kNonZero, kBlack);
    // Every cell is back to nothing, so the next shape starts clean.
    ASSERT_EQ(std::count(cells.begin(), cells.begin() + 900, 0), 900) << i;
    const std::vector<std::uint8_t> after = picture.gray;
    filler.Fill(FillRule::kNonZero, 99);
    ASSERT_EQ(picture.gray, after) << i;
  }
}

TEST(Fill, AShapeThatGoesRoundManyTimesIsFilledAsItsRuleSays) {
  const Outline square = {{2, 2}, {8, 2}, {8, 8}, {2, 8}};
  const auto times = [&square](int count, FillRule rule) {
    Shape shape;
    for (int i = 0; i < count; ++i) {
      shape.push_back(square);
    }
    return Drawn(10, 10, shape, rule).At(5, 5);
  };
  EXPECT_EQ(times(3, FillRule::kNonZero), kBlack);
  EXPECT_EQ(times(3, FillRule::kEvenOdd), kBlack);
  EXPECT_EQ(times(4, FillRule::kEvenOdd), kWhite);
  // Past 127 times round, a cell has lost count. Odd and even it still
  // knows, and the first rule is still right unless the count comes to a
  // multiple of 256.
  EXPECT_EQ(times(200, FillRule::kNonZero), kBlack);
  EXPECT_EQ(times(255, FillRule::kEvenOdd), kBlack);
  EXPECT_EQ(times(300, FillRule::kEvenOdd), kWhite);
  EXPECT_EQ(times(256, FillRule::kNonZero), kWhite);
}

// ---- What it is given to work with ----

TEST(Fill, RefusesAStripItCannotDraw) {
  std::vector<std::uint8_t> gray(200, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(20, 10));
  Filler filler;
  EXPECT_TRUE(filler.Begin(gray, 20, 0, cells));
  EXPECT_TRUE(filler.Begin(gray, 20, -1000000, cells));
  EXPECT_TRUE(filler.Begin(std::span(gray).first(20), 20, 5, cells));
  // No width; rows that are not whole; nothing to draw into.
  EXPECT_FALSE(filler.Begin(gray, 0, 0, cells));
  EXPECT_FALSE(filler.Begin(gray, 30, 0, cells));
  EXPECT_FALSE(filler.Begin(std::span<std::uint8_t>(), 20, 0, cells));
  // Too few cells, by one.
  EXPECT_FALSE(filler.Begin(gray, 20, 0, std::span(cells).first(cells.size() - 1)));
  // Too far from the picture's first row.
  EXPECT_FALSE(filler.Begin(gray, 20, 2000000, cells));
  EXPECT_FALSE(filler.Begin(gray, 20, -2000000, cells));
  // Too wide.
  std::vector<std::uint8_t> wide(Filler::kMostWidth + 1, kWhite);
  std::vector<std::int16_t> manyCells(Filler::CellsFor(wide.size(), 1));
  EXPECT_FALSE(filler.Begin(wide, wide.size(), 0, manyCells));
  EXPECT_TRUE(filler.Begin(std::span(wide).first(Filler::kMostWidth), Filler::kMostWidth, 0, manyCells));

  // Having been refused, or never begun, it draws nothing and does no harm.
  Filler idle;
  EXPECT_FALSE(idle.Begin(gray, 0, 0, cells));
  idle.MoveTo(AsPoint({1, 1}));
  idle.LineTo(AsPoint({5, 1}));
  idle.LineTo(AsPoint({5, 5}));
  idle.Fill(FillRule::kNonZero, kBlack);
  EXPECT_EQ(std::count(gray.begin(), gray.end(), kWhite), 200);
}

TEST(Fill, ALineCostsNoMoreThanItsBoundWhereverItRuns) {
  // For the sanitizers as much as for the bound: lines from anywhere to
  // anywhere, most of them far outside the strip.
  std::mt19937 random = Repeatable(26);
  constexpr std::size_t kWidth = 50;
  constexpr std::size_t kRows = 9;
  std::vector<std::uint8_t> gray(kWidth * kRows, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(kWidth, kRows));
  const std::uint32_t most = Filler::MostCellsPerLine(kWidth, kRows);
  std::uint32_t costliest = 0;
  for (int i = 0; i < 3000; ++i) {
    Filler filler;
    ASSERT_TRUE(filler.Begin(gray, kWidth, static_cast<std::int32_t>(random() % 40) - 20, cells));
    const auto place = [&random]() {
      switch (random() % 4) {
        case 0:
          return static_cast<Fixed>(random());
        case 1:
          return static_cast<Fixed>(random() % 2 == 0 ? INT32_MAX : -INT32_MAX);
        default:
          return static_cast<Fixed>(static_cast<std::int32_t>(random() % 400000) - 100000);
      }
    };
    filler.MoveTo({.x = place(), .y = place()});
    for (int line = 0; line < 6; ++line) {
      const std::uint32_t before = filler.CellsTouched();
      filler.LineTo({.x = place(), .y = place()});
      ASSERT_LE(filler.CellsTouched() - before, most);
      costliest = std::max(costliest, filler.CellsTouched() - before);
    }
    const std::uint32_t before = filler.PixelsVisited();
    filler.Fill(i % 2 == 0 ? FillRule::kEvenOdd : FillRule::kNonZero, kBlack, 128);
    // And filling looks at each pixel of the strip once at the most.
    ASSERT_LE(filler.PixelsVisited() - before, kWidth * kRows);
  }
  // Some line did cross the whole strip, or the bound was not tried.
  EXPECT_GT(costliest, kWidth);
}

// ---- With the reader and the flattener ----

// Draws SVG path data, through a matrix, in strips.
Picture DrawnPath(std::string_view data, std::size_t width, std::size_t height, std::size_t stripRows, bool kept,
                  double scale = 1.0) {
  Picture picture(width, height);
  std::vector<std::int16_t> cells(Filler::CellsFor(width, stripRows));
  Matrix matrix;
  matrix.a = static_cast<bn::draw::Coefficient>(std::lround(scale * bn::draw::kCoefficientOne));
  matrix.d = matrix.a;
  for (std::size_t top = 0; top < height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, height - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(picture.gray).subspan(top * width, rows * width), width,
                             static_cast<std::int32_t>(top), cells));
    PathFlattener flattener(filler, matrix);
    if (kept) {
      flattener.KeepTo(AsPoint({0, static_cast<double>(top)}),
                       AsPoint({static_cast<double>(width), static_cast<double>(top + rows)}));
    }
    SvgPathReader reader(flattener);
    reader.Feed(data);
    EXPECT_EQ(reader.Finish(), SvgPathStatus::kOk);
    filler.Fill(FillRule::kEvenOdd, kBlack);
  }
  return picture;
}

TEST(Fill, ACircleCoversAsMuchAsACircleDoes) {
  // A circle of radius 20 about (25, 25), as two arcs.
  const Picture picture = DrawnPath("M5 25a20 20 0 1 0 40 0a20 20 0 1 0-40 0z", 50, 50, 50, false);
  double covered = 0;
  for (std::size_t y = 0; y < 50; ++y) {
    for (std::size_t x = 0; x < 50; ++x) {
      covered += picture.Cover(x, y);
      // And each pixel is in or out as its middle is, away from the edge.
      const double from = std::hypot(static_cast<double>(x) + 0.5 - 25, static_cast<double>(y) + 0.5 - 25);
      if (from < 19) {
        EXPECT_EQ(picture.At(x, y), kBlack) << x << "," << y;
      } else if (from > 21) {
        EXPECT_EQ(picture.At(x, y), kWhite) << x << "," << y;
      }
    }
  }
  // The lines stand a quarter of a pixel inside the circle at the most, so
  // it comes out a little under.
  const double area = std::numbers::pi * 20 * 20;
  EXPECT_LT(covered, area + 1);
  EXPECT_GT(covered, area - 20);
}

TEST(Fill, CurvesOutsideTheStripCanBeLeftUncutWithoutChangingThePicture) {
  // Curves, arcs and a ring that reach well beyond the picture on every
  // side, drawn in strips with the flattener told what each strip is and
  // without.
  const std::string_view data =
      "M-30 20C10-60 70 110 130 20S60-40 30 70Q-40 90 10 10T80 80"
      "M50-40A70 70 0 1 1 49-40ZM50 5A30 30 0 1 0 51 5Z"
      "M-20-20Q50 50 120-20T-20 120Z";
  for (const double scale : {1.0, 2.5}) {
    const std::size_t width = static_cast<std::size_t>(100 * scale);
    const std::size_t height = static_cast<std::size_t>(100 * scale);
    const Picture whole = DrawnPath(data, width, height, height, false, scale);
    ASSERT_GT(std::count(whole.gray.begin(), whole.gray.end(), kBlack), 500);
    for (const std::size_t stripRows : {std::size_t{1}, std::size_t{13}, std::size_t{64}}) {
      EXPECT_EQ(DrawnPath(data, width, height, stripRows, false, scale).gray, whole.gray) << stripRows;
      EXPECT_EQ(DrawnPath(data, width, height, stripRows, true, scale).gray, whole.gray) << stripRows;
    }
  }
}

TEST(Fill, ACurveOutsideTheStripIsOneLine) {
  class Counted final : public bn::draw::LineSink {
   public:
    void MoveTo(Point /*to*/) noexcept override {}
    void LineTo(Point /*to*/) noexcept override {}
    void Close() noexcept override {}
  };
  // A curve that would be cut into dozens of lines.
  const auto lines = [](bool kept, Real topLeft, Real bottomRight) {
    Counted counted;
    PathFlattener flattener(counted, Matrix());
    if (kept) {
      flattener.KeepTo(AsPoint(topLeft), AsPoint(bottomRight));
    }
    flattener.MoveTo(AsPoint({100, 100}));
    flattener.CubicTo(AsPoint({200, 300}), AsPoint({400, 300}), AsPoint({500, 100}));
    flattener.QuadTo(AsPoint({300, 250}), AsPoint({100, 100}));
    return flattener.Lines();
  };
  const std::uint32_t cut = lines(false, {}, {});
  EXPECT_GT(cut, 20u);
  // Wholly below, above, right and left of what is wanted: two lines.
  EXPECT_EQ(lines(true, {0, 0}, {800, 100}), 2u);
  EXPECT_EQ(lines(true, {0, 300}, {800, 400}), 2u);
  EXPECT_EQ(lines(true, {0, 0}, {100, 800}), 2u);
  EXPECT_EQ(lines(true, {500, 0}, {900, 800}), 2u);
  // Touching it at all, they are cut as ever.
  EXPECT_EQ(lines(true, {0, 0}, {800, 101}), cut);
  EXPECT_EQ(lines(true, {499, 0}, {900, 800}), cut);
}

// ---- Within a box ----

// A shape drawn in black, cut off at the edges of a box.
Picture DrawnWithin(std::size_t width, std::size_t height, const Shape& shape, Real from, Real to,
                    FillRule rule = FillRule::kNonZero, std::size_t stripRows = 0) {
  Picture picture(width, height);
  stripRows = stripRows == 0 ? height : stripRows;
  std::vector<std::int16_t> cells(Filler::CellsFor(width, stripRows));
  for (std::size_t top = 0; top < height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, height - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(picture.gray).subspan(top * width, rows * width), width,
                             static_cast<std::int32_t>(top), cells));
    Outlined(filler, shape);
    filler.Fill(rule, kBlack, 255, AsPoint(from), AsPoint(to));
    // Whatever the box left out, the whole outline has been used up.
    EXPECT_EQ(std::count(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(width * rows), 0),
              static_cast<std::ptrdiff_t>(width * rows));
  }
  return picture;
}

// A box with its corners anywhere, some of them off the picture.
void AnyBox(std::mt19937& random, double width, double height, Real& from, Real& to) {
  std::uniform_real_distribution<double> across(-0.15 * width, 1.15 * width);
  std::uniform_real_distribution<double> down(-0.15 * height, 1.15 * height);
  const Real one = OnGrid({across(random), down(random)});
  const Real other = OnGrid({across(random), down(random)});
  from = {std::min(one.x, other.x), std::min(one.y, other.y)};
  to = {std::max(one.x, other.x), std::max(one.y, other.y)};
}

TEST(Fill, WithinABoxNothingOutsideItIsDrawnAndWhatIsInsideIsAsEver) {
  std::mt19937 random = Repeatable(61);
  for (int i = 0; i < 200; ++i) {
    const Shape shape = {Tangle(random, 30, 24)};
    const FillRule rule = i % 2 == 0 ? FillRule::kEvenOdd : FillRule::kNonZero;
    Real from;
    Real to;
    AnyBox(random, 30, 24, from, to);
    const Picture whole = Drawn(30, 24, shape, rule);
    const Picture cut = DrawnWithin(30, 24, shape, from, to, rule);
    for (std::size_t y = 0; y < 24; ++y) {
      for (std::size_t x = 0; x < 30; ++x) {
        const auto left = static_cast<double>(x);
        const auto top = static_cast<double>(y);
        const bool inside = left >= from.x && left + 1 <= to.x && top >= from.y && top + 1 <= to.y;
        const bool outside = left + 1 <= from.x || left >= to.x || top + 1 <= from.y || top >= to.y;
        if (inside) {
          ASSERT_EQ(cut.At(x, y), whole.At(x, y)) << i << " at " << x << "," << y;
        } else if (outside) {
          ASSERT_EQ(cut.At(x, y), kWhite) << i << " at " << x << "," << y;
        } else {
          // An edge of the box goes through it: it has some of what it
          // would have had, and no more.
          ASSERT_GE(cut.At(x, y), whole.At(x, y)) << i << " at " << x << "," << y;
        }
      }
    }
  }
}

TEST(Fill, ABoxLeavesAPixelItCrossesTheShareThatLiesInside) {
  // A shape that covers the whole picture, so that what is drawn is the
  // box itself.
  const Shape all = {{{-5, -5}, {40, -5}, {40, 40}, {-5, 40}}};
  std::mt19937 random = Repeatable(62);
  for (int i = 0; i < 100; ++i) {
    Real from;
    Real to;
    AnyBox(random, 30, 24, from, to);
    const Picture cut = DrawnWithin(30, 24, all, from, to);
    for (std::size_t y = 0; y < 24; ++y) {
      for (std::size_t x = 0; x < 30; ++x) {
        const auto left = static_cast<double>(x);
        const auto top = static_cast<double>(y);
        const double across = std::clamp(std::min(left + 1, to.x) - std::max(left, from.x), 0.0, 1.0);
        const double down = std::clamp(std::min(top + 1, to.y) - std::max(top, from.y), 0.0, 1.0);
        ASSERT_NEAR(cut.Cover(x, y), across * down, 1.5 / 255) << i << " at " << x << "," << y;
      }
    }
  }
}

TEST(Fill, ABoxWithNothingInItDrawsNothingAndStillUsesUpTheOutline) {
  const Shape square = {{{2, 2}, {8, 2}, {8, 8}, {2, 8}}};
  const Real boxes[][2] = {
      {{5, 5}, {5, 5}},      // A point.
      {{3, 4}, {7, 4}},      // No height.
      {{7, 7}, {3, 3}},      // Inside out.
      {{20, 20}, {30, 30}},  // Off the picture.
      {{-9, -9}, {0, 0}},
  };
  for (const auto& box : boxes) {
    Picture picture(10, 10);
    std::vector<std::int16_t> cells(Filler::CellsFor(10, 10));
    Filler filler;
    ASSERT_TRUE(filler.Begin(picture.gray, 10, 0, cells));
    Outlined(filler, square);
    filler.Fill(FillRule::kNonZero, kBlack, 255, AsPoint(box[0]), AsPoint(box[1]));
    EXPECT_EQ(std::count(picture.gray.begin(), picture.gray.end(), kWhite), 100) << box[0].x;
    // The outline is gone: filling again draws nothing.
    filler.Fill(FillRule::kNonZero, kBlack);
    EXPECT_EQ(std::count(picture.gray.begin(), picture.gray.end(), kWhite), 100) << box[0].x;
  }
}

TEST(Fill, WithinABoxItComesOutTheSameHoweverThePictureIsCutIntoStrips) {
  std::mt19937 random = Repeatable(63);
  for (int i = 0; i < 60; ++i) {
    const Shape shape = {Tangle(random, 30, 24), NoDents(random, 30, 24)};
    Real from;
    Real to;
    AnyBox(random, 30, 24, from, to);
    const Picture whole = DrawnWithin(30, 24, shape, from, to);
    for (const std::size_t stripRows : {std::size_t{1}, std::size_t{5}, std::size_t{23}, std::size_t{64}}) {
      ASSERT_EQ(DrawnWithin(30, 24, shape, from, to, FillRule::kNonZero, stripRows).gray, whole.gray)
          << i << " in strips of " << stripRows;
    }
  }
}

TEST(Fill, ABoxLetsWhatIsUnderShowThroughAThinShapeAsEver) {
  // Half as solid, and half of the pixel inside the box: a quarter of the
  // way from what was there to the gray that is laid on.
  Picture picture(4, 4);
  std::vector<std::int16_t> cells(Filler::CellsFor(4, 4));
  Filler filler;
  ASSERT_TRUE(filler.Begin(picture.gray, 4, 0, cells));
  Outlined(filler, {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}});
  filler.Fill(FillRule::kNonZero, kBlack, 128, AsPoint({1.5, 1}), AsPoint({3, 3}));
  EXPECT_EQ(picture.At(0, 1), kWhite);
  EXPECT_NEAR(picture.At(1, 1), 255 - 64, 1);
  EXPECT_NEAR(picture.At(2, 1), 255 - 128, 1);
  EXPECT_EQ(picture.At(3, 1), kWhite);
  EXPECT_EQ(picture.At(2, 0), kWhite);
  EXPECT_EQ(picture.At(2, 3), kWhite);
}

}  // namespace
