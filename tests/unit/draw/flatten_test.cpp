#include "draw/flatten.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

// The flattener works in whole numbers. Here what it draws is set beside
// the same curves and arcs worked out in floating point, by the formulas
// that define them and by none of the flattener's own steps.

namespace {

using bn::draw::Angle;
using bn::draw::Coefficient;
using bn::draw::Fixed;
using bn::draw::kCoefficientOne;
using bn::draw::kFixedMax;
using bn::draw::kFixedMin;
using bn::draw::kFixedOne;
using bn::draw::LineSink;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::Point;

struct Real {
  double x = 0;
  double y = 0;
};

constexpr double kTolerance = 0.25;  // PathFlattener::kDefaultTolerance, in pixels.

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

Fixed AsLength(double value) { return static_cast<Fixed>(std::lround(value * kFixedOne)); }
Point AsPoint(Real point) { return {.x = AsLength(point.x), .y = AsLength(point.y)}; }
Real AsReal(Point point) {
  return {static_cast<double>(point.x) / kFixedOne, static_cast<double>(point.y) / kFixedOne};
}
// The point a Real becomes once it has been rounded to what a Point holds.
Real Rounded(Real point) { return AsReal(AsPoint(point)); }
Angle AsAngle(double degrees) {
  return static_cast<Angle>(static_cast<std::int64_t>(std::llround(degrees / 360.0 * 4294967296.0)));
}

// Keeps the lines it is given, as the outlines they make up.
class Kept final : public LineSink {
 public:
  void MoveTo(Point to) noexcept override {
    outlines_.emplace_back();
    outlines_.back().push_back(AsReal(to));
    closed_.push_back(false);
  }
  void LineTo(Point to) noexcept override { outlines_.back().push_back(AsReal(to)); }
  void Close() noexcept override { closed_.back() = true; }

  const std::vector<std::vector<Real>>& Outlines() const { return outlines_; }
  const std::vector<Real>& Only() const {
    EXPECT_EQ(outlines_.size(), 1u);
    return outlines_.front();
  }
  bool Closed(std::size_t index) const { return closed_[index]; }

 private:
  std::vector<std::vector<Real>> outlines_;
  std::vector<bool> closed_;
};

double Distance(Real a, Real b) { return std::hypot(a.x - b.x, a.y - b.y); }

// How far `point` is from the nearest part of the line from `a` to `b`.
double DistanceToLine(Real point, Real a, Real b) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double length = (dx * dx) + (dy * dy);
  if (length == 0) {
    return Distance(point, a);
  }
  const double along = std::clamp((((point.x - a.x) * dx) + ((point.y - a.y) * dy)) / length, 0.0, 1.0);
  return Distance(point, {a.x + (along * dx), a.y + (along * dy)});
}

// How far along a run of lines each of its points is.
std::vector<double> LengthsAlong(const std::vector<Real>& lines) {
  std::vector<double> lengths = {0.0};
  for (std::size_t i = 1; i < lines.size(); ++i) {
    lengths.push_back(lengths.back() + Distance(lines[i - 1], lines[i]));
  }
  return lengths;
}

// The furthest that any point of `from` is from `onto`, where both follow
// the same curve from its start to its end. With `middles`, the middle of
// each line of `from` is measured as well as its ends.
//
// A point is measured to the lines of `onto` that are as far along it as
// the point is along `from`, give or take a few pixels: so the two are
// compared along their length, and a point is not excused by being near
// some other part of a curve that crosses or doubles back on itself.
double Stray(const std::vector<Real>& from, bool middles, const std::vector<Real>& onto) {
  // How far the two may be out of step along their length.
  constexpr double kSlack = 6.0;
  const std::vector<double> along = LengthsAlong(from);
  const std::vector<double> lengths = LengthsAlong(onto);
  const double scale = along.back() > 0 ? lengths.back() / along.back() : 0;
  double furthest = 0;
  std::size_t first = 1;
  const auto measure = [&](Real point, double at) {
    if (onto.size() < 2) {
      furthest = std::max(furthest, Distance(point, onto.front()));
      return;
    }
    const double target = at * scale;
    while (first + 1 < onto.size() && lengths[first] < target - kSlack) {
      ++first;
    }
    double nearest = DistanceToLine(point, onto[first - 1], onto[first]);
    for (std::size_t i = first + 1; i < onto.size() && lengths[i - 1] <= target + kSlack; ++i) {
      nearest = std::min(nearest, DistanceToLine(point, onto[i - 1], onto[i]));
    }
    furthest = std::max(furthest, nearest);
  };
  for (std::size_t i = 0; i < from.size(); ++i) {
    if (middles && i > 0) {
      measure({(from[i - 1].x + from[i].x) / 2, (from[i - 1].y + from[i].y) / 2}, (along[i - 1] + along[i]) / 2);
    }
    measure(from[i], along[i]);
  }
  return furthest;
}

