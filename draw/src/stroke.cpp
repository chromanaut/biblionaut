#include "draw/stroke.h"

#include <algorithm>
#include <cstdint>

namespace bn::draw {

namespace {

constexpr int kWayBits = 30;
constexpr std::int64_t kWayOne = 1073741824;
// The most a miter limit is taken to be, which keeps its square in range:
// a miter that long is a corner of a thousandth of a turn.
constexpr Fixed kLongestMiter = 16777216;

std::int64_t Size(std::int64_t value) noexcept { return value < 0 ? -value : value; }

// The way from one point to another, and how far it is.
struct Between {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t length = 0;
};

Between Measure(Point from, Point to) noexcept {
  std::int64_t x = std::int64_t{to.x} - from.x;
  std::int64_t y = std::int64_t{to.y} - from.y;
  // Small enough for their squares to be added.
  int shift = 0;
  while (Size(x) >= 2147483648 || Size(y) >= 2147483648) {
    x = ShiftDown(x, 1);
    y = ShiftDown(y, 1);
    ++shift;
  }
  const std::int64_t length = SquareRoot(static_cast<std::uint64_t>((x * x) + (y * y)));
  if (length == 0) {
    return {};
  }
  return {.x = (x * kWayOne) / length,
          .y = (y * kWayOne) / length,
          .length = length * static_cast<std::int64_t>(std::uint64_t{1} << static_cast<unsigned>(shift))};
}

}  // namespace

void Stroker::Begin(PathSink& out, const StrokeStyle& style) noexcept {
  *this = Stroker();
  out_ = &out;
  style_ = style;
  style_.miterLimit = std::clamp<Fixed>(style.miterLimit, kFixedOne, kLongestMiter);
  half_ = std::max<Fixed>(style.width, 0) / 2;

  // A pattern with a length less than nothing, or with none at all, is no
  // pattern.
  const std::uint8_t count = std::min<std::uint8_t>(style.dashCount, StrokeStyle::kMostDashes);
  std::int64_t sum = 0;
  bool negative = false;
  for (std::uint8_t i = 0; i < count; ++i) {
    negative = negative || style.dashes[i] < 0;
    sum += style.dashes[i];
  }
  dashed_ = count > 0 && !negative && sum > 0;
  if (dashed_) {
    style_.dashCount = count;
    dashTotal_ = static_cast<std::uint8_t>(count % 2 == 1 ? 2 * count : count);
    dashCycle_ = count % 2 == 1 ? 2 * sum : sum;
  }
}

void Stroker::Finish() noexcept {
  if (inPart_) {
    PieceEnd(false);
    inPart_ = false;
  }
}

void Stroker::MoveTo(Point to) noexcept {
  Finish();
  StartPart(to);
}

void Stroker::LineTo(Point to) noexcept {
  if (!inPart_) {
    StartPart(current_);
  }
  Segment(to);
}

void Stroker::Close() noexcept {
  if (!inPart_) {
    return;
  }
  partHasSegment_ = true;
  if (current_ != partStart_) {
    Segment(partStart_);
  }
  // A part drawn whole from where it began has a corner there, not ends.
  PieceEnd(!dashed_ && pieceStart_ == partStart_);
  inPart_ = false;
  current_ = partStart_;
}

void Stroker::StartPart(Point at) noexcept {
  inPart_ = true;
  partHasSegment_ = false;
  partStart_ = at;
  current_ = at;
  if (dashed_) {
    // Where in the pattern the part begins.
    std::int64_t into = style_.dashOffset % dashCycle_;
    if (into < 0) {
      into += dashCycle_;
    }
    dashIndex_ = 0;
    dashLeft_ = style_.dashes[0];
    // A dash of no length at the very start is one, and is drawn.
    while (into > 0 && into >= dashLeft_) {
      into -= dashLeft_;
      dashIndex_ = static_cast<std::uint8_t>((dashIndex_ + 1) % dashTotal_);
      dashLeft_ = style_.dashes[dashIndex_ % style_.dashCount];
    }
    dashLeft_ -= into;
    dashOn_ = dashIndex_ % 2 == 0;
    if (!dashOn_) {
      return;
    }
    if (dashesBegun_ == kMostDashesPerOutline) {
      dashesCut_ = true;
      dashed_ = false;
    } else {
      ++dashesBegun_;
    }
  }
  PieceBegin(at);
}

void Stroker::Segment(Point to) noexcept {
  partHasSegment_ = true;
  const Between between = Measure(current_, to);
  if (between.length == 0) {
    return;
  }
  const Way way = {.x = between.x, .y = between.y};
  if (!dashed_) {
    PieceLine(current_, to, way, between.length);
    current_ = to;
    return;
  }
  segmentWay_ = way;
  // Along the line, the pattern's dashes and gaps begin and end.
  const auto along = [this, &way](std::int64_t distance) noexcept {
    return Point{.x = Clamp32(current_.x + ShiftDown(way.x * distance, kWayBits), clamped_),
                 .y = Clamp32(current_.y + ShiftDown(way.y * distance, kWayBits), clamped_)};
  };
  std::int64_t at = 0;
  Point from = current_;
  while (true) {
    if (dashLeft_ == 0) {
      // A dash or a gap ends here, and the next begins.
      if (dashOn_) {
        PieceEnd(false);
      }
      dashIndex_ = static_cast<std::uint8_t>((dashIndex_ + 1) % dashTotal_);
      dashLeft_ = style_.dashes[dashIndex_ % style_.dashCount];
      dashOn_ = !dashOn_;
      if (dashOn_) {
        if (dashesBegun_ == kMostDashesPerOutline) {
          // The rest is solid.
          dashesCut_ = true;
          dashed_ = false;
          PieceBegin(from);
          if (from != to) {
            PieceLine(from, to, way, between.length - at);
          }
          current_ = to;
          return;
        }
        ++dashesBegun_;
        PieceBegin(from);
      }
      continue;
    }
    if (at == between.length) {
      break;
    }
    const std::int64_t take = std::min(dashLeft_, between.length - at);
    at += take;
    dashLeft_ -= take;
    const Point here = at == between.length ? to : along(at);
    if (dashOn_ && here != from) {
      PieceLine(from, here, way, take);
    }
    from = here;
  }
  current_ = to;
}

void Stroker::PieceBegin(Point at) noexcept {
  inPiece_ = true;
  pieceHasLine_ = false;
  hasLast_ = false;
  pieceStart_ = at;
}

Stroker::Edge Stroker::Square(Point at, const Way& way) noexcept {
  return {.left = Off(at, -way.y, way.x), .right = Off(at, way.y, -way.x), .middle = at, .hasMiddle = false};
}

void Stroker::Emit(const Edge& start, const Edge& end) noexcept {
  // Forwards along the left side and back along the right, as every piece
  // goes.
  out_->MoveTo(start.left);
  out_->LineTo(end.left);
  if (end.hasMiddle) {
    out_->LineTo(end.middle);
  }
  out_->LineTo(end.right);
  out_->LineTo(start.right);
  if (start.hasMiddle) {
    out_->LineTo(start.middle);
  }
  out_->Close();
}

void Stroker::PieceLine(Point from, Point to, const Way& way, std::int64_t length) noexcept {
  if (!pieceHasLine_) {
    // The first line is held until it is known how it begins, which for a
    // part that closes is known at its end.
    pieceHasLine_ = true;
    first_ = {.to = to, .way = way, .length = length, .start = Edge(), .end = Edge()};
    return;
  }
  const Line& before = hasLast_ ? last_ : first_;
  Edge end;
  Edge start;
  Join(from, before, way, length, end, start);
  if (hasLast_) {
    Emit(last_.start, end);
  } else {
    first_.end = end;
  }
  hasLast_ = true;
  last_ = {.to = to, .way = way, .length = length, .start = start, .end = Edge()};
}

void Stroker::PieceEnd(bool closing) noexcept {
  if (!inPiece_) {
    return;
  }
  inPiece_ = false;
  if (!pieceHasLine_) {
    if (partHasSegment_) {
      // A part, or a dash, with no length.
      Dot(pieceStart_, dashed_ ? segmentWay_ : Way());
    }
    return;
  }
  const Line& final = hasLast_ ? last_ : first_;
  if (closing) {
    Edge end;
    Edge start;
    Join(pieceStart_, final, first_.way, first_.length, end, start);
    if (hasLast_) {
      Emit(last_.start, end);
      Emit(start, first_.end);
    } else {
      Emit(start, end);
    }
    return;
  }
  const Edge start = Square(pieceStart_, first_.way);
  if (hasLast_) {
    Emit(start, first_.end);
    Emit(last_.start, Square(last_.to, last_.way));
  } else {
    Emit(start, Square(first_.to, first_.way));
  }
  Cap(pieceStart_, {.x = -first_.way.x, .y = -first_.way.y});
  Cap(final.to, final.way);
}

// The two lines' pieces end along the line that halves the corner, inside
// it, where their inner sides meet, and square on its outer side, from
// which the corner's own piece fills it out as `join` says. That piece
// goes round the same way as the lines' pieces, so it is given in the
// order that keeps to that, and every round edge turns the same way.
void Stroker::Join(Point at, const Line& before, const Way& after, std::int64_t afterLength, Edge& end,
                   Edge& start) noexcept {
  const Way& way = before.way;
  const std::int64_t cross = ShiftDown((way.x * after.y) - (way.y * after.x), kWayBits);
  const std::int64_t dot = ShiftDown((way.x * after.x) + (way.y * after.y), kWayBits);
  if (cross == 0) {
    // Straight on, or straight back: back has a round end, if the corner
    // is round, and a miter or a bevel has nothing to fill.
    end = Square(at, way);
    start = Square(at, after);
    if (dot < 0 && style_.join == StrokeJoin::kRound) {
      Cap(at, way);
    }
    return;
  }
  // The outer side of the corner, where the two lines' sides part: the
  // left of the lines when `side` is one, the right when it is minus one.
  const std::int64_t side = cross < 0 ? 1 : -1;
  const std::int64_t beforeX = side * -way.y;
  const std::int64_t beforeY = side * way.x;
  const std::int64_t afterX = side * -after.y;
  const std::int64_t afterY = side * after.x;
  const Point one = Off(at, beforeX, beforeY);
  const Point other = Off(at, afterX, afterY);
  // 1 + cos of the turn, in 2^30ths. The two lines' sides meet on the
  // halving line, (sum of the ways to the two sides) / (1 + cos turn)
  // half-widths from the corner: on the inner side, as far back along
  // each line as tan(half the turn) = |cross| / (1 + cos turn)
  // half-widths, which is to be no more than half of either line.
  const std::int64_t room = kWayOne + dot;
  const std::int64_t shortest = std::min<std::int64_t>({before.length, afterLength, 4294967296}) / 2;
  if (room > 0 && half_ * Size(cross) <= shortest * room) {
    const Point inner = {.x = Clamp32(at.x - (((beforeX + afterX) * half_) / room), clamped_),
                         .y = Clamp32(at.y - (((beforeY + afterY) * half_) / room), clamped_)};
    const bool left = side > 0;  // The outer side is the left.
    end = {.left = left ? one : inner, .right = left ? inner : one, .middle = at, .hasMiddle = true};
    start = {.left = left ? other : inner, .right = left ? inner : other, .middle = at, .hasMiddle = true};
  } else {
    end = Square(at, way);
    start = Square(at, after);
  }

  const bool ahead = cross < 0;  // Whether `one` comes first round the corner's piece.
  out_->MoveTo(at);
  out_->LineTo(ahead ? one : other);
  switch (style_.join) {
    case StrokeJoin::kBevel:
      break;
    case StrokeJoin::kMiter: {
      // The miter reaches 1 / cos(half the turn) half-widths from the
      // corner, which is within the limit while (1 + cos turn) times the
      // limit squared is at least two. Both sides are in 2^30ths, and the
      // limit in 4,096ths.
      const std::int64_t limit = style_.miterLimit;
      constexpr std::int64_t kTwo = 36028797018963968;  // Two, in 2^30ths times 4,096ths squared: 2^55.
      const std::int64_t least = (kTwo + (limit * limit) - 1) / (limit * limit);
      if (room > 0 && room >= least) {
        const std::int64_t tipX = ((beforeX + afterX) * half_) / room;
        const std::int64_t tipY = ((beforeY + afterY) * half_) / room;
        out_->LineTo({.x = Clamp32(at.x + tipX, clamped_), .y = Clamp32(at.y + tipY, clamped_)});
      }
      break;
    }
    case StrokeJoin::kRound: {
      const Angle turn = AngleOf(static_cast<std::int32_t>(Size(cross)), static_cast<std::int32_t>(dot));
      constexpr Angle kStep = kHalfTurn / (kRoundSteps / 2);
      const std::uint32_t steps = std::max<std::uint32_t>(1, (turn / kStep) + (turn % kStep != 0 ? 1 : 0));
      if (ahead) {
        Arc(at, {.x = beforeX, .y = beforeY}, turn, steps);
      } else {
        Arc(at, {.x = afterX, .y = afterY}, turn, steps);
      }
      break;
    }
  }
  out_->LineTo(ahead ? other : one);
  out_->Close();
}

void Stroker::Cap(Point at, const Way& way) noexcept {
  const std::int64_t leftX = -way.y;
  const std::int64_t leftY = way.x;
  switch (style_.cap) {
    case StrokeCap::kButt:
      return;
    case StrokeCap::kSquare:
      out_->MoveTo(Off(at, leftX, leftY));
      out_->LineTo(Off(at, leftX + way.x, leftY + way.y));
      out_->LineTo(Off(at, way.x - leftX, way.y - leftY));
      out_->LineTo(Off(at, -leftX, -leftY));
      break;
    case StrokeCap::kRound:
      out_->MoveTo(Off(at, leftX, leftY));
      Arc(at, {.x = leftX, .y = leftY}, kHalfTurn, kRoundSteps / 2);
      out_->LineTo(Off(at, -leftX, -leftY));
      break;
  }
  out_->Close();
}

void Stroker::Dot(Point at, const Way& way) noexcept {
  const std::int64_t leftX = -way.y;
  const std::int64_t leftY = way.x;
  switch (style_.cap) {
    case StrokeCap::kButt:
      return;
    case StrokeCap::kSquare:
      out_->MoveTo(Off(at, leftX - way.x, leftY - way.y));
      out_->LineTo(Off(at, leftX + way.x, leftY + way.y));
      out_->LineTo(Off(at, way.x - leftX, way.y - leftY));
      out_->LineTo(Off(at, -leftX - way.x, -leftY - way.y));
      break;
    case StrokeCap::kRound:
      out_->MoveTo(Off(at, leftX, leftY));
      Arc(at, {.x = leftX, .y = leftY}, kHalfTurn, kRoundSteps / 2);
      out_->LineTo(Off(at, -leftX, -leftY));
      Arc(at, {.x = -leftX, .y = -leftY}, kHalfTurn, kRoundSteps / 2);
      break;
  }
  out_->Close();
}

Point Stroker::Off(Point at, std::int64_t x, std::int64_t y) noexcept {
  return {.x = Clamp32(at.x + ShiftDown(x * half_, kWayBits), clamped_),
          .y = Clamp32(at.y + ShiftDown(y * half_, kWayBits), clamped_)};
}

void Stroker::Arc(Point at, const Way& from, Angle turn, std::uint32_t steps) noexcept {
  for (std::uint32_t step = 1; step < steps; ++step) {
    const auto angle = static_cast<Angle>((std::uint64_t{turn} * step) / steps);
    const SinCos sc = SinCosOf(angle);
    // Turned from x towards -y, which is the way every round edge goes.
    const std::int64_t x = ShiftDown((from.x * sc.cos) + (from.y * sc.sin), kUnitBits);
    const std::int64_t y = ShiftDown((from.y * sc.cos) - (from.x * sc.sin), kUnitBits);
    out_->LineTo(Off(at, x, y));
  }
}

}  // namespace bn::draw
