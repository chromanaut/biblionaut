#include "draw/svg_render.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/svg_transform.h"

// The renderer is tried on drawings written here, small enough that what
// each pixel should be can be said by hand, or set beside another drawing
// that has to come out the same: a circle beside the four arcs that make
// one, a path beside the same data given straight to the flattener and the
// filler.

namespace {

using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::PathFlattener;
using bn::draw::SvgLeftOut;
using bn::draw::SvgPathReader;
using bn::draw::SvgRenderer;
using bn::draw::SvgReport;
using bn::draw::SvgSize;
using bn::draw::SvgStatus;

constexpr std::uint8_t kWhite = 255;
constexpr std::uint8_t kBlack = 0;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

constexpr Fixed Pixels(std::size_t count) { return static_cast<Fixed>(count * kFixedOne); }

// What came of drawing a file.
struct Drawing {
  std::size_t width = 0;
  std::size_t height = 0;
  std::vector<std::uint8_t> gray;
  SvgStatus status = SvgStatus::kOk;
  SvgReport report;
  std::uint32_t errorOffset = 0;
  std::size_t calls = 0;

  std::uint8_t At(std::size_t x, std::size_t y) const { return gray[(y * width) + x]; }
  // Whether the pixels from (left, top) up to (right, bottom) are all
  // `inside`, and all the rest `outside`.
  bool IsBox(std::size_t left, std::size_t top, std::size_t right, std::size_t bottom, std::uint8_t inside,
             std::uint8_t outside = kWhite) const {
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        const bool in = x >= left && x < right && y >= top && y < bottom;
        if (At(x, y) != (in ? inside : outside)) {
          ADD_FAILURE() << "the pixel at " << x << "," << y << " is " << static_cast<int>(At(x, y));
          return false;
        }
      }
    }
    return true;
  }
  // How much of the picture is covered, in pixels, if black was drawn on
  // white.
  double Covered() const {
    double covered = 0;
    for (const std::uint8_t pixel : gray) {
      covered += (255.0 - pixel) / 255.0;
    }
    return covered;
  }
};

// Draws `file` in a picture that starts white, in strips of `stripRows`
// rows, giving it to the renderer `piece` bytes at a time.
Drawing Draw(std::string_view file, std::size_t width, std::size_t height, std::size_t stripRows = 0,
             std::size_t piece = SIZE_MAX) {
  Drawing drawing;
  drawing.width = width;
  drawing.height = height;
  drawing.gray.assign(width * height, kWhite);
  stripRows = stripRows == 0 ? height : stripRows;
  std::vector<std::int16_t> cells(Filler::CellsFor(width, stripRows));
  SvgRenderer renderer;
  for (std::size_t top = 0; top < height; top += stripRows) {
    const std::size_t rows = std::min(stripRows, height - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(drawing.gray).subspan(top * width, rows * width), width,
                             static_cast<std::int32_t>(top), cells));
    renderer.Begin(&filler, Pixels(width), Pixels(height));
    for (std::size_t at = 0; at < file.size();) {
      std::string_view bytes = file.substr(at, piece);
      at += bytes.size();
      while (!bytes.empty()) {
        const std::size_t taken = renderer.Step(bytes);
        EXPECT_LE(taken, bytes.size());
        bytes.remove_prefix(std::min(taken, bytes.size()));
        ++drawing.calls;
      }
    }
    drawing.status = renderer.Finish();
    drawing.report = renderer.Report();
    drawing.errorOffset = renderer.ErrorOffset();
  }
  return drawing;
}

// A file whose drawing is `width` by `height` and holds `body`.
std::string Svg(std::size_t width, std::size_t height, std::string_view body) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(width) + "' height='" +
         std::to_string(height) + "'>" + std::string(body) + "</svg>";
}

// `body` drawn in a picture the size the drawing says it is.
Drawing DrawBody(std::string_view body, std::size_t width = 40, std::size_t height = 20) {
  const Drawing drawing = Draw(Svg(width, height, body), width, height);
  EXPECT_EQ(drawing.status, SvgStatus::kOk) << body;
  return drawing;
}

std::uint32_t Bits(std::initializer_list<SvgLeftOut> all) {
  std::uint32_t bits = 0;
  for (const SvgLeftOut what : all) {
    bits |= static_cast<std::uint32_t>(what);
  }
  return bits;
}

// Path data given straight to the flattener and the filler.
std::vector<std::uint8_t> Filled(std::string_view data, std::string_view transform, FillRule rule, std::uint8_t gray,
                                 std::size_t width, std::size_t height) {
  std::vector<std::uint8_t> picture(width * height, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(width, height));
  Filler filler;
  EXPECT_TRUE(filler.Begin(picture, width, 0, cells));
  Matrix matrix;
  bool clamped = false;
  EXPECT_TRUE(bn::draw::ParseSvgTransform(transform, matrix, clamped));
  PathFlattener flattener(filler, matrix, SvgRenderer::kDefaultTolerance);
  SvgPathReader reader(flattener);
  reader.Feed(data);
  reader.Finish();
  filler.Fill(rule, gray);
  return picture;
}

// ---- Where a drawing goes ----

TEST(SvgRender, DrawsAShapeWhereItSaysInTheGrayItSays) {
  const Drawing drawing = DrawBody("<rect x='2' y='3' width='5' height='4' fill='#808080'/>", 20, 10);
  EXPECT_TRUE(drawing.IsBox(2, 3, 7, 7, 128));
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.elements, 2u);
  EXPECT_EQ(drawing.report.shapes, 1u);
  EXPECT_EQ(drawing.report.deepest, 1u);
  // With nothing said of its colour, a shape is black.
  EXPECT_TRUE(DrawBody("<rect x='2' y='3' width='5' height='4'/>", 20, 10).IsBox(2, 3, 7, 7, kBlack));
}

TEST(SvgRender, FitsItsViewBoxToThePicture) {
  // A square drawing, all black, in a picture twice as wide as it is high.
  const auto drawn = [](std::string_view attributes) {
    const std::string file =
        "<svg viewBox='0 0 10 10' " + std::string(attributes) + "><rect width='10' height='10'/></svg>";
    const Drawing drawing = Draw(file, 40, 20);
    EXPECT_EQ(drawing.status, SvgStatus::kOk);
    return drawing;
  };
  // Unless told otherwise it is as large as fits, in the middle.
  EXPECT_TRUE(drawn("").IsBox(10, 0, 30, 20, kBlack));
  EXPECT_TRUE(drawn("preserveAspectRatio='xMidYMid meet'").IsBox(10, 0, 30, 20, kBlack));
  EXPECT_TRUE(drawn("preserveAspectRatio='xMinYMin'").IsBox(0, 0, 20, 20, kBlack));
  EXPECT_TRUE(drawn("preserveAspectRatio='xMaxYMax'").IsBox(20, 0, 40, 20, kBlack));
  EXPECT_TRUE(drawn("preserveAspectRatio='none'").IsBox(0, 0, 40, 20, kBlack));
  EXPECT_TRUE(drawn("preserveAspectRatio='xMidYMid slice'").IsBox(0, 0, 40, 20, kBlack));
  // What it says its own size is makes no difference to where it goes.
  EXPECT_TRUE(drawn("width='500' height='7'").IsBox(10, 0, 30, 20, kBlack));

  // A view box need not begin at nothing.
  const Drawing moved =
      Draw("<svg viewBox='100 -50 20 10'><rect x='105' y='-48' width='10' height='5'/></svg>", 40, 20);
  EXPECT_TRUE(moved.IsBox(10, 4, 30, 14, kBlack));
}

