#include "draw/fixed.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace bn::draw {

namespace {

// Sines, cosines and angles are found by CORDIC: a point is turned through
// a fixed list of ever smaller angles, each of them one whose tangent is a
// power of two, so that a turn is two shifts and two additions. Turning
// towards a wanted angle gives its sine and cosine; turning a point down
// onto the x axis and adding up the turns gives its angle.
//
// The angles whose tangents are 1, 1/2, 1/4 and so on, as Angles.
constexpr std::array<std::int32_t, 31> kTurns = {
    536870912, 316933406, 167458907, 85004756, 42667331, 21354465, 10679838, 5340245, 2670163, 1335087, 667544,
    333772,    166886,    83443,     41722,    20861,    10430,    5215,     2608,    1304,    652,     326,
    163,       81,        41,        20,       10,       5,        3,        1,       1};

// Each of those turns also stretches the point a little. This is what all
// of them together stretch it by, upside down, as a Unit: starting from a
// point this far out ends on the circle of radius one.
constexpr Unit kStartingRadius = 652032874;

// A Coefficient times a Coefficient or a Fixed, in the units of the second.
std::int64_t Scaled(Coefficient factor, std::int32_t value) noexcept {
  constexpr std::int64_t kHalf = 524288;
  return ShiftDown((static_cast<std::int64_t>(factor) * value) + kHalf, kCoefficientBits);
}

Coefficient CoefficientOfUnit(Unit value) noexcept {
  constexpr int kShift = kUnitBits - kCoefficientBits;
  constexpr Unit kHalf = 512;
  return ShiftDown(value + kHalf, kShift);
}

// The tangent of an angle.
Coefficient TangentOf(Angle angle, bool& clamped) noexcept {
  const SinCos at = SinCosOf(angle);
  if (at.cos == 0) {
    clamped = true;
    return at.sin < 0 ? kFixedMin : kFixedMax;
  }
  return Clamp32((static_cast<std::int64_t>(at.sin) * kCoefficientOne) / at.cos, clamped);
}

}  // namespace

std::int32_t Clamp32(std::int64_t value, bool& clamped) noexcept {
  if (value > kFixedMax) {
    clamped = true;
    return kFixedMax;
  }
  if (value < kFixedMin) {
    clamped = true;
    return kFixedMin;
  }
  return static_cast<std::int32_t>(value);
}

SinCos SinCosOf(Angle angle) noexcept {
  // The quarter of the circle the angle is in, and how far into it.
  const unsigned quarter = angle >> 30U;
  auto left = static_cast<std::int32_t>(angle & 0x3FFFFFFFU);
  std::int32_t x = kStartingRadius;
  std::int32_t y = 0;
  int step = 0;
  for (const std::int32_t turn : kTurns) {
    const std::int32_t acrossX = ShiftDown(y, step);
    const std::int32_t acrossY = ShiftDown(x, step);
    if (left >= 0) {
      x -= acrossX;
      y += acrossY;
      left -= turn;
    } else {
      x += acrossX;
      y -= acrossY;
      left += turn;
    }
    ++step;
  }
  // (x, y) is the cosine and sine of the part within the quarter.
  switch (quarter) {
    case 0:
      return {.sin = y, .cos = x};
    case 1:
      return {.sin = x, .cos = -y};
    case 2:
      return {.sin = -y, .cos = -x};
    default:
      return {.sin = -x, .cos = y};
  }
}