// How far apart the lines the flattener drew and the true curve are at
// their worst, the curve being given as many points along it: the furthest
// that any point of the one is from the other. The middle of a line is
// where it stands furthest from the curve it cuts across.
double FurthestApart(const std::vector<Real>& drawn, const std::vector<Real>& curve) {
  return std::max(Stray(curve, false, drawn), Stray(drawn, true, curve));
}

// A matrix in floating point.
struct RealMatrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

  Real Apply(Real p) const { return {(a * p.x) + (c * p.y) + e, (b * p.x) + (d * p.y) + f}; }
  Matrix AsMatrix() const {
    Matrix m;
    m.a = static_cast<Coefficient>(std::lround(a * kCoefficientOne));
    m.b = static_cast<Coefficient>(std::lround(b * kCoefficientOne));
    m.c = static_cast<Coefficient>(std::lround(c * kCoefficientOne));
    m.d = static_cast<Coefficient>(std::lround(d * kCoefficientOne));
    m.e = AsLength(e);
    m.f = AsLength(f);
    return m;
  }
};

// How many places along a true curve it is worked out at.
constexpr int kSamples = 1024;

std::vector<Real> CubicPoints(Real p0, Real p1, Real p2, Real p3, const RealMatrix& matrix) {
  std::vector<Real> points;
  for (int i = 0; i <= kSamples; ++i) {
    const double t = static_cast<double>(i) / kSamples;
    const double s = 1 - t;
    points.push_back(
        matrix.Apply({(s * s * s * p0.x) + (3 * s * s * t * p1.x) + (3 * s * t * t * p2.x) + (t * t * t * p3.x),
                      (s * s * s * p0.y) + (3 * s * s * t * p1.y) + (3 * s * t * t * p2.y) + (t * t * t * p3.y)}));
  }
  return points;
}

std::vector<Real> QuadPoints(Real p0, Real p1, Real p2, const RealMatrix& matrix) {
  std::vector<Real> points;
  for (int i = 0; i <= kSamples; ++i) {
    const double t = static_cast<double>(i) / kSamples;
    const double s = 1 - t;
    points.push_back(matrix.Apply(
        {(s * s * p0.x) + (2 * s * t * p1.x) + (t * t * p2.x), (s * s * p0.y) + (2 * s * t * p1.y) + (t * t * p2.y)}));
  }
  return points;
}

// An arc as SVG defines one: its ellipse's middle, its radii once any
// that were too short have been lengthened, where on the ellipse it starts
// and how far round it goes.
struct TrueArc {
  double middleX = 0, middleY = 0, radiusX = 0, radiusY = 0, turn = 0, start = 0, round = 0;

  Real At(double angle) const {
    const double x = radiusX * std::cos(angle);
    const double y = radiusY * std::sin(angle);
    return {middleX + (std::cos(turn) * x) - (std::sin(turn) * y),
            middleY + (std::sin(turn) * x) + (std::cos(turn) * y)};
  }
  std::vector<Real> Points(const RealMatrix& matrix) const {
    std::vector<Real> points;
    for (int i = 0; i <= kSamples; ++i) {
      points.push_back(matrix.Apply(At(start + (round * i / kSamples))));
    }
    return points;
  }
};

double AngleFrom(double ux, double uy, double vx, double vy) {
  return std::atan2((ux * vy) - (uy * vx), (ux * vx) + (uy * vy));
}

