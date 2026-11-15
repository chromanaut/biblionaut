#include "draw/flatten.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace bn::draw {

namespace {

// How many times a curve may be cut in half, which is what bounds the
// lines it can become.
constexpr std::uint8_t kDeepest = 10;
static_assert(PathFlattener::kMostLinesPerCurve == 1U << kDeepest);
// And an arc is thirty-two curves at the most: a whole turn, in pieces of
// a thirty-second of one.
static_assert(PathFlattener::kMostLinesPerArc == 32 * PathFlattener::kMostLinesPerCurve);

// One, as the square of a Unit.
constexpr std::uint64_t kUnitSquared = 1152921504606846976;

std::int64_t Size(std::int64_t value) noexcept { return value < 0 ? -value : value; }

Point Middle(Point a, Point b) noexcept {
  return {.x = static_cast<Fixed>(ShiftDown(std::int64_t{a.x} + b.x, 1)),
          .y = static_cast<Fixed>(ShiftDown(std::int64_t{a.y} + b.y, 1))};
}

struct Cubic {
  Point p0;
  Point p1;
  Point p2;
  Point p3;
  std::uint8_t depth = 0;
};

struct Quad {
  Point p0;
  Point p1;
  Point p2;
  std::uint8_t depth = 0;
};

// Whether a curve stays within the tolerance of the straight line between
// its ends. A curve is never further from that line than a quarter of how
// far its control points stand from where a straight line would have them:
// (x1, y1) for one control point and (x2, y2) for the other, of which the
// larger across and the larger down are taken, so that the answer errs
// towards cutting. `flatness` is sixteen times the tolerance squared.
bool Within(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2, std::uint64_t flatness) noexcept {
  // Beyond this the squares would not fit, and nothing so far off is flat.
  constexpr std::int64_t kTooFar = 1073741824;
  const std::int64_t across = std::max(Size(x1), Size(x2));
  const std::int64_t down = std::max(Size(y1), Size(y2));
  if (across >= kTooFar || down >= kTooFar) {
    return false;
  }
  return static_cast<std::uint64_t>((across * across) + (down * down)) <= flatness;
}

// Two numbers brought to a size whose squares can be added together:
// what they really are is these, times two to the power of `shift`.
struct Fitted {
  std::int64_t x = 0;
  std::int64_t y = 0;
  int shift = 0;
};

Fitted Fit(std::int64_t x, std::int64_t y) noexcept {
  constexpr std::int64_t kLow = 268435456;
  constexpr std::int64_t kHigh = 536870912;
  Fitted fitted{.x = x, .y = y, .shift = 0};
  std::int64_t larger = std::max(Size(x), Size(y));
  if (larger == 0) {
    return fitted;
  }
  while (larger >= kHigh) {
    fitted.x = ShiftDown(fitted.x, 1);
    fitted.y = ShiftDown(fitted.y, 1);
    larger = ShiftDown(larger, 1);
    ++fitted.shift;
  }
  while (larger < kLow) {
    fitted.x *= 2;
    fitted.y *= 2;
    larger *= 2;
    --fitted.shift;
  }
  return fitted;
}

// An arc's ends are placed on its ellipse from half the way between them,
// which is kept this many bits finer than a Fixed. On a long thin ellipse
// what is lost in rounding across the short axis comes back multiplied by
// how much longer the other is: a thousand times, on some.
constexpr int kFiner = 16;

// `fine`, which is that many bits finer than a Fixed, measured in
// `radius`: as a Unit, or as near as can be held where the radius is next
// to nothing against it.
std::int64_t InRadii(std::int64_t fine, std::int64_t radius) noexcept {
  constexpr std::uint64_t kFits = 2147483648;
  constexpr std::uint64_t kFarOff = 2305843009213693952;
  constexpr std::uint64_t kFinerOne = 65536;
  // The two are brought under 2^31 together, which keeps their ratio and
  // leaves room to multiply by a Unit's one.
  auto over = static_cast<std::uint64_t>(Size(fine));
  std::uint64_t under = static_cast<std::uint64_t>(radius) * kFinerOne;
  while (over >= kFits || under >= kFits) {
    over >>= 1U;
    under >>= 1U;
  }
  const auto ratio =
      static_cast<std::int64_t>(under == 0 ? kFarOff : (over * static_cast<std::uint64_t>(kUnitOne)) / under);
  return fine < 0 ? -ratio : ratio;
}

// `radius` made longer by `length` times two to the power of `shift`,
// which is a Unit: a radius made long enough to reach.
std::int64_t Lengthened(std::int64_t radius, std::int64_t length, int shift, bool& clamped) noexcept {
  const std::uint64_t product = static_cast<std::uint64_t>(radius) * static_cast<std::uint64_t>(length);
  const int down = kUnitBits - shift;
  const auto largest = static_cast<std::uint64_t>(kFixedMax);
  std::uint64_t longer = largest + 1;
  if (down >= 0) {
    longer = product >> static_cast<unsigned>(std::min(down, 63));
  } else if (-down < 32 && product <= (largest >> static_cast<unsigned>(-down))) {
    longer = product << static_cast<unsigned>(-down);
  }
  if (longer > largest) {
    clamped = true;
    return kFixedMax;
  }
  // Rounding must not leave it shorter than it was.
  return std::max(radius, static_cast<std::int64_t>(longer));
}

// What is left of a number multiplied by a Unit, once the Unit's own
// fraction has been taken off again.
std::int64_t InUnits(std::int64_t product) noexcept { return ShiftDown(product, kUnitBits); }

// An ellipse, as the pieces of an arc are placed on it.
struct Ellipse {
  std::int64_t radiusX = 0;
  std::int64_t radiusY = 0;
  SinCos axes;  // How far its axes are turned.
  // The middle of the line between the arc's two ends, and where the
  // ellipse's own middle is from there: the first as a Fixed, the second on
  // the circle of radius one that the ellipse is a stretching of.
  std::int64_t middleX = 0;
  std::int64_t middleY = 0;
  std::int64_t offsetX = 0;
  std::int64_t offsetY = 0;

