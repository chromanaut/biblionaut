#pragma once

#include <cstdint>

// Arithmetic for drawing, in whole numbers. The device's processor has no
// floating point, so positions, factors, sines and angles are each an
// integer that counts some fixed fraction, and nothing here uses a float or
// a double.
//
// Each kind of number has its own fraction, chosen for what it has to hold:
//
//   Fixed        a length or a position, in 1/4096ths: of a unit of a
//                drawing, or of a pixel. Up to 524,287 either way.
//   Coefficient  what a length is multiplied by, in 1/1,048,576ths. Up to
//                2,047 either way.
//   Unit         a sine or a cosine, or one coordinate of a point on a
//                circle of radius one, in 1/2^30ths.
//   Angle        a full turn is 2^32, so that adding angles wraps round as
//                turning does.
//
// They are all plain integers, so the compiler will not notice one used as
// another: the names are for the reader.
//
// Nothing here can overflow. Where a result is too large for its kind it is
// held at the largest that fits, and a flag the caller passes is set, so
// that a drawing with a number in it that is beyond these ends in a picture
// and a report, not in a fault.

namespace bn::draw {

using Fixed = std::int32_t;
inline constexpr int kFixedBits = 12;
inline constexpr Fixed kFixedOne = 4096;
// The ends of what a Fixed is held to. The same distance either side of
// nothing, so that one can always be negated.
inline constexpr Fixed kFixedMax = INT32_MAX;
inline constexpr Fixed kFixedMin = -INT32_MAX;

using Coefficient = std::int32_t;
inline constexpr int kCoefficientBits = 20;
inline constexpr Coefficient kCoefficientOne = 1048576;

using Unit = std::int32_t;
inline constexpr int kUnitBits = 30;
inline constexpr Unit kUnitOne = 1073741824;

using Angle = std::uint32_t;
inline constexpr Angle kEighthTurn = 536870912;
inline constexpr Angle kQuarterTurn = 1073741824;
inline constexpr Angle kHalfTurn = 2147483648;

struct Point {
  Fixed x = 0;
  Fixed y = 0;

  friend bool operator==(const Point&, const Point&) = default;
};

struct SinCos {
  Unit sin = 0;
  Unit cos = kUnitOne;
};

// `value` over two to the power of `bits`, rounded down, whichever side of
// nothing it is: which is what shifting a number right does, as C++20
// defines it. The check that would object to the shift is for older rules,
// under which a negative number could come out as anything, so every such
// shift in the drawing code is made here.
constexpr std::int64_t ShiftDown(std::int64_t value, int bits) noexcept {
  return value >> bits;  // NOLINT(bugprone-signed-bitwise)
}
constexpr std::int32_t ShiftDown(std::int32_t value, int bits) noexcept {
  return value >> bits;  // NOLINT(bugprone-signed-bitwise)
}

// `value` if it is between kFixedMin and kFixedMax, which are also the ends
// of a Coefficient; otherwise the nearer of the two, with `clamped` set.
// `clamped` is never cleared, so one flag can be passed down a whole
// calculation.
std::int32_t Clamp32(std::int64_t value, bool& clamped) noexcept;

// The sine and cosine of an angle, right to within a few parts in 2^30.
SinCos SinCosOf(Angle angle) noexcept;

// The angle of the point (x, y) as seen from (0, 0): 0 along the x axis,
// a quarter turn along the y axis. Only the ratio of the two matters, so
// they may be in any units. 0 if both are 0.
Angle AngleOf(std::int32_t y, std::int32_t x) noexcept;

// The square root, rounded down.
std::uint32_t SquareRoot(std::uint64_t value) noexcept;

// What turns a point of one set of coordinates into a point of another:
//
//   x' = a x + c y + e
//   y' = b x + d y + f
//
// which is the order SVG gives the six numbers in.
struct Matrix {
  Coefficient a = kCoefficientOne;
  Coefficient b = 0;
  Coefficient c = 0;
  Coefficient d = kCoefficientOne;
  Fixed e = 0;
  Fixed f = 0;

  friend bool operator==(const Matrix&, const Matrix&) = default;
};

// The matrix that does what `inner` does and then what `outer` does.
Matrix Multiply(const Matrix& outer, const Matrix& inner, bool& clamped) noexcept;

Point Apply(const Matrix& matrix, Point point, bool& clamped) noexcept;

// A bound on how much `matrix` enlarges: of a length that goes in, neither
// how far across nor how far down it comes out is more than this many times
// it. So the length itself comes out no more than this times the square
// root of two.
Coefficient LargestScale(const Matrix& matrix) noexcept;

Matrix Translation(Fixed x, Fixed y) noexcept;
Matrix Scaling(Coefficient x, Coefficient y) noexcept;
// A turn about (0, 0), from the x axis towards the y axis.
Matrix Rotation(Angle angle) noexcept;
// A slant: x moves by y times the tangent of the angle, or y by x times it.
// An angle of a quarter turn, where the tangent has no end, is clamped.
Matrix SkewX(Angle angle, bool& clamped) noexcept;
Matrix SkewY(Angle angle, bool& clamped) noexcept;

}  // namespace bn::draw