// The working set out in the SVG specification for turning an arc's two
// ends, two radii and two flags into the ellipse it lies on.
TrueArc Solve(Real from, double radiusX, double radiusY, double degrees, bool largeArc, bool sweep, Real to) {
  TrueArc arc;
  arc.turn = degrees / 180.0 * std::numbers::pi;
  arc.radiusX = std::abs(radiusX);
  arc.radiusY = std::abs(radiusY);
  const double cosine = std::cos(arc.turn);
  const double sine = std::sin(arc.turn);
  const double halfX = (cosine * (from.x - to.x) / 2) + (sine * (from.y - to.y) / 2);
  const double halfY = (-sine * (from.x - to.x) / 2) + (cosine * (from.y - to.y) / 2);
  const double reach =
      ((halfX * halfX) / (arc.radiusX * arc.radiusX)) + ((halfY * halfY) / (arc.radiusY * arc.radiusY));
  if (reach > 1) {
    arc.radiusX *= std::sqrt(reach);
    arc.radiusY *= std::sqrt(reach);
  }
  const double rx2 = arc.radiusX * arc.radiusX;
  const double ry2 = arc.radiusY * arc.radiusY;
  const double under = (rx2 * halfY * halfY) + (ry2 * halfX * halfX);
  double factor = std::sqrt(std::max(0.0, ((rx2 * ry2) - under) / under));
  if (largeArc == sweep) {
    factor = -factor;
  }
  const double centreX = factor * arc.radiusX * halfY / arc.radiusY;
  const double centreY = -factor * arc.radiusY * halfX / arc.radiusX;
  arc.middleX = (cosine * centreX) - (sine * centreY) + ((from.x + to.x) / 2);
  arc.middleY = (sine * centreX) + (cosine * centreY) + ((from.y + to.y) / 2);
  const double startX = (halfX - centreX) / arc.radiusX;
  const double startY = (halfY - centreY) / arc.radiusY;
  const double endX = (-halfX - centreX) / arc.radiusX;
  const double endY = (-halfY - centreY) / arc.radiusY;
  arc.start = AngleFrom(1, 0, startX, startY);
  arc.round = AngleFrom(startX, startY, endX, endY);
  if (!sweep && arc.round > 0) {
    arc.round -= 2 * std::numbers::pi;
  } else if (sweep && arc.round < 0) {
    arc.round += 2 * std::numbers::pi;
  }
  return arc;
}

TEST(Flatten, PutsEveryPointThroughTheMatrix) {
  Kept kept;
  RealMatrix matrix;
  matrix.a = 2;
  matrix.d = 0.5;
  matrix.e = 10;
  matrix.f = -20;
  PathFlattener flattener(kept, matrix.AsMatrix());
  flattener.MoveTo(AsPoint({1, 2}));
  flattener.LineTo(AsPoint({3, 4}));
  flattener.LineTo(AsPoint({-5, 6}));
  flattener.Close();
  const std::vector<Real>& lines = kept.Only();
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].x, 12);
  EXPECT_EQ(lines[0].y, -19);
  EXPECT_EQ(lines[1].x, 16);
  EXPECT_EQ(lines[1].y, -18);
  EXPECT_EQ(lines[2].x, 0);
  EXPECT_EQ(lines[2].y, -17);
  EXPECT_TRUE(kept.Closed(0));
  EXPECT_EQ(flattener.Lines(), 2u);
  EXPECT_FALSE(flattener.Clamped());
}

TEST(Flatten, EachMoveBeginsAnotherOutline) {
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo(AsPoint({0, 0}));
  flattener.LineTo(AsPoint({10, 0}));
  flattener.Close();
  // After closing, the pen is back where the outline began.
  flattener.MoveTo(AsPoint({5, 5}));
  flattener.QuadTo(AsPoint({6, 9}), AsPoint({7, 5}));
  ASSERT_EQ(kept.Outlines().size(), 2u);
  EXPECT_TRUE(kept.Closed(0));
  EXPECT_FALSE(kept.Closed(1));
  EXPECT_EQ(kept.Outlines()[1].front().x, 5);
  EXPECT_EQ(kept.Outlines()[1].back().x, 7);
}

TEST(Flatten, ACurveThatIsStraightIsOneLine) {
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo(AsPoint({0, 0}));
  flattener.CubicTo(AsPoint({10, 10}), AsPoint({20, 20}), AsPoint({30, 30}));
  flattener.QuadTo(AsPoint({40, 30}), AsPoint({50, 30}));
  EXPECT_EQ(flattener.Lines(), 2u);
  // A curve that bends by less than the tolerance is one line too.
  flattener.CubicTo(AsPoint({60, 30.2}), AsPoint({70, 30.2}), AsPoint({80, 30}));
  EXPECT_EQ(flattener.Lines(), 3u);
}