TEST(SvgRender, WithNoViewBoxItsOwnSizeIsStretchedToThePicture) {
  const Drawing drawing = Draw("<svg width='10' height='10'><rect width='5' height='5'/></svg>", 40, 20);
  EXPECT_TRUE(drawing.IsBox(0, 0, 20, 10, kBlack));
  // In units other than pixels, too: an inch is 96 of them, and a point a
  // 72nd of an inch.
  const Drawing inches = Draw("<svg width='2in' height='1in'><rect width='1in' height='36pt'/></svg>", 192, 96);
  EXPECT_TRUE(inches.IsBox(0, 0, 96, 48, kBlack));
}

TEST(SvgRender, AGroupMovesAndPaintsWhatIsInIt) {
  const Drawing drawing = DrawBody(
      "<g transform='translate(4 2)' fill='#404040'>"
      "  <rect width='2' height='2'/>"
      "  <g transform='scale(2)'><rect x='2' y='1' width='1' height='1' fill='#c0c0c0'/></g>"
      "  <a><rect x='10' width='2' height='2' fill='#fff' style='fill: inherit'/></a>"
      "</g>"
      "<rect y='8' width='2' height='2'/>");
  EXPECT_EQ(drawing.At(4, 2), 0x40);
  EXPECT_EQ(drawing.At(5, 3), 0x40);
  // Moved by the outer group after being doubled by the inner one.
  EXPECT_EQ(drawing.At(8, 4), 0xC0);
  EXPECT_EQ(drawing.At(9, 5), 0xC0);
  EXPECT_EQ(drawing.At(10, 4), kWhite);
  EXPECT_EQ(drawing.At(14, 2), 0x40);
  // Outside the group, its paint and its move are over.
  EXPECT_EQ(drawing.At(0, 8), kBlack);
  EXPECT_EQ(drawing.At(4, 10), kWhite);
  EXPECT_EQ(drawing.report.shapes, 4u);
  EXPECT_EQ(drawing.report.deepest, 3u);
  EXPECT_EQ(drawing.report.leftOut, 0u);
}

TEST(SvgRender, CurrentColorIsWhateverColorIs) {
  const Drawing drawing = DrawBody(
      "<g color='#808080'><rect width='4' height='4' fill='currentColor'/>"
      "<rect x='10' width='4' height='4' fill='currentColor' color='#404040'/></g>"
      "<rect x='20' width='4' height='4' fill='currentColor'/>");
  EXPECT_EQ(drawing.At(1, 1), 128);
  EXPECT_EQ(drawing.At(11, 1), 0x40);
  EXPECT_EQ(drawing.At(21, 1), kBlack);
}

TEST(SvgRender, WhatStyleSaysWinsWhereverItIsWritten) {
  EXPECT_EQ(DrawBody("<rect width='4' height='4' fill='#000' style='fill:#808080'/>").At(1, 1), 128);
  EXPECT_EQ(DrawBody("<rect width='4' height='4' style='fill:#808080' fill='#000'/>").At(1, 1), 128);
  // Several things, with space, a semicolon at the end, a property it does
  // not know and one that is only for this renderer's betters.
  const Drawing drawing = DrawBody(
      "<path d='M0 0h10v10h-10zM2 2h6v6h-6z' fill-rule='nonzero'"
      " style=' fill : rgb(64, 64, 64) !important ;font-family:\"a;b\"; fill-rule:evenodd;; -inkscape-thing: x(1;2) "
      ";'/>");
  EXPECT_EQ(drawing.At(1, 1), 0x40);
  EXPECT_EQ(drawing.At(5, 5), kWhite);
  EXPECT_EQ(drawing.report.leftOut, 0u);
  // Style on a group is handed on like anything else.
  EXPECT_EQ(DrawBody("<g style='fill:#808080'><rect width='4' height='4'/></g>").At(1, 1), 128);
}

// ---- Shapes ----

TEST(SvgRender, EachShapeIsTheOutlineItStandsFor) {
  const auto same = [](std::string_view shape, std::string_view data) {
    const std::string open = "<g transform='translate(1.5 0.25) scale(2.5)' fill='#333'>";
    const Drawing asShape = DrawBody(open + std::string(shape) + "</g>", 48, 30);
    const Drawing asPath = DrawBody(open + "<path d='" + std::string(data) + "'/></g>", 48, 30);
    EXPECT_EQ(asShape.gray, asPath.gray) << shape;
    EXPECT_GT(asPath.Covered(), 20.0) << shape;
    EXPECT_EQ(asShape.report.shapes, 1u) << shape;
    EXPECT_EQ(asShape.report.leftOut, 0u) << shape;
  };
  same("<rect x='3' y='2' width='10' height='6'/>", "M3 2H13V8H3Z");
  // Rounded corners: a quarter of an ellipse at each.
  same("<rect x='3' y='2' width='10' height='6' rx='2' ry='1'/>",
       "M5 2H11A2 1 0 0 1 13 3V7A2 1 0 0 1 11 8H5A2 1 0 0 1 3 7V3A2 1 0 0 1 5 2Z");
  // A radius written one way only is the same the other way.
  same("<rect x='3' y='2' width='10' height='6' rx='2'/>",
       "M5 2H11A2 2 0 0 1 13 4V6A2 2 0 0 1 11 8H5A2 2 0 0 1 3 6V4A2 2 0 0 1 5 2Z");
  same("<rect x='3' y='2' width='10' height='6' ry='1.5'/>",
       "M4.5 2H11.5A1.5 1.5 0 0 1 13 3.5V6.5A1.5 1.5 0 0 1 11.5 8H4.5A1.5 1.5 0 0 1 3 6.5V3.5A1.5 1.5 0 0 1 4.5 2Z");
  // And neither is more than half the side it is on.
  same("<rect x='3' y='2' width='10' height='6' rx='50'/>",
       "M8 2H8A5 3 0 0 1 13 5V5A5 3 0 0 1 8 8H8A5 3 0 0 1 3 5V5A5 3 0 0 1 8 2Z");
  same("<circle cx='8' cy='5' r='4'/>", "M12 5A4 4 0 0 1 8 9A4 4 0 0 1 4 5A4 4 0 0 1 8 1A4 4 0 0 1 12 5Z");
  same("<ellipse cx='9' cy='5' rx='6' ry='3'/>", "M15 5A6 3 0 0 1 9 8A6 3 0 0 1 3 5A6 3 0 0 1 9 2A6 3 0 0 1 15 5Z");
  same("<polygon points='1,1 9,2 5,8'/>", "M1,1 9,2 5,8Z");
  same("<polygon points=' 1 1 9 2\n5 8 '/>", "M1,1 9,2 5,8Z");
  // An outline left open is filled as if it were closed.
  same("<polyline points='1,1 9,2 5,8 12,9'/>", "M1,1 9,2 5,8 12,9");
}

TEST(SvgRender, AShapeCoversWhatItsGeometrySays) {
  // A circle of radius 10 covers pi times a hundred pixels, and an ellipse
  // pi times its two radii, to within what the lines along its edge leave
  // out.
  const double circle = DrawBody("<circle cx='15' cy='12' r='10'/>", 30, 24).Covered();
  EXPECT_NEAR(circle, std::numbers::pi * 100, 1.5);
  const double ellipse = DrawBody("<ellipse cx='20' cy='10' rx='16' ry='7'/>", 40, 20).Covered();
  EXPECT_NEAR(ellipse, std::numbers::pi * 16 * 7, 1.5);
  // A rectangle with rounded corners is short of the whole by what is
  // outside the four quarters of a circle.
  const double rounded = DrawBody("<rect x='2' y='2' width='30' height='16' rx='4'/>", 40, 20).Covered();
  EXPECT_NEAR(rounded, (30 * 16) - ((4 - std::numbers::pi) * 16), 1.0);
  // A triangle is half its base times its height.
  EXPECT_NEAR(DrawBody("<polygon points='2,2 22,2 2,18'/>", 40, 20).Covered(), 160, 0.5);
}

