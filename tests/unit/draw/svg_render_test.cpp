#include "draw/svg_render.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

using bn::draw::Coefficient;
using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kCoefficientOne;
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
// rows, giving it to the renderer `piece` bytes at a time, with
// `definitions` bytes for it to keep what the file defines in.
Drawing Draw(std::string_view file, std::size_t width, std::size_t height, std::size_t stripRows = 0,
             std::size_t piece = SIZE_MAX, std::size_t definitions = 8192) {
  Drawing drawing;
  drawing.width = width;
  drawing.height = height;
  drawing.gray.assign(width * height, kWhite);
  stripRows = stripRows == 0 ? height : stripRows;
  std::vector<std::int16_t> cells(Filler::CellsFor(width, stripRows));
  std::vector<std::uint8_t> kept(definitions);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
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
        const std::uint32_t looked = renderer.Report().looked;
        const std::size_t taken = renderer.Step(bytes);
        EXPECT_LE(taken, bytes.size());
        // Every drawing here is held to the bound on looking through what
        // is kept.
        EXPECT_LE(renderer.Report().looked - looked, SvgRenderer::kMostLooksPerStep);
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
  // A semicolon inside brackets does not end a value, and brackets are
  // counted as deep as 255: here the first thing said goes on to the last
  // of its brackets, and what is written inside them is not read.
  const Drawing bracketed = DrawBody("<rect width='4' height='4' style='-x:" + std::string(256, '(') + ";fill:#fff" +
                                     std::string(256, ')') + ";fill-opacity:0.5'/>");
  EXPECT_NEAR(bracketed.At(1, 1), 128, 1);
  EXPECT_EQ(bracketed.report.leftOut, 0u);
}

TEST(SvgRender, InheritIsWhateverTheElementRoundItHas) {
  // Each is written as `style`, which wins, over an attribute that says
  // something else: so what is seen is what `inherit` did.
  const Drawing rule = DrawBody(
      "<g fill-rule='evenodd'>"
      "<path d='M0 0h10v10h-10zM2 2h6v6h-6z' fill-rule='nonzero' style='fill-rule:inherit'/></g>");
  EXPECT_EQ(rule.At(1, 1), kBlack);
  EXPECT_EQ(rule.At(5, 5), kWhite);
  EXPECT_EQ(rule.report.leftOut, 0u);
  const Drawing solid =
      DrawBody("<g fill-opacity='.5'><rect width='4' height='4' fill-opacity='1' style='fill-opacity:inherit'/></g>");
  EXPECT_NEAR(solid.At(1, 1), 128, 1);
  EXPECT_EQ(solid.report.leftOut, 0u);
  const Drawing colour = DrawBody(
      "<g color='#808080'>"
      "<rect width='4' height='4' fill='currentColor' color='#000' style='color:inherit'/></g>");
  EXPECT_EQ(colour.At(1, 1), 128);
  EXPECT_EQ(colour.report.leftOut, 0u);
  const Drawing hidden = DrawBody(
      "<g visibility='hidden'>"
      "<rect width='4' height='4' visibility='visible' style='visibility:inherit'/></g>");
  EXPECT_EQ(hidden.Covered(), 0.0);
  EXPECT_EQ(hidden.report.leftOut, 0u);
  // A stroke, and how wide it is: a ring a pixel wide round a square
  // of four by four, from 2 to 7.
  const Drawing stroked = DrawBody(
      "<g stroke='#000' stroke-width='1'><rect x='2.5' y='2.5' width='4' height='4' fill='none' stroke='none' "
      "stroke-width='3' style='stroke:inherit;stroke-width:inherit'/></g>");
  EXPECT_NEAR(stroked.Covered(), 16, 0.01);
  EXPECT_EQ(stroked.report.leftOut, 0u);
  EXPECT_EQ(stroked.report.strokes, 1u);
  const Drawing unstroked = DrawBody(
      "<g stroke='none'><rect x='2' y='2' width='4' height='4' fill='none' stroke='#000' style='stroke:inherit'/></g>");
  EXPECT_EQ(unstroked.Covered(), 0.0);
  EXPECT_EQ(unstroked.report.strokes, 0u);
  // Opacity is not handed on, and to ask for the one round it is no
  // mistake.
  const Drawing opaque = DrawBody("<rect width='4' height='4' opacity='inherit'/>");
  EXPECT_TRUE(opaque.IsBox(0, 0, 4, 4, kBlack));
  EXPECT_EQ(opaque.report.leftOut, 0u);
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
  EXPECT_EQ(boxed.report.leftOut, 0u);
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
      {"<defs><path id='a' d='M0 0h4v4z'/><text>x</text></defs><use href='#a'/>", 0, 8},
      {"<use href='#a'/><rect width='4' height='4'/>", Bits({SvgLeftOut::kUse}), 16},
      {"<style>g rect { fill: none }</style><rect width='4' height='4'/>", Bits({SvgLeftOut::kStyleSheet}), 16},
      {"<style>rect { fill: none }</style><rect width='4' height='4'/>", 0, 0},
      // A stroke is drawn over the fill, half its width outside the
      // outline.
      {"<rect x='2.5' y='2.5' width='4' height='4' stroke='#000'/>", 0, 25},
      {"<rect x='2' y='2' width='4' height='4' fill='none' style='stroke:#000;stroke-width:2'/>", 0, 32},
      {"<g stroke='#000'><rect x='2.5' y='2.5' width='4' height='4'/></g>", 0, 25},
      {"<line x1='2' y1='5.5' x2='22' y2='5.5' stroke='#000'/>", 0, 20},
      // One painted with a gradient or a pattern is not drawn: what is
      // written to stand in for it is.
      {"<rect x='2.5' y='2.5' width='4' height='4' fill='none' stroke='url(#g) #000'/>", Bits({SvgLeftOut::kStroke}),
       16},
      {"<rect x='2' y='2' width='4' height='4' fill='none' stroke='url(#g)'/>", Bits({SvgLeftOut::kStroke}), 0},
      // A stroke of no width, or of no paint, or on something hidden, is no stroke.
      {"<rect width='4' height='4' stroke='#000' stroke-width='0'/>", 0, 16},
      {"<g stroke='currentColor' stroke-width='0'><rect width='4' height='4'/></g>", 0, 16},
      {"<g stroke='#000'><rect width='4' height='4' stroke='none'/></g>", 0, 16},
      {"<g stroke='#000' stroke-width='0'><rect width='4' height='4' stroke-width='inherit'/></g>", 0, 16},
      {"<rect width='4' height='4' stroke='#000' display='none'/>", 0, 0},
      // A paint that names nothing there is is not drawn, unless something is
      // written to stand in for it.
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
      {"<svg><rect width='4' height='4'/></svg>", 0, 16},
      {"<g transform='translate(10 5) rotate(10)'><svg><rect width='4' height='4'/></svg></g>",
       Bits({SvgLeftOut::kViewportClip}), 16},
      // An attribute that cannot be read. One that says where a thing is
      // leaves it undrawn; one that says how it is painted leaves it as it
      // would have been.
      {"<g transform='rotate(bad)'><rect width='4' height='4'/></g>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='4' height='4' transform='matrix(1 2 3)'/>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='four' height='4'/>", Bits({SvgLeftOut::kBadValue}), 0},
      {"<rect width='4' height='4' fill='blurple'/>", Bits({SvgLeftOut::kBadValue}), 16},
      {"<rect width='4' height='4' fill-rule='oddeven' fill-opacity='lots' style='opacity:some;color:#12;'/>",
       Bits({SvgLeftOut::kBadValue}), 16},
      {"<svg viewBox='0 0 4'><rect width='4' height='4'/></svg>", Bits({SvgLeftOut::kBadValue}), 16},
      {"<svg preserveAspectRatio='sideways'><rect width='4' height='4'/></svg>", Bits({SvgLeftOut::kBadValue}), 16},
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
  for (unsigned bit = 0; bit < 24; ++bit) {
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
  // A transform that cannot be read leaves the thing undrawn, however
  // long its data and whichever of the two comes first, and what follows
  // it is drawn.
  const std::string twice = held + held;
  const std::vector<std::string> unreadable = {"<path transform='rotate(' d='" + twice + "'/>",
                                               "<path d='" + twice + "' transform='rotate('/>",
                                               "<path transform='rotate(' d='" + std::string(square) + "'/>",
                                               "<path d='" + std::string(square) + "' transform='rotate('/>"};
  for (const std::string& unread : unreadable) {
    const Drawing drawing = DrawBody(unread + "<rect x='10' width='2' height='2'/>");
    EXPECT_TRUE(drawing.IsBox(10, 0, 12, 2, kBlack)) << unread.substr(0, 40);
    EXPECT_EQ(drawing.report.shapes, 1u) << unread.substr(0, 40);
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kBadValue})) << unread.substr(0, 40);
  }
  // With the transform first, data too long to hold is not gone through
  // at all.
  EXPECT_EQ(DrawBody("<path transform='rotate(' d='" + held + held + "'/>").report.commands, 0u);
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
      "  <defs>\n"
      "    <path id='leaf' d='M0 0c4-6 10-6 14 0c-4 6-10 6-14 0z'/>\n"
      "    <g id='sprig' fill='#555'>\n"
      "      <use xlink:href='#leaf'/><use xlink:href='#leaf' transform='translate(0 8) rotate(-20)'/>\n"
      "    </g>\n"
      "  </defs>\n"
      "  <use xlink:href='#sprig' x='8' y='70'/>\n"
      "  <use href='#leaf' transform='translate(116 84) scale(-1 1)' fill='#999'/>\n"
      "  <svg x='40' y='66' width='20' height='14.5'><circle cx='10' cy='7' r='12' fill='#333'/></svg>\n"
      "  <text x='6' y='84'>Not drawn</text>\n"
      "  <line x1='0' y1='0' x2='120' y2='90' stroke='red'/>\n"
      "</svg>\n";
  const Drawing whole = Draw(file, 60, 45);
  ASSERT_EQ(whole.status, SvgStatus::kOk);
  EXPECT_EQ(whole.report.leftOut, Bits({SvgLeftOut::kText}));
  EXPECT_EQ(whole.report.shapes, 14u);
  EXPECT_EQ(whole.report.strokes, 1u);
  EXPECT_GT(whole.Covered(), 500.0);
  EXPECT_GT(whole.report.kept, 200u);
  EXPECT_GT(whole.report.reused, 250u);
  for (const std::size_t stripRows : {std::size_t{1}, std::size_t{7}, std::size_t{44}, std::size_t{64}}) {
    for (const std::size_t piece : {std::size_t{1}, std::size_t{3}, std::size_t{17}, std::size_t{1000}, SIZE_MAX}) {
      const Drawing cut = Draw(file, 60, 45, stripRows, piece);
      ASSERT_EQ(cut.gray, whole.gray) << "strips of " << stripRows << ", pieces of " << piece;
      EXPECT_EQ(cut.status, SvgStatus::kOk);
      EXPECT_EQ(cut.report.leftOut, whole.report.leftOut);
      EXPECT_EQ(cut.report.elements, whole.report.elements);
      EXPECT_EQ(cut.report.shapes, whole.report.shapes);
      EXPECT_EQ(cut.report.commands, whole.report.commands);
      EXPECT_EQ(cut.report.kept, whole.report.kept);
      EXPECT_EQ(cut.report.reused, whole.report.reused);
    }
  }
}

// ---- What is defined in one place and used in another ----

constexpr std::string_view kBlock = "<defs><rect id='a' width='4' height='3'/></defs>";

std::string With(std::string_view defs, std::string_view rest) {
  std::string body(defs);
  body += rest;
  return body;
}

TEST(SvgRender, AUseDrawsWhatDefsHoldsWhereTheUseIs) {
  // Nothing in `defs` is drawn where it is written.
  const Drawing unused = DrawBody(kBlock);
  EXPECT_TRUE(unused.IsBox(0, 0, 0, 0, kBlack));
  EXPECT_EQ(unused.report.shapes, 0u);
  EXPECT_EQ(unused.report.leftOut, 0u);
  for (const std::string_view use :
       {"<use href='#a' x='10' y='5'/>", "<use xlink:href='#a' x='10' y='5'/>", "<use x='10' href=' #a ' y='5'></use>",
        "<use href='#a' transform='translate(10 5)'/>", "<use href='&#35;a' x='10' y='5'><title>A box</title></use>"}) {
    const Drawing drawing = DrawBody(With(kBlock, use));
    EXPECT_TRUE(drawing.IsBox(10, 5, 14, 8, kBlack)) << use;
    EXPECT_EQ(drawing.report.leftOut, 0u) << use;
    EXPECT_EQ(drawing.report.shapes, 1u) << use;
  }
  // As often as it is used.
  const Drawing twice = DrawBody(With(kBlock, "<use href='#a'/><use href='#a' x='4' y='3'/>"));
  EXPECT_EQ(twice.At(3, 2), kBlack);
  EXPECT_EQ(twice.At(4, 3), kBlack);
  EXPECT_EQ(twice.At(4, 2), kWhite);
  EXPECT_DOUBLE_EQ(twice.Covered(), 24);
  EXPECT_EQ(twice.report.shapes, 2u);
  // From more than one `defs`, and by a name that begins another.
  const Drawing two = DrawBody(
      "<defs><rect id='a' width='4' height='3'/></defs><g><defs><rect id='ab' width='2' height='2'/></defs></g>"
      "<use href='#ab' x='20'/><use href='#a' x='10' y='5'/>");
  EXPECT_EQ(two.report.leftOut, 0u);
  EXPECT_EQ(two.At(21, 1), kBlack);
  EXPECT_EQ(two.At(13, 7), kBlack);
  EXPECT_DOUBLE_EQ(two.Covered(), 16);
}