TEST(Flatten, ACubicStaysWithinTheToleranceOfTheCurve) {
  std::mt19937 random = Repeatable(9);
  std::uniform_real_distribution<double> place(-100.0, 900.0);
  double worst = 0;
  std::uint32_t most = 0;
  for (int i = 0; i < 300; ++i) {
    const Real p0 = Rounded({place(random), place(random)});
    const Real p1 = Rounded({place(random), place(random)});
    const Real p2 = Rounded({place(random), place(random)});
    const Real p3 = Rounded({place(random), place(random)});
    Kept kept;
    PathFlattener flattener(kept, Matrix());
    flattener.MoveTo(AsPoint(p0));
    flattener.CubicTo(AsPoint(p1), AsPoint(p2), AsPoint(p3));
    const std::vector<Real>& lines = kept.Only();
    EXPECT_EQ(lines.back().x, p3.x);
    EXPECT_EQ(lines.back().y, p3.y);
    worst = std::max(worst, FurthestApart(lines, CubicPoints(p0, p1, p2, p3, RealMatrix())));
    most = std::max(most, flattener.Lines());
  }
  // Within the tolerance, and a little for the rounding of each point and
  // for the curve being known only at so many places.
  EXPECT_LT(worst, kTolerance + 0.02);
  // Curves this size are not cut to pieces: a few dozen lines at the most.
  EXPECT_LT(most, 100u);
  EXPECT_GT(most, 8u);
}

TEST(Flatten, AQuadraticStaysWithinTheToleranceOfTheCurve) {
  std::mt19937 random = Repeatable(10);
  std::uniform_real_distribution<double> place(-100.0, 900.0);
  double worst = 0;
  for (int i = 0; i < 300; ++i) {
    const Real p0 = Rounded({place(random), place(random)});
    const Real p1 = Rounded({place(random), place(random)});
    const Real p2 = Rounded({place(random), place(random)});
    Kept kept;
    PathFlattener flattener(kept, Matrix());
    flattener.MoveTo(AsPoint(p0));
    flattener.QuadTo(AsPoint(p1), AsPoint(p2));
    const std::vector<Real>& lines = kept.Only();
    EXPECT_EQ(lines.back().x, p2.x);
    EXPECT_EQ(lines.back().y, p2.y);
    worst = std::max(worst, FurthestApart(lines, QuadPoints(p0, p1, p2, RealMatrix())));
  }
  EXPECT_LT(worst, kTolerance + 0.02);
}

TEST(Flatten, ACurveIsFlattenedWhereItLandsNotWhereItWasDrawn) {
  // The same curve drawn small and brought up a hundred times takes more
  // lines than it does at its own size, and stays as close.
  const Real p0 = {0, 0};
  const Real p1 = {1, 3};
  const Real p2 = {4, 3};
  const Real p3 = {5, 0};
  std::uint32_t small = 0;
  for (const double scale : {1.0, 100.0}) {
    RealMatrix matrix;
    matrix.a = scale;
    matrix.d = scale;
    Kept kept;
    PathFlattener flattener(kept, matrix.AsMatrix());
    flattener.MoveTo(AsPoint(p0));
    flattener.CubicTo(AsPoint(p1), AsPoint(p2), AsPoint(p3));
    EXPECT_LT(FurthestApart(kept.Only(), CubicPoints(p0, p1, p2, p3, matrix)), kTolerance + 0.02) << scale;
    if (scale == 1.0) {
      small = flattener.Lines();
    } else {
      EXPECT_GT(flattener.Lines(), small * 3);
    }
  }
}

TEST(Flatten, ALooserToleranceTakesFewerLines) {
  std::uint32_t last = UINT32_MAX;
  for (const double tolerance : {0.05, 0.25, 1.0, 4.0}) {
    Kept kept;
    PathFlattener flattener(kept, Matrix(), AsLength(tolerance));
    flattener.MoveTo(AsPoint({0, 0}));
    flattener.CubicTo(AsPoint({100, 300}), AsPoint({400, 300}), AsPoint({500, 0}));
    EXPECT_LT(FurthestApart(kept.Only(), CubicPoints({0, 0}, {100, 300}, {400, 300}, {500, 0}, RealMatrix())),
              tolerance + 0.02);
    EXPECT_LT(flattener.Lines(), last) << tolerance;
    last = flattener.Lines();
  }
}

