#pragma once

#include <cstdint>

// How a colour, and a colour that is not wholly solid, become the gray a
// picture is drawn in. Every kind of picture is turned to gray the same
// way, so that a colour looks the same in a drawing as in a photograph.
// How many grays a panel can show is another matter, settled after this.

namespace bn::draw {

// A colour as the gray it looks, from black (0) to white (255): 0.299 of
// its red, 0.587 of its green and 0.114 of its blue, which is how bright
// each looks, and what a JPEG's gray is.
constexpr std::uint8_t GrayOf(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
  return static_cast<std::uint8_t>(((77U * red) + (150U * green) + (29U * blue) + 128U) >> 8U);
}

// `gray`, as solid as `alpha` says (255: wholly; 0: not at all), laid on
// white paper: what shows of a pixel that the page shows through. Rounded
// to the nearest, a half up.
constexpr std::uint8_t OverWhite(std::uint8_t gray, std::uint8_t alpha) noexcept {
  // (gray * alpha + 255 * (255 - alpha)) / 255, rounded. Dividing by 255
  // this way is exact for every number the two can make.
  const std::uint32_t mixed = (std::uint32_t{gray} * alpha) + (255U * (255U - alpha)) + 128U;
  return static_cast<std::uint8_t>((mixed + (mixed >> 8U)) >> 8U);
}

}  // namespace bn::draw
