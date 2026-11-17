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
//            bits 2 to 4: how much memory there is for what the file
//                         defines
//            bits 5 to 6: how the picture is turned into the strips'
//                         rows (SvgRenderer::PlaceOn()): not at all, a
//                         quarter either way, or a half
//            bit 7:       whether no stroke is drawn thinner than a
//                         pixel (SvgRenderer::SetThinnestStroke())
//   the rest a file, or what passes for one
//
// The file is gone through three times: once with nothing to draw into,
// once drawn whole from a file given all at once, and once drawn in strips
// from a file cut up, as the two bytes say. What has to hold, whatever the
// bytes are:
//
//   - a call takes no more than it was given, and there are no more calls
//     than three times the bytes it read, of the file and of what it kept
//     of the file: it cannot be made to go round;
//   - a call gives an outline no more than kMostCommandsPerStep commands
//     and fills no more than one shape, so its lines, the cells they add
//     to and the pixels a filling looks at are bounded too;
//   - a call reads the file or what was kept of it, not both; of what was
//     kept it reads no more than kReusedPerStep bytes, and it begins no
//     more than kTokensPerStep elements;
//   - no more is kept than there is memory for or than the file holds,
//     and no more is read again in a pass than kMostReused bytes and what
//     was being read when that was reached;
//   - whatever the memory for definitions held before a pass changes
//     nothing: it is filled with something else before each;
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
//     was cut into strips and the file into pieces; and so is the file
//     drawn once into a SpoolWriter and given back a band at a time, each
//     step of which reads a chunk and fills once at the most.

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
#include "draw/spool.h"
#include "draw/svg_render.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Filler;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::SpoolStatus;
using bn::draw::SpoolWriter;
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

// How much memory there is for what the file defines. The first, which
// the seeds that say nothing of it get, is more than a file of the length
// the engine makes can fill.
constexpr std::array<std::size_t, 8> kRooms = {16384, 0, 24, 96, 300, 1024, 4096, 8192};

// A few bytes of path data can ask for millions of lines: an arc is up to
// 32,768 of them. Past this many a drawing is left off: what the engine is
// after is in the file, not in how long it can be made to go on. The same
// goes for a few bytes that use what uses what uses something: the
// renderer's own limit on reading again, kMostReused, is far above what
// is worth an engine's time, and a unit test takes a file up to it.
constexpr std::uint64_t kMostLines = 200000;
constexpr std::uint64_t kMostReusedHere = 200000;

// How much a drawing has come to so far, over all its strips.
struct Work {
  std::uint64_t lines = 0;
  std::uint64_t reused = 0;
};

// The memory a renderer is given for what a file defines: of exactly the
// size chosen, so that a byte read or written past either end of it is
// outside any memory there is.
class Room {
 public:
  explicit Room(std::size_t size) : bytes_(size) {}

  std::span<std::uint8_t> Block() noexcept { return bytes_; }
  std::size_t Size() const noexcept { return bytes_.size(); }
  // Leaves in it what a pass might mistake for something kept, a different
  // thing each time, so that a pass that read any of it would not come
  // out as the others do.
  void Litter() noexcept {
    constexpr std::string_view kLitter = "<>/'\"= #&;]au";
    std::fill(bytes_.begin(), bytes_.end(), static_cast<std::uint8_t>(kLitter[passes_ % kLitter.size()]));
    ++passes_;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t passes_ = 0;
};

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
         one.report.shapes == other.report.shapes && one.report.strokes == other.report.strokes &&
         one.report.commands == other.report.commands && one.report.deepest == other.report.deepest &&
         one.report.kept == other.report.kept && one.report.reused == other.report.reused &&
         one.report.rules == other.report.rules && one.report.gradients == other.report.gradients &&
         one.report.looked == other.report.looked && SameSize(one.size, other.size);
}