TEST(SvgRender, AUseMovesWhatItDrawsByItsTransformAndThenToItsPlace) {
  EXPECT_TRUE(DrawBody(With(kBlock, "<use href='#a' transform='scale(2)' x='3' y='1'/>")).IsBox(6, 2, 14, 8, kBlack));
  // A group it is in moves it as it moves anything.
  EXPECT_TRUE(DrawBody(With(kBlock, "<g transform='translate(20 10)'><use href='#a' x='1'/></g>"))
                  .IsBox(21, 10, 25, 13, kBlack));
  // What it draws has its own transform, innermost.
  EXPECT_TRUE(DrawBody("<defs><rect id='a' width='4' height='3' transform='translate(1 1)'/></defs>"
                       "<use href='#a' transform='scale(2)'/>")
                  .IsBox(2, 2, 10, 8, kBlack));
  // Where it was defined moves nothing.
  EXPECT_TRUE(DrawBody("<g transform='translate(20 10)'><defs><rect id='a' width='4' height='3'/></defs></g>"
                       "<use href='#a'/>")
                  .IsBox(0, 0, 4, 3, kBlack));
  // A place given as a part of what the drawing is seen through.
  EXPECT_TRUE(DrawBody(With(kBlock, "<use href='#a' x='50%' y='25%'/>")).IsBox(20, 5, 24, 8, kBlack));
}

TEST(SvgRender, WhatAUseDrawsIsPaintedAsTheUseIsWhereItSaysNothingOfItsOwn) {
  const std::string_view defs =
      "<defs><rect id='plain' width='4' height='4'/>"
      "<rect id='gray' width='4' height='4' fill='#808080'/></defs>";
  const Drawing drawing = DrawBody(With(defs,
                                        "<use href='#plain' fill='#404040'/>"
                                        "<use href='#gray' x='10' fill='#404040'/>"
                                        "<g fill='#c0c0c0'><use href='#plain' y='10'/></g>"
                                        "<use href='#plain' x='20' style='fill:#202020'/>"
                                        "<use href='#plain' x='30' color='#606060' fill='currentColor'/>"));
  EXPECT_EQ(drawing.At(1, 1), 0x40);
  EXPECT_EQ(drawing.At(11, 1), 0x80);
  EXPECT_EQ(drawing.At(1, 11), 0xc0);
  EXPECT_EQ(drawing.At(21, 1), 0x20);
  EXPECT_EQ(drawing.At(31, 1), 0x60);
  EXPECT_EQ(drawing.report.leftOut, 0u);
  // Not as the place it was defined in is.
  EXPECT_EQ(DrawBody("<g fill='#808080'><defs><rect id='a' width='4' height='4'/></defs></g><use href='#a'/>").At(1, 1),
            kBlack);
  // A `use` that is hidden, or not there at all, draws nothing.
  const Drawing hidden = DrawBody(With(defs,
                                       "<use href='#plain' visibility='hidden'/>"
                                       "<use href='#plain' x='10' display='none'/>"
                                       "<use href='#plain' x='20' fill='none'/>"));
  EXPECT_TRUE(hidden.IsBox(0, 0, 0, 0, kBlack));
  EXPECT_EQ(hidden.report.leftOut, 0u);
  // What is hidden can show something in it that says it is to be seen.
  EXPECT_EQ(DrawBody("<defs><rect id='a' width='4' height='4' visibility='visible'/></defs>"
                     "<use href='#a' visibility='hidden'/>")
                .At(1, 1),
            kBlack);
  EXPECT_NEAR(DrawBody(With(defs, "<use href='#plain' fill-opacity='0.5'/>")).At(1, 1), 127, 1);
  // The opacity of a `use` is laid on each thing it draws, as a group's
  // is, and that is reported.
  const Drawing faint = DrawBody(With(defs, "<use href='#plain' opacity='0.5'/>"));
  EXPECT_NEAR(faint.At(1, 1), 127, 1);
  EXPECT_EQ(faint.report.leftOut, Bits({SvgLeftOut::kGroupOpacity}));
}

TEST(SvgRender, AUseDrawsAGroupAndAllThatIsInIt) {
  const Drawing drawing = DrawBody(
      "<defs><g id='pair' transform='translate(2 2)' fill='#808080'>"
      "<rect width='2' height='2'/><title>Two</title>"
      "<rect x='4' width='2' height='2' fill='black'/></g></defs>"
      "<use href='#pair' x='10'/>");
  EXPECT_EQ(drawing.report.shapes, 2u);
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.At(12, 2), 0x80);
  EXPECT_EQ(drawing.At(13, 3), 0x80);
  EXPECT_EQ(drawing.At(14, 2), kWhite);
  EXPECT_EQ(drawing.At(16, 2), kBlack);
  EXPECT_EQ(drawing.At(17, 3), kBlack);
  EXPECT_EQ(drawing.At(18, 3), kWhite);
  // Something inside a group can be used by its own name, without the
  // group: what the group says of it is not said where it is used.
  const Drawing inner = DrawBody(
      "<defs><g id='pair' transform='translate(2 2)' fill='#808080'>"
      "<rect id='one' width='2' height='2'/><rect x='4' width='2' height='2'/></g></defs>"
      "<use href='#one' x='10'/>");
  EXPECT_TRUE(inner.IsBox(10, 0, 12, 2, kBlack));
  // A name inside it that cannot be found, being written with an entity,
  // is not taken for the group's.
  const Drawing unnamed = DrawBody(
      "<defs><g id='a'><rect id='&#97;' width='2' height='2'/>"
      "<rect x='4' width='2' height='2'/></g></defs><use href='#a'/>");
  EXPECT_EQ(unnamed.report.shapes, 2u);
  EXPECT_EQ(unnamed.report.leftOut, 0u);
  // What is left out of what is used is reported where it is used.
  const Drawing texted = DrawBody("<defs><g id='g'><text>Words</text><rect width='2' height='2'/></g></defs>");
  EXPECT_EQ(texted.report.leftOut, 0u);
  const Drawing used = DrawBody(
      "<defs><g id='g'><text>Words</text><rect width='2' height='2'/></g></defs>"
      "<use href='#g'/>");
  EXPECT_EQ(used.report.leftOut, Bits({SvgLeftOut::kText}));
  EXPECT_TRUE(used.IsBox(0, 0, 2, 2, kBlack));
}

TEST(SvgRender, DefinitionsInsideWhatIsUsedAreKeptOnceAndNotAgain) {
  const std::string_view defined =
      "<g id='a'><defs><rect id='b' x='10' width='2' height='2'/></defs><rect width='4' height='4'/></g>";
  const std::string file = Svg(
      40, 20, "<defs>" + std::string(defined) + "</defs><use href='#a'/><use href='#b' y='6'/><use href='#a' y='12'/>");
  const Drawing drawing = Draw(file, 40, 20);
  EXPECT_EQ(drawing.status, SvgStatus::kOk);
  EXPECT_EQ(drawing.report.leftOut, 0u);
  // The group twice, without the `defs` inside it, and what that holds
  // once, by its own name.
  EXPECT_EQ(drawing.report.shapes, 3u);
  EXPECT_EQ(drawing.At(0, 0), kBlack);
  EXPECT_EQ(drawing.At(3, 3), kBlack);
  EXPECT_EQ(drawing.At(10, 0), kWhite);
  EXPECT_EQ(drawing.At(10, 6), kBlack);
  EXPECT_EQ(drawing.At(11, 7), kBlack);
  EXPECT_EQ(drawing.At(3, 15), kBlack);
  EXPECT_DOUBLE_EQ(drawing.Covered(), 16 + 4 + 16);
  // Reading what is used again keeps no more and gives nothing a second
  // name: with room for what `defs` held and its two names, and not a byte
  // over, it is all the same.
  const std::size_t held = defined.size() + std::string_view("</defs>").size();
  EXPECT_EQ(drawing.report.kept, held);
  for (const std::size_t piece : {SIZE_MAX, std::size_t{1}}) {
    const Drawing exact = Draw(file, 40, 20, 0, piece, held + (2 * SvgRenderer::kBytesPerDefinition));
    EXPECT_EQ(exact.gray, drawing.gray) << piece;
    EXPECT_EQ(exact.report.leftOut, 0u) << piece;
    EXPECT_EQ(exact.report.kept, held) << piece;
  }
}

TEST(SvgRender, WhatIsUsedCanItselfUseSomething) {
  const Drawing drawing = DrawBody(
      "<defs><rect id='dot' width='2' height='2'/>"
      "<g id='row'><use href='#dot'/><use href='#dot' x='4'><desc>Again</desc></use></g>"
      "<use id='moved' href='#row' transform='translate(30 0)'/></defs>"
      "<use href='#row' y='3'/><use href='#row' x='20' y='10' fill='#808080'/>"
      "<use href='#moved' y='16'/>");
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.shapes, 6u);
  EXPECT_EQ(drawing.At(0, 3), kBlack);
  EXPECT_EQ(drawing.At(2, 3), kWhite);
  EXPECT_EQ(drawing.At(5, 4), kBlack);
  EXPECT_EQ(drawing.At(20, 10), 0x80);
  EXPECT_EQ(drawing.At(25, 11), 0x80);
  EXPECT_EQ(drawing.At(30, 16), kBlack);
  EXPECT_EQ(drawing.At(35, 17), kBlack);
  EXPECT_NEAR(drawing.Covered(), 16 + (8 * (255.0 - 0x80) / 255), 0.001);
}

// A shape or two for a drawing made by chance, written as SVG.
std::string AnyShape(std::mt19937& random) {
  const auto upTo = [&random](unsigned count) { return std::to_string(random() % count); };
  std::string paint;
  if (random() % 3 == 0) {
    paint = " fill='#" + upTo(10) + upTo(10) + upTo(10) + "'";
  }
  if (random() % 5 == 0) {
    paint += " fill-rule='evenodd'";
  }
  switch (random() % 5) {
    case 0:
      return "<rect x='" + upTo(40) + "' y='" + upTo(30) + "' width='" + upTo(20) + ".5' height='" + upTo(14) +
             ".25' rx='" + upTo(4) + "'" + paint + "/>";
    case 1:
      return "<circle cx='" + upTo(50) + "' cy='" + upTo(36) + "' r='" + upTo(12) + ".5'" + paint + "/>";
    case 2:
      return "<path" + paint + " d='M" + upTo(40) + " " + upTo(30) + "c" + upTo(20) + " -" + upTo(20) + " " + upTo(30) +
             " " + upTo(20) + " " + upTo(12) + " " + upTo(16) + "l-" + upTo(20) + " " + upTo(9) + "z'/>";
    case 3: {
      // Data too long to hold, which is drawn as it is read.
      std::string data = "M" + upTo(30) + " " + upTo(20);
      for (int step = 0; step < 120; ++step) {
        data += "l." + upTo(9) + (step % 2 == 0 ? " ." : "-.") + upTo(9);
      }
      return "<path" + paint + " d='" + data + "l-9 9z'/>";
    }
    default:
      return "<g transform='translate(" + upTo(9) + " " + upTo(9) + ")'" + paint + "><polygon points='" + upTo(30) +
             "," + upTo(20) + " " + upTo(30) + "," + upTo(20) + " " + upTo(30) + "," + upTo(20) + "'/><ellipse cx='" +
             upTo(40) + "' cy='" + upTo(30) + "' rx='" + upTo(9) + "' ry='" + upTo(6) + "'/></g>";
  }
}