TEST(Flatten, ACurveIsNeverCutIntoMoreThanAThousandLines) {
  // From one edge of what a position can be to the other and back.
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo({.x = kFixedMin, .y = kFixedMin});
  flattener.CubicTo({.x = kFixedMax, .y = kFixedMin}, {.x = kFixedMin, .y = kFixedMax},
                    {.x = kFixedMax, .y = kFixedMax});
  // Where the curve changes from bending one way to bending the other it
  // is flat sooner, so it need not come to quite the limit.
  const std::uint32_t cubic = flattener.Lines();
  EXPECT_LE(cubic, PathFlattener::kMostLinesPerCurve);
  EXPECT_GT(cubic, PathFlattener::kMostLinesPerCurve / 2);
  // A curve with one bend the whole way is cut as far as it may be.
  flattener.QuadTo({.x = kFixedMin, .y = kFixedMax}, {.x = kFixedMin, .y = kFixedMin});
  EXPECT_EQ(flattener.Lines() - cubic, PathFlattener::kMostLinesPerCurve);
  EXPECT_EQ(PathFlattener::kMostLinesPerCurve, 1024u);
  EXPECT_FALSE(flattener.Clamped());
}

// One arc with everything about it chosen at random, checked against the
// arc SVG defines.
struct ArcCase {
  Real from, to;
  double radiusX = 0, radiusY = 0, degrees = 0;
  bool largeArc = false, sweep = false;
};

double ArcError(const ArcCase& arc, const RealMatrix& matrix, std::uint32_t* lines = nullptr) {
  Kept kept;
  PathFlattener flattener(kept, matrix.AsMatrix());
  flattener.MoveTo(AsPoint(arc.from));
  flattener.ArcTo(AsLength(arc.radiusX), AsLength(arc.radiusY), AsAngle(arc.degrees), arc.largeArc, arc.sweep,
                  AsPoint(arc.to));
  EXPECT_FALSE(flattener.Clamped());
  if (lines) {
    *lines = flattener.Lines();
  }
  const std::vector<Real>& drawn = kept.Only();
  const Real end = Rounded(matrix.Apply(arc.to));
  EXPECT_NEAR(drawn.back().x, end.x, 0.002);
  EXPECT_NEAR(drawn.back().y, end.y, 0.002);
  const TrueArc real = Solve(arc.from, arc.radiusX, arc.radiusY, arc.degrees, arc.largeArc, arc.sweep, arc.to);
  return FurthestApart(drawn, real.Points(matrix));
}

TEST(Flatten, AnArcIsTheOneOfTheFourThatItsFlagsChoose) {
  // Two points a circle of radius 100 can join four ways.
  for (const bool largeArc : {false, true}) {
    for (const bool sweep : {false, true}) {
      const ArcCase arc = {.from = {100, 200},
                           .to = {220, 260},
                           .radiusX = 100,
                           .radiusY = 100,
                           .degrees = 0,
                           .largeArc = largeArc,
                           .sweep = sweep};
      EXPECT_LT(ArcError(arc, RealMatrix()), kTolerance + 0.02) << largeArc << sweep;
      // The four differ by far more than the tolerance, so being near the
      // right one is being on it. How far round it went shows as well in
      // how many lines it took.
      std::uint32_t lines = 0;
      (void)ArcError(arc, RealMatrix(), &lines);
      if (largeArc) {
        EXPECT_GT(lines, 20u);
      } else {
        EXPECT_LT(lines, 20u);
      }
    }
  }
}

TEST(Flatten, AnyArcStaysWithinTheToleranceOfTheArcItShouldBe) {
  std::mt19937 random = Repeatable(11);
  std::uniform_real_distribution<double> place(-50.0, 850.0);
  std::uniform_real_distribution<double> radius(1.0, 600.0);
  std::uniform_real_distribution<double> degrees(-360.0, 720.0);
  double worst = 0;
  for (int i = 0; i < 800; ++i) {
    ArcCase arc;
    arc.from = Rounded({place(random), place(random)});
    arc.to = Rounded({place(random), place(random)});
    arc.radiusX = Rounded({radius(random), 0}).x;
    arc.radiusY = (i % 3 == 0) ? arc.radiusX : Rounded({radius(random), 0}).x;
    arc.degrees = (i % 5 == 0) ? 0 : degrees(random);
    arc.largeArc = random() % 2 == 0;
    arc.sweep = random() % 2 == 0;
    if (Distance(arc.from, arc.to) < 0.01) {
      continue;
    }
    const double error = ArcError(arc, RealMatrix());
    ASSERT_LT(error, kTolerance + 0.05) << "from " << arc.from.x << "," << arc.from.y << " to " << arc.to.x << ","
                                        << arc.to.y << " radii " << arc.radiusX << "," << arc.radiusY << " turned "
                                        << arc.degrees << " flags " << arc.largeArc << arc.sweep;
    worst = std::max(worst, error);
  }
  EXPECT_LT(worst, kTolerance + 0.05);
}

