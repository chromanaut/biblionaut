#include "draw/stroke.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <random>
#include <vector>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_path.h"

// The stroker's pieces are drawn here by the filler, and set beside where
// the stroke should be, found another way: for round corners and ends, a
// point is in the stroke if it is no further from the outline than half
// the width, and for the others by the corner's or end's own shape.

namespace {

using bn::draw::Angle;
using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::PathSink;
using bn::draw::Point;
using bn::draw::StrokeCap;
using bn::draw::StrokeJoin;
using bn::draw::Stroker;
using bn::draw::StrokeStyle;

struct Real {
  double x = 0;
  double y = 0;
};

constexpr std::uint8_t kWhite = 255;

Point AsPoint(Real point) {
  return {.x = static_cast<Fixed>(std::lround(point.x * kFixedOne)),
          .y = static_cast<Fixed>(std::lround(point.y * kFixedOne))};
}

Fixed Of(double value) { return static_cast<Fixed>(std::lround(value * kFixedOne)); }

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

StrokeStyle Style(double width, StrokeJoin join, StrokeCap cap) {
  StrokeStyle style;
  style.width = Of(width);
  style.join = join;
  style.cap = cap;
  return style;
}

// A picture the stroke is drawn on, white until it is.
struct Picture {
  static constexpr std::size_t kSize = 64;
  std::vector<std::uint8_t> gray = std::vector<std::uint8_t>(kSize * kSize, kWhite);

  // How much of a pixel is covered, from 0 to 1.
  double Cover(std::size_t x, std::size_t y) const { return (kWhite - gray[(y * kSize) + x]) / 255.0; }
};

// Strokes `parts` (each a list of points, closed if it says so) and draws
// the stroke black. Returns the stroker, to be asked what it found.
struct Part {
  std::vector<Real> points;
  bool closed = false;
};

Stroker Draw(Picture& picture, const StrokeStyle& style, const std::vector<Part>& parts) {
  Filler filler;
  std::vector<std::int16_t> cells(Filler::CellsFor(Picture::kSize, Picture::kSize));
  EXPECT_TRUE(filler.Begin(picture.gray, Picture::kSize, 0, cells));
  PathFlattener out(filler, Matrix());
  Stroker stroker;
  stroker.Begin(out, style);
  for (const Part& part : parts) {
    stroker.MoveTo(AsPoint(part.points.front()));
    for (std::size_t i = 1; i < part.points.size(); ++i) {
      stroker.LineTo(AsPoint(part.points[i]));
    }
    if (part.closed) {
      stroker.Close();
    }
  }
  stroker.Finish();
  filler.Fill(FillRule::kNonZero, 0);
  return stroker;
}

double DistanceToSegment(Real p, Real a, Real b) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double length = (dx * dx) + (dy * dy);
  const double t = length == 0 ? 0 : std::clamp((((p.x - a.x) * dx) + ((p.y - a.y) * dy)) / length, 0.0, 1.0);
  return std::hypot(p.x - (a.x + (t * dx)), p.y - (a.y + (t * dy)));
}

// How much of each pixel a test says should be covered, found by trying
// sixteen by sixteen points in it.
template <typename Inside>
std::vector<double> Expected(Inside inside) {
  constexpr int kTries = 16;
  std::vector<double> cover(Picture::kSize * Picture::kSize);
  for (std::size_t y = 0; y < Picture::kSize; ++y) {
    for (std::size_t x = 0; x < Picture::kSize; ++x) {
      int in = 0;
      for (int j = 0; j < kTries; ++j) {
        for (int i = 0; i < kTries; ++i) {
          const Real point = {.x = static_cast<double>(x) + ((i + 0.5) / kTries),
                              .y = static_cast<double>(y) + ((j + 0.5) / kTries)};
          in += inside(point) ? 1 : 0;
        }
      }
      cover[(y * Picture::kSize) + x] = in / static_cast<double>(kTries * kTries);
    }
  }
  return cover;
}

// The largest difference between what was drawn and what should be, the
// largest where it was drawn lighter than it should be, and the mean.
struct Apart {
  double worst = 0;
  double lighter = 0;
  double mean = 0;
};