  // Where on the ellipse a point of that circle falls.
  Point At(std::int64_t x, std::int64_t y, bool& clamped) const noexcept {
    const std::int64_t alongX = ShiftDown(radiusX * (x + offsetX), kUnitBits);
    const std::int64_t alongY = ShiftDown(radiusY * (y + offsetY), kUnitBits);
    return {.x = Clamp32(middleX + InUnits(axes.cos * alongX) - InUnits(axes.sin * alongY), clamped),
            .y = Clamp32(middleY + InUnits(axes.sin * alongX) + InUnits(axes.cos * alongY), clamped)};
  }
};

// An angle that may be more than a turn, or backwards, as an Angle.
Angle Wrapped(std::int64_t angle) noexcept { return static_cast<Angle>(static_cast<std::uint64_t>(angle)); }

}  // namespace

PathFlattener::PathFlattener(LineSink& sink, const Matrix& matrix, Fixed tolerance) noexcept
    : sink_(&sink),
      matrix_(matrix),
      largestScale_(LargestScale(matrix)),
      tolerance_(std::max<Fixed>(tolerance, 1)),
      flatness_(16 * static_cast<std::uint64_t>(tolerance_) * static_cast<std::uint64_t>(tolerance_)) {}

Point PathFlattener::InPixels(Point point) noexcept { return Apply(matrix_, point, clamped_); }

void PathFlattener::Line(Point to) noexcept {
  sink_->LineTo(to);
  currentPixels_ = to;
  ++lines_;
}

void PathFlattener::MoveTo(Point to) noexcept {
  current_ = to;
  start_ = to;
  currentPixels_ = InPixels(to);
  startPixels_ = currentPixels_;
  sink_->MoveTo(currentPixels_);
}

void PathFlattener::LineTo(Point to) noexcept {
  current_ = to;
  Line(InPixels(to));
}

void PathFlattener::Close() noexcept {
  sink_->Close();
  current_ = start_;
  currentPixels_ = startPixels_;
}

void PathFlattener::KeepTo(Point topLeft, Point bottomRight) noexcept {
  kept_ = true;
  keptFrom_ = topLeft;
  keptTo_ = bottomRight;
}