TEST(SvgRender, AShapeWithNoSizeIsNotDrawn) {
  for (const std::string_view shape :
       {"<rect x='2' y='2' width='0' height='5'/>", "<rect x='2' y='2' width='5' height='-5'/>", "<rect x='2' y='2'/>",
        "<circle cx='5' cy='5' r='0'/>", "<circle cx='5' cy='5'/>", "<ellipse cx='5' cy='5' rx='4' ry='0'/>",
        "<ellipse cx='5' cy='5' rx='-4' ry='3'/>", "<line x1='0' y1='0' x2='20' y2='10'/>", "<path/>", "<path d=''/>",
        "<path d='  '/>", "<polygon/>", "<polygon points=''/>", "<polyline points=' '/>"}) {
    const Drawing drawing = DrawBody(shape);
    EXPECT_EQ(drawing.Covered(), 0.0) << shape;
    EXPECT_EQ(drawing.report.shapes, 0u) << shape;
    EXPECT_EQ(drawing.report.leftOut, 0u) << shape;
  }
}

TEST(SvgRender, APathGoesToTheFillerAsItsDataSays) {
  const std::string_view data =
      "M5 3C12-4 30 28 38 6S20 2 14 17Q2 24 6 8T19 14M24 4a6 5 30 1 1 8 9 3 3 0 0 0-8-9zM30 12h6v6h-6z";
  for (const std::string_view transform : {"", "translate(3 2) scale(1.5)", "rotate(20 20 10) skewX(10)"}) {
    for (const bool evenOdd : {false, true}) {
      const std::string file = Svg(48, 30,
                                   "<path fill='#404040' transform='" + std::string(transform) + "' fill-rule='" +
                                       (evenOdd ? "evenodd" : "nonzero") + "' d='" + std::string(data) + "'/>");
      const Drawing drawing = Draw(file, 48, 30);
      EXPECT_EQ(drawing.gray, Filled(data, transform, evenOdd ? FillRule::kEvenOdd : FillRule::kNonZero, 0x40, 48, 30))
          << transform;
      EXPECT_EQ(drawing.report.leftOut, 0u);
      EXPECT_GT(drawing.Covered(), 30.0);
    }
  }
}

// ---- Paint ----

TEST(SvgRender, FillsByEitherRuleAndAsSolidAsItIsTold) {
  const std::string_view rings = "d='M0 0h20v20h-20zM4 4h12v12h-12zM8 8h4v4h-4z'";
  const Drawing nonZero = DrawBody("<path " + std::string(rings) + "/>", 20, 20);
  EXPECT_TRUE(nonZero.IsBox(0, 0, 20, 20, kBlack));
  const Drawing evenOdd = DrawBody("<path fill-rule='evenodd' " + std::string(rings) + "/>", 20, 20);
  EXPECT_EQ(evenOdd.At(1, 1), kBlack);
  EXPECT_EQ(evenOdd.At(5, 5), kWhite);
  EXPECT_EQ(evenOdd.At(9, 9), kBlack);

  // Half as solid, black on white comes out half way.
  EXPECT_NEAR(DrawBody("<rect width='4' height='4' opacity='0.5'/>").At(1, 1), 128, 1);
  EXPECT_NEAR(DrawBody("<rect width='4' height='4' fill-opacity='50%'/>").At(1, 1), 128, 1);
  EXPECT_NEAR(DrawBody("<rect width='4' height='4' fill='rgba(0,0,0,0.5)'/>").At(1, 1), 128, 1);
  // The three multiply.
  EXPECT_NEAR(DrawBody("<rect width='4' height='4' opacity='.5' fill-opacity='.5' fill='#00000080'/>").At(1, 1), 223,
              1);
  // One shape over another shows through as much as it is not solid.
  const Drawing over = DrawBody(
      "<rect width='8' height='4' fill='#404040'/>"
      "<rect x='4' width='8' height='4' fill='#c0c0c0' fill-opacity='0.5'/>");
  EXPECT_EQ(over.At(1, 1), 0x40);
  EXPECT_NEAR(over.At(5, 1), 0x80, 1);
  EXPECT_NEAR(over.At(9, 1), 223, 1);

  // Fill opacity is handed on to what is inside a group. Opacity is the
  // group's own, and is given to each thing in it, which is said.
  const Drawing handedOn = DrawBody("<g fill-opacity='.5'><rect width='4' height='4'/></g>");
  EXPECT_NEAR(handedOn.At(1, 1), 128, 1);
  EXPECT_EQ(handedOn.report.leftOut, 0u);
  const Drawing group = DrawBody("<g opacity='.5'><g opacity='.5'><rect width='4' height='4'/></g></g>");
  EXPECT_NEAR(group.At(1, 1), 191, 1);
  EXPECT_EQ(group.report.leftOut, Bits({SvgLeftOut::kGroupOpacity}));
}

TEST(SvgRender, WhatIsHiddenIsNotDrawn) {
  for (const std::string_view body :
       {"<rect width='9' height='9' display='none'/>", "<rect width='9' height='9' style='display:none'/>",
        "<g display='none'><rect width='9' height='9'/><g display='inline'><rect width='9' height='9'/></g></g>",
        "<rect width='9' height='9' visibility='hidden'/>", "<g visibility='collapse'><rect width='9' height='9'/></g>",
        "<rect width='9' height='9' fill='none'/>", "<g fill='none'><rect width='9' height='9'/></g>",
        "<rect width='9' height='9' opacity='0'/>", "<rect width='9' height='9' fill-opacity='0'/>",
        "<rect width='9' height='9' fill='transparent'/>", "<g opacity='0'><rect width='9' height='9'/></g>",
        "<path d='M0 0h9v9h-9z' fill='none'/>", "<path fill='none' d='M0 0h9v9h-9z'/>",
        "<defs><rect width='9' height='9'/></defs>", "<symbol><rect width='9' height='9'/></symbol>",
        "<clipPath><rect width='9' height='9'/></clipPath>",
        "<pattern><g><rect width='9' height='9'/></g></pattern>"}) {
    const Drawing drawing = DrawBody(body);
    EXPECT_EQ(drawing.Covered(), 0.0) << body;
    EXPECT_EQ(drawing.report.shapes, 0u) << body;
    EXPECT_EQ(drawing.report.leftOut & ~Bits({SvgLeftOut::kGroupOpacity}), 0u) << body;
  }
  // Something inside what is hidden can be made to show again, unless it
  // was hidden by `display`.
  const Drawing shown = DrawBody(
      "<g visibility='hidden'><rect width='4' height='4'/>"
      "<rect x='10' width='4' height='4' visibility='visible'/></g>");
  EXPECT_TRUE(shown.IsBox(10, 0, 14, 4, kBlack));
  // What comes after something hidden is drawn as ever.
  const Drawing after =
      DrawBody("<g display='none'><g><rect width='9' height='9'/></g></g><rect width='4' height='4'/>");
  EXPECT_TRUE(after.IsBox(0, 0, 4, 4, kBlack));
  // Whatever is inside a shape is passed over, and what follows is not.
  const Drawing inside = DrawBody(
      "<rect width='4' height='4'><title>A square</title><rect width='20' height='20'/>"
      "</rect><rect x='10' width='4' height='4'><desc>Another</desc></rect>");
  EXPECT_EQ(inside.report.shapes, 2u);
  EXPECT_EQ(inside.Covered(), 32.0);
}

// ---- Boxes within boxes ----

