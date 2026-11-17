#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "draw/fixed.h"
#include "draw/gray.h"
#include "draw/svg_number.h"

// The small things an SVG file's attributes are written in, other than
// path data and transforms: a length, a paint, an opacity, a view box, and
// how a view box is to be fitted. Each is read from the whole of its text,
// with any space around it, and each reading says whether the text was
// one.

namespace bn::draw {

// ---- Lengths ----

enum class SvgUnit : std::uint8_t {
  kNone,     // A bare number: units of the drawing.
  kPercent,  // Of the width, the height or the size of what the drawing is seen through.
  kPx,
  kPt,  // A 72nd of an inch.
  kPc,  // Twelve points.
  kMm,
  kCm,
  kIn,  // 96 pixels.
  kEm,  // The height of the type, which is taken to be 16 pixels,
  kEx,  // and half of it.
};

struct SvgLength {
  Number value = 0;
  SvgUnit unit = SvgUnit::kNone;
};

// Reads "12", "1.5px", "50%", "2em". False if the text is not a number
// with nothing after it or one of those units.
[[nodiscard]] bool ParseSvgLength(std::string_view text, SvgLength& length, bool& clamped) noexcept;

// Reads a list of lengths with commas or space between them, as
// `stroke-dasharray` is written: "5, 3", "2px 1 4%". `count` is how many
// there were. False if anything in it is not a length, or there are more
// than `lengths` has room for, or none.
[[nodiscard]] bool ParseSvgLengths(std::string_view text, std::span<SvgLength> lengths, std::size_t& count,
                                   bool& clamped) noexcept;

// A length in units of the drawing. `whole` is what a percentage is of.
Fixed LengthIn(const SvgLength& length, Fixed whole, bool& clamped) noexcept;

// ---- Paint ----

// A colour is drawn as the gray it looks: GrayOf() in draw/gray.h.

// What a shape is filled with.
struct SvgPaint {
  enum class Kind : std::uint8_t {
    kNone,     // Nothing.
    kGray,     // A colour, as `gray`, as solid as `alpha` says.
    kCurrent,  // Whatever `color` is.
    kInherit,  // What the element it is in has.
  };

  Kind kind = Kind::kGray;
  std::uint8_t gray = 0;
  std::uint8_t alpha = 255;
  // The paint was written as a reference to a gradient or a pattern
  // defined elsewhere in the file. The rest is then what was written after
  // it to be used in its place, or nothing if nothing was.
  bool server = false;

  friend bool operator==(const SvgPaint&, const SvgPaint&) = default;
};

// Reads a colour: "#rgb", "#rrggbb", either with a fourth part for how
// solid it is; "rgb(...)" and "rgba(...)", in whole numbers or percentages;
// the names of CSS's seventeen basic colours, "grey", "cyan" and "magenta";
// "transparent". False if the text is none of those.
[[nodiscard]] bool ParseSvgColour(std::string_view text, std::uint8_t& gray, std::uint8_t& alpha) noexcept;

// Reads a paint: a colour, "none", "currentColor", "inherit", or
// "url(...)", which may have any of the first three after it. False if the
// text is none of those.
[[nodiscard]] bool ParseSvgPaint(std::string_view text, SvgPaint& paint) noexcept;

// Reads how solid something is, "0.5" or "50%", as a part of 255. What is
// under nothing or over the whole is held to them. False if the text is
// not a number or a percentage.
[[nodiscard]] bool ParseSvgOpacity(std::string_view text, std::uint8_t& opacity) noexcept;

// ---- Boxes ----

struct SvgBox {
  Fixed x = 0;
  Fixed y = 0;
  Fixed width = 0;
  Fixed height = 0;

  friend bool operator==(const SvgBox&, const SvgBox&) = default;
};

// Reads a `viewBox`: four numbers, with spaces or commas between them.
// False if the text is not four numbers. A width or height that is nothing
// or less is read as written: whoever uses the box has to look.
[[nodiscard]] bool ParseSvgViewBox(std::string_view text, SvgBox& box, bool& clamped) noexcept;

// How a view box is fitted into the space there is for it, which is what
// `preserveAspectRatio` says.
struct SvgFit {
  enum class Align : std::uint8_t { kMin, kMid, kMax };

  // Where the box goes, across and down, if it does not fill the space.
  Align x = Align::kMid;
  Align y = Align::kMid;
  // The box is stretched to fill the space each way, and its shape is not
  // kept ("none").
  bool stretch = false;
  // The box is made large enough to cover the space, so that some of it is
  // outside ("slice"), and not small enough to lie inside it ("meet").
  bool slice = false;

  friend bool operator==(const SvgFit&, const SvgFit&) = default;
};

// Reads "none", or "xMidYMid" and the eight like it, with "meet" or
// "slice" after it or neither. False if the text is not one.
[[nodiscard]] bool ParseSvgFit(std::string_view text, SvgFit& fit) noexcept;

// The matrix that puts `box` where `fit` has it within `space`. Both have
// to have a width and a height of more than nothing.
Matrix FitBox(const SvgBox& box, const SvgBox& space, const SvgFit& fit, bool& clamped) noexcept;

// `text` without the space at either end of it.
std::string_view WithoutSpace(std::string_view text) noexcept;

// Whether `text` is `word`, in capitals or small letters. `word` is
// written in small letters.
bool IsWord(std::string_view text, std::string_view word) noexcept;

}  // namespace bn::draw