// A curve never goes outside its control points, and a straight line
// between two points on one side of an edge stays on that side.
bool PathFlattener::Outside(Point a, Point b, Point c, Point d) const noexcept {
  if (!kept_) {
    return false;
  }
  const bool above = a.y <= keptFrom_.y && b.y <= keptFrom_.y && c.y <= keptFrom_.y && d.y <= keptFrom_.y;
  const bool below = a.y >= keptTo_.y && b.y >= keptTo_.y && c.y >= keptTo_.y && d.y >= keptTo_.y;
  const bool left = a.x <= keptFrom_.x && b.x <= keptFrom_.x && c.x <= keptFrom_.x && d.x <= keptFrom_.x;
  const bool right = a.x >= keptTo_.x && b.x >= keptTo_.x && c.x >= keptTo_.x && d.x >= keptTo_.x;
  return above || below || left || right;
}

// A curve is cut in half, and each half again, until every piece is flat
// enough to be a line. The pieces not yet looked at wait in a small array:
// there is never more than one waiting for each time the curve has been
// cut, since the first half of a piece is always taken before the second.

void PathFlattener::QuadTo(Point control, Point to) noexcept {
  const Point pull = InPixels(control);
  const Point end = InPixels(to);
  if (Outside(currentPixels_, pull, end, end)) {
    Line(end);
    current_ = to;
    return;
  }
  std::array<Quad, kDeepest + 1> waiting;
  std::size_t count = 1;
  waiting[0] = {.p0 = currentPixels_, .p1 = pull, .p2 = end, .depth = 0};
  while (count > 0) {
    --count;
    const Quad piece = waiting[count];
    const std::int64_t x = std::int64_t{piece.p0.x} - (2 * std::int64_t{piece.p1.x}) + piece.p2.x;
    const std::int64_t y = std::int64_t{piece.p0.y} - (2 * std::int64_t{piece.p1.y}) + piece.p2.y;
    if (piece.depth >= kDeepest || Within(x, y, 0, 0, flatness_)) {
      Line(piece.p2);
      continue;
    }
    const Point a = Middle(piece.p0, piece.p1);
    const Point b = Middle(piece.p1, piece.p2);
    const Point middle = Middle(a, b);
    const auto depth = static_cast<std::uint8_t>(piece.depth + 1);
    waiting[count] = {.p0 = middle, .p1 = b, .p2 = piece.p2, .depth = depth};
    waiting[count + 1] = {.p0 = piece.p0, .p1 = a, .p2 = middle, .depth = depth};
    count += 2;
  }
  current_ = to;
}

void PathFlattener::CubicTo(Point control1, Point control2, Point to) noexcept {
  const Point pull1 = InPixels(control1);
  const Point pull2 = InPixels(control2);
  const Point end = InPixels(to);
  if (Outside(currentPixels_, pull1, pull2, end)) {
    Line(end);
    current_ = to;
    return;
  }
  std::array<Cubic, kDeepest + 1> waiting;
  std::size_t count = 1;
  waiting[0] = {.p0 = currentPixels_, .p1 = pull1, .p2 = pull2, .p3 = end, .depth = 0};
  while (count > 0) {
    --count;
    const Cubic piece = waiting[count];
    const std::int64_t x1 = (3 * std::int64_t{piece.p1.x}) - (2 * std::int64_t{piece.p0.x}) - piece.p3.x;
    const std::int64_t y1 = (3 * std::int64_t{piece.p1.y}) - (2 * std::int64_t{piece.p0.y}) - piece.p3.y;
    const std::int64_t x2 = (3 * std::int64_t{piece.p2.x}) - (2 * std::int64_t{piece.p3.x}) - piece.p0.x;
    const std::int64_t y2 = (3 * std::int64_t{piece.p2.y}) - (2 * std::int64_t{piece.p3.y}) - piece.p0.y;
    if (piece.depth >= kDeepest || Within(x1, y1, x2, y2, flatness_)) {
      Line(piece.p3);
      continue;
    }
    const Point a = Middle(piece.p0, piece.p1);
    const Point b = Middle(piece.p1, piece.p2);
    const Point c = Middle(piece.p2, piece.p3);
    const Point d = Middle(a, b);
    const Point e = Middle(b, c);
    const Point middle = Middle(d, e);
    const auto depth = static_cast<std::uint8_t>(piece.depth + 1);
    waiting[count] = {.p0 = middle, .p1 = e, .p2 = c, .p3 = piece.p3, .depth = depth};
    waiting[count + 1] = {.p0 = piece.p0, .p1 = a, .p2 = d, .p3 = middle, .depth = depth};
    count += 2;
  }
  current_ = to;
}