TEST(SvgRender, AnSvgInsideAnotherHasItsOwnPlaceAndViewBox) {
  // A view box of two by one, put where it is told and made ten times the
  // size.
  const Drawing boxed = DrawBody(
      "<svg x='10' y='5' width='20' height='10' viewBox='0 0 2 1'>"
      "<rect width='1' height='1'/></svg>");
  EXPECT_TRUE(boxed.IsBox(10, 5, 20, 15, kBlack));
  EXPECT_EQ(boxed.report.leftOut, Bits({SvgLeftOut::kViewportClip}));
  // With a view box of another shape, fitted as the outermost one is.
  const Drawing fitted = DrawBody(
      "<svg x='10' y='5' width='20' height='10' viewBox='0 0 1 1'>"
      "<rect width='1' height='1'/></svg>");
  EXPECT_TRUE(fitted.IsBox(15, 5, 25, 15, kBlack));
  const Drawing stretched = DrawBody(
      "<svg x='10' y='5' width='20' height='10' viewBox='0 0 1 1'"
      " preserveAspectRatio='none'><rect width='1' height='1'/></svg>");
  EXPECT_TRUE(stretched.IsBox(10, 5, 30, 15, kBlack));
  // With none it is only moved.
  const Drawing moved = DrawBody(
      "<g transform='translate(1 1)'><svg x='10' y='5' width='20' height='10'>"
      "<rect width='3' height='2'/></svg></g>");
  EXPECT_TRUE(moved.IsBox(11, 6, 14, 8, kBlack));
  // Left to itself it is the size of what it is in, and a percentage
  // inside it is of its own view box.
  const Drawing whole = DrawBody("<svg viewBox='0 0 4 2'><rect width='50%' height='50%'/></svg>");
  EXPECT_TRUE(whole.IsBox(0, 0, 20, 10, kBlack));
  // One with no size is not drawn, nor what is in it.
  for (const std::string_view attributes : {"width='0' height='5'", "width='5' height='5' viewBox='0 0 0 5'"}) {
    EXPECT_EQ(DrawBody("<svg " + std::string(attributes) + "><rect width='9' height='9'/></svg>").Covered(), 0.0);
  }
}

TEST(SvgRender, APercentageIsOfWhatTheDrawingIsSeenThrough) {
  const Drawing drawing =
      Draw("<svg viewBox='0 0 400 100'><rect x='25%' y='50%' width='50%' height='30%'/></svg>", 40, 10);
  EXPECT_TRUE(drawing.IsBox(10, 5, 30, 8, kBlack));
  // Of its own size, where it has no view box.
  EXPECT_TRUE(DrawBody("<rect x='10%' width='10%' height='100%'/>").IsBox(4, 0, 8, 20, kBlack));
  // A circle's radius is a percentage of something between the two: the
  // diagonal over the square root of two.
  const double radius = 0.25 * std::sqrt(((40.0 * 40.0) + (20.0 * 20.0)) / 2);
  EXPECT_NEAR(DrawBody("<circle cx='50%' cy='50%' r='25%'/>").Covered(), std::numbers::pi * radius * radius, 1.5);
}

// ---- What is left out ----

TEST(SvgRender, SaysWhatItLeftOut) {
  struct Case {
    std::string_view body;
    std::uint32_t leftOut;
    double covered;  // How many pixels are black all the same.
  };
  const Case cases[] = {
      {"<rect width='4' height='4'/>", 0, 16},
      {"<text x='5' y='5'>Words <tspan>and more</tspan></text><rect width='4' height='4'/>", Bits({SvgLeftOut::kText}),
       16},
      {"<image href='a.png' width='9' height='9'/>", Bits({SvgLeftOut::kImage}), 0},
      {"<defs><path id='a' d='M0 0h4v4z'/><text>x</text></defs><use href='#a'/>", Bits({SvgLeftOut::kUse}), 0},
      {"<style>rect { fill: none }</style><rect width='4' height='4'/>", Bits({SvgLeftOut::kStyleSheet}), 16},
      {"<rect width='4' height='4' stroke='#000'/>", Bits({SvgLeftOut::kStroke}), 16},
      {"<rect width='4' height='4' fill='none' style='stroke:red;stroke-width:2'/>", Bits({SvgLeftOut::kStroke}), 0},
      {"<g stroke='blue'><rect width='4' height='4'/></g>", Bits({SvgLeftOut::kStroke}), 16},
      {"<line x2='20' y2='10' stroke='#000'/>", Bits({SvgLeftOut::kStroke}), 0},
      // A stroke of no width, or of no paint, or on something hidden, is no stroke.
      {"<rect width='4' height='4' stroke='#000' stroke-width='0'/>", 0, 16},
      {"<g stroke='currentColor' stroke-width='0'><rect width='4' height='4'/></g>", 0, 16},
      {"<g stroke='#000'><rect width='4' height='4' stroke='none'/></g>", 0, 16},
      {"<g stroke='#000' stroke-width='0'><rect width='4' height='4' stroke-width='inherit'/></g>", 0, 16},
      {"<rect width='4' height='4' stroke='#000' display='none'/>", 0, 0},
      // A gradient is not drawn, unless something is written to stand in for it.
      {"<rect width='4' height='4' fill='url(#g)'/>", Bits({SvgLeftOut::kPaintServer}), 0},
      {"<rect width='4' height='4' fill='url(#g) #000'/>", Bits({SvgLeftOut::kPaintServer}), 16},
      {"<linearGradient id='g'><stop offset='0'/></linearGradient><rect width='4' height='4'/>", 0, 16},
      {"<rect width='4' height='4' clip-path='url(#c)'/>", Bits({SvgLeftOut::kClip}), 16},
      {"<g style='clip-path: url(#c)'><rect width='4' height='4'/></g>", Bits({SvgLeftOut::kClip}), 16},
      {"<rect width='4' height='4' mask='url(#m)'/>", Bits({SvgLeftOut::kMask}), 16},
      {"<rect width='4' height='4' filter='url(#f)'/>", Bits({SvgLeftOut::kFilter}), 16},
      {"<path d='M0 0h4v4z' marker-end='url(#m)'/>", Bits({SvgLeftOut::kMarker}), 8},
      {"<rect width='4' height='4' clip-path='none' mask='none' filter='none' marker='none'/>", 0, 16},
      {"<foreignObject width='9' height='9'><p>Words</p><rect width='4' height='4'/></foreignObject>",
       Bits({SvgLeftOut::kForeign}), 0},
      {"<rect width='4' height='4'><animate attributeName='x' to='9'/></rect>", Bits({SvgLeftOut::kAnimation}), 16},
      {"<script>draw()</script><rect width='4' height='4'/>", Bits({SvgLeftOut::kAnimation}), 16},
      {"<blink><rect width='4' height='4'/></blink>", Bits({SvgLeftOut::kUnknownElement}), 0},
      // What belongs to some other vocabulary is none of its business.
      {"<sodipodi:namedview pagecolor='#fff'/><metadata><rdf:RDF><cc:Work/></rdf:RDF></metadata><rect width='4' "
       "height='4'/>",
       0, 16},
      {"<switch><rect width='4' height='4'/></switch>", Bits({SvgLeftOut::kSwitch}), 16},
      {"<g opacity='0.9'><rect width='4' height='4'/></g>", Bits({SvgLeftOut::kGroupOpacity}), -1},
      {"<svg><rect width='4' height='4'/></svg>", Bits({SvgLeftOut::kViewportClip}), 16},
      // An attribute that cannot be read. One that says where a thing is
      // leaves it undrawn; one that says how it is painted leaves it as it
      // would have been.
      {"<g transform='rotate(bad)'><rect width='4' height='4'/></g>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='4' height='4' transform='matrix(1 2 3)'/>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='four' height='4'/>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='4' height='4' fill='blurple'/>", Bits({SvgLeftOut::kBadValue}), 16},
      {"<rect width='4' height='4' fill-rule='oddeven' fill-opacity='lots' style='opacity:some;color:#12;'/>",
       Bits({SvgLeftOut::kBadValue}), 16},
      {"<svg viewBox='0 0 4'><rect width='4' height='4'/></svg>",
       Bits({SvgLeftOut::kBadValue, SvgLeftOut::kViewportClip}), 16},
      {"<svg preserveAspectRatio='sideways'><rect width='4' height='4'/></svg>",
       Bits({SvgLeftOut::kBadValue, SvgLeftOut::kViewportClip}), 16},
      // Path data that goes wrong is drawn as far as it was right.
      {"<path d='M0 0h4v4h-4z M10 0h4v4 K'/>", Bits({SvgLeftOut::kBadPath}), 24},
      {"<polygon points='0,0 4,0 4,4 0,4 7'/>", Bits({SvgLeftOut::kBadPath}), 16},
      {"<path d='M0 0h99999999v4h-4z'/>", Bits({SvgLeftOut::kHeldNumber}), -1},
      {"<rect width='4' height='4' fill='&mine;#000'/>", Bits({SvgLeftOut::kUnknownEntity}), 16},
  };
  for (const Case& one : cases) {
    const Drawing drawing = DrawBody(one.body);
    EXPECT_EQ(drawing.report.leftOut, one.leftOut) << one.body;
    if (one.covered >= 0) {
      EXPECT_NEAR(drawing.Covered(), one.covered, 0.01) << one.body;
    }
  }

  // Each has a name of its own to be told by.
  std::vector<std::string> names;
  for (unsigned bit = 0; bit < 22; ++bit) {
    names.emplace_back(bn::draw::SvgLeftOutName(static_cast<SvgLeftOut>(1U << bit)));
    EXPECT_NE(names.back(), "unknown") << bit;
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
  EXPECT_STREQ(bn::draw::SvgLeftOutName(SvgLeftOut::kStroke), "a stroke");
}

TEST(SvgRender, AnAttributeTooLongToHoldIsNotRead) {
  const std::string spaces(300, ' ');
  // A transform: the thing is not drawn. A colour: it is, in what it had.
  const Drawing moved = DrawBody("<rect width='4' height='4' transform='" + spaces + "translate(5)'/>");
  EXPECT_EQ(moved.report.leftOut, Bits({SvgLeftOut::kBadValue}));
  EXPECT_EQ(moved.Covered(), 0.0);
  const Drawing painted = DrawBody("<rect width='4' height='4' fill='" + spaces + "#fff'/>");
  EXPECT_EQ(painted.report.leftOut, Bits({SvgLeftOut::kBadValue}));
  EXPECT_EQ(painted.Covered(), 16.0);
  // In `style` only the one thing that is too long is lost.
  const Drawing styled = DrawBody("<rect width='4' height='4' style='fill:" + spaces + "#fff;fill-opacity:0.5'/>");
  EXPECT_EQ(styled.report.leftOut, Bits({SvgLeftOut::kBadValue}));
  EXPECT_NEAR(styled.At(1, 1), 128, 1);
  // What it has no use for can be any length: a picture inside a cover is
  // megabytes of one attribute.
  const Drawing ignored = DrawBody("<rect width='4' height='4' id='" + std::string(100000, 'x') + "'/>");
  EXPECT_EQ(ignored.report.leftOut, 0u);
  EXPECT_EQ(ignored.Covered(), 16.0);
}

TEST(SvgRender, GroupsNestedTooDeepArePassedOver) {
  const auto nested = [](std::size_t groups) {
    std::string body;
    for (std::size_t i = 0; i < groups; ++i) {
      body += "<g>";
    }
    body += "<rect width='4' height='4'/>";
    for (std::size_t i = 0; i < groups; ++i) {
      body += "</g>";
    }
    return body + "<rect x='10' width='4' height='4'/>";
  };
  // The drawing's own element is one of the kDeepest that may be open.
  const Drawing fits = DrawBody(nested(SvgRenderer::kDeepest - 1));
  EXPECT_EQ(fits.report.leftOut, 0u);
  EXPECT_EQ(fits.report.deepest, SvgRenderer::kDeepest);
  EXPECT_EQ(fits.Covered(), 32.0);
  for (const std::size_t groups : {SvgRenderer::kDeepest, SvgRenderer::kDeepest + 1, std::size_t{500}}) {
    const Drawing drawing = DrawBody(nested(groups));
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kTooDeep})) << groups;
    // What is too deep is lost, and what follows it is not.
    EXPECT_TRUE(drawing.IsBox(10, 0, 14, 4, kBlack)) << groups;
  }
}

