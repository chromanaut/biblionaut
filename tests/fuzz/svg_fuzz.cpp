// The fuzz target for the drawing of a whole SVG file (draw/svg_render.h),
// and with it for everything a file is read with: its tags, what its
// attributes say, path data, the flattener and the filler.
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes of the file the renderer is
//                         given at a call
//            bits 3 to 5: how many rows a strip has
//            bits 6 to 7: how large the picture is
//   byte 1   bits 0 to 1: how far a line may stand from its curve
//   the rest a file, or what passes for one
//
// The file is gone through three times: once with nothing to draw into,
// once drawn whole from a file given all at once, and once drawn in strips
// from a file cut up, as the two bytes say. What has to hold, whatever the
// bytes are:
//
//   - a call takes no more than it was given, and there are no more calls
//     than three times the bytes: it cannot be made to go round;
//   - a call gives an outline no more than kMostCommandsPerStep commands
//     and fills no more than one shape, so its lines, the cells they add
//     to and the pixels a filling looks at are bounded too;
//   - once the file has gone wrong, whatever else is given is taken and
//     nothing comes of it, and where it went wrong is not past its end;
//   - no more groups are open at once than kDeepest;
//   - nothing is written outside the strip, and the filler's cells are
//     left as they were found;
//   - a file that is not a drawing leaves the picture as it was;
//   - every strip's reading of the file is the same reading, and so is
//     the reading with nothing to draw into, which draws nothing and says
//     nothing was left out that the drawing does not say too;
//   - the two pictures are the same, byte for byte, however the picture
//     was cut into strips and the file into pieces.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_render.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Filler;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::PathFlattener;
using bn::draw::SvgRenderer;
using bn::draw::SvgReport;
using bn::draw::SvgSize;
using bn::draw::SvgStatus;

struct PictureSize {
  std::size_t width;
  std::size_t height;
};

constexpr std::array<std::size_t, 8> kPieces = {SIZE_MAX, 1, 2, 3, 7, 64, 255, 1024};
// Nothing here stands for the whole picture.
constexpr std::array<std::size_t, 8> kStripRows = {0, 1, 2, 3, 5, 8, 13, 24};
constexpr std::array<PictureSize, 4> kSizes = {
    {{.width = 40, .height = 24}, {.width = 24, .height = 40}, {.width = 7, .height = 5}, {.width = 64, .height = 48}}};
// A sixteenth of a pixel, a quarter, a whole one, and a 64th.
constexpr std::array<Fixed, 4> kTolerances = {256, 1024, 4096, 64};

// A few bytes of path data can ask for millions of lines: an arc is up to
// 32,768 of them. Past this many a drawing is left off: what the engine is
// after is in the file, not in how long it can be made to go on.
constexpr std::uint64_t kMostLines = 200000;

// What is kept either side of a strip, to see that nothing is written
// there.
constexpr std::size_t kGuardBytes = 32;
constexpr std::uint8_t kGuard = 0xA5;

// What one reading of the file came to.
struct Reading {
  SvgStatus status = SvgStatus::kOk;
  std::uint32_t errorOffset = 0;
  SvgReport report;
  SvgSize size;
  bool leftOff = false;  // The drawing reached kMostLines, and the file was not read to its end.
};

bool SameSize(const SvgSize& one, const SvgSize& other) {
  return one.known == other.known && one.hasWidth == other.hasWidth && one.hasHeight == other.hasHeight &&
         one.hasBox == other.hasBox && one.width.value == other.width.value && one.width.unit == other.width.unit &&
         one.height.value == other.height.value && one.height.unit == other.height.unit && one.box == other.box;
}

// Whether two readings found the same file. `lines` are left out: they
// depend on the strip, since a curve outside it is not cut up.
bool SameFile(const Reading& one, const Reading& other) {
  return one.status == other.status && one.errorOffset == other.errorOffset &&
         one.report.leftOut == other.report.leftOut && one.report.elements == other.report.elements &&
         one.report.shapes == other.report.shapes && one.report.commands == other.report.commands &&
         one.report.deepest == other.report.deepest && SameSize(one.size, other.size);
}

