#include "draw/fill.h"

#include <algorithm>

namespace bn::draw {

namespace {

// Lines are placed to a 256th of a pixel, and a pixel wholly covered by a
// shape that goes round it once is 256 in its cell.
constexpr int kStepBits = 8;
constexpr unsigned kStepShift = 8;  // The same, for shifting a number that has no sign.
constexpr std::int32_t kPixel = 256;

// How far `top` may be from the picture's first row.
constexpr std::int32_t kFurthestTop = 1048576;

// `over` over `under`, which is more than nothing, rounded down. Most of
// what is divided here is small, and the device divides 32 bits far more
// quickly than 64.
std::int64_t Floored(std::int64_t over, std::int64_t under) noexcept {
  if (under <= 0) {
    return 0;  // Nothing here divides by that; and nothing could.
  }
  if (over >= INT32_MIN && over <= INT32_MAX && under <= INT32_MAX) {
    const auto small = static_cast<std::int32_t>(over);
    const auto by = static_cast<std::int32_t>(under);
    const std::int32_t quotient = small / by;
    return small % by < 0 ? quotient - 1 : quotient;
  }
  const std::int64_t quotient = over / under;
  return over % under < 0 ? quotient - 1 : quotient;
}

// The row a height is in.
std::int32_t RowOf(std::int32_t y) noexcept { return ShiftDown(y, kStepBits); }

// How much of a pixel is covered, out of 256, where the outline has gone
// round it `winding` 256ths of a time.
std::uint32_t Covered(std::int32_t winding, FillRule rule) noexcept {
  const auto size = static_cast<std::uint32_t>(winding < 0 ? -winding : winding);
  if (rule == FillRule::kNonZero) {
    return std::min<std::uint32_t>(size, kPixel);
  }
  // Round once is in, round twice is out again, and so on: the count is
  // folded back on itself at each whole number.
  const std::uint32_t folded = size & 511U;
  return folded <= kPixel ? folded : 512U - folded;
}

}  // namespace

bool Filler::Begin(std::span<std::uint8_t> gray, std::size_t width, std::int32_t top,
                   std::span<std::int16_t> cells) noexcept {
  rows_ = 0;
  if (width == 0 || width > kMostWidth || gray.empty() || gray.size() % width != 0) {
    return false;
  }
  const std::size_t rows = gray.size() / width;
  if (rows > kMostRows || top > kFurthestTop || top < -kFurthestTop || cells.size() < CellsFor(width, rows)) {
    return false;
  }
  gray_ = gray;
  cells_ = cells.first(CellsFor(width, rows));
  width_ = static_cast<std::int32_t>(width);
  rows_ = static_cast<std::int32_t>(rows);
  top_ = top * kPixel;
  bottom_ = top_ + (rows_ * kPixel);
  firstRow_ = rows_;
  lastRow_ = -1;
  start_ = {};
  current_ = {};
  cellsTouched_ = 0;
  pixelsVisited_ = 0;
  std::fill(cells_.begin(), cells_.end(), std::int16_t{0});
  // After the cells, for each row: the first cell in use and the last. A
  // first after the last says the row has none.
  const std::size_t extents = width * rows;
  for (std::size_t row = 0; row < rows; ++row) {
    cells_[extents + (2 * row)] = static_cast<std::int16_t>(width_);
    cells_[extents + (2 * row) + 1] = -1;
  }
  return true;
}

Filler::Place Filler::PlaceOf(Point point) noexcept {
  constexpr int kShift = kFixedBits - kStepBits;
  constexpr std::int64_t kHalf = 8;
  return {.x = static_cast<std::int32_t>(ShiftDown(std::int64_t{point.x} + kHalf, kShift)),
          .y = static_cast<std::int32_t>(ShiftDown(std::int64_t{point.y} + kHalf, kShift))};
}

void Filler::MoveTo(Point to) noexcept {
  CloseOutline();
  start_ = PlaceOf(to);
  current_ = start_;
}

void Filler::LineTo(Point to) noexcept {
  const Place place = PlaceOf(to);
  AddLine(current_, place);
  current_ = place;
}

void Filler::Close() noexcept { CloseOutline(); }

void Filler::CloseOutline() noexcept {
  if (current_ != start_) {
    AddLine(current_, start_);
    current_ = start_;
  }
}

void Filler::Add(std::int32_t row, std::int32_t pixel, std::int32_t amount) noexcept {
  // What is right of the strip changes nothing in it. What is left of it
  // covers every pixel of its row, as if it were in the first.
  if (amount == 0 || pixel >= width_) {
    return;
  }
  pixel = std::max<std::int32_t>(pixel, 0);
  const auto wide = static_cast<std::size_t>(width_);
  const auto at = static_cast<std::size_t>(row);
  std::int16_t& cell = cells_[(at * wide) + static_cast<std::size_t>(pixel)];
  // Cells wrap round rather than overflow: see the limit in fill.h.
  cell = static_cast<std::int16_t>(
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(cell) + static_cast<std::uint16_t>(amount)));
  const std::size_t extents = (wide * static_cast<std::size_t>(rows_)) + (2 * at);
  cells_[extents] = std::min(cells_[extents], static_cast<std::int16_t>(pixel));
  cells_[extents + 1] = std::max(cells_[extents + 1], static_cast<std::int16_t>(pixel));
  firstRow_ = std::min(firstRow_, row);
  lastRow_ = std::max(lastRow_, row);
  ++cellsTouched_;
}

