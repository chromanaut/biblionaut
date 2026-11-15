#pragma once

#include <string_view>

#include "draw/fixed.h"

namespace bn::draw {

// Reads the value of an SVG `transform`: a list of `matrix(...)`,
// `translate(...)`, `scale(...)`, `rotate(...)`, `skewX(...)` and
// `skewY(...)`, with spaces or a comma between them and between their
// numbers. `matrix` becomes all of them as one, which is what the first
// does after the second, after the third, and so on: the last in the list
// is the first to be done to a point.
//
// False if the text is not such a list, and then `matrix` is left as it
// was: SVG has an element with a transform that cannot be read not drawn at
// all. An empty list is one, and changes nothing.
//
// `clamped` is set if a number was too large to hold, or the matrices
// multiplied out to one, and was held at the largest that fits.
[[nodiscard]] bool ParseSvgTransform(std::string_view text, Matrix& matrix, bool& clamped) noexcept;

}  // namespace bn::draw