// Gives the renderer the file, `piece` bytes at a time, with `filler` to
// draw into or nothing, and holds every call to what a call may do.
// `lines` is how many the drawing has come to so far, over all its strips.
Reading Read(SvgRenderer& renderer, Filler* filler, std::string_view file, std::size_t piece, PictureSize picture,
             Fixed tolerance, std::uint64_t& lines) {
  Reading reading;
  renderer.Begin(filler, static_cast<Fixed>(picture.width * kFixedOne), static_cast<Fixed>(picture.height * kFixedOne),
                 tolerance);
  const std::uint64_t cellsALine = filler == nullptr
                                       ? 0
                                       : Filler::MostCellsPerLine(static_cast<std::uint32_t>(filler->Width()),
                                                                  static_cast<std::uint32_t>(filler->Rows()));
  const std::uint64_t pixels =
      filler == nullptr ? 0 : static_cast<std::uint64_t>(filler->Width()) * static_cast<std::uint64_t>(filler->Rows());
  const std::uint64_t linesBefore = lines;
  std::size_t calls = 0;
  bool failed = false;
  for (std::size_t at = 0; at < file.size() && !reading.leftOff;) {
    std::string_view bytes = file.substr(at, piece);
    at += bytes.size();
    while (!bytes.empty() && !reading.leftOff) {
      const SvgReport before = renderer.Report();
      const std::uint32_t cellsBefore = filler == nullptr ? 0 : filler->CellsTouched();
      const std::uint32_t visitedBefore = filler == nullptr ? 0 : filler->PixelsVisited();
      const std::size_t took = renderer.Step(bytes);
      ++calls;
      BN_FUZZ_CHECK(took <= bytes.size(), "a call took more than it was given");
      const SvgReport& after = renderer.Report();
      const std::uint64_t commands = after.commands - before.commands;
      const std::uint64_t made = after.lines - before.lines;
      BN_FUZZ_CHECK(commands <= SvgRenderer::kMostCommandsPerStep,
                    "a call gave an outline more commands than a call may");
      BN_FUZZ_CHECK(after.shapes - before.shapes <= 1, "a call filled more than one shape");
      BN_FUZZ_CHECK(made <= std::uint64_t{SvgRenderer::kMostCommandsPerStep} * PathFlattener::kMostLinesPerArc,
                    "a call made more lines than its commands can come to");
      if (filler != nullptr) {
        // Each line, and each closing of an outline, which the filler
        // makes a line of for itself.
        BN_FUZZ_CHECK(filler->CellsTouched() - cellsBefore <= (made + commands + 2) * cellsALine,
                      "a call added to more cells than its lines may");
        BN_FUZZ_CHECK(filler->PixelsVisited() - visitedBefore <= pixels,
                      "a call looked at more pixels than the strip has");
      } else {
        BN_FUZZ_CHECK(commands == 0 && made == 0 && after.shapes == 0, "something was drawn with nothing to draw into");
      }
      if (failed) {
        BN_FUZZ_CHECK(took == bytes.size() && after.elements == before.elements && commands == 0 && made == 0 &&
                          after.shapes == before.shapes && after.leftOut == before.leftOut,
                      "a file that had gone wrong was read on");
      }
      failed = renderer.Status() != SvgStatus::kOk;
      BN_FUZZ_CHECK(after.deepest <= SvgRenderer::kDeepest, "more groups were open at once than can be");
      bytes.remove_prefix(took);
      lines = linesBefore + after.lines;
      reading.leftOff = lines > kMostLines;
    }
  }
  BN_FUZZ_CHECK(calls <= (3 * file.size()) + 16, "there were more calls than three times the bytes");

  reading.status = renderer.Finish();
  BN_FUZZ_CHECK(!failed || reading.status == renderer.Status(), "a file that had gone wrong ended otherwise");
  BN_FUZZ_CHECK(reading.status == SvgStatus::kOk || reading.status == SvgStatus::kNotSvg ||
                    reading.status == SvgStatus::kMalformed || reading.status == SvgStatus::kTruncated,
                "a file ended with a status there is not");
  reading.errorOffset = renderer.ErrorOffset();
  reading.report = renderer.Report();
  reading.size = renderer.Size();
  BN_FUZZ_CHECK(reading.errorOffset <= file.size(), "a file is said to have gone wrong past its end");
  if (!reading.leftOff) {
    BN_FUZZ_CHECK(reading.status != SvgStatus::kOk || reading.size.known,
                  "a file was read as a drawing and its size is not known");
    BN_FUZZ_CHECK(reading.status != SvgStatus::kNotSvg || reading.report.shapes == 0,
                  "a file that is not a drawing had a shape filled");
  }
  if (reading.size.known) {
    Fixed width = -1;
    Fixed height = -1;
    reading.size.Natural(width, height);
    BN_FUZZ_CHECK(width >= 0 && height >= 0 && (width == 0) == (height == 0),
                  "a drawing's own size is less than nothing, or nothing one way only");
  }
  return reading;
}

