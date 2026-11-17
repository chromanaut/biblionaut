// bn-svg: reads the parts of an SVG drawing with the code the firmware
// draws one with, and says what it made of them.
//
// `draw` draws a whole SVG file into a picture, in strips, as the firmware
// would, and says what the file held that it left out. The other two read
// path data, the `d` of a `<path>`, by itself: `path` prints each command
// it holds and the straight lines each becomes once it has been put through
// a transform and its curves have been flattened, and `fill` draws the
// shape it outlines.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_number.h"
#include "draw/svg_path.h"
#include "draw/svg_render.h"
#include "draw/svg_transform.h"

namespace {

using bn::draw::Angle;
using bn::draw::Fixed;
using bn::draw::LineSink;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::PathSink;
using bn::draw::Point;
using bn::draw::SvgPathReader;
using bn::draw::SvgPathStatus;
using bn::draw::SvgRenderer;
using bn::draw::SvgStatus;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s draw FILE [--width W] [--height H] [--rows ROWS] [--tolerance PIXELS]\n"
               "                 [--pieces BYTES] [--definitions BYTES] [--out PICTURE]\n"
               "       %s path FILE [--transform TEXT] [--tolerance PIXELS] [--pieces BYTES] [--count]\n"
               "       %s fill FILE --size WxH [--transform TEXT] [--tolerance PIXELS] [--rule RULE]\n"
               "                 [--gray 0-255] [--opacity 0-255] [--rows ROWS] [--out PICTURE]\n"
               "\n"
               "Reads an SVG drawing, or the outline of one shape, the way the firmware does.\n"
               "\n"
               "  draw  draws the SVG file FILE in a picture that starts white, and says what\n"
               "        the drawing took and what the file held that was left out. The picture\n"
               "        is drawn a strip at a time, the file being read through once for each\n"
               "        strip, which is how the firmware draws a picture taller than the memory\n"
               "        it has for one.\n"
               "\n"
               "  --width W           the picture's width in pixels, and\n"
               "  --height H          its height. With one of them, the other keeps the\n"
               "                      drawing's shape; with neither, the picture is the size the\n"
               "                      drawing says it is\n"
               "  --rows ROWS         how many rows a strip has (default: the whole picture)\n"
               "  --tolerance PIXELS  how far a line may stand from its curve (default: 0.0625)\n"
               "  --pieces BYTES      give the file to the reader this many bytes at a time\n"
               "                      (default: 1024)\n"
               "  --definitions BYTES how much memory there is for what the file defines, for\n"
               "                      `use` to draw (default: 32768)\n"
               "  --out PICTURE       write the picture there, as a PGM file: a byte a pixel\n"
               "\n"
               "  path  reads the path data in FILE, which is the value of a `d` attribute and\n"
               "        nothing else, and prints what it draws. For each command, a line with\n"
               "        its letter and its numbers as places in the drawing:\n"
               "            M x y           L x y           Z\n"
               "            Q x1 y1 x y     C x1 y1 x2 y2 x y\n"
               "            A rx ry degrees large sweep x y\n"
               "        and under it, set in by a space, the straight lines it became, in\n"
               "        pixels:  m x y  where an outline begins,  l x y  and  z\n"
               "\n"
               "  --transform TEXT    an SVG transform every point is put through (default: none)\n"
               "  --tolerance PIXELS  how far a line may stand from its curve (default: 0.25)\n"
               "  --pieces BYTES      give the reader the data this many bytes at a time\n"
               "                      (default: all at once)\n"
               "  --count             print no commands or lines, only how many there were\n"
               "\n"
               "  fill  draws the shape the path data in FILE outlines, in a picture W pixels\n"
               "        wide and H high that starts white, and says what the drawing took. The\n"
               "        picture is drawn a strip at a time, the data being read through once\n"
               "        for each strip, which is how the firmware draws a picture taller than\n"
               "        the memory it has for one.\n"
               "\n"
               "  --size WxH          the picture's size in pixels\n"
               "  --rule RULE         nonzero (the default) or evenodd: which parts of an outline\n"
               "                      that crosses itself are inside\n"
               "  --gray 0-255        what the shape is filled with, from black, the default, to\n"
               "                      white\n"
               "  --opacity 0-255     how much of what is under it the shape hides (default: 255,\n"
               "                      all of it)\n"
               "  --rows ROWS         how many rows a strip has (default: the whole picture)\n"
               "  --out PICTURE       write the picture there, as a PGM file: a byte a pixel\n"
               "\n"
               "Ends with 0 if the file was read to its end, 1 if it is not what it should be\n"
               "from some point on (what came before that is still printed, or drawn) or a file\n"
               "cannot be read or written. A drawing with things in it that were left out still\n"
               "ends with 0.\n",
               program, program, program);
}

