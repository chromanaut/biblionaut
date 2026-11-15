#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "draw/fixed.h"
#include "draw/flatten.h"

namespace bn::draw {

// Which parts are inside, of an outline that crosses itself or has other
// outlines within it.
enum class FillRule : std::uint8_t {
  kNonZero,  // Wherever the outline goes round more times one way than the other.
  kEvenOdd,  // Wherever a line out to the edge would cross the outline an odd number of times.
};

// Fills outlines with a gray, a strip of a picture at a time.
//
// It is a LineSink, so it can be what a PathFlattener gives its lines to.
// The lines of one shape are given to it, and then Fill() lays the shape
// over the strip: over whatever is there already, which is how one shape
// comes to lie on top of another. Edges are smoothed: a pixel the outline
// passes through takes as much of the gray as the shape covers of it,
// worked out from the outline itself and not by trying points.
//
// It keeps nothing of an outline. As each line arrives, what it adds to or
// takes from the cover of each pixel is added into a cell for that pixel,
// and the line is forgotten. So the memory it needs is set by the strip,
// two bytes a pixel and four a row, and not by the drawing: an outline of
// a hundred thousand lines takes no more than one of three. A picture
// taller than the memory to hand is drawn as several strips, the drawing
// being gone through once for each, and comes out the same however it is
// cut into strips.
//
// Lines may lie anywhere. What is above or below the strip is passed over,
// what is to the right of it changes nothing, and what is to the left of
// it counts for its height alone.
//
// Nor can a line make a call long: a line adds to no more cells than
// MostCellsPerLine(), wherever it runs, and Fill() looks at each pixel of
// the strip once at the most.
//
// Two limits.
//
// A cell knows how much of its pixel the outline has gone round, counted
// once for each time round, and not which parts of the pixel those were.
// That is exact wherever the count is the same over the whole pixel but
// for one step, which is every pixel with one edge of a shape through it,
// or a corner, or the edge between two parts that overlap. It is only
// near where the outline crosses itself inside the pixel, or runs twice
// along the same edge: there the parts are counted as if they lay side by
// side. A shape drawn twice over in one outline comes out with a heavier
// edge by kNonZero, and by kEvenOdd, where it should not show at all, with
// a line a pixel wide where its edge was.
//
// And an outline that goes round a pixel more than 127 times the same way
// is beyond what a cell can count. Filled by kEvenOdd it still comes out
// right; by kNonZero it can come out empty where it has gone round a
// multiple of 256 times.
//
// Used by one task. The object is 68 bytes on the device.
class Filler final : public LineSink {
 public:
  // The widest a strip can be, and the most rows it can have.
  static constexpr std::size_t kMostWidth = 16384;
  static constexpr std::size_t kMostRows = 16384;

  // How many cells a strip `width` pixels wide and `rows` rows high needs.
  static constexpr std::size_t CellsFor(std::size_t width, std::size_t rows) noexcept { return (width + 2) * rows; }

  // The most cells one line adds to, in a strip of this size.
  static constexpr std::uint32_t MostCellsPerLine(std::uint32_t width, std::uint32_t rows) noexcept {
    return width + (4 * rows) + 4;
  }

  Filler() = default;

  // Sets the strip to draw into. `gray` is its rows, one after another,
  // each `width` bytes, a byte a pixel from black (0) to white (255): they
  // are the caller's, and are drawn over, not cleared. `top` is which row
  // of the picture the first of them is. `cells` is the working memory,
  // CellsFor() of them or more.
  //
  // False, and nothing can be drawn, if the strip is empty or too large,
  // the rows are not a whole number of rows, `top` is more than a million
  // rows from the picture's first, or there are too few cells.
  [[nodiscard]] bool Begin(std::span<std::uint8_t> gray, std::size_t width, std::int32_t top,
                           std::span<std::int16_t> cells) noexcept;

  // The outline of a shape, in pixels of the picture. An outline left open
  // is closed by a line back to where it began.
  void MoveTo(Point to) noexcept override;
  void LineTo(Point to) noexcept override;
  void Close() noexcept override;

  // Lays everything outlined since the last Fill() over the strip, in
  // `gray`. With an `opacity` under 255, what was there shows through.
  void Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity = 255) noexcept;

  // How many times a cell has been added to, and a pixel looked at, since
  // Begin(): what the work so far has come to.
  std::uint32_t CellsTouched() const noexcept { return cellsTouched_; }
  std::uint32_t PixelsVisited() const noexcept { return pixelsVisited_; }

 private:
  // A place in 256ths of a pixel, which is how finely lines are placed.
  struct Place {
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend bool operator==(const Place&, const Place&) = default;
  };

  static Place PlaceOf(Point point) noexcept;
  void CloseOutline() noexcept;
  void AddLine(Place from, Place to) noexcept;
  // A line's part to the left of the strip, from `top` down to `bottom`.
  void AddBeside(std::int32_t top, std::int32_t bottom, std::int32_t sign) noexcept;
  // A line's part across the strip, from (x0, y0) down to (x1, y1).
  void AddAcross(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t sign) noexcept;
  // The part of a line that lies in one row, from x0 at its top to x1 at
  // its bottom, `height` 256ths of a pixel high: less than nothing for a
  // line that runs upwards.
  void AddSlice(std::int32_t row, std::int32_t x0, std::int32_t x1, std::int32_t height) noexcept;
  void Add(std::int32_t row, std::int32_t pixel, std::int32_t amount) noexcept;

  std::span<std::uint8_t> gray_;
  std::span<std::int16_t> cells_;  // A cell a pixel, row by row; then, for each row, the first and last cell in use.
  std::int32_t width_ = 0;
  std::int32_t rows_ = 0;
  std::int32_t top_ = 0;       // The top of the strip, in 256ths of a pixel,
  std::int32_t bottom_ = 0;    // and its bottom.
  std::int32_t firstRow_ = 0;  // The rows that have cells in use,
  std::int32_t lastRow_ = -1;  // none if the last is before the first.
  Place start_;
  Place current_;
  std::uint32_t cellsTouched_ = 0;
  std::uint32_t pixelsVisited_ = 0;
};

}  // namespace bn::draw