TEST(SvgRender, WhatIsUsedComesOutAsIfItHadBeenWrittenWhereItIsUsed) {
  std::mt19937 random = Repeatable(47);
  const auto upTo = [&random](unsigned count) { return std::to_string(random() % count); };
  for (int round = 0; round < 60; ++round) {
    // A few things to use, and then some uses of them, each set beside a
    // group that holds the same thing and says what the use says.
    std::vector<std::string> things;
    std::string used = "<defs>";
    for (int thing = 0; thing < 4; ++thing) {
      things.push_back(AnyShape(random));
      // Its name goes into its first tag.
      std::string named = things.back();
      named.insert(named.find_first_of(" >"), " id='t" + std::to_string(thing) + "'");
      used += named;
    }
    used += "</defs>";
    std::string written;
    for (int use = 0; use < 6; ++use) {
      const std::size_t which = random() % things.size();
      std::string says;
      if (random() % 2 == 0) {
        says += " fill='#" + upTo(10) + upTo(10) + upTo(10) + "'";
      }
      if (random() % 4 == 0) {
        says += " fill-opacity='." + upTo(10) + "'";
      }
      std::string moved;
      std::string place;
      switch (random() % 3) {
        case 0:
          moved = " transform='translate(" + upTo(12) + " " + upTo(8) + ") rotate(" + upTo(40) + ") scale(." +
                  std::to_string(5 + (random() % 5)) + ")'";
          place = moved;
          break;
        case 1: {
          const std::string x = upTo(15);
          const std::string y = upTo(9);
          moved.append(" x='").append(x).append("' y='").append(y).append("'");
          place.append(" transform='translate(").append(x).append(" ").append(y).append(")'");
          break;
        }
        default:
          break;
      }
      used.append("<use href='#t").append(std::to_string(which)).append("'").append(says).append(moved).append("/>");
      written.append("<g").append(says).append(place).append(">").append(things[which]).append("</g>");
    }
    const Drawing byUse = DrawBody(used, 60, 40);
    const Drawing inPlace = DrawBody(written, 60, 40);
    ASSERT_EQ(byUse.gray, inPlace.gray) << round << ": " << used;
    ASSERT_EQ(byUse.report.shapes, inPlace.report.shapes) << round;
    ASSERT_EQ(byUse.report.commands, inPlace.report.commands) << round;
    ASSERT_EQ(byUse.report.leftOut, inPlace.report.leftOut) << round;
    // And the same however the picture and the file are cut up.
    const Drawing cut = Draw(Svg(60, 40, used), 60, 40, 1 + (random() % 40), 1 + (random() % 50));
    ASSERT_EQ(cut.gray, byUse.gray) << round << ": " << used;
    ASSERT_EQ(cut.report.reused, byUse.report.reused) << round;
    ASSERT_EQ(cut.report.kept, byUse.report.kept) << round;
  }
}

TEST(SvgRender, AUseOfWhatCannotBeFoundDrawsNothingAndSaysSo) {
  const std::string longest(SvgRenderer::kLongestId, 'n');
  const std::string cases[] = {
      With(kBlock, "<use href='#b'/>"),  // No such name.
      With(kBlock, "<use href='#A'/>"),
      With(kBlock, "<use href='#ab'/>"),                                    // A name that one begins,
      "<defs><rect id='ab' width='4' height='3'/></defs><use href='#a'/>",  // and one that begins a name.
      With(kBlock, "<use/>"),                                               // No name at all.
      With(kBlock, "<use href=''/>"),
      With(kBlock, "<use href='#'/>"),
      With(kBlock, "<use href='a'/>"),  // Not a name in this file.
      With(kBlock, "<use href='other.svg#a'/>"),
      With(kBlock, "<use href='#a b'/>"),
      "<use href='#a'/><defs><rect id='a' width='4' height='3'/></defs>",      // Defined after it is used.
      "<defs><rect id='&#97;' width='4' height='3'/></defs><use href='#a'/>",  // A name written with an entity.
      "<defs><rect id='x&#97;' width='4' height='3'/></defs><use href='#xa'/>",
      "<defs><rect id='x&#97;' width='4' height='3'/></defs><use href='#x&amp;#97;'/>",
      "<defs><rect id='' width='4' height='3'/></defs><use href='#'/>",
      "<defs><rect id=\"a'b\" width='4' height='3'/></defs><use href='#a'/>",
      "<defs><rect id='" + longest + "n' width='4' height='3'/></defs><use href='#" + longest + "n'/>",  // Too long.
      "<defs><rect xml:id='a' width='4' height='3'/></defs><use href='#a'/>",
  };
  for (const std::string& body : cases) {
    const Drawing drawing = DrawBody(body);
    EXPECT_TRUE(drawing.IsBox(0, 0, 0, 0, kBlack)) << body;
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kUse})) << body;
  }
  // A name as long as a name can be is found.
  EXPECT_TRUE(DrawBody("<defs><rect id='" + longest + "' width='4' height='3'/></defs><use href='#" + longest + "'/>")
                  .IsBox(0, 0, 4, 3, kBlack));
  // Only what `defs` holds can be used: a shape that is drawn where it
  // stands is not kept.
  const Drawing outside = DrawBody("<rect id='a' width='4' height='3'/><use href='#a' x='10'/>");
  EXPECT_TRUE(outside.IsBox(0, 0, 4, 3, kBlack));
  EXPECT_EQ(outside.report.leftOut, Bits({SvgLeftOut::kUse}));
  // Nor what a `defs` holds that is itself inside something passed over.
  const Drawing passedOver = DrawBody("<text><defs><rect id='a' width='4' height='3'/></defs></text><use href='#a'/>");
  EXPECT_TRUE(passedOver.IsBox(0, 0, 0, 0, kBlack));
  EXPECT_EQ(passedOver.report.leftOut, Bits({SvgLeftOut::kText, SvgLeftOut::kUse}));
  // What is written inside a `use` is not drawn, whether or not the use is.
  const Drawing within = DrawBody(With(kBlock,
                                       "<use href='#a'><rect width='9' height='9'/></use>"
                                       "<use href='#none'><rect width='9' height='9'/></use>"));
  EXPECT_TRUE(within.IsBox(0, 0, 4, 3, kBlack));
  // Where two things have one name, it is the first to end that is found.
  const Drawing twice = DrawBody(
      "<defs><rect id='a' width='4' height='3'/><rect id='a' width='9' height='9'/></defs>"
      "<use href='#a'/>");
  EXPECT_TRUE(twice.IsBox(0, 0, 4, 3, kBlack));
}

TEST(SvgRender, KeepsWhatThereIsRoomForAndSaysWhenThereWasNotEnough) {
  const std::string_view first = "<rect id='a' width='4' height='4'/>";
  const std::string_view second = "<rect id='b' x='10' width='4' height='4'/>";
  const std::string file =
      Svg(40, 20, "<defs>" + std::string(first) + std::string(second) + "</defs><use href='#a'/><use href='#b'/>");
  const Drawing roomy = Draw(file, 40, 20);
  EXPECT_EQ(roomy.report.leftOut, 0u);
  EXPECT_DOUBLE_EQ(roomy.Covered(), 32);
  // It takes what `defs` holds, to the end of the tag that closes it, and
  // a record for each thing in it that has a name.
  const std::size_t held = first.size() + second.size() + std::string_view("</defs>").size();
  EXPECT_EQ(roomy.report.kept, held);
  EXPECT_EQ(roomy.report.reused, first.size() + second.size());
  const std::size_t needed = held + (2 * SvgRenderer::kBytesPerDefinition);
  for (const std::size_t piece : {SIZE_MAX, std::size_t{1}, std::size_t{7}}) {
    const Drawing exact = Draw(file, 40, 20, 0, piece, needed);
    EXPECT_EQ(exact.gray, roomy.gray) << piece;
    EXPECT_EQ(exact.report.leftOut, 0u) << piece;
    // A byte less, and it says so, though what it kept whole it draws.
    const Drawing less = Draw(file, 40, 20, 0, piece, needed - 1);
    EXPECT_EQ(less.report.leftOut, Bits({SvgLeftOut::kNoRoom})) << piece;
    EXPECT_EQ(less.gray, roomy.gray) << piece;
    EXPECT_EQ(less.report.kept, held - 1) << piece;
    // Room for the first thing and some of the second: the first is
    // drawn, and the second is not.
    const Drawing half = Draw(file, 40, 20, 0, piece, first.size() + SvgRenderer::kBytesPerDefinition + 20);
    EXPECT_TRUE(half.IsBox(0, 0, 4, 4, kBlack)) << piece;
    EXPECT_EQ(half.report.leftOut, Bits({SvgLeftOut::kNoRoom, SvgLeftOut::kUse})) << piece;
    EXPECT_EQ(half.report.kept, first.size() + 20) << piece;
    // Room for the first thing, and not for the record of it.
    const Drawing unrecorded = Draw(file, 40, 20, 0, piece, first.size() + SvgRenderer::kBytesPerDefinition - 1);
    EXPECT_TRUE(unrecorded.IsBox(0, 0, 0, 0, kBlack)) << piece;
    EXPECT_EQ(unrecorded.report.leftOut, Bits({SvgLeftOut::kNoRoom, SvgLeftOut::kUse})) << piece;
    // And none at all.
    const Drawing none = Draw(file, 40, 20, 0, piece, 0);
    EXPECT_TRUE(none.IsBox(0, 0, 0, 0, kBlack)) << piece;
    EXPECT_EQ(none.report.leftOut, Bits({SvgLeftOut::kNoRoom, SvgLeftOut::kUse})) << piece;
    EXPECT_EQ(none.report.kept, 0u) << piece;
    EXPECT_EQ(none.status, SvgStatus::kOk) << piece;
  }
  // Something there was room for the first part of is not drawn in part.
  const std::string_view inner = "<g id='a'><g><rect width='4' height='4'/></g>";
  const std::string nested =
      Svg(40, 20, "<defs>" + std::string(inner) + "<rect x='10' width='4' height='4'/></g></defs><use href='#a'/>");
  EXPECT_DOUBLE_EQ(Draw(nested, 40, 20).Covered(), 32);
  for (const std::size_t piece : {SIZE_MAX, std::size_t{1}}) {
    const Drawing part = Draw(nested, 40, 20, 0, piece, inner.size() + (2 * SvgRenderer::kBytesPerDefinition));
    EXPECT_TRUE(part.IsBox(0, 0, 0, 0, kBlack)) << piece;
    EXPECT_EQ(part.report.leftOut, Bits({SvgLeftOut::kNoRoom, SvgLeftOut::kUse})) << piece;
  }
  // Things with names, one inside another: as deep as kDeepestNamed each
  // can be used, and one deeper cannot, though what it is inside can.
  std::string deep = "<defs>";
  for (std::size_t level = 0; level <= SvgRenderer::kDeepestNamed; ++level) {
    deep += "<g id='n" + std::to_string(level) + "'>";
  }
  deep += "<rect width='4' height='3'/>";
  for (std::size_t level = 0; level <= SvgRenderer::kDeepestNamed; ++level) {
    deep += "</g>";
  }
  deep += "</defs>";
  const Drawing outermost = DrawBody(deep + "<use href='#n0'/>");
  EXPECT_TRUE(outermost.IsBox(0, 0, 4, 3, kBlack));
  EXPECT_EQ(outermost.report.leftOut, Bits({SvgLeftOut::kNoRoom}));
  const Drawing deepest =
      DrawBody(deep + "<use href='#n" + std::to_string(SvgRenderer::kDeepestNamed - 1) + "' x='10'/>");
  EXPECT_TRUE(deepest.IsBox(10, 0, 14, 3, kBlack));
  const Drawing deeper = DrawBody(deep + "<use href='#n" + std::to_string(SvgRenderer::kDeepestNamed) + "'/>");
  EXPECT_TRUE(deeper.IsBox(0, 0, 0, 0, kBlack));
  EXPECT_EQ(deeper.report.leftOut, Bits({SvgLeftOut::kNoRoom, SvgLeftOut::kUse}));
  // A file that defines nothing needs none, and says nothing of it.
  const Drawing plain = Draw(Svg(40, 20, "<defs></defs><rect width='4' height='4'/>"), 40, 20, 0, SIZE_MAX, 0);
  EXPECT_EQ(plain.report.leftOut, Bits({SvgLeftOut::kNoRoom}));
  EXPECT_EQ(Draw(Svg(40, 20, "<defs/><rect width='4' height='4'/>"), 40, 20, 0, SIZE_MAX, 0).report.leftOut, 0u);
}