// What drawing the file came to.
struct Drawing {
  std::vector<std::uint8_t> picture;
  Reading reading;  // The first strip's.
};

Drawing Draw(std::string_view file, PictureSize size, std::size_t stripRows, std::size_t piece, Fixed tolerance) {
  Drawing drawing;
  drawing.picture.assign(size.width * size.height, 255);
  SvgRenderer renderer;
  std::uint64_t lines = 0;
  for (std::size_t top = 0; top < size.height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, size.height - top);
    // The strip and its cells are each in memory of their own, of exactly
    // their size, with something either side of the strip that must still
    // be there afterwards.
    std::vector<std::uint8_t> strip((size.width * rows) + (2 * kGuardBytes), kGuard);
    const std::span<std::uint8_t> gray = std::span(strip).subspan(kGuardBytes, size.width * rows);
    std::fill(gray.begin(), gray.end(), std::uint8_t{255});
    std::vector<std::int16_t> cells(Filler::CellsFor(size.width, rows));
    Filler filler;
    BN_FUZZ_CHECK(filler.Begin(gray, size.width, static_cast<std::int32_t>(top), cells),
                  "a strip of the picture was refused");

    const Reading reading = Read(renderer, &filler, file, piece, size, tolerance, lines);
    BN_FUZZ_CHECK(std::all_of(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(size.width * rows),
                              [](std::int16_t cell) { return cell == 0; }),
                  "a drawing left something in the filler's cells");
    BN_FUZZ_CHECK(
        std::all_of(strip.begin(), strip.begin() + kGuardBytes, [](std::uint8_t byte) { return byte == kGuard; }) &&
            std::all_of(strip.end() - kGuardBytes, strip.end(), [](std::uint8_t byte) { return byte == kGuard; }),
        "something was written outside the strip");
    std::copy(gray.begin(), gray.end(), drawing.picture.begin() + static_cast<std::ptrdiff_t>(top * size.width));
    if (top == 0 || reading.leftOff) {
      drawing.reading = reading;
    } else {
      BN_FUZZ_CHECK(SameFile(reading, drawing.reading), "the file was read differently for one strip than for another");
    }
    if (reading.leftOff) {
      break;
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
  const std::string_view file(reinterpret_cast<const char*>(rest.data()), rest.size());

  const std::size_t piece = kPieces[first & 7U];
  const PictureSize picture = kSizes[(first >> 6U) & 3U];
  const std::size_t chosenRows = kStripRows[(first >> 3U) & 7U];
  const std::size_t stripRows = chosenRows == 0 ? picture.height : std::min(chosenRows, picture.height);
  const Fixed tolerance = kTolerances[second & 3U];

  // With nothing to draw into.
  SvgRenderer renderer;
  std::uint64_t noLines = 0;
  const Reading read = Read(renderer, nullptr, file, piece, picture, tolerance, noLines);
  BN_FUZZ_CHECK(read.report.shapes == 0 && read.report.commands == 0 && read.report.lines == 0,
                "a reading with nothing to draw into drew something");

  const Drawing whole = Draw(file, picture, picture.height, SIZE_MAX, tolerance);
  const Drawing cut = Draw(file, picture, stripRows, piece, tolerance);
  if (whole.reading.leftOff || cut.reading.leftOff) {
    return 0;
  }
  BN_FUZZ_CHECK(SameFile(cut.reading, whole.reading), "a file cut up was read differently from the same file whole");
  BN_FUZZ_CHECK(cut.picture == whole.picture, "a file drawn in strips is not the file drawn whole");

  // The reading that drew nothing found the same file, and all that it
  // says was left out the drawing says too. (The drawing can say more:
  // what goes wrong in an outline is only found by drawing it.)
  BN_FUZZ_CHECK(read.status == whole.reading.status && read.errorOffset == whole.reading.errorOffset &&
                    read.report.elements == whole.reading.report.elements &&
                    read.report.deepest == whole.reading.report.deepest && SameSize(read.size, whole.reading.size),
                "a file read with nothing to draw into was another file");
  BN_FUZZ_CHECK((read.report.leftOut & ~whole.reading.report.leftOut) == 0,
                "a reading with nothing to draw into says something was left out that the drawing does not");
  if (whole.reading.status == SvgStatus::kNotSvg) {
    BN_FUZZ_CHECK(
        std::all_of(whole.picture.begin(), whole.picture.end(), [](std::uint8_t pixel) { return pixel == 255; }),
        "a file that is not a drawing was drawn");
  }
  return 0;
}
