// The fuzz target for SVG path data: the reader (draw/svg_path.h), the
// flattener that makes its curves into lines (draw/flatten.h), and the
// filler those lines go to (draw/fill.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes of the data the reader is given
//                         at a call
//            bits 3 to 4: how many commands a call may give
//            bits 5 to 6: the matrix every point is put through
//            bit 7:       which rule the shape is filled by
//   byte 1   bits 0 to 1: how far a line may stand from its curve
//            bits 2 to 4: how many rows a strip has
//            bit 5:       whether the flattener is told which strip is
//                         being drawn
//   the rest path data, or what passes for it
//
// The shape is drawn twice into a picture of 40 by 28: once in one piece,
// and once cut up as the two bytes say, the data being read again for each
// strip. What has to hold, whatever the bytes are:
//
//   - a call takes no more than it was given and at least a byte, gives
//     no more commands than it was asked for and one, and asked for none
//     takes nothing;
//   - after a mistake nothing more is given out, the rest is taken unread,
//     and where the mistake is said to be is not past the data;
//   - a line is one line, a curve no more than kMostLinesPerCurve and an
//     arc no more than kMostLinesPerArc;
//   - no line comes before the outline has begun;
//   - a line adds to no more cells than MostCellsPerLine(), filling looks
//     at no pixel twice, and leaves every cell as it found it;
//   - nothing is written outside the strip;
//   - the two readings give the same commands and end the same way;
//   - the two pictures are the same, byte for byte, however the picture
//     was cut into strips and whether or not the flattener was told of
//     them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_path.h"
#include "draw/svg_transform.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Angle;
using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::Point;
using bn::draw::SvgPathReader;
using bn::draw::SvgPathStatus;

constexpr std::size_t kWidth = 40;
constexpr std::size_t kHeight = 28;

constexpr std::array<std::size_t, 8> kPieces = {SIZE_MAX, 1, 2, 3, 7, 16, 100, 513};
constexpr std::array<std::uint32_t, 4> kCommandsACall = {64, 1, 2, 16};
// None; one that leaves a shape in the picture; one that turns and slants;
// and one as large as a factor can be, with the picture upside down.
constexpr std::array<std::string_view, 4> kTransforms = {
    "", "translate(4 3) scale(2)", "matrix(0.8 0.3 -0.5 1.1 12 -2)", "translate(20 14) scale(2047 -900)"};
// A sixteenth of a pixel, a quarter, a whole one, and a 256th.
constexpr std::array<Fixed, 4> kTolerances = {256, 1024, 4096, 16};
constexpr std::array<std::size_t, 8> kStripRows = {kHeight, 1, 2, 3, 5, 8, 13, 27};

// A few bytes of path data can ask for millions of lines: an arc is up to
// 32,768 of them. Past this many a drawing is left off: what the engine is
// after is in the data, not in how long it can be made to go on.
constexpr std::uint64_t kMostLines = 200000;

// What is kept either side of a strip, to see that nothing is written
// there.
constexpr std::size_t kGuardBytes = 32;
constexpr std::uint8_t kGuard = 0xA5;

// Stands between the flattener and the filler: holds each line to what a
// line may cost.
class Lines final : public bn::draw::LineSink {
 public:
  Lines(Filler& filler, std::uint32_t mostCells) noexcept : filler_(filler), mostCells_(mostCells) {}

  void MoveTo(Point to) noexcept override {
    const std::uint32_t before = filler_.CellsTouched();
    filler_.MoveTo(to);
    Cost(before);
    begun_ = true;
  }
  void LineTo(Point to) noexcept override {
    BN_FUZZ_CHECK(begun_, "a line came before any outline had begun");
    const std::uint32_t before = filler_.CellsTouched();
    filler_.LineTo(to);
    Cost(before);
  }
  void Close() noexcept override {
    const std::uint32_t before = filler_.CellsTouched();
    filler_.Close();
    Cost(before);
  }

 private:
  void Cost(std::uint32_t before) const noexcept {
    BN_FUZZ_CHECK(filler_.CellsTouched() - before <= mostCells_, "a line added to more cells than a line may");
  }

  Filler& filler_;
  std::uint32_t mostCells_;
  bool begun_ = false;
};

// Stands between the reader and the flattener: writes down each command,
// and holds it to the lines it may become.
class Commands final : public bn::draw::PathSink {
 public:
  Commands(PathFlattener& flattener, std::string& told) noexcept : flattener_(flattener), told_(told) {}