// A position as it is printed: to four places, which tells any two apart.
std::string Text(Fixed value) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%.4f", static_cast<double>(value) / bn::draw::kFixedOne);
  return text.data();
}

std::string Text(Point point) { return Text(point.x) + " " + Text(point.y); }

// Prints the lines it is given.
class LinePrinter final : public LineSink {
 public:
  explicit LinePrinter(bool quiet) : quiet_(quiet) {}

  void MoveTo(Point to) noexcept override { Print("m", &to); }
  void LineTo(Point to) noexcept override { Print("l", &to); }
  void Close() noexcept override { Print("z", nullptr); }

 private:
  void Print(const char* letter, const Point* point) const noexcept {
    if (quiet_) {
      return;
    }
    if (point) {
      std::printf(" %s %s\n", letter, Text(*point).c_str());
    } else {
      std::printf(" %s\n", letter);
    }
  }

  bool quiet_;
};

// Prints each command it is told of, and passes it on to be flattened.
class CommandPrinter final : public PathSink {
 public:
  CommandPrinter(PathSink& next, bool quiet) : next_(next), quiet_(quiet) {}

  void MoveTo(Point to) noexcept override {
    Print("M " + Text(to));
    next_.MoveTo(to);
  }
  void LineTo(Point to) noexcept override {
    Print("L " + Text(to));
    next_.LineTo(to);
  }
  void QuadTo(Point control, Point to) noexcept override {
    Print("Q " + Text(control) + " " + Text(to));
    next_.QuadTo(control, to);
  }
  void CubicTo(Point control1, Point control2, Point to) noexcept override {
    Print("C " + Text(control1) + " " + Text(control2) + " " + Text(to));
    next_.CubicTo(control1, control2, to);
  }
  void ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept override {
    std::array<char, 32> degrees{};
    std::snprintf(degrees.data(), degrees.size(), "%.4f", static_cast<double>(turn) / 4294967296.0 * 360.0);
    Print("A " + Text(radiusX) + " " + Text(radiusY) + " " + degrees.data() + (largeArc ? " 1" : " 0") +
          (sweep ? " 1 " : " 0 ") + Text(to));
    next_.ArcTo(radiusX, radiusY, turn, largeArc, sweep, to);
  }
  void Close() noexcept override {
    Print("Z");
    next_.Close();
  }

 private:
  void Print(const std::string& line) const noexcept {
    if (!quiet_) {
      std::printf("%s\n", line.c_str());
    }
  }

  PathSink& next_;
  bool quiet_;
};

struct Options {
  std::string file;
  std::string transform;
  Fixed tolerance = PathFlattener::kDefaultTolerance;
  bool toleranceGiven = false;
  std::size_t pieces = 0;
  bool count = false;
  // For `fill` and `draw`.
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t rows = 0;
  bn::draw::FillRule rule = bn::draw::FillRule::kNonZero;
  std::uint8_t gray = 0;
  std::uint8_t opacity = 255;
  std::string out;
  // For `draw`: how much memory there is for what a file defines.
  std::size_t definitions = 32768;
};

// The whole of a file. False, having said so, if it cannot be read.
bool Read(const std::string& path, std::string& data) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return false;
  }
  data.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return true;
}

