#include "draw/fixed.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>

namespace {

using bn::draw::Angle;
using bn::draw::AngleOf;
using bn::draw::Apply;
using bn::draw::Clamp32;
using bn::draw::Coefficient;
using bn::draw::Fixed;
using bn::draw::kCoefficientOne;
using bn::draw::kEighthTurn;
using bn::draw::kFixedMax;
using bn::draw::kFixedMin;
using bn::draw::kFixedOne;
using bn::draw::kHalfTurn;
using bn::draw::kQuarterTurn;
using bn::draw::kUnitOne;
using bn::draw::LargestScale;
using bn::draw::Matrix;
using bn::draw::Multiply;
using bn::draw::Point;
using bn::draw::Rotation;
using bn::draw::Scaling;
using bn::draw::SinCos;
using bn::draw::SinCosOf;
using bn::draw::SkewX;
using bn::draw::SkewY;
using bn::draw::SquareRoot;
using bn::draw::Translation;

constexpr double kTurn = 4294967296.0;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

double Radians(Angle angle) { return static_cast<double>(angle) / kTurn * 2.0 * std::numbers::pi; }
double InUnits(std::int32_t value) { return static_cast<double>(value) / kUnitOne; }
double Factor(Coefficient value) { return static_cast<double>(value) / kCoefficientOne; }
double Length(Fixed value) { return static_cast<double>(value) / kFixedOne; }
Coefficient AsFactor(double value) { return static_cast<Coefficient>(std::lround(value * kCoefficientOne)); }
Fixed AsLength(double value) { return static_cast<Fixed>(std::lround(value * kFixedOne)); }

// How far apart two angles are, the short way round, in radians.
double AngleBetween(Angle a, double radians) {
  const double difference = std::remainder(Radians(a) - radians, 2.0 * std::numbers::pi);
  return std::abs(difference);
}

TEST(Fixed, GivesSinesAndCosinesRightToAFewPartsInAHundredMillion) {
  std::mt19937 random = Repeatable(1);
  double worst = 0;
  const auto check = [&worst](Angle angle) {
    const SinCos at = SinCosOf(angle);
    worst = std::max(worst, std::abs(InUnits(at.sin) - std::sin(Radians(angle))));
    worst = std::max(worst, std::abs(InUnits(at.cos) - std::cos(Radians(angle))));
  };
  for (int i = 0; i < 200000; ++i) {
    check(static_cast<Angle>(random()));
  }
  // The quarters, and either side of each, where the working changes over.
  for (std::uint64_t quarter = 0; quarter < 4; ++quarter) {
    for (const std::int64_t beside : {-2, -1, 0, 1, 2}) {
      check(static_cast<Angle>((quarter * kQuarterTurn) + static_cast<std::uint64_t>(beside)));
    }
  }
  EXPECT_LT(worst, 5e-8);
}

TEST(Fixed, TheSineOfAQuarterTurnIsAsNearOneAsMakesNoDifference) {
  EXPECT_NEAR(SinCosOf(kQuarterTurn).sin, kUnitOne, 64);
  EXPECT_NEAR(SinCosOf(kQuarterTurn).cos, 0, 64);
  EXPECT_NEAR(SinCosOf(0).cos, kUnitOne, 64);
  EXPECT_NEAR(SinCosOf(0).sin, 0, 64);
  EXPECT_NEAR(SinCosOf(kHalfTurn).cos, -kUnitOne, 64);
  EXPECT_NEAR(SinCosOf(kEighthTurn).sin, SinCosOf(kEighthTurn).cos, 64);
}

TEST(Fixed, FindsTheAngleOfAPointWhateverItsSize) {
  std::mt19937 random = Repeatable(2);
  double worst = 0;
  for (int i = 0; i < 200000; ++i) {
    // Sizes from a single unit up to all that fits.
    const int bits = 1 + static_cast<int>(random() % 31);
    const auto limit = static_cast<std::int32_t>((std::uint64_t{1} << static_cast<unsigned>(bits)) - 1);
    std::uniform_int_distribution<std::int32_t> within(-limit, limit);
    const std::int32_t x = within(random);
    const std::int32_t y = within(random);
    if (x == 0 && y == 0) {
      continue;
    }
    // A point a few units from (0, 0) has no finer angle than its own
    // coarseness; only larger ones are held to the tight bound.
    const double error = AngleBetween(AngleOf(y, x), std::atan2(static_cast<double>(y), static_cast<double>(x)));
    if (bits >= 12) {
      worst = std::max(worst, error);
    } else {
      EXPECT_LT(error, 1e-6) << x << "," << y;
    }
  }
  EXPECT_LT(worst, 1e-7);
}

TEST(Fixed, TheAngleOfEachAxisIsAQuarterTurnFromTheLast) {
  // Right to within a few parts in a hundred million of a turn.
  constexpr double kNear = 64.0;
  EXPECT_NEAR(static_cast<double>(AngleOf(100, 100)), static_cast<double>(kEighthTurn), kNear);
  EXPECT_NEAR(static_cast<double>(AngleOf(100, 0)), static_cast<double>(kQuarterTurn), kNear);
  EXPECT_NEAR(static_cast<double>(AngleOf(0, -100)), static_cast<double>(kHalfTurn), kNear);
  EXPECT_NEAR(static_cast<double>(AngleOf(-100, 0)), 3.0 * kQuarterTurn, kNear);
  EXPECT_NEAR(static_cast<double>(AngleOf(INT32_MAX, INT32_MAX)), static_cast<double>(kEighthTurn), kNear);
  EXPECT_NEAR(static_cast<double>(AngleOf(-INT32_MAX, -INT32_MAX)), 5.0 * kEighthTurn, kNear);
  // Along the x axis the angle may come out a hair either side of nothing.
  EXPECT_LT(AngleBetween(AngleOf(0, 100), 0.0), 1e-7);
}

TEST(Fixed, TheAngleOfNoPointAtAllIsNothing) { EXPECT_EQ(AngleOf(0, 0), 0u); }

TEST(Fixed, TakesSquareRootsRoundedDown) {
  const auto check = [](std::uint64_t value) {
    const std::uint64_t root = SquareRoot(value);
    EXPECT_LE(root * root, value) << value;
    // The next one up is too large, or would not fit.
    if (root < UINT32_MAX) {
      EXPECT_GT((root + 1) * (root + 1), value) << value;
    }
  };
  for (const std::uint64_t value :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{4}, std::uint64_t{15},
        std::uint64_t{16}, std::uint64_t{17}, std::uint64_t{UINT32_MAX}, std::uint64_t{UINT32_MAX} + 1,
        std::uint64_t{1} << 62U, UINT64_MAX - 1, UINT64_MAX}) {
    check(value);
  }
  EXPECT_EQ(SquareRoot(UINT64_MAX), UINT32_MAX);
  EXPECT_EQ(SquareRoot(1152921504606846976), 1073741824u);
  // Numbers of every length, from one bit to sixty-four.
  std::mt19937 random = Repeatable(3);
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t all = (std::uint64_t{random()} << 32U) | random();
    check(all >> (random() % 64));
  }
}