// ---- The order attributes come in ----

TEST(SvgRender, ATransformWrittenAfterPathDataIsUsedIfTheDataIsShort) {
  const std::string_view square = "M0 0h4v4h-4z";
  const Drawing before = DrawBody("<path transform='translate(10 2)' d='" + std::string(square) + "'/>");
  ASSERT_TRUE(before.IsBox(10, 2, 14, 6, kBlack));
  const Drawing after = DrawBody("<path d='" + std::string(square) + "' transform='translate(10 2)'/>");
  EXPECT_EQ(after.gray, before.gray);
  EXPECT_EQ(after.report.leftOut, 0u);

  // Data as long as can be held, and a byte longer.
  const std::string held = std::string(square) + std::string(SvgRenderer::kHeldPathData - square.size(), ' ');
  const Drawing longest = DrawBody("<path d='" + held + "' transform='translate(10 2)'/>");
  EXPECT_EQ(longest.gray, before.gray);
  EXPECT_EQ(longest.report.leftOut, 0u);
  const Drawing tooLong = DrawBody("<path d='" + held + " ' transform='translate(10 2)'/><rect width='2' height='2'/>");
  EXPECT_EQ(tooLong.report.leftOut, Bits({SvgLeftOut::kLateTransform}));
  // The shape is left out, not drawn in the wrong place; and the next is
  // drawn.
  EXPECT_TRUE(tooLong.IsBox(0, 0, 2, 2, kBlack));
  EXPECT_EQ(tooLong.report.shapes, 1u);

  // However long the data, a transform written before it is used, and so
  // is one on a group.
  EXPECT_EQ(DrawBody("<path transform='translate(10 2)' d='" + held + held + "'/>").gray, before.gray);
  EXPECT_EQ(DrawBody("<g transform='translate(10 2)'><path d='" + held + held + "'/></g>").gray, before.gray);
  // And so is paint written after it.
  const Drawing painted = DrawBody("<path transform='translate(10 2)' d='" + held + held + "' fill='#808080'/>");
  EXPECT_TRUE(painted.IsBox(10, 2, 14, 6, 128));
  const Drawing unpainted = DrawBody("<path d='" + held + held + "' fill='none'/><rect x='10' width='2' height='2'/>");
  EXPECT_TRUE(unpainted.IsBox(10, 0, 12, 2, kBlack));
  EXPECT_EQ(unpainted.report.shapes, 1u);

  // A polygon's points are held and let go the same way.
  const std::string points = "0,0 4,0 4,4 0,4" + std::string(SvgRenderer::kHeldPathData, ' ');
  EXPECT_EQ(DrawBody("<polygon points='0,0 4,0 4,4 0,4' transform='translate(10 2)'/>").gray, before.gray);
  EXPECT_EQ(DrawBody("<polygon transform='translate(10 2)' points='" + points + "'/>").gray, before.gray);
  EXPECT_EQ(DrawBody("<polygon points='" + points + "' transform='translate(10 2)'/>").report.leftOut,
            Bits({SvgLeftOut::kLateTransform}));
}