Angle AngleOf(std::int32_t y, std::int32_t x) noexcept {
  if (x == 0 && y == 0) {
    return 0;
  }
  std::int64_t wideX = x;
  std::int64_t wideY = y;
  // The larger of the two is brought to 28 bits: large enough to lose
  // nothing in the shifts below, small enough that the stretching cannot
  // overflow.
  constexpr std::int64_t kLow = 134217728;
  constexpr std::int64_t kHigh = 268435456;
  std::int64_t larger = std::max(wideX < 0 ? -wideX : wideX, wideY < 0 ? -wideY : wideY);
  while (larger >= kHigh) {
    wideX = ShiftDown(wideX, 1);
    wideY = ShiftDown(wideY, 1);
    larger = ShiftDown(larger, 1);
  }
  while (larger < kLow) {
    wideX *= 2;
    wideY *= 2;
    larger *= 2;
  }
  // The turning below reaches a little over a quarter turn either way, so a
  // point left of the y axis is first turned half way round.
  Angle angle = 0;
  if (wideX < 0) {
    wideX = -wideX;
    wideY = -wideY;
    angle = kHalfTurn;
  }
  auto atX = static_cast<std::int32_t>(wideX);
  auto atY = static_cast<std::int32_t>(wideY);
  std::int32_t turned = 0;
  int step = 0;
  for (const std::int32_t turn : kTurns) {
    const std::int32_t acrossX = ShiftDown(atY, step);
    const std::int32_t acrossY = ShiftDown(atX, step);
    if (atY > 0) {
      atX += acrossX;
      atY -= acrossY;
      turned += turn;
    } else {
      atX -= acrossX;
      atY += acrossY;
      turned -= turn;
    }
    ++step;
  }
  return angle + static_cast<Angle>(turned);
}

std::uint32_t SquareRoot(std::uint64_t value) noexcept {
  std::uint32_t root = 0;
  for (std::uint32_t bit = 0x80000000U; bit != 0; bit >>= 1U) {
    const std::uint64_t trial = root | bit;
    if (trial * trial <= value) {
      root |= bit;
    }
  }
  return root;
}

Matrix Multiply(const Matrix& outer, const Matrix& inner, bool& clamped) noexcept {
  Matrix both;
  both.a = Clamp32(Scaled(outer.a, inner.a) + Scaled(outer.c, inner.b), clamped);
  both.b = Clamp32(Scaled(outer.b, inner.a) + Scaled(outer.d, inner.b), clamped);
  both.c = Clamp32(Scaled(outer.a, inner.c) + Scaled(outer.c, inner.d), clamped);
  both.d = Clamp32(Scaled(outer.b, inner.c) + Scaled(outer.d, inner.d), clamped);
  both.e = Clamp32(Scaled(outer.a, inner.e) + Scaled(outer.c, inner.f) + outer.e, clamped);
  both.f = Clamp32(Scaled(outer.b, inner.e) + Scaled(outer.d, inner.f) + outer.f, clamped);
  return both;
}

Point Apply(const Matrix& matrix, Point point, bool& clamped) noexcept {
  return {.x = Clamp32(Scaled(matrix.a, point.x) + Scaled(matrix.c, point.y) + matrix.e, clamped),
          .y = Clamp32(Scaled(matrix.b, point.x) + Scaled(matrix.d, point.y) + matrix.f, clamped)};
}

Coefficient LargestScale(const Matrix& matrix) noexcept {
  const auto size = [](Coefficient value) noexcept -> std::int64_t { return value < 0 ? -std::int64_t{value} : value; };
  bool clamped = false;
  return Clamp32(std::max(size(matrix.a) + size(matrix.c), size(matrix.b) + size(matrix.d)), clamped);
}

Matrix Translation(Fixed x, Fixed y) noexcept {
  Matrix matrix;
  matrix.e = x;
  matrix.f = y;
  return matrix;
}

Matrix Scaling(Coefficient x, Coefficient y) noexcept {
  Matrix matrix;
  matrix.a = x;
  matrix.d = y;
  return matrix;
}

Matrix Rotation(Angle angle) noexcept {
  const SinCos at = SinCosOf(angle);
  Matrix matrix;
  matrix.a = CoefficientOfUnit(at.cos);
  matrix.b = CoefficientOfUnit(at.sin);
  matrix.c = -matrix.b;
  matrix.d = matrix.a;
  return matrix;
}

Matrix SkewX(Angle angle, bool& clamped) noexcept {
  Matrix matrix;
  matrix.c = TangentOf(angle, clamped);
  return matrix;
}

Matrix SkewY(Angle angle, bool& clamped) noexcept {
  Matrix matrix;
  matrix.b = TangentOf(angle, clamped);
  return matrix;
}

}  // namespace bn::draw