TEST(SvgRender, WhatOneFileDefinedIsGoneWhenTheNextBegins) {
  std::vector<std::uint8_t> gray(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  std::vector<std::uint8_t> kept(4096);
  Filler filler;
  ASSERT_TRUE(filler.Begin(gray, 40, 0, cells));
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  const auto draw = [&renderer, &filler](std::string_view file) {
    renderer.Begin(&filler, Pixels(40), Pixels(20));
    while (!file.empty()) {
      file.remove_prefix(renderer.Step(file));
    }
    return renderer.Finish();
  };
  EXPECT_EQ(draw(Svg(40, 20, With(kBlock, "<use href='#a'/>"))), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().shapes, 1u);
  EXPECT_EQ(renderer.Report().leftOut, 0u);
  // The same name, in a file that does not define it.
  EXPECT_EQ(draw(Svg(40, 20, "<use href='#a' x='10'/>")), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().shapes, 0u);
  EXPECT_EQ(renderer.Report().kept, 0u);
  EXPECT_EQ(renderer.Report().leftOut, Bits({SvgLeftOut::kUse}));
  // A file that ends in the middle of what it defines, or of what it is
  // using, leaves nothing behind either.
  EXPECT_EQ(draw("<svg width='40' height='20'><defs><g id='a'><rect width='4' height='3'/>"), SvgStatus::kTruncated);
  EXPECT_EQ(draw(Svg(40, 20, With(kBlock, "<use href='#a'/>")).substr(0, 97)), SvgStatus::kTruncated);
  EXPECT_EQ(draw(Svg(40, 20, "<use href='#a' x='10'/>")), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().leftOut, Bits({SvgLeftOut::kUse}));
  EXPECT_EQ(draw(Svg(40, 20, With(kBlock, "<use href='#a' x='20' y='10'/>"))), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().shapes, 1u);
  EXPECT_EQ(gray[(std::size_t{11} * 40) + 21], kBlack);
}

TEST(SvgRender, ReadsWhatIsUsedEvenWithNothingToDrawInto) {
  const std::string file = Svg(40, 20,
                               "<defs><g id='g'><text>Words</text><rect width='2' height='2'/></g></defs>"
                               "<use href='#g'/><use href='#g' x='9'/><use href='#none'/>");
  const Drawing drawn = Draw(file, 40, 20);
  std::vector<std::uint8_t> kept(4096);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  renderer.Begin(nullptr, 0, 0);
  std::string_view bytes = file;
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().leftOut, drawn.report.leftOut);
  EXPECT_EQ(renderer.Report().leftOut, Bits({SvgLeftOut::kText, SvgLeftOut::kUse}));
  EXPECT_EQ(renderer.Report().elements, drawn.report.elements);
  EXPECT_EQ(renderer.Report().kept, drawn.report.kept);
  EXPECT_EQ(renderer.Report().reused, drawn.report.reused);
  EXPECT_EQ(renderer.Report().shapes, 0u);
  EXPECT_EQ(drawn.report.shapes, 2u);
}

TEST(SvgRender, AFileCannotAskForEndlessWorkByUsingWhatUsesThings) {
  // Something that uses itself is drawn as deep as one use may be inside
  // another, and no deeper.
  const Drawing itself = DrawBody(
      "<defs><g id='a'><rect width='2' height='2'/><use href='#a' x='3'/></g></defs>"
      "<use href='#a'/>");
  EXPECT_EQ(itself.status, SvgStatus::kOk);
  EXPECT_EQ(itself.report.leftOut, Bits({SvgLeftOut::kUseLimit}));
  EXPECT_EQ(itself.report.shapes, SvgRenderer::kDeepestUse);
  EXPECT_DOUBLE_EQ(itself.Covered(), 4.0 * SvgRenderer::kDeepestUse);
  // A chain exactly that deep is drawn whole, and one more is not.
  std::string chain = "<defs><rect id='u0' width='2' height='2'/>";
  for (std::size_t link = 1; link <= SvgRenderer::kDeepestUse; ++link) {
    chain += "<use id='u" + std::to_string(link) + "' href='#u" + std::to_string(link - 1) + "' x='3'/>";
  }
  chain += "</defs>";
  const Drawing deep = DrawBody(chain + "<use href='#u" + std::to_string(SvgRenderer::kDeepestUse - 1) + "'/>");
  EXPECT_EQ(deep.report.leftOut, 0u);
  EXPECT_TRUE(deep.IsBox(9, 0, 11, 2, kBlack));
  const Drawing deeper = DrawBody(chain + "<use href='#u" + std::to_string(SvgRenderer::kDeepestUse) + "'/>");
  EXPECT_EQ(deeper.report.leftOut, Bits({SvgLeftOut::kUseLimit}));
  EXPECT_TRUE(deeper.IsBox(0, 0, 0, 0, kBlack));

  // A `use` is a group round what it draws, and inside as many groups as
  // may be open at once there is no room for one more.
  std::string nest;
  for (std::size_t group = 1; group < SvgRenderer::kDeepest; ++group) {
    nest += "<g>";
  }
  std::string unnest;
  for (std::size_t group = 1; group < SvgRenderer::kDeepest; ++group) {
    unnest += "</g>";
  }
  const Drawing nested = DrawBody(With(kBlock, nest + "<use href='#a'/>" + unnest + "<use href='#a' x='10'/>"));
  EXPECT_EQ(nested.report.leftOut, Bits({SvgLeftOut::kTooDeep}));
  EXPECT_TRUE(nested.IsBox(10, 0, 14, 3, kBlack));
  const Drawing shallower =
      DrawBody(With(kBlock, nest.substr(3) + "<use href='#a'/>" + unnest.substr(4) + "<use href='#a' x='10'/>"));
  EXPECT_EQ(shallower.report.leftOut, 0u);
  EXPECT_DOUBLE_EQ(shallower.Covered(), 24);

  // Things that use a hundred things that use a hundred things: a few
  // thousand bytes that ask for tens of millions to be read. It stops at
  // its limit, give or take what was being read when it reached it.
  std::string many = "<defs><rect id='m0' width='1' height='1'/>";
  for (int level = 1; level <= 3; ++level) {
    many += "<g id='m" + std::to_string(level) + "'>";
    for (int use = 0; use < 100; ++use) {
      many += "<use href='#m" + std::to_string(level - 1) + "'/>";
    }
    many += "</g>";
  }
  many += "</defs><use href='#m3'/><use href='#m3'/><rect x='10' width='4' height='4'/>";
  ASSERT_LT(many.size(), 6000u);
  const Drawing limited = DrawBody(many);
  EXPECT_EQ(limited.status, SvgStatus::kOk);
  EXPECT_EQ(limited.report.leftOut, Bits({SvgLeftOut::kUseLimit}));
  EXPECT_GE(limited.report.reused, SvgRenderer::kMostReused);
  EXPECT_LT(limited.report.reused, SvgRenderer::kMostReused + (SvgRenderer::kDeepestUse * many.size()));
  // What comes after is drawn as ever.
  EXPECT_EQ(limited.At(12, 2), kBlack);
  // Each call read a little, so there were many.
  EXPECT_GT(limited.calls, SvgRenderer::kMostReused / SvgRenderer::kReusedPerStep);
  EXPECT_LT(limited.calls, 4 * (many.size() + limited.report.reused));
}

TEST(SvgRender, ACallDoesNoMoreThanItsBoundOnWhatItHasKept) {
  // As the test of a call's bound above, with all of it kept and then
  // used: an outline of thousands of commands, one short enough to hold,
  // hundreds of small shapes, and thousands of tags that draw nothing.
  std::string body = "<defs><path id='long' d='M0 0";
  for (int i = 0; i < 4000; ++i) {
    body += "h1";
  }
  body += "'/><path id='held' d='M0 0";
  for (int i = 0; i < 160; ++i) {
    body += "v1";
  }
  body += "' fill='#000'/><g id='shapes'>";
  for (int i = 0; i < 300; ++i) {
    body += "<rect width='3' height='3' rx='1'/><circle r='2'/>";
  }
  body += "</g><g id='nothing'>";
  for (int i = 0; i < 3000; ++i) {
    body += "<g/>";
  }
  // And something with an attribute of thousands of bytes that says
  // nothing to the renderer, which only the bound on bytes cuts up.
  body += "</g><rect id='wordy' width='2' height='2' data-note='" + std::string(6000, 'n') + "'/>";
  body +=
      "</defs><use href='#long'/><use href='#held'/><use href='#shapes'/><use href='#nothing'/><use href='#wordy'/>";
  const std::string file = Svg(40, 20, body);

  std::vector<std::uint8_t> gray(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  std::vector<std::uint8_t> kept(65536);
  Filler filler;
  ASSERT_TRUE(filler.Begin(gray, 40, 0, cells));
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  renderer.Begin(&filler, Pixels(40), Pixels(20));
  std::string_view bytes = file;
  std::size_t callsThatReused = 0;
  std::uint32_t mostCommands = 0;
  std::uint32_t mostShapes = 0;
  std::uint32_t mostElements = 0;
  std::uint32_t mostReused = 0;
  while (!bytes.empty()) {
    const SvgReport before = renderer.Report();
    const std::size_t taken = renderer.Step(bytes);
    const SvgReport& after = renderer.Report();
    mostCommands = std::max(mostCommands, after.commands - before.commands);
    mostShapes = std::max(mostShapes, after.shapes - before.shapes);
    mostElements = std::max(mostElements, after.elements - before.elements);
    mostReused = std::max(mostReused, after.reused - before.reused);
    ASSERT_LE(after.lines - before.lines, SvgRenderer::kMostCommandsPerStep * PathFlattener::kMostLinesPerArc);
    // It reads the file or what it has kept, not both in one call.
    ASSERT_TRUE(taken == 0 || after.reused == before.reused);
    callsThatReused += after.reused != before.reused ? 1 : 0;
    bytes.remove_prefix(taken);
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_EQ(renderer.Report().leftOut, 0u);
  EXPECT_LE(mostCommands, SvgRenderer::kMostCommandsPerStep);
  EXPECT_GE(mostCommands, SvgRenderer::kCommandsPerStep);
  EXPECT_EQ(mostShapes, 1u);
  EXPECT_LE(mostElements, SvgRenderer::kTokensPerStep);
  EXPECT_EQ(mostReused, SvgRenderer::kReusedPerStep);
  // The work on what was kept was shared out as the work on a file is:
  // about a call for every sixteen commands, one for every shape, and one
  // for every sixteen tags that drew nothing.
  EXPECT_GT(callsThatReused, (4160 / 17) + 600 + (3000 / 16));
  EXPECT_EQ(renderer.Report().commands, 4001u + 161u + (300 * 16) + 5);
  EXPECT_EQ(renderer.Report().shapes, 603u);
  EXPECT_EQ(renderer.Report().reused, renderer.Report().kept - std::string_view("</defs>").size());
}

TEST(SvgRender, ACallLooksOnlyOnceForWhatAUseNames) {
  // Looking for a name goes through everything that is defined. So of
  // uses that find nothing, which are otherwise no work at all, one is
  // enough for a call.
  std::string body = "<defs>";
  for (int thing = 0; thing < 50; ++thing) {
    body += "<rect id='thing-" + std::to_string(thing) + "' width='2' height='2'/>";
  }
  body += "</defs>";
  for (int use = 0; use < 200; ++use) {
    body += "<use href='#thing-none'/>";
  }
  const std::string defined = Svg(40, 20, body);
  const Drawing looked = Draw(defined, 40, 20);
  EXPECT_EQ(looked.status, SvgStatus::kOk);
  EXPECT_EQ(looked.report.leftOut, Bits({SvgLeftOut::kUse}));
  // The same file with nothing to look for is read sixteen things at a
  // call, and each use is four.
  std::string plain = defined;
  for (std::size_t at = plain.find(" href='#thing-none'"); at != std::string::npos;
       at = plain.find(" href='#thing-none'", at)) {
    plain.replace(at, std::string_view(" href='#thing-none'").size(), " x='1'");
  }
  const Drawing unlooked = Draw(plain, 40, 20);
  EXPECT_EQ(unlooked.report.leftOut, Bits({SvgLeftOut::kUse}));
  EXPECT_GE(looked.calls, unlooked.calls + 140);
  EXPECT_GE(looked.calls, 200u);
}

// ---- The edges of an svg inside another ----

constexpr std::string_view kAll = "<rect x='-50' y='-50' width='200' height='200'/>";

TEST(SvgRender, AnSvgInsideAnotherCutsOffWhatIsInItAtItsEdges) {
  const auto inner = [](std::string_view attributes, std::string_view holds = kAll) {
    return "<svg " + std::string(attributes) + ">" + std::string(holds) + "</svg>";
  };
  const Drawing cut = DrawBody(inner("x='10' y='5' width='8' height='6'"));
  EXPECT_TRUE(cut.IsBox(10, 5, 18, 11, kBlack));
  EXPECT_EQ(cut.report.leftOut, 0u);
  // Whatever it holds: groups, and what a `use` draws.
  EXPECT_TRUE(DrawBody(With("<defs><g id='all'><rect x='-50' y='-50' width='200' height='200'/></g></defs>",
                            inner("x='10' y='5' width='8' height='6'", "<g><g><use href='#all'/></g></g>")))
                  .IsBox(10, 5, 18, 11, kBlack));
  // "hidden" and "scroll" say what it does anyway.
  for (const std::string_view overflow : {"overflow='hidden'", "overflow='scroll'", "style='overflow: hidden'"}) {
    EXPECT_TRUE(
        DrawBody(inner("x='10' y='5' width='8' height='6' " + std::string(overflow))).IsBox(10, 5, 18, 11, kBlack))
        << overflow;
  }
  // Unless it says that what is in it may show beyond them.
  for (const std::string_view overflow : {"overflow='visible'", "overflow='auto'", "style='overflow:visible'"}) {
    const Drawing whole = DrawBody(inner("x='10' y='5' width='8' height='6' " + std::string(overflow)));
    EXPECT_TRUE(whole.IsBox(0, 0, 40, 20, kBlack)) << overflow;
    EXPECT_EQ(whole.report.leftOut, 0u) << overflow;
  }
  // A view box fitted so as to cover it runs over two of its edges.
  EXPECT_TRUE(
      DrawBody(inner("x='10' y='5' width='8' height='8' viewBox='0 0 20 10' preserveAspectRatio='xMidYMid slice'",
                     "<rect width='20' height='10'/>"))
          .IsBox(10, 5, 18, 13, kBlack));
  // One inside another: what is in both.
  EXPECT_TRUE(DrawBody(inner("x='10' y='5' width='8' height='6'", inner("x='4' y='-2' width='10' height='5'")))
                  .IsBox(14, 5, 18, 8, kBlack));
  // And nothing, where they do not meet.
  EXPECT_TRUE(DrawBody(inner("x='10' y='5' width='8' height='6'", inner("x='9' y='0' width='10' height='5'")))
                  .IsBox(0, 0, 0, 0, kBlack));
  // The inner one may let things show beyond its own edges: they are
  // still within the outer one's.
  EXPECT_TRUE(DrawBody(inner("x='10' y='5' width='8' height='6'",
                             inner("x='4' y='-2' width='10' height='5' overflow='visible'")))
                  .IsBox(10, 5, 18, 11, kBlack));
  // A group that moves it and makes it larger moves and enlarges its
  // edges, and one that turns it over leaves them where they then are.
  EXPECT_TRUE(DrawBody("<g transform='translate(4 2) scale(2)'>" + inner("x='1' y='1' width='3' height='2'") + "</g>")
                  .IsBox(6, 4, 12, 8, kBlack));
  EXPECT_TRUE(
      DrawBody("<g transform='translate(20 0) scale(-1 1)'>" + inner("x='2' y='3' width='5' height='4'") + "</g>")
          .IsBox(13, 3, 18, 7, kBlack));
  // The drawing's own element cuts nothing off but at the picture's edges.
  EXPECT_TRUE(DrawBody(kAll).IsBox(0, 0, 40, 20, kBlack));
}

TEST(SvgRender, AnEdgeThatGoesThroughAPixelLeavesItTheShareThatIsInside) {
  // Cut off at edges between pixels' edges, it is as a box drawn there.
  const Drawing cut = DrawBody("<svg x='10.5' y='5.25' width='4.25' height='2'>" + std::string(kAll) + "</svg>");
  const Drawing drawn = DrawBody("<rect x='10.5' y='5.25' width='4.25' height='2'/>");
  for (std::size_t y = 0; y < 20; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      ASSERT_NEAR(cut.At(x, y), drawn.At(x, y), 1) << x << "," << y;
    }
  }
  EXPECT_NEAR(cut.Covered(), 8.5, 0.02);
  EXPECT_NEAR(cut.At(10, 5), 255 * (1 - (0.5 * 0.75)), 1);
  // A shape's own gray edge and the cut's, in one pixel: a circle cut in
  // half covers half of what the circle does.
  const Drawing half = DrawBody("<svg x='0' y='0' width='20' height='20'><circle cx='20' cy='10' r='8.5'/></svg>");
  const Drawing all = DrawBody("<circle cx='20' cy='10' r='8.5'/>");
  EXPECT_NEAR(half.Covered(), all.Covered() / 2, 0.2);
  EXPECT_EQ(half.At(19, 10), kBlack);
  EXPECT_EQ(half.At(20, 10), kWhite);
}

// ---- Placed in another picture ----

// `file` drawn `width` by `height` and put by `place` into a picture of
// `across` by `down`, in strips of `stripRows`.
Drawing DrawPlaced(std::string_view file, std::size_t width, std::size_t height, const Matrix& place,
                   std::size_t across, std::size_t down, std::size_t stripRows) {
  Drawing drawing;
  drawing.width = across;
  drawing.height = down;
  drawing.gray.assign(across * down, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(across, stripRows));
  std::vector<std::uint8_t> kept(8192);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  renderer.PlaceOn(place);
  for (std::size_t top = 0; top < down; top += stripRows) {
    const std::size_t rows = std::min(stripRows, down - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(drawing.gray).subspan(top * across, rows * across), across,
                             static_cast<std::int32_t>(top), cells));
    renderer.Begin(&filler, Pixels(width), Pixels(height));
    for (std::string_view bytes = file; !bytes.empty();) {
      bytes.remove_prefix(renderer.Step(bytes));
    }
    drawing.status = renderer.Finish();
    drawing.report = renderer.Report();
  }
  return drawing;
}