// Gives the renderer the file, `piece` bytes at a time, with `filler` to
// draw into or nothing, and holds every call to what a call may do.
// `room` is the memory it has for definitions, and `work` what the
// drawing has come to so far, over all its strips.
Reading Read(SvgRenderer& renderer, Filler* filler, std::string_view file, std::size_t piece, PictureSize picture,
             Fixed tolerance, Room& room, Work& work) {
  Reading reading;
  room.Litter();
  renderer.Begin(filler, static_cast<Fixed>(picture.width * kFixedOne), static_cast<Fixed>(picture.height * kFixedOne),
                 tolerance);
  const std::uint64_t cellsALine = filler == nullptr
                                       ? 0
                                       : Filler::MostCellsPerLine(static_cast<std::uint32_t>(filler->Width()),
                                                                  static_cast<std::uint32_t>(filler->Rows()));
  const std::uint64_t pixels =
      filler == nullptr ? 0 : static_cast<std::uint64_t>(filler->Width()) * static_cast<std::uint64_t>(filler->Rows());
  const Work workBefore = work;
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
      BN_FUZZ_CHECK((after.shapes - before.shapes) + (after.strokes - before.strokes) <= 1,
                    "a call filled more than one shape or stroke");
      const std::uint64_t again = after.reused - before.reused;
      BN_FUZZ_CHECK(after.elements - before.elements <= SvgRenderer::kTokensPerStep,
                    "a call began more elements than a call may");
      BN_FUZZ_CHECK(again <= SvgRenderer::kReusedPerStep, "a call read more of what is kept than a call may");
      BN_FUZZ_CHECK(after.looked - before.looked <= SvgRenderer::kMostLooksPerStep,
                    "a call looked through what is kept more often than a call may");
      BN_FUZZ_CHECK(took == 0 || again == 0, "a call read the file and what is kept of it");
      BN_FUZZ_CHECK(after.kept >= before.kept && after.kept <= room.Size(),
                    "more is kept than there is memory for, or less than was");
      BN_FUZZ_CHECK(made <= SvgRenderer::kMostLinesPerStep, "a call made more lines than its commands can come to");
      if (filler != nullptr) {
        // Each line, and each closing of an outline, which the filler
        // makes a line of for itself: one for each command, and one for
        // each of a stroke's pieces, which have more lines than that.
        BN_FUZZ_CHECK(filler->CellsTouched() - cellsBefore <= ((2 * made) + commands + 2) * cellsALine,
                      "a call added to more cells than its lines may");
        BN_FUZZ_CHECK(filler->PixelsVisited() - visitedBefore <= pixels,
                      "a call looked at more pixels than the strip has");
      } else {
        BN_FUZZ_CHECK(commands == 0 && made == 0 && after.shapes == 0 && after.strokes == 0,
                      "something was drawn with nothing to draw into");
      }
      if (failed) {
        BN_FUZZ_CHECK(took == bytes.size() && after.elements == before.elements && commands == 0 && made == 0 &&
                          after.shapes == before.shapes && after.leftOut == before.leftOut && again == 0 &&
                          after.kept == before.kept && after.looked == before.looked,
                      "a file that had gone wrong was read on");
      }
      failed = renderer.Status() != SvgStatus::kOk;
      BN_FUZZ_CHECK(after.deepest <= SvgRenderer::kDeepest, "more groups were open at once than can be");
      bytes.remove_prefix(took);
      work.lines = workBefore.lines + after.lines;
      work.reused = workBefore.reused + after.reused;
      reading.leftOff = work.lines > kMostLines || work.reused > kMostReusedHere;
    }
  }
  BN_FUZZ_CHECK(calls <= (3 * (file.size() + renderer.Report().reused)) + 16,
                "there were more calls than three times the bytes read");

  reading.status = renderer.Finish();
  BN_FUZZ_CHECK(!failed || reading.status == renderer.Status(), "a file that had gone wrong ended otherwise");
  BN_FUZZ_CHECK(reading.status == SvgStatus::kOk || reading.status == SvgStatus::kNotSvg ||
                    reading.status == SvgStatus::kMalformed || reading.status == SvgStatus::kTruncated,
                "a file ended with a status there is not");
  reading.errorOffset = renderer.ErrorOffset();
  reading.report = renderer.Report();
  reading.size = renderer.Size();
  BN_FUZZ_CHECK(reading.errorOffset <= file.size(), "a file is said to have gone wrong past its end");
  BN_FUZZ_CHECK(reading.report.kept <= file.size(), "more is kept of a file than it holds");
  BN_FUZZ_CHECK(reading.report.reused <= SvgRenderer::kMostReused + (SvgRenderer::kDeepestUse * room.Size()),
                "more was read again than the most that may be");
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