TEST(Fixed, HoldsWhatIsTooLargeAtTheEdgeAndSaysSo) {
  bool clamped = false;
  EXPECT_EQ(Clamp32(123, clamped), 123);
  EXPECT_EQ(Clamp32(-123, clamped), -123);
  EXPECT_EQ(Clamp32(kFixedMax, clamped), kFixedMax);
  EXPECT_EQ(Clamp32(kFixedMin, clamped), kFixedMin);
  EXPECT_FALSE(clamped);

  EXPECT_EQ(Clamp32(std::int64_t{kFixedMax} + 1, clamped), kFixedMax);
  EXPECT_TRUE(clamped);
  clamped = false;
  EXPECT_EQ(Clamp32(INT64_MIN, clamped), kFixedMin);
  EXPECT_TRUE(clamped);
  // Once set it stays set, so one flag can follow a whole calculation.
  EXPECT_EQ(Clamp32(5, clamped), 5);
  EXPECT_TRUE(clamped);
}

// A matrix in floating point, to compare against.
struct Real {
  double a, b, c, d, e, f;
};

Real AsReal(const Matrix& m) { return {Factor(m.a), Factor(m.b), Factor(m.c), Factor(m.d), Length(m.e), Length(m.f)}; }

Real Product(const Real& outer, const Real& inner) {
  return {(outer.a * inner.a) + (outer.c * inner.b),           (outer.b * inner.a) + (outer.d * inner.b),
          (outer.a * inner.c) + (outer.c * inner.d),           (outer.b * inner.c) + (outer.d * inner.d),
          (outer.a * inner.e) + (outer.c * inner.f) + outer.e, (outer.b * inner.e) + (outer.d * inner.f) + outer.f};
}

