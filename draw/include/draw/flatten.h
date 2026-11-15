#pragma once

#include <cstdint>

#include "draw/fixed.h"
#include "draw/svg_path.h"

namespace bn::draw {

// Takes an outline that has been made into straight lines, in pixels.
class LineSink {
 public:
  // Begins a new part of the outline at `to`.
  virtual void MoveTo(Point to) noexcept = 0;
  virtual void LineTo(Point to) noexcept = 0;
  // Draws back to where this part began, and ends it.
  virtual void Close() noexcept = 0;

 protected:
  ~LineSink() = default;  // Never destroyed through the interface.
};

// Turns an outline in a drawing's own coordinates, curves and all, into
// straight lines in pixels.
//
// It is a PathSink, so it can be what an SvgPathReader tells. Each point is
// put through `matrix`, and each curve is cut into as many lines as it
// takes for none of them to stand further from the curve than `tolerance`.
// A flat curve takes few lines and a tight one many, up to 1,024.
//
// It keeps nothing of the outline: each line goes to the LineSink as it is
// made. The object is under a hundred bytes, and cutting a curve takes
// about half a kilobyte of stack.
//
// An arc is first made into curves, each an eighth of a turn at most and
// less on an ellipse thousands of pixels across, where the drawing's
// coordinates are, so it comes out right whatever `matrix` does to it.
// What SVG says of an arc that cannot be drawn as asked is
// followed: radii too short to reach are made just long enough, and with
// no radius at all it is a line. An arc so shallow that it stays within
// the tolerance of a straight line is drawn as that line, which is what
// keeps a radius of any size from being a difficulty.
//
// What one thing it is told can cost is bounded, whatever the numbers in
// it: a line is one line, a curve kMostLinesPerCurve at the most, and an
// arc kMostLinesPerArc.
class PathFlattener final : public PathSink {
 public:
  // A quarter of a pixel.
  static constexpr Fixed kDefaultTolerance = 1024;

  // The most lines a curve becomes: it is cut in half ten times at the
  // most. And the most an arc becomes, which is thirty-two curves at the
  // most: a whole turn, in the smallest pieces an arc is ever cut into.
  static constexpr std::uint32_t kMostLinesPerCurve = 1024;
  static constexpr std::uint32_t kMostLinesPerArc = 32768;

  PathFlattener(LineSink& sink, const Matrix& matrix, Fixed tolerance = kDefaultTolerance) noexcept;

  void MoveTo(Point to) noexcept override;
  void LineTo(Point to) noexcept override;
  void QuadTo(Point control, Point to) noexcept override;
  void CubicTo(Point control1, Point control2, Point to) noexcept override;
  void ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept override;
  void Close() noexcept override;

  // A point came out further off than a position can be, and was held at
  // the edge.
  bool Clamped() const noexcept { return clamped_; }
  // Lines given to the sink so far.
  std::uint32_t Lines() const noexcept { return lines_; }

 private:
  Point InPixels(Point point) noexcept;
  void Line(Point to) noexcept;
  bool NearlyStraight(Fixed halfX, Fixed halfY, Fixed radiusX, Fixed radiusY) const noexcept;

  LineSink* sink_;
  Matrix matrix_;
  Coefficient largestScale_;
  Fixed tolerance_;
  std::uint64_t flatness_;  // Sixteen times the square of the tolerance.
  bool clamped_ = false;
  std::uint32_t lines_ = 0;
  // Where the outline has got to and where this part of it began, in the
  // drawing's coordinates and in pixels.
  Point current_;
  Point start_;
  Point currentPixels_;
  Point startPixels_;
};

}  // namespace bn::draw