TEST(SvgRender, LongPathDataIsDrawnAsItArrives) {
  // An outline of a thousand small steps, far longer than is held: a
  // staircase down the picture.
  std::string data = "M0 0";
  for (int step = 0; step < 500; ++step) {
    data += "h.08v.04";
  }
  data += "H0Z";
  ASSERT_GT(data.size(), 4 * SvgRenderer::kHeldPathData);
  const std::string file = Svg(40, 20, "<path d='" + data + "'/>");
  const Drawing drawing = Draw(file, 40, 20);
  EXPECT_EQ(drawing.status, SvgStatus::kOk);
  EXPECT_EQ(drawing.gray, Filled(data, "", FillRule::kNonZero, kBlack, 40, 20));
  EXPECT_NEAR(drawing.Covered(), 400, 1);
  EXPECT_EQ(drawing.report.commands, 1003u);
  // It makes no difference how the file is cut up on its way in.
  for (const std::size_t piece : {std::size_t{1}, std::size_t{5}, std::size_t{511}, std::size_t{4096}}) {
    EXPECT_EQ(Draw(file, 40, 20, 0, piece).gray, drawing.gray) << piece;
  }
}

// ---- Strips and pieces ----

TEST(SvgRender, ComesOutTheSameInAnyStripsAndAnyPieces) {
  std::string longPath = "M2 30";
  for (int step = 0; step < 300; ++step) {
    longPath += "l.15-.05 .02.09";
  }
  const std::string file =
      "<?xml version='1.0'?>\n<!-- Something of everything. -->\n"
      "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 120 90' width='60' height='45'>\n"
      "  <title>Everything</title>\n"
      "  <rect width='100%' height='100%' fill='#eee'/>\n"
      "  <g transform='translate(6 6)' fill='#444'>\n"
      "    <rect width='36' height='24' rx='5'/>\n"
      "    <circle cx='60' cy='12' r='12' fill='black'/>\n"
      "    <ellipse cx='90' cy='12' rx='15' ry='7' style='fill: rgb(50%, 50%, 50%); fill-opacity: .5'/>\n"
      "    <g transform='rotate(15 24 54)'>\n"
      "      <polygon points='6,36 42,36 42,66 24,54 6,66' fill='#888' fill-rule='evenodd'/>\n"
      "      <polyline points='12 42 36 42 24 51' fill='white'/>\n"
      "    </g>\n"
      "  </g>\n"
      "  <path fill-rule='evenodd' d='M66 42h42v36h-42z M75 51v18h24v-18z' opacity='0.8'/>\n"
      "  <path d='M70&#32;45c5-9 12 9 20 0s9 12 2 14z' transform='translate(2)' fill='#222'/>\n"
      "  <svg x='72' y='55' width='30' height='10' viewBox='0 0 100 100' preserveAspectRatio='none'>\n"
      "    <circle cx='50' cy='50' r='50' fill='&#35;222'/>\n"
      "  </svg>\n"
      "  <path fill='#666' d='" +
      longPath +
      "L50 80 2 80z'/>\n"
      "  <g display='none'><rect width='120' height='90'/></g>\n"
      "  <text x='6' y='84'>Not drawn</text>\n"
      "  <line x1='0' y1='0' x2='120' y2='90' stroke='red'/>\n"
      "</svg>\n";
  const Drawing whole = Draw(file, 60, 45);
  ASSERT_EQ(whole.status, SvgStatus::kOk);
  EXPECT_EQ(whole.report.leftOut, Bits({SvgLeftOut::kText, SvgLeftOut::kStroke, SvgLeftOut::kViewportClip}));
  EXPECT_EQ(whole.report.shapes, 10u);
  EXPECT_GT(whole.Covered(), 500.0);
  for (const std::size_t stripRows : {std::size_t{1}, std::size_t{7}, std::size_t{44}, std::size_t{64}}) {
    for (const std::size_t piece : {std::size_t{1}, std::size_t{3}, std::size_t{17}, std::size_t{1000}, SIZE_MAX}) {
      const Drawing cut = Draw(file, 60, 45, stripRows, piece);
      ASSERT_EQ(cut.gray, whole.gray) << "strips of " << stripRows << ", pieces of " << piece;
      EXPECT_EQ(cut.status, SvgStatus::kOk);
      EXPECT_EQ(cut.report.leftOut, whole.report.leftOut);
      EXPECT_EQ(cut.report.elements, whole.report.elements);
      EXPECT_EQ(cut.report.shapes, whole.report.shapes);
      EXPECT_EQ(cut.report.commands, whole.report.commands);
    }
  }
}

// ---- Size ----

// What a file says of its size, read with nothing to draw into.
SvgSize SizeOf(std::string_view file) {
  SvgRenderer renderer;
  renderer.Begin(nullptr, 0, 0);
  while (!file.empty()) {
    file.remove_prefix(renderer.Step(file));
  }
  renderer.Finish();
  return renderer.Size();
}

TEST(SvgRender, SaysHowLargeTheDrawingIs) {
  const auto natural = [](std::string_view attributes) {
    const SvgSize size = SizeOf("<svg " + std::string(attributes) + "/>");
    EXPECT_TRUE(size.known);
    Fixed width = -1;
    Fixed height = -1;
    size.Natural(width, height);
    return std::to_string(static_cast<double>(width) / kFixedOne) + " by " +
           std::to_string(static_cast<double>(height) / kFixedOne);
  };
  const auto size = [](double width, double height) { return std::to_string(width) + " by " + std::to_string(height); };
  EXPECT_EQ(natural("width='300' height='200' viewBox='0 0 30 20'"), size(300, 200));
  EXPECT_EQ(natural("width='300px' height='200px'"), size(300, 200));
  EXPECT_EQ(natural("width='1in' height='72pt'"), size(96, 96));
  // With only a view box, it is the view box.
  EXPECT_EQ(natural("viewBox='5 5 30 20'"), size(30, 20));
  // With one of the two, the other keeps the view box's shape.
  EXPECT_EQ(natural("width='60' viewBox='0 0 30 20'"), size(60, 40));
  EXPECT_EQ(natural("height='60' viewBox='0 0 30 20'"), size(90, 60));
  // A percentage is of something only whoever shows the drawing knows, and
  // says nothing here.
  EXPECT_EQ(natural("width='100%' height='100%' viewBox='0 0 30 20'"), size(30, 20));
  EXPECT_EQ(natural("width='100%' height='40' viewBox='0 0 30 20'"), size(60, 40));
  // With nothing to go on, it is what a browser makes it.
  EXPECT_EQ(natural(""), size(300, 150));
  EXPECT_EQ(natural("width='80'"), size(80, 150));
  EXPECT_EQ(natural("height='80' viewBox='0 0 0 0'"), size(300, 80));
  // A size of nothing, or less, is no size.
  EXPECT_EQ(natural("width='0' height='200'"), size(0, 0));
  EXPECT_EQ(natural("width='10' height='-5' viewBox='0 0 30 20'"), size(0, 0));

  const SvgSize read = SizeOf("<svg width='50%' height='2em' viewBox='1 2 3 4'/>");
  EXPECT_TRUE(read.hasWidth);
  EXPECT_EQ(read.width.unit, bn::draw::SvgUnit::kPercent);
  EXPECT_TRUE(read.hasHeight);
  EXPECT_EQ(read.height.unit, bn::draw::SvgUnit::kEm);
  EXPECT_TRUE(read.hasBox);
  EXPECT_EQ(read.box, (bn::draw::SvgBox{.x = 4096, .y = 8192, .width = 12288, .height = 16384}));
}

TEST(SvgRender, KnowsTheSizeAsSoonAsTheFirstTagHasBeenRead) {
  const std::string_view head = "<?xml version='1.0'?><svg width='30' height='20' viewBox='0 0 3 2'>";
  SvgRenderer renderer;
  renderer.Begin(nullptr, 0, 0);
  // Not a byte before the tag is closed.
  std::string_view bytes = head.substr(0, head.size() - 1);
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  EXPECT_FALSE(renderer.Size().known);
  bytes = head.substr(head.size() - 1);
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  EXPECT_TRUE(renderer.Size().known);
  Fixed width = 0;
  Fixed height = 0;
  renderer.Size().Natural(width, height);
  EXPECT_EQ(width, Pixels(30));
  EXPECT_EQ(height, Pixels(20));
}

