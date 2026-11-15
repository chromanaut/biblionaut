// bn-svg: reads the parts of an SVG drawing with the code the firmware
// draws one with, and says what it made of them.
//
// So far it reads path data, the `d` of a `<path>`: each command it holds,
// and the straight lines each becomes once it has been put through a
// transform and its curves have been flattened.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_number.h"
#include "draw/svg_path.h"
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

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s path FILE [--transform TEXT] [--tolerance PIXELS] [--pieces BYTES] [--count]\n"
               "\n"
               "Reads SVG path data the way the firmware does.\n"
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
               "Ends with 0 if the data was read to its end, 1 if it is not path data from some\n"
               "point on (what came before that is still printed) or FILE cannot be read.\n",
               program);
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
  std::size_t pieces = 0;
  bool count = false;
};

int Path(const Options& options) {
  std::ifstream input(options.file, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "error: cannot read %s\n", options.file.c_str());
    return 1;
  }
  const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::strcmp(argv[1], "path") != 0) {
    PrintUsage(argv[0]);
    return 2;
  }
  Options options;
  options.file = argv[2];
  for (int at = 3; at < argc; ++at) {
    const std::string_view option = argv[at];
    const char* value = at + 1 < argc ? argv[at + 1] : nullptr;
    if (option == "--count") {
      options.count = true;
    } else if (option == "--transform" && value) {
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
      ++at;
    } else if (option == "--pieces" && value) {
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
  return Path(options);
}