  void MoveTo(Point to) noexcept override {
    Tell('M', {to});
    const std::uint32_t before = flattener_.Lines();
    flattener_.MoveTo(to);
    BN_FUZZ_CHECK(flattener_.Lines() == before, "the start of an outline was a line");
  }
  void LineTo(Point to) noexcept override {
    Tell('L', {to});
    const std::uint32_t before = flattener_.Lines();
    flattener_.LineTo(to);
    BN_FUZZ_CHECK(flattener_.Lines() - before == 1, "a line was not one line");
  }
  void QuadTo(Point control, Point to) noexcept override {
    Tell('Q', {control, to});
    const std::uint32_t before = flattener_.Lines();
    flattener_.QuadTo(control, to);
    Curve(before);
  }
  void CubicTo(Point control1, Point control2, Point to) noexcept override {
    Tell('C', {control1, control2, to});
    const std::uint32_t before = flattener_.Lines();
    flattener_.CubicTo(control1, control2, to);
    Curve(before);
  }
  void ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept override {
    Tell('A', {{.x = radiusX, .y = radiusY},
               {.x = static_cast<Fixed>(turn >> 1U), .y = (largeArc ? 2 : 0) + (sweep ? 1 : 0)},
               to});
    const std::uint32_t before = flattener_.Lines();
    flattener_.ArcTo(radiusX, radiusY, turn, largeArc, sweep, to);
    BN_FUZZ_CHECK(flattener_.Lines() - before <= PathFlattener::kMostLinesPerArc,
                  "an arc became more lines than an arc may");
  }
  void Close() noexcept override {
    Tell('Z', {});
    const std::uint32_t before = flattener_.Lines();
    flattener_.Close();
    BN_FUZZ_CHECK(flattener_.Lines() == before, "the closing of an outline was counted as a line");
  }

 private:
  void Tell(char kind, std::initializer_list<Point> points) noexcept {
    told_ += kind;
    for (const Point point : points) {
      told_ += std::to_string(point.x) + "," + std::to_string(point.y) + " ";
    }
  }
  void Curve(std::uint32_t before) const noexcept {
    const std::uint32_t lines = flattener_.Lines() - before;
    BN_FUZZ_CHECK(lines >= 1 && lines <= PathFlattener::kMostLinesPerCurve,
                  "a curve became no line, or more than a curve may");
  }

  PathFlattener& flattener_;
  std::string& told_;
};

// What drawing the data came to.
struct Drawing {
  std::vector<std::uint8_t> picture;
  std::string told;  // The commands the reader gave, in the first strip's reading.
  SvgPathStatus status = SvgPathStatus::kOk;
  std::uint32_t errorOffset = 0;
  std::uint32_t commands = 0;
  bool clamped = false;
  bool leftOff = false;  // It reached kMostLines and was not drawn to its end.
};

