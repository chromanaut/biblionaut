#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_path.h"

namespace bn::draw {

// How the corners of a stroke are drawn, and its open ends.
enum class StrokeJoin : std::uint8_t { kMiter, kRound, kBevel };
enum class StrokeCap : std::uint8_t { kButt, kRound, kSquare };

// What a stroke looks like, in the units of the outline it is drawn along.
struct StrokeStyle {
  static constexpr std::size_t kMostDashes = 8;

  Fixed width = kFixedOne;
  StrokeJoin join = StrokeJoin::kMiter;
  StrokeCap cap = StrokeCap::kButt;
  // How far a miter may reach from the corner, in widths of the stroke:
  // past it, the corner is bevelled. Taken as one if it is less.
  Fixed miterLimit = 4 * kFixedOne;
  // The lengths of the dashes and of the gaps between them, taken in turn
  // and over again; an odd number of them is taken twice over, as SVG has
  // it. None, or any less than nothing, or all of them nothing, and the
  // stroke is solid. `dashOffset` is how far into the pattern each part of
  // the outline begins.
  std::array<Fixed, kMostDashes> dashes{};
  std::uint8_t dashCount = 0;
  Fixed dashOffset = 0;

  friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

// Draws a line along an outline: turns the outline's lines into the
// outline of the stroke, which filled by the nonzero rule is the stroke.
//
// It is a LineSink, so a PathFlattener with no matrix can hand it an
// outline in the coordinates it was written in, and what it makes goes on
// to a PathSink, a PathFlattener with the outline's matrix, which puts it
// into pixels. So the stroke is as wide as SVG has it in the outline's own
// coordinates, and a matrix that stretches one way more than the other
// stretches the stroke with it.
//
// Each line becomes a piece as wide as the stroke; each corner between two
// lines, a piece that fills its outer side as `join` says; each open end,
// a piece as `cap` says. Every piece goes round the same way, so where any
// overlap they add up, and the nonzero rule fills them as one. A part of
// the outline that has no length is a dot, round or square, if the ends
// are. A round corner or end is cut into sides of at most a sixty-fourth
// of a turn: for a stroke 100 pixels wide they stand 0.06 of a pixel
// inside the circle at the most. A dashed part that is closed is drawn as
// if it were open where it begins and ends.
//
// Where two lines meet, their pieces end along the line that halves the
// corner, so that they share that edge and do not overlap, and the
// corner's own piece shares its edges with them. That matters because the
// filler counts a pixel twice where two overlapping pieces' edges cross
// it (see Filler). Only where a corner is too sharp for the lines beside
// it, so that the halving line would reach past the middle of either, do
// the pieces end square and overlap on the corner's inner side, and the
// pixels along that side's edge there come out darker than they should.
//
// It keeps nothing of the outline but where it is, where the part in hand
// began, and its first and last lines until it is known how they begin
// and end. One line given to it becomes no more than kMostLinesPerLine
// lines, and the end of a part or of a dash no more than kMostLinesPerEnd.
// No more than kMostDashesPerOutline dashes are begun between one Begin()
// and the next: past them the stroke is solid, which DashesCut() says, so
// that no pattern can make one outline endless work. Used by one task.
class Stroker final : public LineSink {
 public:
  static constexpr std::uint32_t kMostDashesPerOutline = 4096;
  // How many sides a whole turn of a round corner, end or dot is cut into.
  static constexpr std::uint32_t kRoundSteps = 64;
  // A line's piece has six sides at the most; a corner, half a turn of
  // round edge, its two ends and the corner itself; a round end, half a
  // turn and the line across. A line given to it gives out the piece of
  // the line before and the corner between them. A part's end, or a
  // dash's, gives out the pieces of its first and last lines, and two ends
  // or the corner that closes it; or a dot.
  static constexpr std::uint32_t kMostLinesPerPiece = 6;
  static constexpr std::uint32_t kMostLinesPerJoin = (kRoundSteps / 2) + 2;
  static constexpr std::uint32_t kMostLinesPerCap = (kRoundSteps / 2) + 1;
  static constexpr std::uint32_t kMostLinesPerLine = kMostLinesPerPiece + kMostLinesPerJoin;
  static constexpr std::uint32_t kMostLinesPerEnd = (2 * kMostLinesPerPiece) + (2 * kMostLinesPerCap);