int Path(const Options& options) {
  std::string data;
  if (!Read(options.file, data)) {
    return 1;
  }

  Matrix matrix;
  bool clamped = false;
  if (!bn::draw::ParseSvgTransform(options.transform, matrix, clamped)) {
    std::fprintf(stderr, "error: not a transform: %s\n", options.transform.c_str());
    return 2;
  }

  LinePrinter lines(options.count);
  PathFlattener flattener(lines, matrix, options.tolerance);
  CommandPrinter commands(flattener, options.count);
  SvgPathReader reader(commands);
  const std::size_t piece = options.pieces == 0 ? std::max<std::size_t>(data.size(), 1) : options.pieces;
  for (std::size_t at = 0; at < data.size(); at += piece) {
    reader.Feed(std::string_view(data.data() + at, std::min(piece, data.size() - at)));
  }
  const SvgPathStatus status = reader.Finish();
  std::fflush(stdout);

  if (clamped || reader.Clamped() || flattener.Clamped()) {
    std::fprintf(stderr, "a number was too large and was held at the largest that fits\n");
  }
  if (status != SvgPathStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s at byte %u, after %u commands and %u lines\n", options.file.c_str(),
                 bn::draw::SvgPathStatusName(status), static_cast<unsigned>(reader.ErrorOffset()),
                 static_cast<unsigned>(reader.Commands()), static_cast<unsigned>(flattener.Lines()));
    return 1;
  }
  std::fprintf(stderr, "%u commands, %u lines, %zu bytes\n", static_cast<unsigned>(reader.Commands()),
               static_cast<unsigned>(flattener.Lines()), data.size());
  return 0;
}

int Fill(const Options& options) {
  std::string data;
  if (!Read(options.file, data)) {
    return 1;
  }
  Matrix matrix;
  bool clamped = false;
  if (!bn::draw::ParseSvgTransform(options.transform, matrix, clamped)) {
    std::fprintf(stderr, "error: not a transform: %s\n", options.transform.c_str());
    return 2;
  }

  // The picture is whole here so that it can be written out. The firmware
  // would hold one strip of it at a time and hand each on as it was done.
  const std::size_t stripRows = options.rows == 0 ? options.height : std::min(options.rows, options.height);
  std::vector<std::uint8_t> picture(options.width * options.height, 255);
  std::vector<std::int16_t> cells(bn::draw::Filler::CellsFor(options.width, stripRows));
  const auto pixels = [](std::size_t count) { return static_cast<Fixed>(count * bn::draw::kFixedOne); };

  SvgPathStatus status = SvgPathStatus::kOk;
  std::uint32_t errorOffset = 0;
  std::uint32_t commands = 0;
  std::uint64_t lines = 0;
  std::uint64_t cellsTouched = 0;
  std::size_t strips = 0;
  for (std::size_t top = 0; top < options.height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, options.height - top);
    bn::draw::Filler filler;
    if (!filler.Begin(std::span(picture).subspan(top * options.width, rows * options.width), options.width,
                      static_cast<std::int32_t>(top), cells)) {
      std::fprintf(stderr, "error: a strip of %zu rows of %zu pixels cannot be drawn\n", rows, options.width);
      return 2;
    }
    PathFlattener flattener(filler, matrix, options.tolerance);
    // Curves that lie wholly outside this strip need no cutting up.
    flattener.KeepTo({.x = 0, .y = pixels(top)}, {.x = pixels(options.width), .y = pixels(top + rows)});
    SvgPathReader reader(flattener);
    reader.Feed(data);
    status = reader.Finish();
    filler.Fill(options.rule, options.gray, options.opacity);
    clamped = clamped || reader.Clamped() || flattener.Clamped();
    errorOffset = reader.ErrorOffset();
    commands = reader.Commands();
    lines += flattener.Lines();
    cellsTouched += filler.CellsTouched();
    ++strips;
  }

  if (!options.out.empty()) {
    std::ofstream output(options.out, std::ios::binary);
    output << "P5\n" << options.width << " " << options.height << "\n255\n";
    output.write(reinterpret_cast<const char*>(picture.data()),  // NOLINT(*-reinterpret-cast): bytes, as characters.
                 static_cast<std::streamsize>(picture.size()));
    if (!output) {
      std::fprintf(stderr, "error: cannot write %s\n", options.out.c_str());
      return 1;
    }
  }
  if (clamped) {
    std::fprintf(stderr, "a number was too large and was held at the largest that fits\n");
  }
  std::fprintf(stderr,
               "%zu by %zu in %zu %s of %zu rows: %u commands, %llu lines and %llu cells added to in all, "
               "%zu bytes of cells\n",
               options.width, options.height, strips, strips == 1 ? "strip" : "strips", stripRows,
               static_cast<unsigned>(commands), static_cast<unsigned long long>(lines),
               static_cast<unsigned long long>(cellsTouched), cells.size() * sizeof(std::int16_t));
  if (status != SvgPathStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s at byte %u\n", options.file.c_str(), bn::draw::SvgPathStatusName(status),
                 static_cast<unsigned>(errorOffset));
    return 1;
  }
  return 0;
}