TEST(SvgRender, UsedAgainItRemembersNothingOfTheFileBefore) {
  // A file whose last view box is an inner one, with everything else an
  // element can leave behind; and then, with the same renderer, a file
  // that says nothing of itself at all.
  SvgRenderer renderer;
  renderer.Begin(nullptr, 0, 0);
  std::string_view bytes =
      "<svg width='30' height='20' viewBox='0 0 3 2' preserveAspectRatio='none'><svg x='1' y='2' "
      "viewBox='-5 0 20 10' transform='scale(2)' opacity='.5' style='fill:none'><text/></svg></svg>";
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  ASSERT_EQ(renderer.Finish(), SvgStatus::kOk);
  ASSERT_TRUE(renderer.Size().hasBox);
  ASSERT_NE(renderer.Report().leftOut, 0u);

  renderer.Begin(nullptr, 0, 0);
  bytes = "<svg><rect width='4' height='4'/></svg>";
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  ASSERT_EQ(renderer.Finish(), SvgStatus::kOk);
  const SvgSize& size = renderer.Size();
  EXPECT_TRUE(size.known);
  EXPECT_FALSE(size.hasWidth);
  EXPECT_FALSE(size.hasHeight);
  EXPECT_FALSE(size.hasBox);
  EXPECT_EQ(size.box, bn::draw::SvgBox());
  EXPECT_EQ(renderer.Report().leftOut, 0u);
  EXPECT_EQ(renderer.Report().elements, 2u);

  // And drawn with it, the second file comes out as it does from a
  // renderer that has drawn nothing before.
  std::vector<std::uint8_t> gray(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  Filler filler;
  ASSERT_TRUE(filler.Begin(gray, 40, 0, cells));
  renderer.Begin(&filler, Pixels(40), Pixels(20));
  bytes = "<svg width='40' height='20'><rect width='4' height='4'/></svg>";
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  ASSERT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_EQ(gray, Draw("<svg width='40' height='20'><rect width='4' height='4'/></svg>", 40, 20).gray);
}

TEST(SvgRender, WithNothingToDrawIntoItOnlyReads) {
  std::string data = "M0 0";
  for (int step = 0; step < 400; ++step) {
    data += "h.1v.1";
  }
  const std::string file = Svg(40, 20,
                               "<text>x</text><g stroke='#000' opacity='.5'><path d='" + data +
                                   "'/><rect width='4' height='4' fill='url(#g)'/></g><use href='#a'/>");
  SvgRenderer renderer;
  renderer.Begin(nullptr, 0, 0);
  std::string_view bytes = file;
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  // What a file has that is not drawn is known all the same.
  EXPECT_EQ(renderer.Report().leftOut, Bits({SvgLeftOut::kText, SvgLeftOut::kStroke, SvgLeftOut::kGroupOpacity,
                                             SvgLeftOut::kPaintServer, SvgLeftOut::kUse}));
  EXPECT_EQ(renderer.Report().elements, 6u);
  // No outline is read, and nothing is filled.
  EXPECT_EQ(renderer.Report().commands, 0u);
  EXPECT_EQ(renderer.Report().shapes, 0u);

  // A filler that was given a strip it could not take is as good as none.
  Filler refused;
  std::vector<std::uint8_t> nothing;
  std::vector<std::int16_t> cells(10);
  EXPECT_FALSE(refused.Begin(nothing, 4, 0, cells));
  renderer.Begin(&refused, Pixels(40), Pixels(20));
  bytes = file;
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().shapes, 0u);
}

// ---- Files that are not what they should be ----

TEST(SvgRender, AFileThatIsNotADrawingIsRefused) {
  for (const std::string_view file :
       {"", "   \n", "hello", "<html><svg><rect width='4' height='4'/></svg></html>", "<?xml version='1.0'?><!-- x -->",
        "<rect width='4' height='4'/>", "<SVG><rect width='4' height='4'/></SVG>", "<?xml", "\xFF\xFE<\0s\0v\0g\0"}) {
    const Drawing drawing = Draw(file, 40, 20);
    EXPECT_EQ(drawing.status, SvgStatus::kNotSvg) << file;
    EXPECT_EQ(drawing.Covered(), 0.0) << file;
  }
  // What stands before and after the drawing's own element is passed
  // over.
  const Drawing drawing =
      Draw("\xEF\xBB\xBF<?xml version='1.0'?>\n<!DOCTYPE svg PUBLIC 'a' 'b'>\n<!-- c -->\n" +
               Svg(40, 20, "<rect width='4' height='4'/>") +
               "\n<!-- d --><rect width='40' height='20'/><svg><rect width='40' height='20'/></svg>",
           40, 20);
  EXPECT_EQ(drawing.status, SvgStatus::kOk);
  EXPECT_TRUE(drawing.IsBox(0, 0, 4, 4, kBlack));
  // A drawing whose elements are written with a prefix is one all the
  // same.
  const Drawing prefixed = Draw(
      "<svg:svg xmlns:svg='http://www.w3.org/2000/svg' width='40' height='20'>"
      "<svg:g><svg:rect width='4' height='4'/></svg:g></svg:svg>",
      40, 20);
  EXPECT_EQ(prefixed.status, SvgStatus::kOk);
  EXPECT_TRUE(prefixed.IsBox(0, 0, 4, 4, kBlack));
  EXPECT_STREQ(bn::draw::SvgStatusName(SvgStatus::kNotSvg), "not svg");
  EXPECT_STREQ(bn::draw::SvgStatusName(SvgStatus::kTruncated), "truncated");
}

TEST(SvgRender, WhatComesBeforeAFaultIsDrawn) {
  const std::string start =
      "<svg width='40' height='20'><rect width='4' height='4'/><g><rect x='10' width='4' height='4'/>";
  // Tags that go wrong.
  const Drawing malformed = Draw(start + "<rect x='20' width='4' height='4'/><<<</g></svg>", 40, 20);
  EXPECT_EQ(malformed.status, SvgStatus::kMalformed);
  // At the second `<` of the three: one may begin a tag, and another may
  // not follow it.
  EXPECT_EQ(malformed.errorOffset, start.size() + 36);
  EXPECT_EQ(malformed.Covered(), 48.0);
  const Drawing unbalanced = Draw(start + "</g></svg></svg>", 40, 20);
  EXPECT_EQ(unbalanced.status, SvgStatus::kMalformed);
  EXPECT_EQ(unbalanced.Covered(), 32.0);
  // A file that stops short: between elements, inside a tag, and inside an
  // outline, long or short. The shape it stopped in is not drawn.
  const std::string longData = "M20 0h4v4h-4z" + std::string(2 * SvgRenderer::kHeldPathData, ' ') + "M30 0h4v4";
  for (const std::string& rest :
       {std::string(), std::string("<rect x='20' width='4' hei"), std::string("<path d='M20 0h4v4h-4"),
        "<path d='" + longData, "<path d='" + longData + "'", std::string("</g>")}) {
    for (const std::size_t stripRows : {std::size_t{0}, std::size_t{3}}) {
      const Drawing truncated = Draw(start + rest, 40, 20, stripRows, 7);
      EXPECT_EQ(truncated.status, SvgStatus::kTruncated) << rest.substr(0, 30);
      EXPECT_EQ(truncated.Covered(), 32.0) << rest.substr(0, 30);
    }
  }
  // Once it has gone wrong, whatever else it is given is taken and
  // nothing comes of it.
  SvgRenderer renderer;
  renderer.Begin(nullptr, 0, 0);
  EXPECT_EQ(renderer.Step("<svg><<"), 7u);
  EXPECT_EQ(renderer.Status(), SvgStatus::kMalformed);
  EXPECT_EQ(renderer.Step("<rect/></svg>"), 13u);
  EXPECT_EQ(renderer.Finish(), SvgStatus::kMalformed);
}