TEST(SvgRender, APictureTurnedByQuartersIsThePictureTurned) {
  // Shapes, a curve, a gradient over a box, and an svg that cuts off what
  // is in it: 20 by 30, and turned into 30 by 20 (or 20 by 30 again).
  const std::string file =
      Svg(20, 30,
          std::string("<linearGradient id='g' x2='0' y2='1'><stop/>"
                      "<stop offset='1' stop-color='#fff'/></linearGradient>") +
              "<rect x='2' y='3' width='9' height='14' fill='url(#g)'/>"
              "<path d='M12 4c6 0 6 9 0 9z' fill='#555'/>"
              "<svg x='3' y='19' width='12' height='7'><circle cx='6' cy='3.5' r='6' fill='#888'/></svg>");
  const Drawing upright = Draw(file, 20, 30);
  EXPECT_EQ(upright.report.leftOut, 0u);
  constexpr Coefficient kOne = kCoefficientOne;
  struct Turn {
    Matrix place;
    std::size_t across = 0;
    std::size_t down = 0;
    // Where the pixel at (x, y) of the upright picture goes.
    std::size_t (*x)(std::size_t, std::size_t) = nullptr;
    std::size_t (*y)(std::size_t, std::size_t) = nullptr;
  };
  const Turn turns[] = {
      {.place = {.a = 0, .b = -kOne, .c = kOne, .d = 0, .e = 0, .f = Pixels(20)},
       .across = 30,
       .down = 20,
       .x = [](std::size_t, std::size_t y) { return y; },
       .y = [](std::size_t x, std::size_t) { return 19 - x; }},
      {.place = {.a = 0, .b = kOne, .c = -kOne, .d = 0, .e = Pixels(30), .f = 0},
       .across = 30,
       .down = 20,
       .x = [](std::size_t, std::size_t y) { return 29 - y; },
       .y = [](std::size_t x, std::size_t) { return x; }},
      {.place = {.a = -kOne, .b = 0, .c = 0, .d = -kOne, .e = Pixels(20), .f = Pixels(30)},
       .across = 20,
       .down = 30,
       .x = [](std::size_t x, std::size_t) { return 19 - x; },
       .y = [](std::size_t, std::size_t y) { return 29 - y; }},
  };
  for (const Turn& turn : turns) {
    for (const std::size_t rows : {std::size_t{1}, std::size_t{7}, turn.down}) {
      const Drawing turned = DrawPlaced(file, 20, 30, turn.place, turn.across, turn.down, rows);
      // What is cut off at the edges of the svg inside still is.
      EXPECT_EQ(turned.report.leftOut, 0u);
      int worst = 0;
      for (std::size_t y = 0; y < 30; ++y) {
        for (std::size_t x = 0; x < 20; ++x) {
          worst = std::max(worst, std::abs(turned.At(turn.x(x, y), turn.y(x, y)) - upright.At(x, y)));
        }
      }
      // A pixel's cover is worked out along rows, and an edge turned to run
      // the other way can come out a part in 255 apart.
      EXPECT_LE(worst, 2) << turn.across << " " << rows;
    }
  }
}

TEST(SvgRender, APictureIsPutWhereItsPlaceSays) {
  // Moved into the middle of a larger picture.
  const std::string file = Svg(10, 10, "<rect width='10' height='10'/>");
  const Drawing moved = DrawPlaced(
      file, 10, 10, {.a = kCoefficientOne, .b = 0, .c = 0, .d = kCoefficientOne, .e = Pixels(15), .f = Pixels(5)}, 40,
      20, 20);
  EXPECT_TRUE(moved.IsBox(15, 5, 25, 15, kBlack));
}

TEST(SvgRender, AnSvgThatIsTurnedIsDrawnWholeAndThatIsReported) {
  // Its edges are no longer the edges of rows and columns of pixels.
  const Drawing turned = DrawBody(
      "<g transform='translate(20 10) rotate(45)'>"
      "<svg width='4' height='4'><rect x='-2' y='-2' width='8' height='8'/></svg></g>");
  EXPECT_EQ(turned.report.leftOut, Bits({SvgLeftOut::kViewportClip}));
  EXPECT_NEAR(turned.Covered(), 64, 0.5);
  // Inside one that is not turned, it is still within that one's edges.
  const Drawing within = DrawBody(
      "<svg x='16' y='6' width='8' height='8'><g transform='translate(4 4) rotate(45)'>"
      "<svg width='4' height='4'><rect x='-20' y='-20' width='40' height='40'/></svg></g></svg>");
  EXPECT_EQ(within.report.leftOut, Bits({SvgLeftOut::kViewportClip}));
  EXPECT_TRUE(within.IsBox(16, 6, 24, 14, kBlack));
  // One that says nothing is to be cut off has nothing to report.
  EXPECT_EQ(DrawBody("<g transform='rotate(45)'><svg overflow='visible'><rect width='4' height='4'/></svg></g>")
                .report.leftOut,
            0u);
  // Slanted one way only, its edges are no more rows and columns of
  // pixels than if it were turned.
  for (const std::string_view slant : {"skewX(30)", "skewY(30)"}) {
    const Drawing slanted =
        DrawBody("<g transform='translate(12 6) " + std::string(slant) +
                 "'><svg width='4' height='4'><rect x='-2' y='-2' width='8' height='8'/></svg></g>");
    EXPECT_EQ(slanted.report.leftOut, Bits({SvgLeftOut::kViewportClip})) << slant;
    EXPECT_NEAR(slanted.Covered(), 64, 0.5) << slant;
  }
}

TEST(SvgRender, ASymbolIsDrawnOnlyWhereItIsUsedAndAsAnSvgThere) {
  const std::string_view defs =
      "<defs><symbol id='s' viewBox='0 0 10 10'>"
      "<rect x='-50' y='-50' width='200' height='200'/></symbol></defs>";
  // Not where it is written, in `defs` or out of it.
  EXPECT_TRUE(DrawBody(defs).IsBox(0, 0, 0, 0, kBlack));
  EXPECT_TRUE(DrawBody("<symbol><rect width='5' height='5'/></symbol>").IsBox(0, 0, 0, 0, kBlack));
  // Used, it is as large as the `use` says, with its view box fitted to
  // that, and cut off at those edges.
  const Drawing sized = DrawBody(With(defs, "<use href='#s' x='5' y='5' width='6' height='4'/>"));
  EXPECT_TRUE(sized.IsBox(5, 5, 11, 9, kBlack));
  EXPECT_EQ(sized.report.leftOut, 0u);
  // With no size it is as large as what the `use` is seen through.
  EXPECT_TRUE(DrawBody(With(defs, "<use href='#s' x='5' y='5'/>")).IsBox(5, 5, 40, 20, kBlack));
  // What is in it is placed by its view box.
  EXPECT_TRUE(DrawBody("<defs><symbol id='s' viewBox='0 0 10 10' preserveAspectRatio='none'>"
                       "<rect x='5' y='5' width='5' height='5'/></symbol></defs>"
                       "<use href='#s' x='10' width='20' height='10'/>")
                  .IsBox(20, 5, 30, 10, kBlack));
  // An `svg` that is used takes the size the `use` gives, where it gives
  // one, in place of its own.
  const std::string_view kept =
      "<defs><svg id='v' width='2' height='2' viewBox='0 0 10 10' preserveAspectRatio='none'>"
      "<rect width='10' height='10'/></svg></defs>";
  EXPECT_TRUE(DrawBody(With(kept, "<use href='#v'/>")).IsBox(0, 0, 2, 2, kBlack));
  EXPECT_TRUE(DrawBody(With(kept, "<use href='#v' x='1' width='6' height='4'/>")).IsBox(1, 0, 7, 4, kBlack));
  EXPECT_TRUE(DrawBody(With(kept, "<use href='#v' height='4'/>")).IsBox(0, 0, 2, 4, kBlack));
  // A size on a `use` of anything else changes nothing.
  EXPECT_TRUE(DrawBody(With(kBlock, "<use href='#a' width='20' height='20'/>")).IsBox(0, 0, 4, 3, kBlack));
}

// ---- A style sheet ----