void Filler::AddLine(Place from, Place to) noexcept {
  if (rows_ == 0 || from.y == to.y) {
    return;
  }
  // The line is taken from its upper end to its lower, and `sign` says
  // which way it really ran.
  std::int32_t sign = 1;
  if (from.y > to.y) {
    std::swap(from, to);
    sign = -1;
  }
  if (to.y <= top_ || from.y >= bottom_) {
    return;
  }

  // A pixel either side of the strip is kept, and what lies beyond that is
  // cut off: on the left it counts for its height alone, on the right for
  // nothing. Where the line crosses those two edges is worked out from the
  // whole line, so it is the same whichever strip is being drawn.
  const std::int32_t left = -kPixel;
  const std::int32_t right = (width_ + 1) * kPixel;
  if (from.x <= left && to.x <= left) {
    AddBeside(from.y, to.y, sign);
    return;
  }
  if (from.x >= right && to.x >= right) {
    return;
  }
  const std::int64_t dx = std::int64_t{to.x} - from.x;
  const std::int64_t dy = std::int64_t{to.y} - from.y;
  // The height at which the line is `x` across. It only crosses an edge if
  // it is not upright, so `dx` is not nothing here.
  const auto heightAt = [&from, &to, dx, dy](std::int32_t x) noexcept {
    const std::int64_t along = (std::int64_t{x} - from.x) * dy;
    const std::int64_t y = from.y + (dx > 0 ? Floored(along, dx) : Floored(-along, -dx));
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(y, from.y, to.y));
  };

  Place upper = from;
  if (from.x < left) {
    upper = {.x = left, .y = heightAt(left)};
    AddBeside(from.y, upper.y, sign);
  } else if (from.x > right) {
    upper = {.x = right, .y = heightAt(right)};
  }
  Place lower = to;
  if (to.x < left) {
    lower = {.x = left, .y = heightAt(left)};
    AddBeside(lower.y, to.y, sign);
  } else if (to.x > right) {
    lower = {.x = right, .y = heightAt(right)};
  }
  if (upper.y < lower.y) {
    AddAcross(upper.x, upper.y, lower.x, lower.y, sign);
  }
}

void Filler::AddBeside(std::int32_t top, std::int32_t bottom, std::int32_t sign) noexcept {
  top = std::max(top, top_);
  bottom = std::min(bottom, bottom_);
  while (top < bottom) {
    const std::int32_t rowEnd = (RowOf(top) + 1) * kPixel;
    const std::int32_t next = std::min(rowEnd, bottom);
    Add(RowOf(top - top_), 0, sign * (next - top));
    top = next;
  }
}

