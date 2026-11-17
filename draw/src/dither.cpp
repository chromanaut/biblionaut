#include "draw/dither.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::draw {

bool Ditherer::Begin(std::size_t width, std::uint32_t levels, std::span<std::int16_t> errors) noexcept {
  width_ = 0;
  if (width == 0 || width > kMostWidth || levels < 2 || levels > kMostLevels || errors.size() < ErrorsFor(width)) {
    return false;
  }
  width_ = width;
  levels_ = levels;
  errors_ = errors.first(ErrorsFor(width));
  std::fill(errors_.begin(), errors_.end(), std::int16_t{0});
  return true;
}

bool Ditherer::Row(std::span<std::uint8_t> row) noexcept {
  if (width_ == 0 || row.size() != width_) {
    return false;
  }
  // What is carried is kept in sixteenths of a gray, so that the parts of
  // it add up to the whole.
  const std::span<std::int16_t> here = errors_.first(width_ + 2);
  const std::span<std::int16_t> below = errors_.subspan(width_ + 2, width_ + 2);
  const auto steps = static_cast<std::int32_t>(levels_ - 1);
  for (std::size_t x = 0; x < width_; ++x) {
    // Pixel x is at x + 1 in each row of what is carried.
    const std::int32_t wanted = (std::int32_t{row[x]} * 16) + here[x + 1];
    // The nearest level to what is wanted, held to black and white.
    const std::int32_t clamped = std::clamp<std::int32_t>(wanted, 0, 255 * 16);
    const std::int32_t level = ((clamped * steps) + (255 * 8)) / (255 * 16);
    const std::int32_t gray = ((level * 255) + (steps / 2)) / steps;
    row[x] = static_cast<std::uint8_t>(gray);
    // What is left over, to the right and below. Held to what a gray can
    // be, so that what is carried cannot grow without end.
    const std::int32_t left = std::clamp<std::int32_t>(wanted - (gray * 16), -255 * 16, 255 * 16);
    // The last part is what the others leave, so that nothing is lost to
    // rounding. None of them can come to more than a gray's worth, and a
    // pixel is given parts from four at the most: within 16 bits.
    const std::int32_t right = (left * 7) / 16;
    const std::int32_t belowLeft = (left * 3) / 16;
    const std::int32_t straightBelow = (left * 5) / 16;
    here[x + 2] = static_cast<std::int16_t>(here[x + 2] + right);
    below[x] = static_cast<std::int16_t>(below[x] + belowLeft);
    below[x + 1] = static_cast<std::int16_t>(below[x + 1] + straightBelow);
    below[x + 2] = static_cast<std::int16_t>(below[x + 2] + (left - right - belowLeft - straightBelow));
  }
  // The row below is the next one's to take from.
  std::copy(below.begin(), below.end(), here.begin());
  std::fill(below.begin(), below.end(), std::int16_t{0});
  return true;
}

}  // namespace bn::draw