// What the picture, `page` across and down, is put through to go into the
// strips' rows, for each of the four turns; and how large it is there.
bn::draw::Matrix Turned(unsigned turn, PictureSize page) {
  constexpr bn::draw::Coefficient kOne = bn::draw::kCoefficientOne;
  const auto across = static_cast<Fixed>(page.width * kFixedOne);
  const auto down = static_cast<Fixed>(page.height * kFixedOne);
  switch (turn) {
    case 1:
      return {.a = 0, .b = -kOne, .c = kOne, .d = 0, .e = 0, .f = across};
    case 2:
      return {.a = 0, .b = kOne, .c = -kOne, .d = 0, .e = down, .f = 0};
    case 3:
      return {.a = -kOne, .b = 0, .c = 0, .d = -kOne, .e = across, .f = down};
    default:
      return {};
  }
}
PictureSize TurnedSize(unsigned turn, PictureSize page) {
  return turn == 1 || turn == 2 ? PictureSize{.width = page.height, .height = page.width} : page;
}

Drawing Draw(std::string_view file, PictureSize page, unsigned turn, std::size_t stripRows, std::size_t piece,
             Fixed tolerance, std::size_t roomSize, Fixed thinnest) {
  const PictureSize size = TurnedSize(turn, page);
  Drawing drawing;
  drawing.picture.assign(size.width * size.height, 255);
  Room room(roomSize);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(room.Block());
  renderer.PlaceOn(Turned(turn, page));
  renderer.SetThinnestStroke(thinnest);
  Work work;
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

    const Reading reading = Read(renderer, &filler, file, piece, page, tolerance, room, work);
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

// A recording kept in memory, which counts its reads.
class MemorySpool final : public bn::draw::SpoolStore {
 public:
  std::size_t reads = 0;
  bool Append(std::span<const std::uint8_t> bytes) noexcept override {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return true;
  }
  bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> bytes) noexcept override {
    ++reads;
    if (std::size_t{offset} + bytes.size() > bytes_.size()) {
      return false;
    }
    std::copy_n(bytes_.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }

 private:
  std::vector<std::uint8_t> bytes_;
};

// The file read once into a SpoolWriter, and each band of `stripRows`
// rows drawn from that: what a band is given back has to be what drawing
// into it gives, and each step reads one chunk and fills once at the most.
std::vector<std::uint8_t> Spooled(std::string_view file, PictureSize page, unsigned turn, std::size_t stripRows,
                                  Fixed tolerance, std::size_t roomSize, Fixed thinnest) {
  const PictureSize size = TurnedSize(turn, page);
  const std::size_t fewest = (size.height + SpoolWriter::kMostBands - 1) / SpoolWriter::kMostBands;
  const std::size_t rows = std::max(stripRows, fewest);
  const std::size_t bands =
      SpoolWriter::BandsFor(static_cast<std::int32_t>(size.height), static_cast<std::int32_t>(rows));
  std::vector<std::uint8_t> picture(size.width * size.height, 255);
  MemorySpool store;
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(bands));
  std::vector<std::uint16_t> links(SpoolWriter::kMostChunks);
  SpoolWriter writer;
  BN_FUZZ_CHECK(writer.Begin(static_cast<std::int32_t>(size.width), static_cast<std::int32_t>(size.height),
                             static_cast<std::int32_t>(rows), buffers, links, store),
                "a picture could not be kept");
  Room room(roomSize);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(room.Block());
  renderer.PlaceOn(Turned(turn, page));
  renderer.SetThinnestStroke(thinnest);
  renderer.Begin(&writer, static_cast<Fixed>(page.width * kFixedOne), static_cast<Fixed>(page.height * kFixedOne),
                 tolerance);
  std::string_view bytes = file;
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  static_cast<void>(renderer.Finish());
  BN_FUZZ_CHECK(writer.Finish() == SpoolStatus::kOk, "a recording in memory with links enough did not go right");
  std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
  for (std::size_t band = 0; band < bands; ++band) {
    const std::size_t top = band * rows;
    const std::size_t here = std::min(rows, size.height - top);
    std::vector<std::int16_t> cells(Filler::CellsFor(size.width, here));
    Filler filler;
    BN_FUZZ_CHECK(filler.Begin(std::span(picture).subspan(top * size.width, size.width * here), size.width,
                               static_cast<std::int32_t>(top), cells) &&
                      writer.BeginBand(band, chunk),
                  "a band could not be given back");
    for (bool more = true; more;) {
      const std::size_t reads = store.reads;
      const std::uint32_t visited = filler.PixelsVisited();
      more = writer.ReplayStep(filler);
      BN_FUZZ_CHECK(store.reads - reads <= 1 && filler.PixelsVisited() - visited <= size.width * here,
                    "a step of giving back read more than a chunk or filled more than once");
    }
    BN_FUZZ_CHECK(writer.Status() == SpoolStatus::kOk, "a recording in memory could not be given back");
  }
  return picture;
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
  const std::size_t roomSize = kRooms[(second >> 2U) & 7U];
  const unsigned turn = (second >> 5U) & 3U;
  const Fixed thinnest = (second & 0x80U) != 0 ? kFixedOne : 0;

  // With nothing to draw into.
  Room room(roomSize);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(room.Block());
  renderer.PlaceOn(Turned(turn, picture));
  renderer.SetThinnestStroke(thinnest);
  Work nothing;
  const Reading read = Read(renderer, nullptr, file, piece, picture, tolerance, room, nothing);
  BN_FUZZ_CHECK(read.report.shapes == 0 && read.report.commands == 0 && read.report.lines == 0,
                "a reading with nothing to draw into drew something");

  const std::size_t turnedRows = TurnedSize(turn, picture).height;
  const Drawing whole = Draw(file, picture, turn, turnedRows, SIZE_MAX, tolerance, roomSize, thinnest);
  const Drawing cut = Draw(file, picture, turn, std::min(stripRows, turnedRows), piece, tolerance, roomSize, thinnest);
  if (read.leftOff || whole.reading.leftOff || cut.reading.leftOff) {
    return 0;
  }
  BN_FUZZ_CHECK(SameFile(cut.reading, whole.reading), "a file cut up was read differently from the same file whole");
  BN_FUZZ_CHECK(cut.picture == whole.picture, "a file drawn in strips is not the file drawn whole");
  BN_FUZZ_CHECK(
      Spooled(file, picture, turn, std::min(stripRows, turnedRows), tolerance, roomSize, thinnest) == whole.picture,
      "a file kept and given back a band at a time is not the file drawn whole");

  // The reading that drew nothing found the same file, and all that it
  // says was left out the drawing says too. (The drawing can say more:
  // what goes wrong in an outline is only found by drawing it.)
  BN_FUZZ_CHECK(read.status == whole.reading.status && read.errorOffset == whole.reading.errorOffset &&
                    read.report.elements == whole.reading.report.elements &&
                    read.report.deepest == whole.reading.report.deepest &&
                    read.report.kept == whole.reading.report.kept &&
                    read.report.reused == whole.reading.report.reused && SameSize(read.size, whole.reading.size),
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