// A length as it is said in a line of summary: as few figures as tell it.
std::string Brief(Fixed value) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%g", static_cast<double>(value) / bn::draw::kFixedOne);
  return text.data();
}

// How many pixels a length comes to, rounded up, and one at the least.
std::size_t PixelsFor(std::int64_t length) {
  return static_cast<std::size_t>(std::max<std::int64_t>((length + bn::draw::kFixedOne - 1) / bn::draw::kFixedOne, 1));
}

int Draw(const Options& options) {
  std::string data;
  if (!Read(options.file, data)) {
    return 1;
  }
  const std::size_t piece = options.pieces == 0 ? 1024 : options.pieces;
  const Fixed tolerance = options.toleranceGiven ? options.tolerance : SvgRenderer::kDefaultTolerance;
  const auto pixels = [](std::size_t count) { return static_cast<Fixed>(count * bn::draw::kFixedOne); };
  // Gives the renderer the file a piece at a time: all of it, or only as
  // far as the tag that says how large the drawing is.
  const auto read = [&data, piece](SvgRenderer& renderer, bool onlyTheSize) {
    for (std::size_t at = 0; at < data.size(); at += piece) {
      std::string_view bytes(data.data() + at, std::min(piece, data.size() - at));
      while (!bytes.empty()) {
        if (onlyTheSize && renderer.Size().known) {
          return;
        }
        bytes.remove_prefix(renderer.Step(bytes));
      }
    }
  };
  const auto failed = [&options](const SvgRenderer& renderer, SvgStatus status) {
    if (status == SvgStatus::kMalformed) {
      std::fprintf(stderr, "error: %s: %s at byte %u\n", options.file.c_str(), bn::draw::SvgStatusName(status),
                   static_cast<unsigned>(renderer.ErrorOffset()));
    } else {
      std::fprintf(stderr, "error: %s: %s\n", options.file.c_str(), bn::draw::SvgStatusName(status));
    }
    return 1;
  };

  // First the drawing's size, which its first tag says, and from that the
  // picture's.
  SvgRenderer renderer;
  std::vector<std::uint8_t> definitions(options.definitions);
  renderer.KeepDefinitionsIn(definitions);
  renderer.Begin(nullptr, 0, 0);
  read(renderer, true);
  if (!renderer.Size().known) {
    const SvgStatus status = renderer.Finish();
    return failed(renderer, status == SvgStatus::kOk ? SvgStatus::kNotSvg : status);
  }
  Fixed naturalWidth = 0;
  Fixed naturalHeight = 0;
  renderer.Size().Natural(naturalWidth, naturalHeight);
  if (naturalWidth <= 0 || naturalHeight <= 0) {
    std::fprintf(stderr, "error: %s: a drawing of no size\n", options.file.c_str());
    return 1;
  }
  std::size_t width = options.width;
  std::size_t height = options.height;
  if (width == 0 && height == 0) {
    width = PixelsFor(naturalWidth);
    height = PixelsFor(naturalHeight);
  } else if (height == 0) {
    height = PixelsFor((std::int64_t{pixels(width)} * naturalHeight) / naturalWidth);
  } else if (width == 0) {
    width = PixelsFor((std::int64_t{pixels(height)} * naturalWidth) / naturalHeight);
  }
  if (width > bn::draw::Filler::kMostWidth || height > bn::draw::Filler::kMostRows) {
    std::fprintf(stderr, "error: %s: a picture of %zu by %zu is too large to draw\n", options.file.c_str(), width,
                 height);
    return 1;
  }

  // The picture is whole here so that it can be written out. The firmware
  // would hold one strip of it at a time and hand each on as it was done.
  const std::size_t stripRows = options.rows == 0 ? height : std::min(options.rows, height);
  std::vector<std::uint8_t> picture(width * height, 255);
  std::vector<std::int16_t> cells(bn::draw::Filler::CellsFor(width, stripRows));
  SvgStatus status = SvgStatus::kOk;
  std::uint64_t lines = 0;
  std::size_t strips = 0;
  for (std::size_t top = 0; top < height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, height - top);
    bn::draw::Filler filler;
    if (!filler.Begin(std::span(picture).subspan(top * width, rows * width), width, static_cast<std::int32_t>(top),
                      cells)) {
      std::fprintf(stderr, "error: a strip of %zu rows of %zu pixels cannot be drawn\n", rows, width);
      return 2;
    }
    renderer.Begin(&filler, pixels(width), pixels(height), tolerance);
    read(renderer, false);
    status = renderer.Finish();
    lines += renderer.Report().lines;
    ++strips;
  }

  if (!options.out.empty()) {
    std::ofstream output(options.out, std::ios::binary);
    output << "P5\n" << width << " " << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(picture.data()),  // NOLINT(*-reinterpret-cast): bytes, as characters.
                 static_cast<std::streamsize>(picture.size()));
    if (!output) {
      std::fprintf(stderr, "error: cannot write %s\n", options.out.c_str());
      return 1;
    }
  }
  const bn::draw::SvgReport& report = renderer.Report();
  std::fprintf(stderr,
               "a drawing of %s by %s, as %zu by %zu in %zu %s of %zu rows: %u elements, %u shapes filled, "
               "%u stroked, %u commands, %llu lines in all, %zu bytes of cells\n",
               Brief(naturalWidth).c_str(), Brief(naturalHeight).c_str(), width, height, strips,
               strips == 1 ? "strip" : "strips", stripRows, static_cast<unsigned>(report.elements),
               static_cast<unsigned>(report.shapes), static_cast<unsigned>(report.strokes),
               static_cast<unsigned>(report.commands), static_cast<unsigned long long>(lines),
               cells.size() * sizeof(std::int16_t));
  if (report.kept != 0) {
    std::fprintf(stderr, "definitions: %u bytes kept of %zu, and %u gone through again in a strip\n",
                 static_cast<unsigned>(report.kept), definitions.size(), static_cast<unsigned>(report.reused));
  }
  if (report.rules != 0 || report.gradients != 0) {
    std::fprintf(stderr, "style: %u rules of a style sheet and %u linear gradients kept\n",
                 static_cast<unsigned>(report.rules), static_cast<unsigned>(report.gradients));
  }
  if (report.leftOut != 0) {
    std::string list;
    for (unsigned bit = 0; bit < 32; ++bit) {
      const auto what = static_cast<bn::draw::SvgLeftOut>(1U << bit);
      if (report.Has(what)) {
        list += (list.empty() ? "" : ", ") + std::string(bn::draw::SvgLeftOutName(what));
      }
    }
    std::fprintf(stderr, "left out, or not drawn as written: %s\n", list.c_str());
  }
  if (status != SvgStatus::kOk) {
    return failed(renderer, status);
  }
  return 0;
}