void Filler::AddAcross(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t sign) noexcept {
  const std::int64_t dx = std::int64_t{x1} - x0;
  const std::int64_t dy = std::int64_t{y1} - y0;
  // How far across the line is at a height: always worked out from its two
  // ends, never from where the last row left off, so that every strip
  // finds it in the same place.
  const auto acrossAt = [x0, y0, x1, y1, dx, dy](std::int32_t y) noexcept {
    if (y == y0) {
      return x0;
    }
    if (y == y1) {
      return x1;
    }
    return static_cast<std::int32_t>(x0 + Floored(dx * (std::int64_t{y} - y0), dy));
  };

  std::int32_t y = std::max(y0, top_);
  const std::int32_t end = std::min(y1, bottom_);
  if (y >= end) {
    return;
  }
  std::int32_t x = acrossAt(y);
  while (y < end) {
    const std::int32_t rowEnd = (RowOf(y) + 1) * kPixel;
    const std::int32_t next = std::min(rowEnd, end);
    const std::int32_t nextX = acrossAt(next);
    AddSlice(RowOf(y - top_), x, nextX, sign * (next - y));
    x = nextX;
    y = next;
  }
}

// What a slice of a line adds to its row. Every pixel to the right of the
// line gains the slice's height, every pixel to the left of it nothing,
// and a pixel the line passes through gains as much of the height as lies
// to the right of the line within it.
//
// Cells hold differences: a cell is what its pixel gains beyond what the
// pixel before it gained. So the cells a slice adds to are those of the
// pixels it passes through and the one after, and what it adds to them
// comes to its height exactly, however the parts are rounded. That is what
// keeps a shape's cover from leaking along the row past its far edge.
void Filler::AddSlice(std::int32_t row, std::int32_t x0, std::int32_t x1, std::int32_t height) noexcept {
  if (height == 0) {
    return;
  }
  const std::int32_t nearer = std::min(x0, x1);
  const std::int32_t further = std::max(x0, x1);
  const std::int32_t first = ShiftDown(nearer, kStepBits);
  const std::int32_t firstEdge = first * kPixel;
  if (further <= firstEdge + kPixel) {
    // All within one pixel. It gains the height less the share that lies
    // left of the line, which goes on the middle of the slice; and the
    // pixel after it gains the rest.
    const std::int32_t middle = (x0 + x1) - (2 * firstEdge);
    const std::int32_t rest = ShiftDown((height * middle) + kPixel, kStepBits + 1);
    Add(row, first, height - rest);
    Add(row, first + 1, rest);
    return;
  }

  // Across several pixels. What each has gained, once the line has passed
  // it, is worked out a pixel at a time and then rounded, and each cell is
  // given the difference between its pixel and the one before.
  const std::int32_t sign = height < 0 ? -1 : 1;
  const auto size = static_cast<std::uint32_t>(height < 0 ? -height : height);
  const auto span = static_cast<std::uint32_t>(further - nearer);
  // What a whole pixel of the line's way across adds, in 16,384ths.
  constexpr unsigned kFiner = 14;
  constexpr std::uint64_t kHalf = 8192;
  const std::uint64_t perPixel = (size << (kFiner + kStepShift)) / span;
  const std::int32_t last = ShiftDown(further - 1, kStepBits);
  const auto inFirst = static_cast<std::uint64_t>(firstEdge + kPixel - nearer);
  const auto inLast = static_cast<std::uint64_t>(further - (last * kPixel));
  const std::uint64_t whole = std::uint64_t{size} << kFiner;

  // The line cuts a corner off the first pixel and off the last, each a
  // triangle; between them the gain goes up by the same amount a pixel.
  constexpr unsigned kHalfOfSquare = 1 + (2 * kStepShift);
  std::uint64_t gained = (perPixel * inFirst * inFirst) >> kHalfOfSquare;
  std::int32_t given = 0;
  for (std::int32_t pixel = first; pixel <= last; ++pixel) {
    if (pixel == last) {
      gained = whole - ((perPixel * inLast * inLast) >> kHalfOfSquare);
    } else if (pixel == first + 1) {
      gained = (perPixel * (inFirst + (kPixel / 2))) >> kStepShift;
    } else if (pixel > first) {
      gained += perPixel;
    }
    const auto rounded = static_cast<std::int32_t>((gained + kHalf) >> kFiner);
    Add(row, pixel, sign * (rounded - given));
    given = rounded;
  }
  Add(row, last + 1, sign * (static_cast<std::int32_t>(size) - given));
}