TEST(Flatten, AnArcComesOutRightWhateverTheMatrixDoesToIt) {
  std::mt19937 random = Repeatable(12);
  std::uniform_real_distribution<double> place(-40.0, 40.0);
  std::uniform_real_distribution<double> radius(0.5, 60.0);
  std::uniform_real_distribution<double> factor(-9.0, 9.0);
  std::uniform_real_distribution<double> degrees(0.0, 360.0);
  for (int i = 0; i < 400; ++i) {
    ArcCase arc;
    arc.from = Rounded({place(random), place(random)});
    arc.to = Rounded({place(random), place(random)});
    arc.radiusX = Rounded({radius(random), 0}).x;
    arc.radiusY = Rounded({radius(random), 0}).x;
    arc.degrees = degrees(random);
    arc.largeArc = random() % 2 == 0;
    arc.sweep = random() % 2 == 0;
    if (Distance(arc.from, arc.to) < 0.01) {
      continue;
    }
    // Stretched, turned, slanted and turned over, all at once.
    RealMatrix matrix;
    matrix.a = factor(random);
    matrix.b = factor(random);
    matrix.c = factor(random);
    matrix.d = factor(random);
    matrix.e = place(random) * 5;
    matrix.f = place(random) * 5;
    const RealMatrix rounded = {static_cast<double>(matrix.AsMatrix().a) / kCoefficientOne,
                                static_cast<double>(matrix.AsMatrix().b) / kCoefficientOne,
                                static_cast<double>(matrix.AsMatrix().c) / kCoefficientOne,
                                static_cast<double>(matrix.AsMatrix().d) / kCoefficientOne,
                                Rounded({matrix.e, 0}).x,
                                Rounded({matrix.f, 0}).x};
    ASSERT_LT(ArcError(arc, rounded), kTolerance + 0.05)
        << "from " << arc.from.x << "," << arc.from.y << " to " << arc.to.x << "," << arc.to.y << " radii "
        << arc.radiusX << "," << arc.radiusY << " turned " << arc.degrees << " flags " << arc.largeArc << arc.sweep;
  }
}

TEST(Flatten, ACircleDrawnAsFourArcsStaysOnTheCircle) {
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  const Fixed radius = AsLength(200);
  flattener.MoveTo(AsPoint({500, 300}));
  flattener.ArcTo(radius, radius, 0, false, true, AsPoint({300, 500}));
  flattener.ArcTo(radius, radius, 0, false, true, AsPoint({100, 300}));
  flattener.ArcTo(radius, radius, 0, false, true, AsPoint({300, 100}));
  flattener.ArcTo(radius, radius, 0, false, true, AsPoint({500, 300}));
  flattener.Close();
  const std::vector<Real>& lines = kept.Only();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    EXPECT_NEAR(Distance(lines[i], {300, 300}), 200.0, 0.02) << i;
    if (i > 0) {
      const Real middle = {(lines[i - 1].x + lines[i].x) / 2, (lines[i - 1].y + lines[i].y) / 2};
      EXPECT_GT(Distance(middle, {300, 300}), 200.0 - kTolerance - 0.01) << i;
    }
  }
  // It goes round the way it was asked to: from the x axis towards the y.
  EXPECT_GT(lines[1].y, 300.0);
  EXPECT_TRUE(kept.Closed(0));
}

TEST(Flatten, RadiiTooShortToReachAreMadeJustLongEnough) {
  // Twenty apart, with a radius of five: SVG has this drawn as half a
  // circle of radius ten.
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo(AsPoint({100, 100}));
  flattener.ArcTo(AsLength(5), AsLength(5), 0, false, true, AsPoint({120, 100}));
  const std::vector<Real>& lines = kept.Only();
  for (const Real point : lines) {
    EXPECT_NEAR(Distance(point, {110, 100}), 10.0, 0.02);
  }
  // With the sweep set it goes from the x axis towards the y axis: from
  // the left end, that is up the page as SVG has it, where y is smaller.
  EXPECT_LT(lines[lines.size() / 2].y, 91.0);
  EXPECT_EQ(lines.back().x, 120);
  EXPECT_EQ(lines.back().y, 100);
  EXPECT_FALSE(flattener.Clamped());

  // An ellipse keeps the proportion of its radii.
  const ArcCase ellipse = {
      .from = {0, 0}, .to = {300, 40}, .radiusX = 20, .radiusY = 5, .degrees = 30, .largeArc = true, .sweep = false};
  EXPECT_LT(ArcError(ellipse, RealMatrix()), kTolerance + 0.05);
}