Matrix RandomMatrix(std::mt19937& random, double scale, double shift) {
  std::uniform_real_distribution<double> factor(-scale, scale);
  std::uniform_real_distribution<double> offset(-shift, shift);
  Matrix m;
  m.a = AsFactor(factor(random));
  m.b = AsFactor(factor(random));
  m.c = AsFactor(factor(random));
  m.d = AsFactor(factor(random));
  m.e = AsLength(offset(random));
  m.f = AsLength(offset(random));
  return m;
}

TEST(Matrix, MultipliesAsMatricesDo) {
  std::mt19937 random = Repeatable(4);
  for (int i = 0; i < 20000; ++i) {
    const Matrix outer = RandomMatrix(random, 8.0, 2000.0);
    const Matrix inner = RandomMatrix(random, 8.0, 2000.0);
    bool clamped = false;
    const Real got = AsReal(Multiply(outer, inner, clamped));
    const Real want = Product(AsReal(outer), AsReal(inner));
    ASSERT_FALSE(clamped);
    // Each factor is right to a part in a million, each offset to a
    // 4096th.
    ASSERT_NEAR(got.a, want.a, 2e-6);
    ASSERT_NEAR(got.b, want.b, 2e-6);
    ASSERT_NEAR(got.c, want.c, 2e-6);
    ASSERT_NEAR(got.d, want.d, 2e-6);
    ASSERT_NEAR(got.e, want.e, 1.0 / 2048);
    ASSERT_NEAR(got.f, want.f, 1.0 / 2048);
  }
}

TEST(Matrix, MovesAPointAsItsSixNumbersSay) {
  std::mt19937 random = Repeatable(5);
  std::uniform_real_distribution<double> place(-30000.0, 30000.0);
  for (int i = 0; i < 20000; ++i) {
    const Matrix m = RandomMatrix(random, 4.0, 2000.0);
    const double x = place(random);
    const double y = place(random);
    bool clamped = false;
    const Point got = Apply(m, {.x = AsLength(x), .y = AsLength(y)}, clamped);
    const Real real = AsReal(m);
    ASSERT_FALSE(clamped);
    ASSERT_NEAR(Length(got.x), (real.a * x) + (real.c * y) + real.e, 0.002);
    ASSERT_NEAR(Length(got.y), (real.b * x) + (real.d * y) + real.f, 0.002);
  }
}

TEST(Matrix, AMatrixLeftAloneChangesNothing) {
  bool clamped = false;
  const Point point = {.x = AsLength(12.5), .y = AsLength(-7.25)};
  EXPECT_EQ(Apply(Matrix(), point, clamped), point);
  const Matrix other = Multiply(Translation(40960, 81920), Scaling(AsFactor(2.0), AsFactor(3.0)), clamped);
  EXPECT_EQ(Multiply(Matrix(), other, clamped), other);
  EXPECT_EQ(Multiply(other, Matrix(), clamped), other);
  EXPECT_FALSE(clamped);
}

TEST(Matrix, TheInnerOneIsDoneFirst) {
  bool clamped = false;
  // Twice as large, and then ten across: not the other way about.
  const Matrix both = Multiply(Translation(AsLength(10.0), 0), Scaling(AsFactor(2.0), AsFactor(2.0)), clamped);
  EXPECT_EQ(Apply(both, {.x = AsLength(1.0), .y = AsLength(1.0)}, clamped),
            (Point{.x = AsLength(12.0), .y = AsLength(2.0)}));
  EXPECT_FALSE(clamped);
}