TEST(SvgRender, AStyleSheetPaintsByClassAndByElementsName) {
  const std::string_view sheet = "<style>.dark{fill:#404040} rect{fill:#c0c0c0} .light , .pale {fill:#e0e0e0}</style>";
  const Drawing drawing = DrawBody(std::string(sheet) +
                                   "<rect width='2' height='2'/>"
                                   "<rect x='4' width='2' height='2' class='dark'/>"
                                   "<rect x='8' width='2' height='2' class='other  pale'/>"
                                   "<path d='M12 0h2v2h-2z' class='dark'/>"
                                   "<path d='M16 0h2v2h-2z' class='nothing'/>");
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.rules, 3u);
  EXPECT_EQ(drawing.At(0, 0), 0xC0);
  // A class wins over an element's name, wherever the rules are.
  EXPECT_EQ(drawing.At(4, 0), 0x40);
  EXPECT_EQ(drawing.At(8, 1), 0xE0);
  EXPECT_EQ(drawing.At(12, 0), 0x40);
  EXPECT_EQ(drawing.At(16, 0), kBlack);
  // Between two rules that count the same, the later wins.
  EXPECT_EQ(
      DrawBody("<style>.a{fill:#404040}.b{fill:#808080}</style><rect width='2' height='2' class='b a'/>").At(0, 0),
      0x80);
  // A name is the whole of a class's name, letter for letter.
  EXPECT_EQ(DrawBody("<style>.a{fill:#808080}</style><rect width='2' height='2' class='ab A'/>").At(0, 0), kBlack);
}

TEST(SvgRender, WhatARuleSetsWinsOverAnAttributeAndLosesToStyle) {
  const std::string_view sheet = "<style>.a{fill:#808080}</style>";
  EXPECT_EQ(DrawBody(std::string(sheet) + "<rect width='2' height='2' fill='#404040' class='a'/>").At(0, 0), 0x80);
  EXPECT_EQ(DrawBody(std::string(sheet) + "<rect class='a' fill='#404040' width='2' height='2'/>").At(0, 0), 0x80);
  EXPECT_EQ(DrawBody(std::string(sheet) + "<rect width='2' height='2' style='fill:#404040' class='a'/>").At(0, 0),
            0x40);
  // What a rule sets on a group is handed on, as if written there.
  const Drawing group = DrawBody(
      "<style>.half{fill-opacity:.5} .gone{display:none} .grey{fill:#808080}</style>"
      "<g class='half'><rect width='4' height='4'/><rect x='6' width='4' height='4' class='grey'/></g>"
      "<g class='gone'><rect x='12' width='4' height='4'/></g>");
  EXPECT_NEAR(group.At(1, 1), 128, 1);
  EXPECT_NEAR(group.At(7, 1), 191, 1);
  EXPECT_EQ(group.At(13, 1), kWhite);
  // What it cannot read it reports, and the rest of the rule is used.
  const Drawing bad = DrawBody("<style>.a{fill:blurple;fill-opacity:.5}</style><rect width='2' height='2' class='a'/>");
  EXPECT_EQ(bad.report.leftOut, Bits({SvgLeftOut::kBadValue}));
  EXPECT_NEAR(bad.At(0, 0), 128, 1);
}

TEST(SvgRender, ASheetStylesWhatComesAfterItAndSaysWhenItCameLate) {
  const Drawing drawing = DrawBody(
      "<rect width='2' height='2' class='a'/><style>.a{fill:#808080}</style>"
      "<rect x='4' width='2' height='2' class='a'/>");
  EXPECT_EQ(drawing.At(0, 0), kBlack);
  EXPECT_EQ(drawing.At(4, 0), 0x80);
  EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kStyleSheet}));
  // A sheet after only groups, or in `defs`, is in time.
  EXPECT_EQ(DrawBody("<g><style>.a{fill:#808080}</style></g><rect width='2' height='2' class='a'/>").At(0, 0), 0x80);
  const Drawing inDefs =
      DrawBody("<defs><style><![CDATA[ .a{fill:#808080} ]]></style></defs><rect width='2' height='2' class='a'/>");
  EXPECT_EQ(inDefs.At(0, 0), 0x80);
  EXPECT_EQ(inDefs.report.leftOut, 0u);
}

TEST(SvgRender, WhatIsUsedIsStyledByTheSheetAsWhereItIsDrawn) {
  const Drawing drawing = DrawBody(
      "<style>.a{fill:#808080}</style><defs><rect id='r' class='a' width='2' height='2'/></defs>"
      "<use href='#r'/><use href='#r' x='4'/>");
  EXPECT_EQ(drawing.At(0, 0), 0x80);
  EXPECT_EQ(drawing.At(4, 0), 0x80);
}

TEST(SvgRender, ASheetIsKeptInTheMemoryForDefinitionsAndWhatDoesNotFitIsSaid) {
  const std::string file = Svg(40, 20,
                               "<style>.a{fill:#808080}.b{fill:#404040}</style><rect width='2' height='2' class='a'/>"
                               "<rect x='4' width='2' height='2' class='b'/>");
  const Drawing roomy = Draw(file, 40, 20);
  EXPECT_EQ(roomy.report.rules, 2u);
  // Each rule is a record, eight bytes, and what it says: a byte for how
  // many selectors, two and the name for each, a byte for how many things
  // it sets, and two and the value for each.
  const std::size_t rule = SvgRenderer::kBytesPerDefinition + 1 + 2 + 1 + 1 + 2 + 7;
  const Drawing room = Draw(file, 40, 20, 0, SIZE_MAX, 2 * rule);
  EXPECT_EQ(room.gray, roomy.gray);
  EXPECT_EQ(room.report.leftOut, 0u);
  const Drawing less = Draw(file, 40, 20, 0, SIZE_MAX, (2 * rule) - 1);
  EXPECT_EQ(less.report.rules, 1u);
  EXPECT_EQ(less.report.leftOut, Bits({SvgLeftOut::kNoRoom}));
  EXPECT_EQ(less.At(0, 0), 0x80);
  EXPECT_EQ(less.At(4, 0), kBlack);
  // A rule longer than one is kept for is left out, and said.
  std::string many = "<style>";
  for (int selector = 0; selector < 6; ++selector) {
    many += ".class-number-" + std::to_string(selector) + ",";
  }
  many += ".a{fill:#808080}</style><rect width='2' height='2' class='a'/>";
  const Drawing longRule = DrawBody(many);
  EXPECT_EQ(longRule.At(0, 0), 0x80);
  std::string longer = "<style>.a{fill:#808080";
  for (int setting = 0; setting < 25; ++setting) {
    longer += ";fill-opacity:0.99999999";
  }
  longer += "}</style><rect width='2' height='2' class='a'/>";
  const Drawing tooLong = DrawBody(longer);
  EXPECT_EQ(tooLong.At(0, 0), kBlack);
  EXPECT_EQ(tooLong.report.leftOut, Bits({SvgLeftOut::kStyleSheet}));
}

TEST(SvgRender, ACallStylesOneElementAtTheMost) {
  // Styling an element goes through every rule, so each element that is
  // styled is enough for a call.
  std::string body = "<style>";
  for (int rule = 0; rule < 30; ++rule) {
    body += ".c" + std::to_string(rule) + "{fill:#808080}";
  }
  body += "</style>";
  for (int element = 0; element < 200; ++element) {
    body += "<g/>";
  }
  const Drawing styled = Draw(Svg(40, 20, body), 40, 20);
  EXPECT_EQ(styled.report.rules, 30u);
  EXPECT_GE(styled.calls, 200u);
}

// ---- Gradients ----

// The gray a gradient from black to white has at `along`, from 0 at its
// start to 1 at its end, held there beyond them.
double BlackToWhite(double along) { return 255.0 * std::clamp(along, 0.0, 1.0); }

// Whether every pixel wholly inside the shape from (left, top) to (right,
// bottom) is within a gray or so of what `gray` says for the middle of it.
bool GradedAs(const Drawing& drawing, std::size_t left, std::size_t top, std::size_t right, std::size_t bottom,
              const std::function<double(double, double)>& gray, double within = 1.01) {
  for (std::size_t y = top; y < bottom; ++y) {
    for (std::size_t x = left; x < right; ++x) {
      const double wanted = gray(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      if (std::abs(drawing.At(x, y) - wanted) > within) {
        ADD_FAILURE() << "the pixel at " << x << "," << y << " is " << static_cast<int>(drawing.At(x, y)) << ", not "
                      << wanted;
        return false;
      }
    }
  }
  return true;
}

constexpr std::string_view kBlackToWhite = "<stop offset='0' stop-color='#000'/><stop offset='1' stop-color='#fff'/>";

std::string Gradient(std::string_view attributes, std::string_view stops = kBlackToWhite) {
  return "<defs><linearGradient id='g' " + std::string(attributes) + ">" + std::string(stops) +
         "</linearGradient></defs>";
}

TEST(SvgRender, ALinearGradientGivesEachPixelTheGrayWhereItLiesAlongIt) {
  // Across the shape's own box, unless it says otherwise: from its left to
  // its right.
  const Drawing drawing = DrawBody(Gradient("") + "<rect x='10' y='2' width='20' height='10' fill='url(#g)'/>");
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.gradients, 1u);
  EXPECT_TRUE(GradedAs(drawing, 10, 2, 30, 12, [](double x, double) { return BlackToWhite((x - 10) / 20); }));
  EXPECT_EQ(drawing.At(9, 5), kWhite);
  // Down, and part of the way, in the box's own terms.
  const Drawing down =
      DrawBody(Gradient("x1='0' y1='25%' x2='0' y2='.75'") + "<rect y='4' width='8' height='16' fill='url(#g)'/>");
  EXPECT_TRUE(GradedAs(down, 0, 4, 8, 20, [](double, double y) { return BlackToWhite((y - 8) / 8); }));
  // In the space the shape is drawn in, where a percentage is of what the
  // drawing is seen through.
  const Drawing user = DrawBody(Gradient("gradientUnits='userSpaceOnUse' x1='10%' x2='30'") +
                                "<g transform='translate(1 0)'><rect width='39' height='20' fill='url(#g)'/></g>");
  EXPECT_TRUE(GradedAs(user, 1, 0, 40, 20, [](double x, double) { return BlackToWhite((x - 1 - 4) / 26); }));
}

TEST(SvgRender, AGradientIsLaidOutByItsTransformAndTheShapesAsTheyAre) {
  // Slanted, stretched and moved, both: the gray of each pixel is found
  // here in floating point, by taking its middle back into the gradient's
  // own space and seeing how far along it is.
  const Drawing drawing = DrawBody(
      Gradient(
          "gradientUnits='userSpaceOnUse' x1='2' y1='1' x2='9' y2='5' gradientTransform='matrix(1 .3 -.4 1.5 2 1)'") +
          "<g transform='matrix(1.6 .2 .5 1.1 -3 -2)'><rect x='-10' y='-10' width='60' height='40' "
          "fill='url(#g)'/></g>",
      40, 20);
  const auto gray = [](double x, double y) {
    // Take the shape's transform back, then the gradient's.
    const auto back = [](double a, double b, double c, double d, double e, double f, double& px, double& py) {
      const double det = (a * d) - (b * c);
      const double qx = px - e;
      const double qy = py - f;
      px = ((d * qx) - (c * qy)) / det;
      py = ((-b * qx) + (a * qy)) / det;
    };
    back(1.6, .2, .5, 1.1, -3, -2, x, y);
    back(1, .3, -.4, 1.5, 2, 1, x, y);
    const double dx = 9 - 2;
    const double dy = 5 - 1;
    return BlackToWhite((((x - 2) * dx) + ((y - 1) * dy)) / ((dx * dx) + (dy * dy)));
  };
  // The ramp has 256 grays along it, and its places are worked out in
  // whole numbers, so a gray may be one out.
  EXPECT_TRUE(GradedAs(drawing, 0, 0, 40, 20, gray, 2.01));
  // Laid over the box of a shape that has been turned.
  const Drawing turned = DrawBody(Gradient("gradientTransform='rotate(90 .5 .5)'") +
                                  "<g transform='translate(20 10) scale(2 1)'>"
                                  "<rect x='-5' y='-5' width='10' height='10' fill='url(#g)'/></g>");
  EXPECT_TRUE(GradedAs(turned, 11, 6, 29, 14, [](double, double y) { return BlackToWhite((y - 5) / 10); }, 2.01));
}