TEST(Flatten, AnArcWithNoRadiusIsALineAndOneThatGoesNowhereIsNothing) {
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo(AsPoint({10, 10}));
  flattener.ArcTo(0, AsLength(5), 0, true, true, AsPoint({20, 20}));
  flattener.ArcTo(AsLength(5), 0, 0, true, true, AsPoint({30, 10}));
  EXPECT_EQ(flattener.Lines(), 2u);
  flattener.ArcTo(AsLength(5), AsLength(5), 0, true, true, AsPoint({30, 10}));
  EXPECT_EQ(flattener.Lines(), 2u);
  const std::vector<Real>& lines = kept.Only();
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[1].x, 20);
  EXPECT_EQ(lines[2].x, 30);
}

TEST(Flatten, AnArcThatIsAllButStraightIsDrawnAsALine) {
  // The radius of an arc can be anything at all: two books have arcs whose
  // radius is seventeen digits long, which arrives here as the largest a
  // length can be. Five units of such a circle is a straight line.
  Kept kept;
  PathFlattener flattener(kept, Matrix());
  flattener.MoveTo(AsPoint({10, 10}));
  flattener.ArcTo(kFixedMax, kFixedMax, 0, false, false, AsPoint({15, 12}));
  EXPECT_EQ(flattener.Lines(), 1u);
  flattener.ArcTo(AsLength(8971.87), AsLength(8971.87), 0, false, true, AsPoint({-8.7, 17}));
  EXPECT_EQ(flattener.Lines(), 2u);
  EXPECT_FALSE(flattener.Clamped());
  EXPECT_EQ(kept.Only().back().y, 17);
}

TEST(Flatten, AShallowArcIsNeverFurtherFromItsLineThanTheTolerance) {
  // Long radii and short ways between the ends, where the choice between
  // drawing the arc and drawing a line is made. Whichever is chosen, what
  // is drawn has to be near the true arc.
  std::mt19937 random = Repeatable(13);
  std::uniform_real_distribution<double> place(0.0, 800.0);
  std::uniform_real_distribution<double> power(1.0, 5.3);
  std::uniform_real_distribution<double> step(-60.0, 60.0);
  std::uniform_real_distribution<double> degrees(0.0, 360.0);
  std::size_t straight = 0;
  std::size_t curved = 0;
  for (int i = 0; i < 800; ++i) {
    ArcCase arc;
    arc.from = Rounded({place(random), place(random)});
    arc.to = Rounded({arc.from.x + step(random), arc.from.y + step(random)});
    arc.radiusX = Rounded({std::pow(10.0, power(random)), 0}).x;
    arc.radiusY = (i % 2 == 0) ? arc.radiusX : Rounded({std::pow(10.0, power(random)), 0}).x;
    arc.degrees = degrees(random);
    arc.largeArc = false;
    arc.sweep = random() % 2 == 0;
    if (Distance(arc.from, arc.to) < 0.01) {
      continue;
    }
    std::uint32_t lines = 0;
    const double error = ArcError(arc, RealMatrix(), &lines);
    ASSERT_LT(error, kTolerance + 0.05) << "from " << arc.from.x << "," << arc.from.y << " to " << arc.to.x << ","
                                        << arc.to.y << " radii " << arc.radiusX << "," << arc.radiusY << " turned "
                                        << arc.degrees << " sweep " << arc.sweep << " lines " << lines;
    (lines == 1 ? straight : curved) += 1;
  }
  // Both ways were taken, or the test tried only one of them.
  EXPECT_GT(straight, 80u);
  EXPECT_GT(curved, 80u);
}