TEST(Matrix, AQuarterTurnTakesTheXAxisOntoTheYAxisExactly) {
  bool clamped = false;
  const Matrix turn = Rotation(kQuarterTurn);
  EXPECT_EQ(Apply(turn, {.x = AsLength(100.0), .y = 0}, clamped), (Point{.x = 0, .y = AsLength(100.0)}));
  EXPECT_EQ(Apply(turn, {.x = 0, .y = AsLength(100.0)}, clamped), (Point{.x = AsLength(-100.0), .y = 0}));
  EXPECT_EQ(Rotation(0), Matrix());
  EXPECT_FALSE(clamped);
}

TEST(Matrix, TurnsByAnyAngle) {
  for (const double degrees : {30.0, 45.0, 123.4, 270.0, 359.0}) {
    const auto angle = static_cast<Angle>(degrees / 360.0 * kTurn);
    const Real got = AsReal(Rotation(angle));
    const double radians = degrees / 180.0 * std::numbers::pi;
    EXPECT_NEAR(got.a, std::cos(radians), 2e-6) << degrees;
    EXPECT_NEAR(got.b, std::sin(radians), 2e-6) << degrees;
    EXPECT_NEAR(got.c, -std::sin(radians), 2e-6) << degrees;
    EXPECT_NEAR(got.d, std::cos(radians), 2e-6) << degrees;
  }
}

TEST(Matrix, SlantsByTheTangentOfTheAngle) {
  bool clamped = false;
  const auto angle = static_cast<Angle>(30.0 / 360.0 * kTurn);
  const double tangent = std::tan(30.0 / 180.0 * std::numbers::pi);
  EXPECT_NEAR(Factor(SkewX(angle, clamped).c), tangent, 2e-6);
  EXPECT_EQ(SkewX(angle, clamped).b, 0);
  EXPECT_NEAR(Factor(SkewY(angle, clamped).b), tangent, 2e-6);
  EXPECT_EQ(SkewY(angle, clamped).c, 0);
  EXPECT_FALSE(clamped);
  // A slant of a quarter turn has no end to it.
  const Matrix flat = SkewX(kQuarterTurn, clamped);
  EXPECT_TRUE(clamped);
  EXPECT_TRUE(flat.c == kFixedMax || flat.c == kFixedMin);
}

TEST(Matrix, HoldsAProductThatIsTooLargeAtTheEdgeAndSaysSo) {
  bool clamped = false;
  const Matrix large = Scaling(AsFactor(1500.0), AsFactor(1500.0));
  const Matrix both = Multiply(large, large, clamped);
  EXPECT_TRUE(clamped);
  EXPECT_EQ(both.a, kFixedMax);

  clamped = false;
  const Point far = Apply(large, {.x = AsLength(400000.0), .y = AsLength(-400000.0)}, clamped);
  EXPECT_TRUE(clamped);
  EXPECT_EQ(far, (Point{.x = kFixedMax, .y = kFixedMin}));
}

TEST(Matrix, NothingOverflowsAtTheEdges) {
  // For the sanitizers: every number as large as it can be, both ways.
  for (const std::int32_t value : {kFixedMax, kFixedMin}) {
    Matrix m;
    m.a = m.b = m.c = m.d = m.e = m.f = value;
    bool clamped = false;
    (void)Multiply(m, m, clamped);
    (void)Apply(m, {.x = value, .y = value}, clamped);
    (void)Apply(m, {.x = -value, .y = value}, clamped);
    EXPECT_TRUE(clamped);
    EXPECT_EQ(LargestScale(m), kFixedMax);
  }
}

TEST(Matrix, NoLengthGrowsByMoreThanItsLargestScale) {
  std::mt19937 random = Repeatable(6);
  std::uniform_real_distribution<double> around(0.0, 2.0 * std::numbers::pi);
  for (int i = 0; i < 2000; ++i) {
    const Matrix m = RandomMatrix(random, 6.0, 100.0);
    const Real real = AsReal(m);
    const double bound = Factor(LargestScale(m));
    for (int step = 0; step < 16; ++step) {
      const double angle = around(random);
      const double x = (real.a * std::cos(angle)) + (real.c * std::sin(angle));
      const double y = (real.b * std::cos(angle)) + (real.d * std::sin(angle));
      ASSERT_LE(std::hypot(x, y), bound * std::numbers::sqrt2 + 1e-6);
      ASSERT_LE(std::max(std::abs(x), std::abs(y)), bound + 1e-6);
    }
  }
}

}  // namespace
