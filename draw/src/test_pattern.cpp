#include "draw/test_pattern.h"

#include <algorithm>

namespace bn::draw {

namespace {

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

// Whether the pixel `dx` across and `dy` down inside a textured rectangle is
// black.
using Texture = bool (*)(int dx, int dy);

bool VerticalLines(int dx, int /*dy*/) { return dx % 2 == 0; }
bool HorizontalLines(int /*dx*/, int dy) { return dy % 2 == 0; }
bool Checks1(int dx, int dy) { return (dx + dy) % 2 == 0; }
bool Checks2(int dx, int dy) { return ((dx / 2) + (dy / 2)) % 2 == 0; }
bool Checks8(int dx, int dy) { return ((dx / 8) + (dy / 8)) % 2 == 0; }
bool QuarterDots(int dx, int dy) { return dx % 2 == 0 && dy % 2 == 0; }
bool ThreeQuarterDots(int dx, int dy) { return dx % 2 == 0 || dy % 2 == 0; }
bool HorizontalBars(int /*dx*/, int dy) { return (dy / 8) % 2 == 0; }
bool VerticalBars(int dx, int /*dy*/) { return (dx / 8) % 2 == 0; }

// The rows of the picture that are being rendered: everything drawn is
// clipped to them.
class Band {
 public:
  Band(std::span<std::uint8_t> bits, int width, int top) noexcept
      : bits_(bits),
        width_(width),
        rowBytes_((width + 7) / 8),
        top_(top),
        rows_(rowBytes_ > 0 ? static_cast<int>(bits.size()) / rowBytes_ : 0) {}

  void Fill(Rect rect, bool black) noexcept {
    const Rect clipped = Clip(rect);
    for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
      FillSpan(y, clipped.x, clipped.x + clipped.w, black);
    }
  }

  // The outline of a rectangle, `stroke` pixels thick, drawn inside it.
  void Frame(Rect rect, int stroke) noexcept {
    Fill({rect.x, rect.y, rect.w, stroke}, true);
    Fill({rect.x, rect.y + rect.h - stroke, rect.w, stroke}, true);
    Fill({rect.x, rect.y, stroke, rect.h}, true);
    Fill({rect.x + rect.w - stroke, rect.y, stroke, rect.h}, true);
  }

  void FillTexture(Rect rect, Texture texture) noexcept {
    const Rect clipped = Clip(rect);
    for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
      for (int x = clipped.x; x < clipped.x + clipped.w; ++x) {
        FillSpan(y, x, x + 1, texture(x - rect.x, y - rect.y));
      }
    }
  }

  int Top() const noexcept { return top_; }
  int Bottom() const noexcept { return top_ + rows_; }

 private:
  // The part of `rect` that lies in this band; empty (w or h of 0) if none.
  Rect Clip(Rect rect) const noexcept {
    const int left = std::max(rect.x, 0);
    const int right = std::min(rect.x + rect.w, width_);
    const int top = std::max(rect.y, top_);
    const int bottom = std::min(rect.y + rect.h, top_ + rows_);
    return {left, top, std::max(right - left, 0), std::max(bottom - top, 0)};
  }

  // Pixels [x0, x1) of picture row `y`, which must lie inside the band.
  void FillSpan(int y, int x0, int x1, bool black) noexcept {
    if (x0 >= x1) {
      return;
    }
    std::uint8_t* row = bits_.data() + (static_cast<std::size_t>(y - top_) * static_cast<std::size_t>(rowBytes_));
    const int firstByte = x0 / 8;
    const int lastByte = (x1 - 1) / 8;
    // Within a byte the leftmost pixel is the top bit.
    const auto firstMask = static_cast<std::uint8_t>(0xFFU >> static_cast<unsigned>(x0 % 8));
    const auto lastMask = static_cast<std::uint8_t>(0xFFU << static_cast<unsigned>(7 - ((x1 - 1) % 8)));
    for (int byte = firstByte; byte <= lastByte; ++byte) {
      std::uint8_t mask = 0xFF;
      if (byte == firstByte) {
        mask &= firstMask;
      }
      if (byte == lastByte) {
        mask &= lastMask;
      }
      // Unsigned arithmetic throughout: a narrow type would be widened to a
      // signed one by the operators.
      const unsigned bits = row[byte];
      row[byte] = static_cast<std::uint8_t>(black ? bits & ~static_cast<unsigned>(mask) : bits | mask);
    }
  }