// A whole number from nothing up to `most`, written out in full. False if
// `text` is not one.
bool Whole(std::string_view text, std::size_t most, std::size_t& value) {
  if (text.empty() || text.size() > 9) {
    return false;
  }
  std::size_t number = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    number = (number * 10) + static_cast<std::size_t>(c - '0');
  }
  value = number;
  return number <= most;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view command = argc > 1 ? argv[1] : "";
  if (argc < 3 || (command != "path" && command != "fill" && command != "draw")) {
    PrintUsage(argv[0]);
    return 2;
  }
  const bool filling = command == "fill";
  const bool drawing = command == "draw";
  const bool listing = command == "path";
  Options options;
  options.file = argv[2];
  for (int at = 3; at < argc; ++at) {
    const std::string_view option = argv[at];
    const char* value = at + 1 < argc ? argv[at + 1] : nullptr;
    std::size_t whole = 0;
    if (option == "--count" && listing) {
      options.count = true;
    } else if (option == "--size" && filling && value) {
      const std::string_view size = value;
      const std::size_t by = size.find('x');
      if (by == std::string_view::npos || !Whole(size.substr(0, by), bn::draw::Filler::kMostWidth, options.width) ||
          !Whole(size.substr(by + 1), bn::draw::Filler::kMostRows, options.height)) {
        PrintUsage(argv[0]);
        return 2;
      }
      ++at;
    } else if (option == "--rule" && filling && value && std::string_view(value) == "nonzero") {
      options.rule = bn::draw::FillRule::kNonZero;
      ++at;
    } else if (option == "--rule" && filling && value && std::string_view(value) == "evenodd") {
      options.rule = bn::draw::FillRule::kEvenOdd;
      ++at;
    } else if (option == "--gray" && filling && value && Whole(value, 255, whole)) {
      options.gray = static_cast<std::uint8_t>(whole);
      ++at;
    } else if (option == "--opacity" && filling && value && Whole(value, 255, whole)) {
      options.opacity = static_cast<std::uint8_t>(whole);
      ++at;
    } else if (option == "--rows" && !listing && value && Whole(value, bn::draw::Filler::kMostRows, whole) &&
               whole > 0) {
      options.rows = whole;
      ++at;
    } else if (option == "--out" && !listing && value) {
      options.out = value;
      ++at;
    } else if (option == "--width" && drawing && value && Whole(value, bn::draw::Filler::kMostWidth, whole) &&
               whole > 0) {
      options.width = whole;
      ++at;
    } else if (option == "--height" && drawing && value && Whole(value, bn::draw::Filler::kMostRows, whole) &&
               whole > 0) {
      options.height = whole;
      ++at;
    } else if (option == "--definitions" && drawing && value && Whole(value, 16777216, whole)) {
      options.definitions = whole;
      ++at;
    } else if (option == "--transform" && !drawing && value) {
      options.transform = value;
      ++at;
    } else if (option == "--tolerance" && value) {
      bn::draw::Number number = 0;
      std::size_t end = 0;
      bool clamped = false;
      const std::string_view text = value;
      if (!bn::draw::ScanNumber(text, end, number, clamped) || end != text.size() || number <= 0) {
        PrintUsage(argv[0]);
        return 2;
      }
      options.tolerance = bn::draw::FixedOf(number, clamped);
      options.toleranceGiven = true;
      ++at;
    } else if (option == "--pieces" && !filling && value) {
      char* end = nullptr;
      const unsigned long long asked = std::strtoull(value, &end, 10);
      if (end == nullptr || *end != '\0' || asked == 0 || asked > (1ULL << 30U)) {
        PrintUsage(argv[0]);
        return 2;
      }
      options.pieces = static_cast<std::size_t>(asked);
      ++at;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (listing) {
    return Path(options);
  }
  if (drawing) {
    return Draw(options);
  }
  if (options.width == 0 || options.height == 0) {
    PrintUsage(argv[0]);
    return 2;
  }
  return Fill(options);
}