Apart Compare(const Picture& picture, const std::vector<double>& expected) {
  Apart apart;
  for (std::size_t y = 0; y < Picture::kSize; ++y) {
    for (std::size_t x = 0; x < Picture::kSize; ++x) {
      const double difference = picture.Cover(x, y) - expected[(y * Picture::kSize) + x];
      apart.worst = std::max(apart.worst, std::abs(difference));
      apart.lighter = std::max(apart.lighter, -difference);
      apart.mean += std::abs(difference);
    }
  }
  apart.mean /= Picture::kSize * Picture::kSize;
  return apart;
}

// Whether a point is within `half` of any line of an outline: the stroke
// with round corners and round ends.
bool NearOutline(Real p, const std::vector<Real>& points, double half, bool closed) {
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    if (DistanceToSegment(p, points[i], points[i + 1]) <= half) {
      return true;
    }
  }
  return closed && DistanceToSegment(p, points.back(), points.front()) <= half;
}

TEST(Stroker, ALineWithButtEndsIsARectangleAsLongAsTheLine) {
  Picture picture;
  Draw(picture, Style(6, StrokeJoin::kMiter, StrokeCap::kButt), {{.points = {{10, 20}, {50, 20}}}});
  const Apart apart =
      Compare(picture, Expected([](Real p) { return p.x >= 10 && p.x <= 50 && p.y >= 17 && p.y <= 23; }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, ASquareEndReachesHalfTheWidthPastTheLine) {
  Picture picture;
  Draw(picture, Style(6, StrokeJoin::kMiter, StrokeCap::kSquare), {{.points = {{10, 20}, {50, 20}}}});
  const Apart apart =
      Compare(picture, Expected([](Real p) { return p.x >= 7 && p.x <= 53 && p.y >= 17 && p.y <= 23; }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, RoundCornersAndEndsAreEverythingWithinHalfTheWidth) {
  const std::vector<Real> points = {{8, 50}, {20, 10}, {35, 40}, {56, 12}, {58, 55}};
  Picture picture;
  Draw(picture, Style(7, StrokeJoin::kRound, StrokeCap::kRound), {{.points = points}});
  const Apart apart = Compare(picture, Expected([&points](Real p) { return NearOutline(p, points, 3.5, false); }));
  EXPECT_LT(apart.worst, 0.1);
  EXPECT_LT(apart.mean, 0.003);
}

TEST(Stroker, AWideStrokeAlongACurveIsAsDarkAsItShouldBeOnBothEdges) {
  // A circle as a curve comes to the stroker: many short lines, each
  // turning a little from the one before. The pieces meet without
  // overlapping, so the inner edge is no darker than the outer.
  std::vector<Real> points;
  for (int i = 0; i < 64; ++i) {
    const double angle = i * 2 * std::numbers::pi / 64;
    points.push_back({32 + (20 * std::cos(angle)), 32 + (20 * std::sin(angle))});
  }
  // A point can be nearest only the line its angle lies in, or one beside it.
  const auto near = [&points](Real p, double half) {
    const double angle = std::atan2(p.y - 32, p.x - 32);
    const auto line = static_cast<int>(
        std::floor((angle < 0 ? angle + (2 * std::numbers::pi) : angle) / (2 * std::numbers::pi / 64)));
    for (int i = line - 1; i <= line + 1; ++i) {
      const auto from = static_cast<std::size_t>((i + 64) % 64);
      if (DistanceToSegment(p, points[from], points[(from + 1) % 64]) <= half) {
        return true;
      }
    }
    return false;
  };
  for (const double width : {1.0, 3.0, 6.0, 12.0}) {
    const std::vector<double> expected = Expected([&](Real p) { return near(p, width / 2); });
    for (const StrokeJoin join : {StrokeJoin::kMiter, StrokeJoin::kRound, StrokeJoin::kBevel}) {
      Picture picture;
      Draw(picture, Style(width, join, StrokeCap::kButt), {{.points = points, .closed = true}});
      EXPECT_LT(Compare(picture, expected).worst, 0.02) << "width " << width;
    }
  }
}

TEST(Stroker, AnOutlineThatCrossesItselfIsStrokedWhereverItGoes) {
  // Where the stroke goes over itself it is no less black: every piece
  // goes round the same way, so they add up. Where an edge of the stroke
  // crosses an edge of where it goes over itself, a pixel can come out
  // darker than it should (see the class comment), and no lighter.
  const std::vector<Real> points = {{10, 10}, {54, 54}, {54, 10}, {10, 54}, {32, 5}};
  Picture picture;
  Draw(picture, Style(9, StrokeJoin::kRound, StrokeCap::kRound), {{.points = points, .closed = true}});
  const Apart apart = Compare(picture, Expected([&points](Real p) { return NearOutline(p, points, 4.5, true); }));
  EXPECT_LT(apart.lighter, 0.02);
  EXPECT_LT(apart.mean, 0.003);
}

TEST(Stroker, RandomOutlinesWithRoundCornersComeOutAsTheirDistance) {
  // Random points make outlines that cross themselves and turn sharply
  // between short lines, where pieces overlap: there pixels come out
  // darker, and never lighter.
  std::mt19937 random = Repeatable(7);
  std::uniform_real_distribution<double> place(8.0, 56.0);
  std::uniform_real_distribution<double> width(0.5, 12.0);
  for (int round = 0; round < 24; ++round) {
    std::vector<Real> points;
    const int count = 2 + (round % 6);
    points.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      points.push_back({place(random), place(random)});
    }
    const double half = width(random) / 2;
    const bool closed = round % 3 == 0;
    Picture picture;
    Draw(picture, Style(half * 2, StrokeJoin::kRound, StrokeCap::kRound), {{.points = points, .closed = closed}});
    const Apart apart = Compare(picture, Expected([&](Real p) { return NearOutline(p, points, half, closed); }));
    EXPECT_LT(apart.lighter, 0.04) << "round " << round;
    EXPECT_LT(apart.mean, 0.006) << "round " << round;
  }
}

TEST(Stroker, AMiteredCornerIsSquareAndABevelledOneIsCutAcross) {
  // A right angle, the outside of its corner towards the top right.
  const std::vector<Part> corner = {{.points = {{10, 20}, {40, 20}, {40, 55}}}};
  Picture mitered;
  Draw(mitered, Style(8, StrokeJoin::kMiter, StrokeCap::kButt), corner);
  Picture bevelled;
  Draw(bevelled, Style(8, StrokeJoin::kBevel, StrokeCap::kButt), corner);
  // The pixel at the corner's outer tip, from (43, 16) to (44, 17).
  EXPECT_GT(mitered.Cover(43, 16), 0.98);
  EXPECT_LT(bevelled.Cover(43, 16), 0.02);
  // A miter is the two sides carried on until they meet.
  const Apart apart = Compare(mitered, Expected([](Real p) {
                                return (p.x >= 10 && p.x <= 44 && p.y >= 16 && p.y <= 24) ||
                                       (p.x >= 36 && p.x <= 44 && p.y >= 16 && p.y <= 55);
                              }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, AMiterLongerThanTheLimitIsBevelled) {
  // A corner of 20 degrees, whose miter reaches 5.76 widths from it,
  // along the line that halves it: to about (56.3, 28).
  const std::vector<Part> sharp = {{.points = {{5, 30}, {45, 30}, {5, 44.56}}}};
  StrokeStyle style = Style(4, StrokeJoin::kMiter, StrokeCap::kButt);
  Picture within;
  style.miterLimit = Of(6);
  Draw(within, style, sharp);
  Picture beyond;
  style.miterLimit = Of(5);
  Draw(beyond, style, sharp);
  // Beyond the corner, along where the miter would be.
  EXPECT_GT(within.Cover(50, 28), 0.5);
  EXPECT_LT(beyond.Cover(50, 28), 0.02);
}

TEST(Stroker, AClosedOutlineHasACornerWhereItBeganAndNoEnds) {
  const std::vector<Part> square = {{.points = {{16, 16}, {48, 16}, {48, 48}, {16, 48}}, .closed = true}};
  Picture picture;
  Draw(picture, Style(8, StrokeJoin::kMiter, StrokeCap::kRound), square);
  const Apart apart = Compare(picture, Expected([](Real p) {
                                const bool outer = p.x >= 12 && p.x <= 52 && p.y >= 12 && p.y <= 52;
                                const bool inner = p.x > 20 && p.x < 44 && p.y > 20 && p.y < 44;
                                return outer && !inner;
                              }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, DashesAndGapsGoAlongTheLineFromTheOffset) {
  StrokeStyle style = Style(4, StrokeJoin::kMiter, StrokeCap::kButt);
  style.dashes = {8, 4};
  style.dashes[0] = Of(8);
  style.dashes[1] = Of(4);
  style.dashCount = 2;
  style.dashOffset = Of(2);
  Picture picture;
  Draw(picture, style, {{.points = {{4, 30}, {60, 30}}}});
  // From 4: a dash of 6 (8 less the offset), a gap of 4, a dash of 8...
  const Apart apart = Compare(picture, Expected([](Real p) {
                                if (p.y < 28 || p.y > 32 || p.x < 4 || p.x > 60) {
                                  return false;
                                }
                                const double along = std::fmod(p.x - 4 + 2, 12);
                                return along <= 8;
                              }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, AnOddNumberOfDashLengthsIsTakenTwice) {
  StrokeStyle style = Style(2, StrokeJoin::kMiter, StrokeCap::kButt);
  style.dashes[0] = Of(5);
  style.dashCount = 1;
  Picture picture;
  Draw(picture, style, {{.points = {{0, 10}, {60, 10}}}});
  const Apart apart =
      Compare(picture, Expected([](Real p) { return p.y >= 9 && p.y <= 11 && p.x <= 60 && std::fmod(p.x, 10) <= 5; }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, DashesGoRoundCornersWithTheirJoins) {
  // A dash of 30 from (10, 10): 20 along the top and 10 down the side,
  // with the corner between filled.
  StrokeStyle style = Style(4, StrokeJoin::kMiter, StrokeCap::kButt);
  style.dashes[0] = Of(30);
  style.dashes[1] = Of(100);
  style.dashCount = 2;
  Picture picture;
  Draw(picture, style, {{.points = {{10, 10}, {30, 10}, {30, 50}}}});
  const Apart apart = Compare(picture, Expected([](Real p) {
                                return (p.x >= 10 && p.x <= 32 && p.y >= 8 && p.y <= 12) ||
                                       (p.x >= 28 && p.x <= 32 && p.y >= 8 && p.y <= 20);
                              }));
  EXPECT_LT(apart.worst, 0.02);
}

TEST(Stroker, AnUnusablePatternMakesASolidStroke) {
  for (const std::initializer_list<double> lengths :
       {std::initializer_list<double>{0, 0}, std::initializer_list<double>{5, -1}}) {
    StrokeStyle style = Style(4, StrokeJoin::kMiter, StrokeCap::kButt);
    for (const double length : lengths) {
      style.dashes[style.dashCount] = Of(length);
      ++style.dashCount;
    }
    Picture picture;
    Draw(picture, style, {{.points = {{10, 20}, {50, 20}}}});
    const Apart apart =
        Compare(picture, Expected([](Real p) { return p.x >= 10 && p.x <= 50 && p.y >= 18 && p.y <= 22; }));
    EXPECT_LT(apart.worst, 0.02);
  }
}

TEST(Stroker, DashesOfNoLengthWithRoundEndsAreDots) {
  StrokeStyle style = Style(6, StrokeJoin::kMiter, StrokeCap::kRound);
  style.dashes[0] = 0;
  style.dashes[1] = Of(16);
  style.dashCount = 2;
  Picture picture;
  Draw(picture, style, {{.points = {{10, 30}, {60, 30}}}});
  // Dots at 10, 26 and 42, and at 58.
  const Apart apart = Compare(picture, Expected([](Real p) {
                                for (const double x : {10.0, 26.0, 42.0, 58.0}) {
                                  if (std::hypot(p.x - x, p.y - 30) <= 3) {
                                    return true;
                                  }
                                }
                                return false;
                              }));
  EXPECT_LT(apart.worst, 0.1);
}

TEST(Stroker, APartWithNoLengthIsADotOnlyIfTheEndsAreRoundOrSquare) {
  const std::vector<Part> dot = {{.points = {{32, 32}, {32, 32}}}};
  Picture round;
  Draw(round, Style(10, StrokeJoin::kMiter, StrokeCap::kRound), dot);
  EXPECT_LT(Compare(round, Expected([](Real p) { return std::hypot(p.x - 32, p.y - 32) <= 5; })).worst, 0.1);
  Picture square;
  Draw(square, Style(10, StrokeJoin::kMiter, StrokeCap::kSquare), dot);
  EXPECT_LT(Compare(square, Expected([](Real p) { return std::abs(p.x - 32) <= 5 && std::abs(p.y - 32) <= 5; })).worst,
            0.02);
  Picture butt;
  Draw(butt, Style(10, StrokeJoin::kMiter, StrokeCap::kButt), dot);
  EXPECT_EQ(Compare(butt, Expected([](Real) { return false; })).worst, 0);
  // A part that is only a move goes nowhere, and is not drawn at all.
  Picture moved;
  Draw(moved, Style(10, StrokeJoin::kMiter, StrokeCap::kRound), {{.points = {{32, 32}}}});
  EXPECT_EQ(Compare(moved, Expected([](Real) { return false; })).worst, 0);
}

// Counts what the stroker gives, and draws nothing.
class Counter final : public PathSink {
 public:
  std::uint32_t lines = 0;
  void MoveTo(Point /*to*/) noexcept override {}
  void LineTo(Point /*to*/) noexcept override { ++lines; }
  void QuadTo(Point /*control*/, Point /*to*/) noexcept override {}
  void CubicTo(Point /*control1*/, Point /*control2*/, Point /*to*/) noexcept override {}
  void ArcTo(Fixed /*radiusX*/, Fixed /*radiusY*/, Angle /*turn*/, bool /*largeArc*/, bool /*sweep*/,
             Point /*to*/) noexcept override {}
  void Close() noexcept override { ++lines; }
};

TEST(Stroker, ALineComesToNoMoreThanItsBound) {
  std::mt19937 random = Repeatable(11);
  std::uniform_int_distribution<Fixed> place(-2000000000, 2000000000);
  for (const StrokeJoin join : {StrokeJoin::kMiter, StrokeJoin::kRound, StrokeJoin::kBevel}) {
    for (const StrokeCap cap : {StrokeCap::kButt, StrokeCap::kRound, StrokeCap::kSquare}) {
      Counter counter;
      Stroker stroker;
      StrokeStyle style = Style(5000, join, cap);
      stroker.Begin(counter, style);
      stroker.MoveTo({.x = place(random), .y = place(random)});
      std::uint32_t most = 0;
      for (int i = 0; i < 2000; ++i) {
        const std::uint32_t before = counter.lines;
        stroker.LineTo({.x = place(random), .y = place(random)});
        most = std::max(most, counter.lines - before);
      }
      const std::uint32_t before = counter.lines;
      stroker.Close();
      EXPECT_LE(most, Stroker::kMostLinesPerLine);
      // Closing is a line back, and the end.
      EXPECT_LE(counter.lines - before, Stroker::kMostLinesPerLine + Stroker::kMostLinesPerEnd);
      // A dot.
      const std::uint32_t dotFrom = counter.lines;
      stroker.MoveTo({.x = 0, .y = 0});
      stroker.LineTo({.x = 0, .y = 0});
      stroker.Finish();
      EXPECT_LE(counter.lines - dotFrom, Stroker::kMostLinesPerEnd);
    }
  }
}

TEST(Stroker, ADashPatternCanBeginNoMoreThanItsBoundOfDashes) {
  Counter counter;
  Stroker stroker;
  StrokeStyle style = Style(1, StrokeJoin::kMiter, StrokeCap::kRound);
  style.dashes[0] = 1;  // A 4,096th, and a gap as long.
  style.dashCount = 1;
  stroker.Begin(counter, style);
  stroker.MoveTo({.x = 0, .y = 0});
  stroker.LineTo({.x = 2000000000, .y = 0});
  stroker.Finish();
  EXPECT_TRUE(stroker.DashesCut());
  // Each dash a line and its end, and the rest one line and its end.
  EXPECT_LE(counter.lines,
            (Stroker::kMostDashesPerOutline + 1) * (Stroker::kMostLinesPerLine + Stroker::kMostLinesPerEnd));
}

TEST(Stroker, PointsBeyondWhatAPositionCanBeAreHeldAndSaid) {
  Counter counter;
  Stroker stroker;
  stroker.Begin(counter, Style(1000, StrokeJoin::kMiter, StrokeCap::kSquare));
  stroker.MoveTo({.x = 2147483000, .y = 0});
  stroker.LineTo({.x = 2147483600, .y = 0});
  stroker.Finish();
  EXPECT_TRUE(stroker.Clamped());
}

}  // namespace