  std::span<std::uint8_t> bits_;
  int width_;
  int rowBytes_;
  int top_;
  int rows_;
};

// Rows the left-hand strip spends on each colour: the height of the bands
// pictures are sent to a panel in.
constexpr int kStripPeriod = 80;

}  // namespace

void TestPattern::RenderRows(std::uint16_t firstRow, std::span<std::uint8_t> out) const noexcept {
  // Not memset: `out` may be empty, and memset must not be handed the null
  // pointer an empty span can have, even to write nothing.
  std::fill(out.begin(), out.end(), std::uint8_t{0xFF});
  const int width = width_;
  const int height = height_;
  if (width <= 0 || height <= 0) {
    return;
  }
  // Rows past the bottom stay white.
  const std::size_t rowsInPicture = firstRow < height_ ? static_cast<std::size_t>(height_ - firstRow) : 0;
  const std::size_t rows = std::min(out.size() / RowBytes(), rowsInPicture);
  Band band(out.first(rows * RowBytes()), width, firstRow);

  // The frame around the edge.
  band.Frame({0, 0, width, height}, 2);

  // The strip that changes every kStripPeriod rows.
  for (int top = 0, block = 0; top < height; top += kStripPeriod, ++block) {
    if (block % 2 == 0) {
      band.Fill({6, top, 8, std::min(kStripPeriod, height - top)}, true);
    }
  }

  // The diagonal, first pixel to last. Each row's piece reaches the start of
  // the next row's, so the line has no gaps however steep it is.
  if (height > 1) {
    for (int y = band.Top(); y < band.Bottom(); ++y) {
      const int x = (y * (width - 1)) / (height - 1);
      const int next = ((y + 1) * (width - 1)) / (height - 1);
      band.Fill({x, y, std::max(next - x + 1, 3), 1}, true);
    }
  }

  // The marks that tell the corners apart. Everything from here on is placed
  // clear of the diagonal.
  band.Fill({32, 96, 16, 96}, true);   // The F: its stem,
  band.Fill({32, 96, 64, 16}, true);   // top bar,
  band.Fill({32, 136, 48, 16}, true);  // and middle bar.
  band.Fill({width - 72, 32, 40, 40}, true);
  band.Frame({32, height - 72, 40, 40}, 4);

  // Fine detail, above the diagonal.
  band.FillTexture({250, 40, 96, 96}, VerticalLines);
  band.FillTexture({362, 40, 96, 96}, HorizontalLines);
  band.FillTexture({474, 40, 96, 96}, Checks1);
  band.FillTexture({586, 40, 96, 96}, Checks2);
  band.FillTexture({440, 152, 96, 96}, QuarterDots);
  band.FillTexture({552, 152, 96, 96}, ThreeQuarterDots);
  band.FillTexture({664, 152, 96, 96}, Checks8);

  // What changes from step to step, below the diagonal.
  for (int box = 0; box < kStepCount; ++box) {
    const Rect rect{60 + (72 * box), 300, 48, 48};
    if (box < step_) {
      band.Fill(rect, true);
    } else {
      band.Frame(rect, 3);
    }
  }
  const Rect turning{100, 372, 160, 64};
  band.FillTexture(turning, step_ % 2 == 1 ? HorizontalBars : VerticalBars);
  band.Frame(turning, 2);
  band.Fill({292, 372, 96, 64}, true);
}

}  // namespace bn::draw