void Filler::Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity) noexcept {
  FillWithin(rule, gray, opacity, nullptr, nullptr);
}

void Filler::Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity, Point from, Point to) noexcept {
  const Place first = PlaceOf(from);
  const Place last = PlaceOf(to);
  FillWithin(rule, gray, opacity, &first, &last);
}

void Filler::FillWithin(FillRule rule, std::uint8_t gray, std::uint8_t opacity, const Place* from,
                        const Place* to) noexcept {
  CloseOutline();
  // Opacity out of 256, so that the whole of it leaves nothing showing.
  const std::uint32_t strength = std::uint32_t{opacity} + (opacity >> 7U);
  const auto wide = static_cast<std::size_t>(width_);
  const std::size_t extents = wide * static_cast<std::size_t>(rows_);
  const bool boxed = from != nullptr && to != nullptr;
  // How much of the pixel, or of the row, that begins at `start` lies
  // between two edges of the box, in 256ths.
  const auto share = [](std::int64_t start, std::int32_t low, std::int32_t high) noexcept {
    const std::int64_t inside = std::min<std::int64_t>(start + kPixel, high) - std::max<std::int64_t>(start, low);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(inside, 0, kPixel));
  };
  for (std::int32_t row = firstRow_; row <= lastRow_; ++row) {
    const auto at = static_cast<std::size_t>(row);
    const std::uint32_t rowShare = boxed ? share(std::int64_t{top_} + (std::int64_t{row} * kPixel), from->y, to->y)
                                         : static_cast<std::uint32_t>(kPixel);
    const std::int32_t first = cells_[extents + (2 * at)];
    const std::int32_t last = cells_[extents + (2 * at) + 1];
    cells_[extents + (2 * at)] = static_cast<std::int16_t>(width_);
    cells_[extents + (2 * at) + 1] = -1;
    // Going along the row, the cells add up to how many times the outline
    // has gone round each pixel. Past the last cell in use that changes no
    // more: the row is done unless the shape runs on to the strip's edge.
    std::uint16_t sum = 0;
    for (std::int32_t pixel = first; pixel < width_; ++pixel) {
      const std::size_t index = (at * wide) + static_cast<std::size_t>(pixel);
      if (pixel <= last) {
        sum = static_cast<std::uint16_t>(sum + static_cast<std::uint16_t>(cells_[index]));
        cells_[index] = 0;
      } else if (sum == 0) {
        break;
      }
      ++pixelsVisited_;
      std::uint32_t cover = Covered(static_cast<std::int16_t>(sum), rule) * strength;
      if (boxed && cover != 0) {
        // Only as much of it as is inside the box, across and down.
        const std::uint32_t inside = ((share(std::int64_t{pixel} * kPixel, from->x, to->x) * rowShare) + 128U) >> 8U;
        cover = ((cover * inside) + 128U) >> 8U;
      }
      if (cover != 0) {
        std::uint8_t& under = gray_[index];
        constexpr std::int32_t kHalf = 32768;
        const std::int32_t change = (std::int32_t{gray} - under) * static_cast<std::int32_t>(cover);
        under = static_cast<std::uint8_t>(under + ShiftDown(change + kHalf, 16));
      }
    }
  }
  firstRow_ = rows_;
  lastRow_ = -1;
}

}  // namespace bn::draw