TEST(SvgRender, AGradientsStopsSayItsGraysAndHowSolid) {
  const std::string_view stops =
      "<stop offset='0' stop-color='#000'/><stop offset='50%' style='stop-color:#808080;stop-opacity:.5'/>"
      "<stop offset='.25' stop-color='#fff'/><stop offset='1' stop-color='#fff' stop-opacity='0'/>";
  const Drawing drawing = DrawBody(Gradient("", stops) + "<rect width='40' height='4' fill='url(#g)'/>");
  // The third stop is before the second, so is taken to be where it is.
  // Over white, with the second stop half solid: at the middle of pixel 0,
  // an 80th of the way, 3 of black, and nearly solid; at pixel 10 more than
  // half way to the second stop, 67 at three quarters solid.
  EXPECT_NEAR(drawing.At(0, 0), 6, 1);
  EXPECT_NEAR(drawing.At(10, 0), 116, 1);
  // At the middle the gray changes at once from nearly half solid to white.
  EXPECT_NEAR(drawing.At(19, 0), 188, 1);
  EXPECT_NEAR(drawing.At(21, 0), 255, 1);
  // One stop is one gray; and none, nothing.
  EXPECT_TRUE(DrawBody(Gradient("", "<stop stop-color='#808080'/>") + "<rect width='4' height='4' fill='url(#g)'/>")
                  .IsBox(0, 0, 4, 4, 0x80));
  EXPECT_EQ(DrawBody(Gradient("", "") + "<rect width='4' height='4' fill='url(#g) #000'/>").Covered(), 0.0);
  // With no length to it, it is its last stop.
  EXPECT_TRUE(DrawBody(Gradient("x2='0'") + "<rect width='4' height='4' fill='url(#g)'/>").IsBox(0, 0, 4, 4, kWhite));
  // Its stops and the shape's opacity together.
  EXPECT_NEAR(DrawBody(Gradient("", "<stop stop-color='#000' stop-opacity='.5'/>") +
                       "<rect width='4' height='4' fill='url(#g)' fill-opacity='.5'/>")
                  .At(1, 1),
              191, 1);
  // More stops than are kept: the rest are left out, and it is said.
  std::string many;
  for (std::size_t stop = 0; stop <= SvgRenderer::kMostStops; ++stop) {
    many += "<stop offset='" + std::to_string(stop) + "%' stop-color='#000'/>";
  }
  EXPECT_EQ(DrawBody(Gradient("", many) + "<rect width='4' height='4' fill='url(#g)'/>").report.leftOut,
            Bits({SvgLeftOut::kPaintServer}));
}

TEST(SvgRender, BeyondItsEndsAGradientIsWhatItsSpreadSays) {
  const auto drawn = [](std::string_view spread) {
    return DrawBody(
        Gradient("gradientUnits='userSpaceOnUse' x1='10' x2='20' spreadMethod='" + std::string(spread) + "'") +
        "<rect width='40' height='2' fill='url(#g)'/>");
  };
  EXPECT_TRUE(GradedAs(drawn("pad"), 0, 0, 40, 2, [](double x, double) { return BlackToWhite((x - 10) / 10); }));
  EXPECT_TRUE(GradedAs(drawn("reflect"), 0, 0, 40, 2, [](double x, double) {
    const double along = std::fmod(std::abs(x - 10) / 10, 2);
    return BlackToWhite(along > 1 ? 2 - along : along);
  }));
  EXPECT_TRUE(GradedAs(drawn("repeat"), 21, 0, 29, 2,
                       [](double x, double) { return BlackToWhite(std::fmod((x - 10) / 10, 1)); }));
}

TEST(SvgRender, AGradientTakesWhatItDoesNotSayFromTheOneItNames) {
  const Drawing drawing =
      DrawBody("<defs><linearGradient id='base' x2='0' y2='1'>" + std::string(kBlackToWhite) +
               "</linearGradient><linearGradient id='g' xlink:href='#base' x2='1'/>"
               "<linearGradient id='h' href='#g' y2='0'><stop stop-color='#808080'/></linearGradient></defs>"
               "<rect width='20' height='20' fill='url(#g)'/><rect x='20' width='20' height='20' fill='url(#h)'/>");
  EXPECT_EQ(drawing.report.gradients, 3u);
  // Its stops, and down as well as across, from the one it names.
  EXPECT_TRUE(GradedAs(
      drawing, 0, 0, 20, 20, [](double x, double y) { return BlackToWhite(((x / 20) + (y / 20)) / 2); }, 2.01));
  // Its own stops, and the rest from the one it names.
  EXPECT_TRUE(GradedAs(drawing, 20, 0, 40, 20, [](double, double) { return 128.0; }));
}

TEST(SvgRender, AGradientThatCannotBeFoundOrIsNotLinearIsReported) {
  for (const std::string_view body :
       {"<rect width='4' height='4' fill='url(#g)'/><defs><linearGradient id='g'/></defs>",
        "<defs><radialGradient id='g'><stop/></radialGradient></defs><rect width='4' height='4' fill='url(#g)'/>",
        "<defs><pattern id='g'><rect width='1' height='1'/></pattern></defs><rect width='4' height='4' "
        "fill='url(#g)'/>"}) {
    const Drawing drawing = DrawBody(body);
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kPaintServer})) << body;
    EXPECT_EQ(drawing.Covered(), 0.0) << body;
  }
  // Of two of one name, the first is the one.
  const Drawing twice = DrawBody(
      "<linearGradient id='g'><stop stop-color='#808080'/></linearGradient>"
      "<linearGradient id='g'><stop stop-color='#404040'/></linearGradient><rect width='2' height='2' "
      "fill='url(#g)'/>");
  EXPECT_EQ(twice.At(0, 0), 0x80);
  EXPECT_EQ(twice.report.gradients, 2u);
  // Found outside `defs`, and in a rule of a style sheet, and handed on.
  const Drawing found = DrawBody(
      "<linearGradient id='g'><stop stop-color='#808080'/></linearGradient><style>.a{fill:url(#g)}</style>"
      "<rect width='2' height='2' class='a'/><g fill='url( \"#g\" )'><rect x='4' width='2' height='2'/></g>");
  EXPECT_EQ(found.report.leftOut, 0u);
  EXPECT_EQ(found.At(0, 0), 0x80);
  EXPECT_EQ(found.At(4, 0), 0x80);
}

TEST(SvgRender, AGradientInsideAnSvgIsCutOffAtItsEdges) {
  const Drawing drawing = DrawBody(Gradient("gradientUnits='userSpaceOnUse' x2='40'") +
                                   "<svg x='10' y='4' width='20' height='8'>"
                                   "<rect x='-50' y='-50' width='100' height='100' fill='url(#g)'/></svg>");
  // Laid out in the inner svg's own space, which begins ten pixels in.
  EXPECT_TRUE(GradedAs(drawing, 10, 4, 30, 12, [](double x, double) { return BlackToWhite((x - 10) / 40); }));
  EXPECT_EQ(drawing.At(9, 6), kWhite);
  EXPECT_EQ(drawing.At(30, 6), kWhite);
  EXPECT_EQ(drawing.At(15, 3), kWhite);
  EXPECT_EQ(drawing.At(15, 12), kWhite);
}

TEST(SvgRender, AGradientComesOutTheSameInAnyStripsAndAnyPieces) {
  const std::string file = Svg(40, 20,
                               Gradient("gradientTransform='rotate(30)' spreadMethod='reflect' y2='.3'") +
                                   "<style>.g{fill:url(#g)}</style>"
                                   "<path class='g' d='M2 3c10-8 30 0 35 5s-20 15-30 10z'/>"
                                   "<circle cx='20' cy='10' r='6' fill='url(#g)' fill-opacity='.7'/>");
  const Drawing whole = Draw(file, 40, 20);
  EXPECT_EQ(whole.report.leftOut, 0u);
  EXPECT_GT(whole.Covered(), 50);
  for (const std::size_t rows : {std::size_t{1}, std::size_t{3}, std::size_t{7}}) {
    for (const std::size_t piece : {std::size_t{1}, std::size_t{13}, SIZE_MAX}) {
      const Drawing cut = Draw(file, 40, 20, rows, piece);
      EXPECT_EQ(cut.gray, whole.gray) << rows << " " << piece;
      EXPECT_EQ(cut.report.gradients, 1u);
      EXPECT_EQ(cut.report.rules, 1u);
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
  EXPECT_EQ(renderer.Report().leftOut,
            Bits({SvgLeftOut::kText, SvgLeftOut::kGroupOpacity, SvgLeftOut::kPaintServer, SvgLeftOut::kUse}));
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

TEST(SvgRender, ACallStrokesNoMoreThanItsBound) {
  // Strokes with round corners and ends of outlines held and too long to
  // hold, dashes as many as an outline may have and more, and shapes
  // both filled and stroked: a call fills one shape or one stroke, and
  // gives the filler no more lines than its bound.
  std::string data = "M1 1";
  for (int i = 0; i < 3000; ++i) {
    data += i % 2 == 0 ? "l.01 .006" : "l.006 -.004";
  }
  const std::string body =
      "<g stroke='#000' stroke-linejoin='round' stroke-linecap='round' fill='none'><path d='" + data +
      "'/><path d='M0 0c10 20 30 -20 40 0' stroke-dasharray='.001'/>"
      "<path d='M0 5h40' stroke-dasharray='.01 .002 0 .003'/></g>"
      "<rect x='3' y='3' width='9' height='9' rx='2' stroke='#000' stroke-width='3'/>"
      "<circle cx='20' cy='10' r='8' fill='#888' stroke='#444' stroke-dasharray='1 1' stroke-linecap='square'/>";
  const std::string file = Svg(40, 20, body);
  std::vector<std::uint8_t> gray(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  Filler filler;
  ASSERT_TRUE(filler.Begin(gray, 40, 0, cells));
  SvgRenderer renderer;
  renderer.Begin(&filler, Pixels(40), Pixels(20));
  std::string_view bytes = file;
  std::uint64_t mostLines = 0;
  while (!bytes.empty()) {
    const SvgReport before = renderer.Report();
    bytes.remove_prefix(renderer.Step(bytes));
    const SvgReport& after = renderer.Report();
    EXPECT_LE((after.shapes - before.shapes) + (after.strokes - before.strokes), 1u);
    EXPECT_LE(after.commands - before.commands, SvgRenderer::kMostCommandsPerStep);
    mostLines = std::max<std::uint64_t>(mostLines, after.lines - before.lines);
  }
  EXPECT_EQ(renderer.Finish(), SvgStatus::kOk);
  EXPECT_LE(mostLines, SvgRenderer::kMostLinesPerStep);
  // The dashes past what one outline may have are a solid stroke, and said
  // to be.
  EXPECT_EQ(renderer.Report().strokes, 5u);
  EXPECT_EQ(renderer.Report().shapes, 2u);
  EXPECT_EQ(renderer.Report().leftOut, Bits({SvgLeftOut::kStroke}));
}

// ---- Strokes ----

TEST(SvgRender, AStrokeGoesOverTheFillHalfInsideTheOutlineAndHalfOut) {
  const Drawing drawing =
      DrawBody("<rect x='2.5' y='2.5' width='8' height='8' fill='#808080' stroke='#000' stroke-width='1'/>");
  EXPECT_EQ(drawing.At(6, 6), 128);
  EXPECT_EQ(drawing.At(2, 6), 0);
  EXPECT_EQ(drawing.At(10, 6), 0);
  EXPECT_EQ(drawing.At(1, 6), kWhite);
  EXPECT_EQ(drawing.At(11, 6), kWhite);
  EXPECT_NEAR(drawing.Covered(), (81 - 49) + (49 * 127.0 / 255), 0.1);
  EXPECT_EQ(drawing.report.shapes, 1u);
  EXPECT_EQ(drawing.report.strokes, 1u);
  // As solid as its own opacity, and the shape's, say.
  const Drawing faint = DrawBody(
      "<rect x='2.5' y='2.5' width='8' height='8' fill='none' stroke='#000' stroke-opacity='.5' opacity='.5'/>");
  EXPECT_NEAR(faint.At(2, 6), 191, 1);
}

// `body` drawn 40 by 20 in one strip, with no stroke thinner than
// `thinnest` pixels.
Drawing DrawThinnest(std::string_view body, double thinnest) {
  Drawing drawing;
  drawing.width = 40;
  drawing.height = 20;
  drawing.gray.assign(std::size_t{40} * 20, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(40, 20));
  Filler filler;
  EXPECT_TRUE(filler.Begin(drawing.gray, 40, 0, cells));
  SvgRenderer renderer;
  renderer.SetThinnestStroke(static_cast<Fixed>(thinnest * kFixedOne));
  renderer.Begin(&filler, Pixels(40), Pixels(20));
  const std::string file = Svg(40, 20, body);
  std::string_view bytes = file;
  while (!bytes.empty()) {
    bytes.remove_prefix(renderer.Step(bytes));
  }
  drawing.status = renderer.Finish();
  drawing.report = renderer.Report();
  return drawing;
}

TEST(SvgRender, AStrokeThinnerThanAskedForIsDrawnThatWide) {
  const std::string_view hairline = "<line x1='4' y1='10.5' x2='36' y2='10.5' stroke='#000' stroke-width='.1'/>";
  // As it is, a tenth of a pixel: a light gray.
  const Drawing asItIs = DrawThinnest(hairline, 0);
  EXPECT_NEAR(asItIs.Covered(), 3.2, 0.1);
  EXPECT_GT(asItIs.At(20, 10), 200);
  EXPECT_EQ(asItIs.report.widened, 0u);
  // A pixel wide, and black where it covers a whole pixel.
  const Drawing widened = DrawThinnest(hairline, 1);
  EXPECT_NEAR(widened.Covered(), 32, 0.1);
  EXPECT_EQ(widened.At(20, 10), 0);
  EXPECT_EQ(widened.report.strokes, 1u);
  EXPECT_EQ(widened.report.widened, 1u);
  // A stroke as wide as that already, or wider, is drawn as it is.
  const std::string_view wide = "<line x1='4' y1='10' x2='36' y2='10' stroke='#000' stroke-width='2'/>";
  EXPECT_EQ(DrawThinnest(wide, 1).gray, DrawThinnest(wide, 0).gray);
  EXPECT_EQ(DrawThinnest(wide, 1).report.widened, 0u);
  // What a drawing's own scale makes thin counts: a line a unit wide in a
  // drawing drawn at a fifth of its size is a fifth of a pixel.
  const Drawing small =
      DrawThinnest("<g transform='scale(.2)'><line x1='20' y1='52.5' x2='180' y2='52.5' stroke='#000'/></g>", 1);
  // A fifth is not exact in 2^20ths, and the width comes out a little over.
  EXPECT_NEAR(small.Covered(), 32, 0.2);
  EXPECT_EQ(small.report.widened, 1u);
}

TEST(SvgRender, AStrokeIsWidenedByWhereItsMatrixMakesThingsSmallest) {
  // Stretched four times across: a line along the stretch, a tenth of a
  // unit thick, is a tenth of a pixel, and is made a unit, a pixel; so is
  // one down it, which a unit makes four pixels wide.
  const Drawing drawing = DrawThinnest(
      "<g transform='scale(4 1)'><line x1='1' y1='5.5' x2='9' y2='5.5' stroke='#000' stroke-width='.1'/>"
      "<line x1='2' y1='8' x2='2' y2='16' stroke='#000' stroke-width='.1'/></g>",
      1);
  EXPECT_NEAR(drawing.Covered(), 32 + 32, 0.1);
  EXPECT_EQ(drawing.report.widened, 2u);
}

TEST(SvgRender, AStrokeIsAsWideAsItIsInItsShapesOwnUnits) {
  // Stretched four times across: a line along it is as thick as it was, and
  // one down it four times as thick.
  const Drawing drawing = DrawBody(
      "<g transform='scale(4 1)'><line x1='0.5' y1='5.5' x2='4.5' y2='5.5' stroke='#000'/>"
      "<line x1='2' y1='8' x2='2' y2='16' stroke='#000'/></g>");
  EXPECT_NEAR(drawing.Covered(), 16 + 32, 0.1);
  EXPECT_EQ(drawing.At(6, 12), 0);
  EXPECT_EQ(drawing.At(9, 12), 0);
  EXPECT_EQ(drawing.At(10, 12), kWhite);
  // A percentage is of the diagonal over the square root of two, as a
  // radius's is: of 40 by 20, 31.6.
  const Drawing wide = DrawBody("<line x1='10' y1='10' x2='30' y2='10' stroke='#000' stroke-width='10%'/>");
  EXPECT_NEAR(wide.Covered(), 20 * 3.162, 0.5);
}

TEST(SvgRender, LongPathDataIsStrokedAsItArrivesIfItHasAStrokeAndNoFill) {
  std::string data = "M2 10";
  for (int step = 0; step < 600; ++step) {
    data += "h.06";
  }
  ASSERT_GT(data.size(), 4 * SvgRenderer::kHeldPathData);
  const std::string file =
      Svg(40, 20, "<path fill='none' stroke='#000' stroke-width='2' stroke-linecap='square' d='" + data + "'/>");
  const Drawing drawing = Draw(file, 40, 20);
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.strokes, 1u);
  // A band 36 long and 2 wide, and its square ends: from 1 to 39.
  EXPECT_NEAR(drawing.Covered(), 38 * 2, 1);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{511}}) {
    EXPECT_EQ(Draw(file, 40, 20, 3, piece).gray, drawing.gray) << piece;
  }
}

TEST(SvgRender, LongPathDataIsFilledOrStrokedAndWhatIsNotDrawnIsSaid) {
  std::string data = "M2 2";
  for (int step = 0; step < 300; ++step) {
    data += "l.1 .05";
  }
  data += "V18H2Z";
  // Filled and stroked: it is filled.
  const Drawing both = DrawBody("<path stroke='#000' d='" + data + "'/>");
  EXPECT_EQ(both.report.leftOut, Bits({SvgLeftOut::kStroke}));
  EXPECT_EQ(both.report.shapes, 1u);
  EXPECT_EQ(both.report.strokes, 0u);
  // A stroke that is said, or changed, after its data has begun, by the
  // tag or by the sheet, is not drawn.
  for (const std::string_view late :
       {"<path fill='none' stroke='#000' d='DATA' stroke-width='3'/>",
        "<style>.wide { stroke-width: 3 }</style><path class='wide' fill='none' stroke='#000' d='DATA'/>",
        "<path fill='none' d='DATA' stroke='#000'/>"}) {
    std::string body(late);
    body.replace(body.find("DATA"), 4, data);
    const Drawing drawing = DrawBody(body);
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kStroke})) << late;
    EXPECT_EQ(drawing.report.strokes, 0u) << late;
    EXPECT_EQ(drawing.Covered(), 0.0) << late;
  }
  // Nor is a fill said after the data of an outline that is being stroked.
  const Drawing lateFill = DrawBody("<path stroke='#000' fill='none' d='" + data + "' style='fill:#000'/>");
  EXPECT_EQ(lateFill.report.leftOut, Bits({SvgLeftOut::kStroke}));
  EXPECT_EQ(lateFill.report.shapes, 0u);
  EXPECT_EQ(lateFill.report.strokes, 1u);
}

