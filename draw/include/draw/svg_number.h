#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "draw/fixed.h"

namespace bn::draw {

// A number as it comes out of the text of an SVG file: in 1/2^32ths, which
// is finer than any of the kinds in fixed.h, so that the same reading
// serves a coordinate, a factor of scale and an angle. Held to 2^30 either
// way.
using Number = std::int64_t;
inline constexpr Number kNumberOne = 4294967296;

// Reads one number as SVG writes them, a character at a time: "12", "-.5",
// "+3.25", "1e3", "2.5E-2". It is given characters until it refuses one, and
// that one is the first that is not part of the number. So "1.5.5" is two
// numbers and so is "10-5", as SVG has them, and a number may arrive in
// pieces.
//
// It keeps what it needs in a few bytes and divides nothing, except for a
// number written with an exponent, which path data hardly ever is.
class SvgNumber {
 public:
  // Starts on a new number.
  void Reset() noexcept { *this = SvgNumber(); }

  // Takes `c` if it carries the number on. False if it does not: the number
  // is then over, and `c` belongs to whatever follows.
  [[nodiscard]] bool Take(char c) noexcept;

  // Whether what has been taken is a number. "1", "1." and ".5" are; "-",
  // "." and "1e" are not.
  bool Valid() const noexcept;

  // The number. One too large to hold comes out as the largest that can
  // be, with `clamped` set. More digits after the point than can matter are
  // dropped.
  Number Value(bool& clamped) const noexcept;

 private:
  enum class Stage : std::uint8_t {
    kStart,
    kSign,          // A sign, and nothing after it yet.
    kWhole,         // Digits before the point.
    kPoint,         // Digits, then the point.
    kLonePoint,     // The point, with no digits before it.
    kFraction,      // Digits after the point.
    kExponentMark,  // The "e".
    kExponentSign,
    kExponent,
  };

  bool TakeExponentMark(char c) noexcept;

  Stage stage_ = Stage::kStart;
  bool negative_ = false;
  bool exponentNegative_ = false;
  bool wholeTooLarge_ = false;
  std::uint8_t fractionDigits_ = 0;
  std::uint16_t exponent_ = 0;
  std::uint32_t whole_ = 0;
  std::uint64_t fraction_ = 0;  // What follows the point, in 1/2^48ths.
};

// A Number as a length or a position, as a factor, and, taken as degrees,
// as an angle.
Fixed FixedOf(Number number, bool& clamped) noexcept;
Coefficient CoefficientOf(Number number, bool& clamped) noexcept;
Angle AngleOfDegrees(Number number) noexcept;

// Reads the number that begins at `text[at]`, and moves `at` past it. False,
// with `at` where it was, if there is no number there.
[[nodiscard]] bool ScanNumber(std::string_view text, std::size_t& at, Number& number, bool& clamped) noexcept;

// The characters SVG counts as space between things.
constexpr bool IsSvgSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

}  // namespace bn::draw