// ---- How much one call does ----

TEST(SvgRender, ACallDoesNoMoreThanItsBound) {
  // An outline of thousands of commands written as tightly as they can
  // be, one held and gone through after its tag, hundreds of small
  // shapes, and thousands of tags that draw nothing: all given to it at
  // once, so that nothing but its own bound stops a call.
  std::string body = "<path d='M0 0";
  for (int i = 0; i < 4000; ++i) {
    body += "h1";
  }
  body += "'/><path d='M0 0";
  for (int i = 0; i < 160; ++i) {
    body += "v1";
  }
  body += "' fill='#000'/>";
  for (int i = 0; i < 300; ++i) {
    body += "<rect width='3' height='3' rx='1'/><circle r='2'/>";
  }
  for (int i = 0; i < 3000; ++i) {
    body += "<g/>";
  }
  const std::string file = Svg(40, 20, body);

  std::vector<std::uint8_t> gray(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  Filler filler;
  ASSERT_TRUE(filler.Begin(gray, 40, 0, cells));
  SvgRenderer renderer;
  renderer.Begin(&filler, Pixels(40), Pixels(20));
  std::string_view bytes = file;
  std::size_t calls = 0;
  std::uint32_t mostCommands = 0;
  std::uint32_t mostShapes = 0;
  std::size_t mostBytes = 0;
  while (!bytes.empty()) {
    const SvgReport before = renderer.Report();
    const std::size_t taken = renderer.Step(bytes);
    const SvgReport& after = renderer.Report();
    mostCommands = std::max(mostCommands, after.commands - before.commands);
    mostShapes = std::max(mostShapes, after.shapes - before.shapes);
    mostBytes = std::max(mostBytes, taken);
    // A curve is at the most kMostLinesPerCurve lines and an arc
    // kMostLinesPerArc, so the commands bound the lines.
    ASSERT_LE(after.lines - before.lines, SvgRenderer::kMostCommandsPerStep * PathFlattener::kMostLinesPerArc);
    bytes.remove_prefix(taken);
    ++calls;
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_LE(mostCommands, SvgRenderer::kMostCommandsPerStep);
  EXPECT_GE(mostCommands, SvgRenderer::kCommandsPerStep);
  EXPECT_EQ(mostShapes, 1u);
  // It takes no more bytes than the tags it handles are made of, and the
  // path data it can hold.
  EXPECT_LT(mostBytes, SvgRenderer::kHeldPathData + 200);
  // So the work was shared out among the calls: about one for every
  // sixteen commands, one for every shape, and one for every sixteen tags
  // that drew nothing.
  EXPECT_GT(calls, (4160 / 17) + 600 + (6000 / 16));
  EXPECT_EQ(renderer.Report().commands, 4001u + 161u + (300 * 16));
  EXPECT_EQ(renderer.Report().shapes, 602u);
}

// ---- Anything at all ----

TEST(SvgRender, AnyBytesAtAllEndInAStatus) {
  // Files made of the pieces of a drawing put together any which way, and
  // of bytes that mean something to the tags or to nothing at all.
  std::mt19937 random = Repeatable(41);
  const std::string_view parts[] = {
      "<rect width='10' height='8'",
      "<rect x='5' y='5' width='20' height='9' rx='3'",
      "<circle cx='9' cy='9' r='7'",
      "<ellipse cx='20' cy='10' rx='15' ry='6'",
      "<path d='M3 3h20v10z'",
      "<path d='M1 1c5 9 20-9 30 9v8h-30z'",
      "<polygon points='2,2 30,4 12,18'",
      "<polyline points='5 5 35 5 20 15'",
      "<g>",
      "<g transform='translate(3 4)'>",
      "<g fill='#888' opacity='.5'>",
      "<svg x='4' y='4' width='20' height='10' viewBox='0 0 2 1'>",
      " fill='#444'",
      " transform='rotate(10)'",
      " style='fill:#999;fill-rule:evenodd'",
      " fill-opacity='.5'",
      "/>",
      "/>",
      "/>",
      "</g>",
      "<svg",
      "<g",
      "<path",
      "<rect",
      "<circle",
      "<ellipse",
      "<polygon",
      "<polyline",
      "<line",
      "<use",
      "<text",
      "<style",
      "<defs",
      "<svg:g",
      "</g>",
      "</svg>",
      "</path>",
      ">",
      "/>",
      " d='",
      " d=\"",
      " points='",
      " transform='",
      " style='",
      " fill='",
      " viewBox='",
      " width='",
      " height='",
      " x='",
      " rx='",
      " r='",
      " opacity='",
      " stroke='",
      " preserveAspectRatio='",
      "'",
      "\"",
      "M0 0",
      "h10v10",
      "L5 5 9 1",
      "a5 5 0 1 1 3 3",
      "c1 2 3 4 5 6",
      "z",
      "99999999999",
      "-",
      ".",
      "e",
      "1 2 3 4",
      "0 0 40 20",
      "translate(3 4)",
      "scale(2)",
      "rotate(30)",
      "matrix(1 0 0 1 0 0)",
      "#888",
      "none",
      "url(#a)",
      "fill:red;",
      "opacity:.5",
      "50%",
      "10",
      "&amp;",
      "&#32;",
      "&x;",
      "<!--",
      "-->",
      "<![CDATA[",
      "]]>",
      "<?",
      "?>",
      "<!DOCTYPE",
      " ",
      "\n",
      ";",
      ":",
      "(",
      ")",
      ",",
      "xMidYMid",
      "slice",
      "evenodd",
  };
  std::size_t drew = 0;
  std::size_t failed = 0;
  for (int i = 0; i < 6000; ++i) {
    std::string file = (i % 3 == 0) ? "" : "<svg width='40' height='20'>";
    const std::size_t count = random() % 40;
    for (std::size_t part = 0; part < count; ++part) {
      // The first twenty parts are whole shapes and groups, and are taken
      // more often than the rest, so that there is often something to
      // draw before a file goes wrong.
      const auto kind = random() % 12;
      if (kind == 0) {
        file += static_cast<char>(random() % 256);
      } else if (kind < 8) {
        file += parts[random() % 20];
      } else {
        file += parts[random() % std::size(parts)];
      }
    }
    const std::size_t piece = (i % 2 == 0) ? SIZE_MAX : 1 + (random() % 9);
    const Drawing drawing = Draw(file, 40, 20, (i % 5 == 0) ? 6 : 0, piece);
    drew += drawing.report.shapes > 0 ? 1 : 0;
    failed += drawing.status == SvgStatus::kOk ? 0 : 1;
    // It cannot be made to go round: a call takes a byte, or works off
    // some of what it holds.
    ASSERT_LT(drawing.calls, (4 * (file.size() + 600)) * ((i % 5 == 0) ? 4 : 1)) << i;
    // And however it is cut up, it comes to the same.
    const Drawing whole = Draw(file, 40, 20);
    ASSERT_EQ(drawing.gray, whole.gray) << i << ": " << file;
    ASSERT_EQ(drawing.status, whole.status) << i << ": " << file;
    ASSERT_EQ(drawing.report.leftOut, whole.report.leftOut) << i << ": " << file;
  }
  // Hundreds of them drew something, and most went wrong somewhere.
  EXPECT_GT(drew, 300u) << drew;
  EXPECT_GT(failed, 3000u) << failed;
  EXPECT_LT(failed, 6000u);
}

}  // namespace
