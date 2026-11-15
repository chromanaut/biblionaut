#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "draw/fixed.h"
#include "draw/svg_number.h"

namespace bn::draw {

// Takes the outline of a shape as it is read: where each part of it goes
// to, in the drawing's own coordinates. Every position is a place, never a
// distance from the last one, and each part starts where the last ended.
class PathSink {
 public:
  // Begins a new part of the outline at `to`, without drawing.
  virtual void MoveTo(Point to) noexcept = 0;
  virtual void LineTo(Point to) noexcept = 0;
  // A curve pulled towards one point on its way, and towards two.
  virtual void QuadTo(Point control, Point to) noexcept = 0;
  virtual void CubicTo(Point control1, Point control2, Point to) noexcept = 0;
  // Part of an ellipse with these two radii, its axes turned by `turn`. Of
  // the four such arcs between two points, `largeArc` picks the two that go
  // more than half way round, and `sweep` the one of those that turns from
  // the x axis towards the y axis.
  virtual void ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept = 0;
  // Draws back to where this part of the outline began, and ends it.
  virtual void Close() noexcept = 0;

 protected:
  ~PathSink() = default;  // Never destroyed through the interface.
};

// How reading path data ended.
enum class SvgPathStatus : std::uint8_t {
  kOk = 0,
  kBadData,  // It is not path data from some point on. What came before that has been given out.
};

// The name of a status, for diagnostics: "ok", "bad data".
const char* SvgPathStatusName(SvgPathStatus status) noexcept;

// Reads SVG path data, the `d` of a `<path>`, and tells a PathSink what it
// draws.
//
// The data is given in pieces of any size, cut anywhere, so it can be read
// as it comes out of a file and never has to be whole in memory: one path
// in a book is over 3 MB long. The object is 168 bytes on the device.
//
// What the sink is told is simpler than what was written. Distances from
// the last point have become places, a command written once and followed
// by several sets of numbers has become several commands, the horizontal
// and vertical lines have become lines, and the curves that take their
// first control point from the curve before have been given it.
//
// The data is not trusted. Whatever bytes it holds, reading ends with a
// status. Where the data goes wrong, everything before the command with the
// mistake in it has been drawn and nothing after is, which is what SVG asks
// for. A number too large to hold is not a mistake: it is held at the
// largest that fits, and Clamped() says so.
//
// Nor can the data make a call long. Each byte is looked at once, and
// Step() comes back once it has given the sink as many commands as it was
// asked for, so whoever reads a path a piece at a time decides how much one
// piece may draw. A command is one thing told to the sink, or two where it
// begins a new outline after the last was closed.
class SvgPathReader {
 public:
  explicit SvgPathReader(PathSink& sink) noexcept : sink_(&sink) {}

  // Starts again, on another path.
  void Reset() noexcept { *this = SvgPathReader(*sink_); }

  // The next piece of the data, all of it, however much it draws.
  void Feed(std::string_view text) noexcept;

  // Takes bytes from the front of `text` until it has given the sink
  // `commands` commands or has taken them all, and returns how many it
  // took: the rest is for the next call. It can give one command more than
  // it was asked for, since the letter that closes an outline can also be
  // what ends the number before it. Asked for none, it takes nothing.
  [[nodiscard]] std::size_t Step(std::string_view text, std::uint32_t commands) noexcept;

  // Says that the data has ended, which may complete its last command.
  SvgPathStatus Finish() noexcept;

  SvgPathStatus Status() const noexcept { return status_; }
  // A number, or a place worked out from one, was held at the largest that
  // fits.
  bool Clamped() const noexcept { return clamped_; }
  // Commands given to the sink so far.
  std::uint32_t Commands() const noexcept { return commands_; }
  // How many bytes had been read when the data went wrong.
  std::uint32_t ErrorOffset() const noexcept { return errorOffset_; }

 private:
  enum class Curve : std::uint8_t { kNone, kCubic, kQuad };

  void Take(char c) noexcept;
  void EndNumber() noexcept;
  void Store(Number value) noexcept;
  void Emit() noexcept;
  void Fail() noexcept;
  bool WantsFlag() const noexcept;
  // A place as it is kept while the path is read: as fine as the numbers
  // were read, which is far finer than a Point. Distances written to more
  // places of decimals than a Point holds then still add up to the right
  // place, however many of them there are. A place is rounded to a Point
  // only as it is given out.
  struct Place {
    Number x = 0;
    Number y = 0;
  };
  // As far from nothing as a place can be: the largest a Point can hold.
  static constexpr Number kFurthest = 2251799812636672;

  // `value`, or the furthest a place can be if it is beyond that.
  Number Held(Number value) noexcept;
  // The place two of the command's numbers give, starting at `index`.
  Place PlaceAt(std::size_t index, bool relative) noexcept;
  Number Along(Number from, std::size_t index, bool relative) noexcept;
  // The last curve's control point, seen from the other side of where it
  // ended.
  Place Mirrored() noexcept;
  Point Rounded(Place place) noexcept;
  void BeginDrawing() noexcept;

  PathSink* sink_;
  SvgNumber number_;
  std::array<Number, 7> arguments_{};
  SvgPathStatus status_ = SvgPathStatus::kOk;
  char command_ = 0;                // The command whose numbers are being read; 0 before the first.
  std::uint8_t argumentCount_ = 0;  // How many of its numbers have been read.
  bool inNumber_ = false;
  bool commaAllowed_ = false;  // A number has just ended, so a comma may follow.
  bool commaPending_ = false;  // A comma came, so a number has to.
  bool satisfied_ = true;      // The command read last has had the numbers it needs.
  bool needsMove_ = false;     // The outline was closed: what is drawn next begins again from the same place.
  bool clamped_ = false;
  Curve lastCurve_ = Curve::kNone;
  Place current_;
  Place start_;
  Place lastControl_;
  std::uint32_t commands_ = 0;
  std::uint32_t bytes_ = 0;
  std::uint32_t errorOffset_ = 0;
};

}  // namespace bn::draw