TEST(Flatten, HoldsAPointTooFarOffAtTheEdgeAndSaysSo) {
  Kept kept;
  RealMatrix matrix;
  matrix.a = 100;
  matrix.d = 100;
  PathFlattener flattener(kept, matrix.AsMatrix());
  flattener.MoveTo(AsPoint({0, 0}));
  EXPECT_FALSE(flattener.Clamped());
  flattener.LineTo(AsPoint({400000, -400000}));
  EXPECT_TRUE(flattener.Clamped());
  EXPECT_EQ(kept.Only().back().x, static_cast<double>(kFixedMax) / kFixedOne);
  EXPECT_EQ(kept.Only().back().y, static_cast<double>(kFixedMin) / kFixedOne);
}

TEST(Flatten, NothingOverflowsOrRunsOnWhateverItIsGiven) {
  // For the sanitizers, and for the bound on what one command can cost:
  // arcs and curves made of the largest and smallest numbers there are,
  // through matrices of the same.
  std::mt19937 random = Repeatable(14);
  const std::array<std::int32_t, 9> edges = {kFixedMin, kFixedMin + 1, -4096, -1, 0, 1, 4096, kFixedMax - 1, kFixedMax};
  const auto edge = [&random, &edges]() { return edges[random() % edges.size()]; };
  const auto any = [&random, &edge]() { return (random() % 3 == 0) ? edge() : static_cast<std::int32_t>(random()); };
  // Takes the lines and keeps nothing: there can be a great many.
  class Dropped final : public LineSink {
   public:
    void MoveTo(Point /*to*/) noexcept override {}
    void LineTo(Point /*to*/) noexcept override {}
    void Close() noexcept override {}
  };
  std::uint32_t costliest = 0;
  for (int i = 0; i < 600; ++i) {
    Matrix matrix;
    if (i % 2 == 0) {
      matrix.a = any();
      matrix.b = any();
      matrix.c = any();
      matrix.d = any();
      matrix.e = any();
      matrix.f = any();
    }
    Dropped dropped;
    PathFlattener flattener(dropped, matrix, (i % 7 == 0) ? any() : PathFlattener::kDefaultTolerance);
    flattener.MoveTo({.x = any(), .y = any()});
    for (int part = 0; part < 4; ++part) {
      const std::uint32_t before = flattener.Lines();
      std::uint32_t most = 1;
      switch (random() % 4) {
        case 0:
          flattener.LineTo({.x = any(), .y = any()});
          break;
        case 1:
          flattener.QuadTo({.x = any(), .y = any()}, {.x = any(), .y = any()});
          most = PathFlattener::kMostLinesPerCurve;
          break;
        case 2:
          flattener.CubicTo({.x = any(), .y = any()}, {.x = any(), .y = any()}, {.x = any(), .y = any()});
          most = PathFlattener::kMostLinesPerCurve;
          break;
        default:
          flattener.ArcTo(std::max(any(), -kFixedMax), std::max(any(), -kFixedMax), static_cast<Angle>(random()),
                          random() % 2 == 0, random() % 2 == 0, {.x = any(), .y = any()});
          most = PathFlattener::kMostLinesPerArc;
          break;
      }
      // Whatever the numbers, one command is no more lines than its kind
      // may be.
      ASSERT_LE(flattener.Lines() - before, most);
      costliest = std::max(costliest, flattener.Lines() - before);
    }
    flattener.Close();
  }
  // And the numbers tried do bring an arc near the most it can cost, or
  // the bound was not put to the test.
  EXPECT_GT(costliest, PathFlattener::kMostLinesPerArc / 4);
}

TEST(Flatten, AnArcCanBeAsManyLinesAsItsBoundAndNoMore) {
  // All but a whole turn of a circle 300,000 pixels in radius, held to the
  // finest tolerance there is: thirty-two curves, each cut as far as a
  // curve may be.
  class Dropped final : public LineSink {
   public:
    void MoveTo(Point /*to*/) noexcept override {}
    void LineTo(Point /*to*/) noexcept override {}
    void Close() noexcept override {}
  };
  Dropped dropped;
  PathFlattener flattener(dropped, Matrix(), 1);
  flattener.MoveTo(AsPoint({-300000, 0}));
  flattener.ArcTo(AsLength(300000), AsLength(300000), 0, true, true, AsPoint({-300000, 1}));
  EXPECT_FALSE(flattener.Clamped());
  EXPECT_LE(flattener.Lines(), PathFlattener::kMostLinesPerArc);
  EXPECT_GT(flattener.Lines(), PathFlattener::kMostLinesPerArc / 2);
  EXPECT_EQ(PathFlattener::kMostLinesPerArc, 32768u);
}

}  // namespace
