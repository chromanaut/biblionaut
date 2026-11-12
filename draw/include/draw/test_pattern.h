#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::draw {

// A test picture for a panel, produced a band of rows at a time, the way
// every picture reaches the panel. Each part of it is there to make one kind
// of fault visible:
//
//   - a frame around the very edge: a picture that is shifted or cut off;
//   - a diagonal from the first pixel to the last: rows out of order, or a
//     band put in the wrong place, breaks the line;
//   - a strip down the left that changes colour every 80 rows;
//   - a letter F near the first corner, a solid square in the corner along
//     the first row and a hollow one in the corner down the first column: a
//     picture that is mirrored or turned;
//   - squares of fine lines, checks and dots: bits in the wrong order within
//     a byte, and how cleanly the panel shows single pixels;
//   - four boxes, as many filled as the step number, and a block whose
//     stripes turn with each step: something that changes between one
//     picture and the next, for refreshes that only redraw what changed.
//
// It is laid out for 800 x 480; on another size whatever falls outside is
// clipped.
class TestPattern {
 public:
  // Steps run from 1 to kStepCount.
  static constexpr std::uint8_t kStepCount = 4;

  constexpr TestPattern(std::uint16_t width, std::uint16_t height, std::uint8_t step) noexcept
      : width_(width), height_(height), step_(step) {}

  // Bytes in one row: a bit per pixel.
  constexpr std::size_t RowBytes() const noexcept { return (static_cast<std::size_t>(width_) + 7) / 8; }

  // Renders rows of the picture into `out`, starting at row `firstRow`, as
  // many as `out` has room for whole rows of. In each row the first byte is
  // the leftmost eight pixels, its top bit the leftmost, and a set bit is
  // white. Rows past the bottom of the picture come out white.
  void RenderRows(std::uint16_t firstRow, std::span<std::uint8_t> out) const noexcept;

 private:
  std::uint16_t width_;
  std::uint16_t height_;
  std::uint8_t step_;
};

}  // namespace bn::draw
