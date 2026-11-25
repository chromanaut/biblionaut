#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::draw {

// Turns a picture of 256 grays into one of fewer, a row at a time, for a
// panel that shows only so many: two on a panel driven black and white,
// four or more on one driven in grays. How many is the panel's; nothing
// here assumes a number.
//
// Each pixel becomes the nearest of the levels, evenly spaced from black
// to white, to its gray and what was carried to it, and what that leaves
// over is carried on to the pixels after it and below it, as Floyd and
// Steinberg did it: seven sixteenths to the next in its row, and three,
// five and one to the three beneath. So over any patch the levels come
// out, on the whole, as light as the grays were, and a gray between two
// levels comes out as a mixture of them. Black and white, and any gray
// that is one of the levels exactly, stay as they are where nothing is
// carried to them.
//
// The rows are given in order from the top, as a picture drawn in strips
// produces them: what is carried down from the last row of one strip goes
// into the first of the next. The memory it carries in is the caller's,
// ErrorsFor() of it; the object itself is the gray of each level, 256
// bytes, and little else. Used by one task.
//
// A pixel that is white or black, with nothing carried to it, is passed
// over: it is a level already, and leaves nothing to carry on. That is
// most of a page of text or of a drawing.
class Ditherer {
 public:
  // The most levels, and the widest row.
  static constexpr std::uint32_t kMostLevels = 256;
  static constexpr std::size_t kMostWidth = 16384;

  static constexpr std::size_t ErrorsFor(std::size_t width) noexcept { return 2 * (width + 2); }

  // Begins a picture `width` pixels wide, in `levels` levels. False, if
  // the width or the levels are not ones it can do (from 2 to kMostLevels),
  // or `errors` has less room than ErrorsFor(width).
  [[nodiscard]] bool Begin(std::size_t width, std::uint32_t levels, std::span<std::int16_t> errors) noexcept;

  // The next row down, `width` grays, made the levels in place: each pixel
  // is then the gray of its level. False, changing nothing, if the row is
  // not that wide or Begin() refused.
  [[nodiscard]] bool Row(std::span<std::uint8_t> row) noexcept;

 private:
  std::span<std::int16_t> errors_;  // Two rows, each a pixel wider at either end: this one's, and the next's.
  std::size_t width_ = 0;
  std::uint32_t levels_ = 0;
  std::array<std::uint8_t, kMostLevels> grays_{};  // The gray of each level, worked out once.
};

}  // namespace bn::draw