Drawing Draw(std::string_view data, std::size_t piece, std::uint32_t commandsACall, const Matrix& matrix,
             Fixed tolerance, std::size_t stripRows, bool keep, FillRule rule) {
  Drawing drawing;
  drawing.picture.assign(kWidth * kHeight, 255);
  std::uint64_t lines = 0;
  for (std::size_t top = 0; top < kHeight && !drawing.leftOff; top += stripRows) {
    const std::size_t rows = std::min(stripRows, kHeight - top);
    // The strip and its cells are each in memory of their own, of exactly
    // their size, with something either side of the strip that must still
    // be there afterwards.
    std::vector<std::uint8_t> strip((kWidth * rows) + (2 * kGuardBytes), kGuard);
    const std::span<std::uint8_t> gray = std::span(strip).subspan(kGuardBytes, kWidth * rows);
    std::fill(gray.begin(), gray.end(), std::uint8_t{255});
    std::vector<std::int16_t> cells(Filler::CellsFor(kWidth, rows));
    Filler filler;
    BN_FUZZ_CHECK(filler.Begin(gray, kWidth, static_cast<std::int32_t>(top), cells),
                  "a strip of the picture was refused");
    Lines watched(filler, Filler::MostCellsPerLine(kWidth, static_cast<std::uint32_t>(rows)));
    PathFlattener flattener(watched, matrix, tolerance);
    if (keep) {
      flattener.KeepTo(
          {.x = 0, .y = static_cast<Fixed>(top * kFixedOne)},
          {.x = static_cast<Fixed>(kWidth * kFixedOne), .y = static_cast<Fixed>((top + rows) * kFixedOne)});
    }
    std::string told;
    Commands commands(flattener, told);
    SvgPathReader reader(commands);

    BN_FUZZ_CHECK(reader.Step(data, 0) == 0 && reader.Commands() == 0, "a reader asked for no commands did something");
    bool failed = false;
    for (std::size_t at = 0; at < data.size() && !drawing.leftOff;) {
      std::string_view bytes = data.substr(at, piece);
      at += bytes.size();
      while (!bytes.empty() && !drawing.leftOff) {
        const std::uint32_t before = reader.Commands();
        const std::size_t took = reader.Step(bytes, commandsACall);
        BN_FUZZ_CHECK(took >= 1 && took <= bytes.size(), "a call took nothing, or more than it was given");
        BN_FUZZ_CHECK(reader.Commands() - before <= commandsACall + 1,
                      "a call gave more commands than it was asked for and one");
        if (failed) {
          BN_FUZZ_CHECK(reader.Commands() == before && took == bytes.size(),
                        "a reader went on after a mistake, or did not take what it was given");
        }
        failed = reader.Status() != SvgPathStatus::kOk;
        bytes.remove_prefix(took);
        drawing.leftOff = lines + flattener.Lines() > kMostLines;
      }
    }
    const SvgPathStatus status = reader.Finish();
    BN_FUZZ_CHECK(!failed || status == SvgPathStatus::kBadData, "a mistake in the data was forgotten by its end");
    BN_FUZZ_CHECK(reader.Finish() == status, "the end of a reading changed when it was asked for again");
    BN_FUZZ_CHECK(reader.ErrorOffset() <= data.size(), "a mistake is said to be past the end of the data");
    lines += flattener.Lines();

    const std::uint32_t visitedBefore = filler.PixelsVisited();
    filler.Fill(rule, 0);
    BN_FUZZ_CHECK(filler.PixelsVisited() - visitedBefore <= kWidth * rows, "filling looked at a pixel twice");
    BN_FUZZ_CHECK(std::all_of(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(kWidth * rows),
                              [](std::int16_t cell) { return cell == 0; }),
                  "filling left something in a cell");
    BN_FUZZ_CHECK(
        std::all_of(strip.begin(), strip.begin() + kGuardBytes, [](std::uint8_t byte) { return byte == kGuard; }) &&
            std::all_of(strip.end() - kGuardBytes, strip.end(), [](std::uint8_t byte) { return byte == kGuard; }),
        "something was written outside the strip");
    std::copy(gray.begin(), gray.end(), drawing.picture.begin() + static_cast<std::ptrdiff_t>(top * kWidth));

    if (drawing.leftOff) {
      break;
    }
    if (top == 0) {
      drawing.told = told;
      drawing.status = status;
      drawing.errorOffset = reader.ErrorOffset();
      drawing.commands = reader.Commands();
      drawing.clamped = reader.Clamped();
    } else {
      // Every strip's reading of the data is the same reading.
      BN_FUZZ_CHECK(told == drawing.told && status == drawing.status && reader.ErrorOffset() == drawing.errorOffset &&
                        reader.Commands() == drawing.commands && reader.Clamped() == drawing.clamped,
                    "the data was read differently for one strip than for another");
    }
  }
  return drawing;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned first = input.Byte();
  const unsigned second = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  const std::string_view pathData(reinterpret_cast<const char*>(rest.data()), rest.size());

  Matrix matrix;
  bool clamped = false;
  BN_FUZZ_CHECK(bn::draw::ParseSvgTransform(kTransforms[(first >> 5U) & 3U], matrix, clamped),
                "a transform of the target's own was refused");
  const FillRule rule = (first & 0x80U) != 0 ? FillRule::kEvenOdd : FillRule::kNonZero;
  const Fixed tolerance = kTolerances[second & 3U];

  const Drawing whole = Draw(pathData, SIZE_MAX, kCommandsACall[0], matrix, tolerance, kHeight, false, rule);
  const Drawing cut = Draw(pathData, kPieces[first & 7U], kCommandsACall[(first >> 3U) & 3U], matrix, tolerance,
                           kStripRows[(second >> 2U) & 7U], (second & 0x20U) != 0, rule);
  if (whole.leftOff || cut.leftOff) {
    return 0;
  }
  BN_FUZZ_CHECK(cut.told == whole.told, "data cut up gave other commands than the same data whole");
  BN_FUZZ_CHECK(cut.status == whole.status && cut.errorOffset == whole.errorOffset && cut.commands == whole.commands &&
                    cut.clamped == whole.clamped,
                "data cut up ended differently from the same data whole");
  BN_FUZZ_CHECK(cut.picture == whole.picture, "a shape drawn in strips is not the shape drawn whole");
  return 0;
}