TEST(SvgRender, ShortPathDataIsGoneThroughOnceForTheFillAndAgainForTheStroke) {
  const std::string data = "M4 4C10 0 20 20 30 4V16H4Z";
  const Drawing drawing =
      DrawBody("<path d='" + data + "' fill='#ccc' stroke='#000' stroke-width='2' stroke-dasharray='3 1'/>");
  EXPECT_EQ(drawing.report.leftOut, 0u);
  EXPECT_EQ(drawing.report.shapes, 1u);
  EXPECT_EQ(drawing.report.strokes, 1u);
  EXPECT_EQ(drawing.report.commands, 2u * 5);
  // In any strips and pieces.
  const std::string file =
      Svg(40, 20, "<path d='" + data + "' fill='#ccc' stroke='#000' stroke-width='2' stroke-dasharray='3 1'/>");
  for (const std::size_t rows : {std::size_t{1}, std::size_t{6}}) {
    EXPECT_EQ(Draw(file, 40, 20, rows, 9).gray, drawing.gray) << rows;
  }
}

TEST(SvgRender, ReadsEveryStrokeProperty) {
  // Each said as an attribute, in `style`, and by a rule, and taken from
  // the group round it.
  const char* const ways[] = {
      "<path d='M10 4h20v12' fill='none' stroke='#000' stroke-width='2' stroke-linejoin='bevel' "
      "stroke-linecap='square' stroke-miterlimit='2' stroke-dasharray='4,2' stroke-dashoffset='1' "
      "stroke-opacity='1'/>",
      "<path d='M10 4h20v12' style='fill:none;stroke:#000;stroke-width:2;stroke-linejoin:bevel;"
      "stroke-linecap:square;stroke-miterlimit:2;stroke-dasharray:4 2;stroke-dashoffset:1'/>",
      "<style>path { fill: none; stroke: #000; stroke-width: 2; stroke-linejoin: bevel; stroke-linecap: square; "
      "stroke-miterlimit: 2; stroke-dasharray: 4 2; stroke-dashoffset: 1 }</style><path d='M10 4h20v12'/>",
      "<g fill='none' stroke='#000' stroke-width='2' stroke-linejoin='bevel' stroke-linecap='square' "
      "stroke-miterlimit='2' stroke-dasharray='4 2' stroke-dashoffset='1'><path d='M10 4h20v12' "
      "stroke-dasharray='inherit' stroke-linejoin='inherit'/></g>"};
  const Drawing first = DrawBody(ways[0]);
  EXPECT_EQ(first.report.leftOut, 0u);
  EXPECT_EQ(first.report.strokes, 1u);
  for (const char* const way : ways) {
    const Drawing drawing = DrawBody(way);
    EXPECT_EQ(drawing.gray, first.gray) << way;
    EXPECT_EQ(drawing.report.leftOut, 0u) << way;
  }
  // Values that are not right are reported, and what the stroke would
  // have been without them is drawn.
  for (const std::string_view bad :
       {"stroke-width='-1'", "stroke-linejoin='pointed'", "stroke-linecap='flat'", "stroke-miterlimit='0.5'",
        "stroke-dasharray='4,,2'", "stroke-dasharray='1 2 3 4 5 6 7 8 9'", "stroke-dasharray='4 -2'",
        "stroke-dashoffset='far'", "stroke-opacity='lots'"}) {
    const Drawing drawing = DrawBody("<path d='M10 4h20v12' fill='none' stroke='#000' " + std::string(bad) + "/>");
    EXPECT_EQ(drawing.report.leftOut, Bits({SvgLeftOut::kBadValue})) << bad;
    EXPECT_EQ(drawing.report.strokes, 1u) << bad;
  }
}

// ---- Anything at all ----

TEST(SvgRender, AnyBytesAtAllEndInAStatus) {
  // Files made of the pieces of a drawing put together any which way, and
  // of bytes that mean something to the tags or to nothing at all.
  std::mt19937 random = Repeatable(41);
  const std::string_view parts[] = {
      "<defs><rect id='a' width='9' height='6'/><g id='b'><circle r='5'/><use href='#a' x='3'/></g></defs>",
      "<defs><path id='c' d='M0 0h9l-4 7z'/><g id='d'><use href='#d'/><use href='#c'/></g></defs>",
      "<use href='#a' x='12' y='4'/>",
      "<use xlink:href='#b' transform='translate(20 10)'/>",
      "<use href='#c' fill='#777'/>",
      "<use href='#d'/>",
      "<use href='#a'",
      "/>",
      "/>",
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
      "<defs>",
      "</defs>",
      "<symbol",
      " id='",
      " id='a'",
      " href='#",
      " href='#a'",
      " overflow='visible'",
      "a",
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
  std::size_t used = 0;
  for (int i = 0; i < 6000; ++i) {
    std::string file = (i % 3 == 0) ? "" : "<svg width='40' height='20'>";
    // One in four begins with things defined, for what follows to use.
    if (i % 4 == 1) {
      file += parts[0];
      file += parts[1];
    }
    const std::size_t count = random() % 40;
    for (std::size_t part = 0; part < count; ++part) {
      // The first twenty-nine parts are whole shapes, groups, definitions
      // and uses, and are taken more often than the rest, so that there
      // is often something to draw before a file goes wrong.
      const auto kind = random() % 12;
      if (kind == 0) {
        file += static_cast<char>(random() % 256);
      } else if (kind < 8) {
        file += parts[random() % 29];
      } else {
        file += parts[random() % std::size(parts)];
      }
    }
    const std::size_t piece = (i % 2 == 0) ? SIZE_MAX : 1 + (random() % 9);
    const Drawing drawing = Draw(file, 40, 20, (i % 5 == 0) ? 6 : 0, piece);
    drew += drawing.report.shapes > 0 ? 1 : 0;
    failed += drawing.status == SvgStatus::kOk ? 0 : 1;
    // It cannot be made to go round: a call takes a byte, or works off
    // some of what it holds, or reads some of what it has kept.
    ASSERT_LT(drawing.calls, (4 * (file.size() + drawing.report.reused + 600)) * ((i % 5 == 0) ? 4 : 1)) << i;
    // And however it is cut up, it comes to the same.
    const Drawing whole = Draw(file, 40, 20);
    ASSERT_EQ(drawing.gray, whole.gray) << i << ": " << file;
    ASSERT_EQ(drawing.status, whole.status) << i << ": " << file;
    ASSERT_EQ(drawing.report.leftOut, whole.report.leftOut) << i << ": " << file;
    ASSERT_EQ(drawing.report.kept, whole.report.kept) << i << ": " << file;
    ASSERT_EQ(drawing.report.reused, whole.report.reused) << i << ": " << file;
    used += drawing.report.reused > 0 ? 1 : 0;
  }
  EXPECT_GT(used, 250u) << used;
  // Hundreds of them drew something, and most went wrong somewhere.
  EXPECT_GT(drew, 300u) << drew;
  EXPECT_GT(failed, 3000u) << failed;
  EXPECT_LT(failed, 6000u);
}

}  // namespace