  // Begins a stroke. What it makes goes to `out`, as closed outlines.
  void Begin(PathSink& out, const StrokeStyle& style) noexcept;
  // Ends the part of the outline in hand, if one is: its ends are capped.
  // Call it once the outline is over.
  void Finish() noexcept;
  // A dash pattern came to more than kMostDashesPerOutline dashes.
  bool DashesCut() const noexcept { return dashesCut_; }
  // A point of the stroke was further off than a position can be, and was
  // held at the edge.
  bool Clamped() const noexcept { return clamped_; }

  void MoveTo(Point to) noexcept override;
  void LineTo(Point to) noexcept override;
  void Close() noexcept override;

 private:
  // A way along the outline, a unit long in 2^30ths.
  struct Way {
    std::int64_t x = kUnitOne;
    std::int64_t y = 0;
  };
  // How a line's piece ends where it meets the next, or begins: from its
  // left side to its right, by way of `middle` if it has one.
  struct Edge {
    Point left;
    Point right;
    Point middle;
    bool hasMiddle = false;
  };
  // A line of the piece in hand that has yet to be given out.
  struct Line {
    Point to;
    Way way;
    std::int64_t length = 0;
    Edge start;
    Edge end;
  };

  void StartPart(Point at) noexcept;
  void Segment(Point to) noexcept;
  // A dash, or the whole of a part if there are no dashes.
  void PieceBegin(Point at) noexcept;
  void PieceLine(Point from, Point to, const Way& way, std::int64_t length) noexcept;
  void PieceEnd(bool closing) noexcept;
  // The corner at `at` between `before` and the line that goes on from it:
  // gives out its piece, and says how the two lines' pieces end and begin.
  void Join(Point at, const Line& before, const Way& after, std::int64_t afterLength, Edge& end, Edge& start) noexcept;
  void Cap(Point at, const Way& way) noexcept;
  void Dot(Point at, const Way& way) noexcept;
  Edge Square(Point at, const Way& way) noexcept;
  void Emit(const Edge& start, const Edge& end) noexcept;
  // `at` moved by (x, y), in half-widths of the stroke, in 2^30ths.
  Point Off(Point at, std::int64_t x, std::int64_t y) noexcept;
  // The points between `from`, a half-width from `at` that way, and where
  // it ends up turned by `turn` from x towards -y, in `steps` even steps:
  // not the two ends.
  void Arc(Point at, const Way& from, Angle turn, std::uint32_t steps) noexcept;

  PathSink* out_ = nullptr;
  StrokeStyle style_;
  Fixed half_ = 0;
  bool clamped_ = false;

  // Dashes: whether there are, how many lengths the pattern has in all
  // (twice those given, if they are an odd number) and how long it is,
  // which is in hand, how much of it is left, and whether it is a dash or
  // a gap.
  bool dashed_ = false;
  std::uint8_t dashTotal_ = 0;
  std::int64_t dashCycle_ = 0;
  std::uint8_t dashIndex_ = 0;
  std::int64_t dashLeft_ = 0;
  bool dashOn_ = true;
  std::uint32_t dashesBegun_ = 0;
  bool dashesCut_ = false;
  Way segmentWay_;  // The way of the line the dashes are along.

  // The part of the outline in hand.
  bool inPart_ = false;
  bool partHasSegment_ = false;  // A line, even one of no length, or a close.
  Point partStart_;
  Point current_;

  // The piece in hand: where it began, its first line, and its last.
  bool inPiece_ = false;
  bool pieceHasLine_ = false;
  bool hasLast_ = false;
  Point pieceStart_;
  Line first_;
  Line last_;
};

}  // namespace bn::draw