// `halfX` and `halfY` are half the way between an arc's two ends, across
// the two axes of its ellipse. Between two points that close together on
// an ellipse, the shorter way round stands no further from the straight
// line than twice (half / smaller radius) squared times the larger radius.
// If that times the matrix's largest scale is under half the tolerance,
// the arc is within the tolerance of the line in pixels, and the line will
// do.
bool PathFlattener::NearlyStraight(Fixed halfX, Fixed halfY, Fixed radiusX, Fixed radiusY) const noexcept {
  const std::int64_t half = std::max(Size(halfX), Size(halfY));
  const std::int64_t smaller = std::min(radiusX, radiusY);
  const std::int64_t larger = std::max(radiusX, radiusY);
  if (half >= smaller) {
    return false;
  }
  const std::int64_t ratio = (half * kCoefficientOne) / smaller;
  const std::int64_t bulge = ShiftDown(ShiftDown(ratio * ratio, kCoefficientBits) * larger, kCoefficientBits);
  const std::int64_t pixels = ShiftDown(bulge * 2 * largestScale_, kCoefficientBits);
  return pixels <= tolerance_ / 2;
}

// The arc is worked out on a circle of radius one, which the ellipse is a
// stretching of: there the two ends are two points of the circle, the arc
// is the way round from one to the other, and a piece of it is a curve
// whose control points are easy to place. Each piece is then stretched and
// turned onto the ellipse and drawn as any other curve.
//
// How the ellipse is found from the arc's two ends, its radii and its
// flags, and what is done about radii too short to reach, is the working
// in the SVG specification's notes on implementing arcs, carried out here
// in whole numbers.
void PathFlattener::ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept {
  const Point from = current_;
  if (from == to) {
    return;
  }
  if (radiusX <= 0 || radiusY <= 0) {
    LineTo(to);
    return;
  }

  // Half the way from the end back to the start, as the ellipse's own axes
  // see it: as a Fixed, and finer.
  const SinCos axes = SinCosOf(turn);
  const std::int64_t backX = std::int64_t{from.x} - to.x;
  const std::int64_t backY = std::int64_t{from.y} - to.y;
  const std::int64_t fineX = ShiftDown((axes.cos * backX) + (axes.sin * backY), kUnitBits + 1 - kFiner);
  const std::int64_t fineY = ShiftDown((axes.cos * backY) - (axes.sin * backX), kUnitBits + 1 - kFiner);
  const Fixed halfX = Clamp32(ShiftDown(fineX, kFiner), clamped_);
  const Fixed halfY = Clamp32(ShiftDown(fineY, kFiner), clamped_);
  if ((halfX == 0 && halfY == 0) || (!largeArc && NearlyStraight(halfX, halfY, radiusX, radiusY))) {
    LineTo(to);
    return;
  }

  // The same half, on the circle: (u, v), each measured in its own radius.
  // Its direction and its length come from the two brought to a size that
  // can be squared.
  const std::int64_t u = InRadii(fineX, radiusX);
  const std::int64_t v = InRadii(fineY, radiusY);
  Fitted across = Fit(u, v);
  if (across.x == 0 && across.y == 0) {
    // The two ends are too close together, against the radii, for that to
    // have a direction. The two halves multiplied across have the same one.
    across = Fit(std::int64_t{halfX} * radiusY, std::int64_t{halfY} * radiusX);
  }
  const auto length =
      static_cast<std::int64_t>(SquareRoot(static_cast<std::uint64_t>((across.x * across.x) + (across.y * across.y))));
  const std::int64_t directionX = (across.x * kUnitOne) / length;
  const std::int64_t directionY = (across.y * kUnitOne) / length;

  Ellipse ellipse{.radiusX = radiusX,
                  .radiusY = radiusY,
                  .axes = axes,
                  .middleX = ShiftDown(std::int64_t{from.x} + to.x, 1),
                  .middleY = ShiftDown(std::int64_t{from.y} + to.y, 1)};
  // Where the arc starts on the circle, and how far round it goes: more
  // than nothing for a turn from the x axis towards the y axis.
  std::int64_t startX = directionX;
  std::int64_t startY = directionY;
  std::int64_t round = 0;
  bool reaches = false;
  if (Size(u) <= kUnitOne && Size(v) <= kUnitOne) {
    const auto square = static_cast<std::uint64_t>((u * u) + (v * v));
    if (square <= kUnitSquared) {
      // The two ends are within the circle's reach of each other. Its
      // middle is off to one side of the line between them: which side is
      // what the two flags choose.
      reaches = true;
      const auto distance = static_cast<std::int64_t>(SquareRoot(kUnitSquared - square));
      const std::int64_t side = largeArc != sweep ? 1 : -1;
      ellipse.offsetX = side * InUnits(distance * directionY);
      ellipse.offsetY = -side * InUnits(distance * directionX);
      startX = u - ellipse.offsetX;
      startY = v - ellipse.offsetY;
      bool unused = false;
      const Angle startAngle = AngleOf(Clamp32(startY, unused), Clamp32(startX, unused));
      const Angle endAngle = AngleOf(Clamp32(-v - ellipse.offsetY, unused), Clamp32(-u - ellipse.offsetX, unused));
      Angle way = sweep ? endAngle - startAngle : startAngle - endAngle;
      // Rounding can make next to nothing look like next to a whole turn,
      // and the other way about. `largeArc` says which it was.
      if (!largeArc && way > kHalfTurn) {
        way = way > kHalfTurn + kQuarterTurn ? 0 : kHalfTurn;
      }
      if (largeArc && way < kHalfTurn) {
        way = way < kQuarterTurn ? UINT32_MAX : kHalfTurn;
      }
      round = sweep ? std::int64_t{way} : -std::int64_t{way};
    }
  }
  if (!reaches) {
    // The radii are too short for the ellipse to reach from one end to the
    // other: (u, v) is longer than one. They are made longer by as much as
    // it is, which is just enough, and puts the two ends opposite each
    // other with the ellipse's middle between them.
    ellipse.radiusX = Lengthened(radiusX, length, across.shift, clamped_);
    ellipse.radiusY = Lengthened(radiusY, length, across.shift, clamped_);
    round = sweep ? std::int64_t{kHalfTurn} : -std::int64_t{kHalfTurn};
  }
  if (round == 0) {
    LineTo(to);
    return;
  }

  // Pieces of an eighth of a turn or less. A curve stands in for an eighth
  // of a circle of radius one to within four parts in a million, and for
  // half as much sixty-four times more closely. Four parts in a million of
  // a large ellipse is too much of a pixel, so its pieces are smaller.
  constexpr std::int64_t kLarge = 16777216;        // 4,096 pixels.
  constexpr std::int64_t kVeryLarge = 1073741824;  // 262,144 pixels.
  const std::int64_t reach = ShiftDown(std::max(ellipse.radiusX, ellipse.radiusY) * largestScale_, kCoefficientBits);
  const std::int64_t most = reach > kVeryLarge ? kEighthTurn / 4 : (reach > kLarge ? kEighthTurn / 2 : kEighthTurn);
  const std::int64_t pieces = (Size(round) + most - 1) / most;
  // A curve's control points stand off from its ends, along the circle, by
  // four thirds of the tangent of a quarter of the angle it covers.
  const std::int64_t step = round / pieces;
  const SinCos quarter = SinCosOf(Wrapped(step / 4));
  const std::int64_t handle = (((std::int64_t{quarter.sin} * kUnitOne) / quarter.cos) * 4) / 3;
  bool unused = false;
  const Angle startAngle = AngleOf(Clamp32(startY, unused), Clamp32(startX, unused));
  SinCos at = SinCosOf(startAngle);
  for (std::int64_t piece = 1; piece <= pieces; ++piece) {
    const SinCos next = SinCosOf(startAngle + Wrapped(step * piece));
    const Point control1 = ellipse.At(at.cos - InUnits(handle * at.sin), at.sin + InUnits(handle * at.cos), clamped_);
    const Point control2 =
        ellipse.At(next.cos + InUnits(handle * next.sin), next.sin - InUnits(handle * next.cos), clamped_);
    CubicTo(control1, control2, piece == pieces ? to : ellipse.At(next.cos, next.sin, clamped_));
    at = next;
  }
}

}  // namespace bn::draw
