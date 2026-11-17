#include "draw/svg_values.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

using bn::draw::FitBox;
using bn::draw::Fixed;
using bn::draw::GrayOf;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::ParseSvgColour;
using bn::draw::ParseSvgFit;
using bn::draw::ParseSvgLength;
using bn::draw::ParseSvgOpacity;
using bn::draw::ParseSvgPaint;
using bn::draw::ParseSvgViewBox;
using bn::draw::Point;
using bn::draw::SvgBox;
using bn::draw::SvgFit;
using bn::draw::SvgLength;
using bn::draw::SvgPaint;
using bn::draw::SvgUnit;

constexpr Fixed Units(double value) { return static_cast<Fixed>(value * kFixedOne); }

// ---- Lengths ----

// A length in pixels, where a percentage is of 200. Or -1 if `text` is not
// a length.
double Pixels(std::string_view text) {
  SvgLength length;
  bool clamped = false;
  if (!ParseSvgLength(text, length, clamped)) {
    return -1;
  }
  return static_cast<double>(bn::draw::LengthIn(length, Units(200), clamped)) / kFixedOne;
}

TEST(SvgValues, ReadsALengthInEachOfItsUnits) {
  EXPECT_DOUBLE_EQ(Pixels("12"), 12);
  EXPECT_DOUBLE_EQ(Pixels("1.5px"), 1.5);
  EXPECT_DOUBLE_EQ(Pixels(" 7 "), 7);
  EXPECT_DOUBLE_EQ(Pixels("50%"), 100);
  EXPECT_DOUBLE_EQ(Pixels("72pt"), 96);
  EXPECT_DOUBLE_EQ(Pixels("3pc"), 48);
  EXPECT_DOUBLE_EQ(Pixels("1in"), 96);
  // The number is held to a 4096th before it is made into pixels.
  EXPECT_NEAR(Pixels("2.54cm"), 96, 0.01);
  EXPECT_NEAR(Pixels("25.4mm"), 96, 0.01);
  // The "e" of "em" is not the start of an exponent, and that of "1e2" is.
  EXPECT_DOUBLE_EQ(Pixels("2em"), 32);
  EXPECT_DOUBLE_EQ(Pixels("2ex"), 16);
  EXPECT_DOUBLE_EQ(Pixels("1e2"), 100);
  EXPECT_DOUBLE_EQ(Pixels("1e1px"), 10);
  EXPECT_DOUBLE_EQ(Pixels("3PX"), 3);
  EXPECT_DOUBLE_EQ(Pixels(".5"), 0.5);

  SvgLength length;
  bool clamped = false;
  ASSERT_TRUE(ParseSvgLength("-4.25mm", length, clamped));
  EXPECT_EQ(length.unit, SvgUnit::kMm);
  EXPECT_NEAR(static_cast<double>(bn::draw::LengthIn(length, 0, clamped)) / kFixedOne, -4.25 * 96 / 25.4, 0.001);
  EXPECT_FALSE(clamped);
}

TEST(SvgValues, RefusesWhatIsNotALength) {
  for (const std::string_view text :
       {"", " ", "px", "%", "12 px", "1.2.3", "abc", "5e", "12furlongs", "1,5", "12px;"}) {
    SvgLength length = {.value = 7, .unit = SvgUnit::kIn};
    bool clamped = false;
    EXPECT_FALSE(ParseSvgLength(text, length, clamped)) << text;
    // And leaves what it was given as it was.
    EXPECT_EQ(length.value, 7) << text;
    EXPECT_EQ(length.unit, SvgUnit::kIn) << text;
  }
}

TEST(SvgValues, ALengthTooLargeIsHeld) {
  SvgLength length;
  bool clamped = false;
  ASSERT_TRUE(ParseSvgLength("400000in", length, clamped));
  EXPECT_EQ(bn::draw::LengthIn(length, 0, clamped), bn::draw::kFixedMax);
  EXPECT_TRUE(clamped);
}

TEST(SvgValues, ReadsAListOfLengthsWithCommasOrSpaceBetween) {
  std::array<SvgLength, 4> lengths{};
  std::size_t count = 0;
  bool clamped = false;
  ASSERT_TRUE(bn::draw::ParseSvgLengths(" 5, 3px\t2%  1 ", lengths, count, clamped));
  ASSERT_EQ(count, 4U);
  EXPECT_EQ(bn::draw::LengthIn(lengths[0], 0, clamped), Units(5));
  EXPECT_EQ(lengths[1].unit, SvgUnit::kPx);
  EXPECT_EQ(bn::draw::LengthIn(lengths[2], Units(200), clamped), Units(4));
  EXPECT_EQ(bn::draw::LengthIn(lengths[3], 0, clamped), Units(1));
  ASSERT_TRUE(bn::draw::ParseSvgLengths("7", lengths, count, clamped));
  EXPECT_EQ(count, 1U);
}

TEST(SvgValues, RefusesAListWithAnythingElseInIt) {
  for (const std::string_view text : {"", " ", "none", "5,,3", ",5", "5,", "5 a", "1 2 3 4 5", "5;3"}) {
    std::array<SvgLength, 4> lengths{};
    std::size_t count = 0;
    bool clamped = false;
    EXPECT_FALSE(bn::draw::ParseSvgLengths(text, lengths, count, clamped)) << text;
  }
}

// ---- Colours ----

// The gray a colour is drawn in, or -1 if `text` is not a colour.
int Gray(std::string_view text) {
  std::uint8_t gray = 99;
  std::uint8_t alpha = 99;
  return ParseSvgColour(text, gray, alpha) ? gray : -1;
}

// How solid it is.
int Alpha(std::string_view text) {
  std::uint8_t gray = 99;
  std::uint8_t alpha = 99;
  return ParseSvgColour(text, gray, alpha) ? alpha : -1;
}

TEST(SvgValues, AColourIsTheGrayItLooks) {
  EXPECT_EQ(GrayOf(0, 0, 0), 0);
  EXPECT_EQ(GrayOf(255, 255, 255), 255);
  EXPECT_EQ(GrayOf(128, 128, 128), 128);
  // Green looks brightest, then red, and blue darkest: 0.587, 0.299 and
  // 0.114 of the whole.
  EXPECT_NEAR(GrayOf(255, 0, 0), 0.299 * 255, 1);
  EXPECT_NEAR(GrayOf(0, 255, 0), 0.587 * 255, 1);
  EXPECT_NEAR(GrayOf(0, 0, 255), 0.114 * 255, 1);
  for (int red = 0; red < 256; red += 5) {
    for (int green = 0; green < 256; green += 15) {
      for (int blue = 0; blue < 256; blue += 51) {
        EXPECT_NEAR(
            GrayOf(static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green), static_cast<std::uint8_t>(blue)),
            (0.299 * red) + (0.587 * green) + (0.114 * blue), 1);
      }
    }
  }
}

TEST(SvgValues, ReadsAColourEachWayItIsWritten) {
  EXPECT_EQ(Gray("#000"), 0);
  EXPECT_EQ(Gray("#fff"), 255);
  EXPECT_EQ(Gray("#FFFFFF"), 255);
  EXPECT_EQ(Gray("#808080"), 128);
  EXPECT_EQ(Gray(" #333333 "), 0x33);
  // A digit by itself stands for two the same.
  EXPECT_EQ(Gray("#abc"), Gray("#aabbcc"));
  EXPECT_EQ(Gray("#f00"), GrayOf(255, 0, 0));
  // A fourth part says how solid it is.
  EXPECT_EQ(Alpha("#fff"), 255);
  EXPECT_EQ(Alpha("#ff000080"), 128);
  EXPECT_EQ(Gray("#ff000080"), GrayOf(255, 0, 0));
  EXPECT_EQ(Alpha("#f008"), 0x88);

  EXPECT_EQ(Gray("rgb(255, 0, 0)"), GrayOf(255, 0, 0));
  EXPECT_EQ(Gray("rgb(100%,0%,0%)"), GrayOf(255, 0, 0));
  EXPECT_EQ(Gray("rgb(50%, 50%, 50%)"), 128);
  EXPECT_EQ(Gray("RGB( 1 , 2 , 3 )"), GrayOf(1, 2, 3));
  EXPECT_EQ(Gray("rgb(10 20 30)"), GrayOf(10, 20, 30));
  // Parts beyond what there is are held to it.
  EXPECT_EQ(Gray("rgb(300, -5, 255.4)"), GrayOf(255, 0, 255));
  EXPECT_EQ(Alpha("rgba(0, 0, 0, 0.5)"), 128);
  EXPECT_EQ(Alpha("rgb(0 0 0 / 50%)"), 128);
  EXPECT_EQ(Alpha("rgba(0,0,0,1)"), 255);
  EXPECT_EQ(Alpha("rgba(0,0,0,7)"), 255);

  EXPECT_EQ(Gray("black"), 0);
  EXPECT_EQ(Gray("White"), 255);
  EXPECT_EQ(Gray("RED"), GrayOf(255, 0, 0));
  EXPECT_EQ(Gray("gray"), 128);
  EXPECT_EQ(Gray("grey"), 128);
  EXPECT_EQ(Gray("silver"), 192);
  EXPECT_EQ(Gray("orange"), GrayOf(255, 165, 0));
  EXPECT_EQ(Gray("green"), GrayOf(0, 128, 0));
  EXPECT_EQ(Gray("lime"), GrayOf(0, 255, 0));
  EXPECT_EQ(Gray("cyan"), Gray("aqua"));
  EXPECT_EQ(Gray("magenta"), Gray("fuchsia"));
  EXPECT_EQ(Alpha("black"), 255);
  EXPECT_EQ(Alpha("transparent"), 0);
}

TEST(SvgValues, RefusesWhatIsNotAColour) {
  for (const std::string_view text :
       {"", "#", "#12", "#12345", "#1234567", "#ggg", "#12 34", "rgb(1,2)", "rgb(1,2,3", "rgb(1,2,3)x",
        "rgb(1,2,3,4,5)", "rgb(a,b,c)", "rgb", "blurple", "blackk", "none", "currentColor", "0", "url(#a)"}) {
    EXPECT_EQ(Gray(text), -1) << text;
  }
}

TEST(SvgValues, ReadsAPaint) {
  SvgPaint paint;
  ASSERT_TRUE(ParseSvgPaint(" #808080 ", paint));
  EXPECT_EQ(paint, (SvgPaint{.kind = SvgPaint::Kind::kGray, .gray = 128, .alpha = 255, .server = false}));
  ASSERT_TRUE(ParseSvgPaint("none", paint));
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kNone);
  EXPECT_FALSE(paint.server);
  ASSERT_TRUE(ParseSvgPaint("currentColor", paint));
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kCurrent);
  ASSERT_TRUE(ParseSvgPaint("INHERIT", paint));
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kInherit);
  ASSERT_TRUE(ParseSvgPaint("rgba(0,0,0,.25)", paint));
  EXPECT_EQ(paint.alpha, 64);

  // A reference to something defined elsewhere, alone and with what to
  // use in its place.
  ASSERT_TRUE(ParseSvgPaint("url(#gradient)", paint));
  EXPECT_TRUE(paint.server);
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kNone);
  ASSERT_TRUE(ParseSvgPaint("url( '#a' ) none", paint));
  EXPECT_TRUE(paint.server);
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kNone);
  ASSERT_TRUE(ParseSvgPaint("URL(#a) #fff", paint));
  EXPECT_EQ(paint, (SvgPaint{.kind = SvgPaint::Kind::kGray, .gray = 255, .alpha = 255, .server = true}));
  ASSERT_TRUE(ParseSvgPaint("url(#a) currentColor", paint));
  EXPECT_TRUE(paint.server);
  EXPECT_EQ(paint.kind, SvgPaint::Kind::kCurrent);

  for (const std::string_view text : {"", "url(#a", "url(#a) bogus", "url(#a) inherit", "blurple", "nonee", "#12"}) {
    SvgPaint untouched = {.kind = SvgPaint::Kind::kCurrent, .gray = 7, .alpha = 8, .server = true};
    EXPECT_FALSE(ParseSvgPaint(text, untouched)) << text;
    EXPECT_EQ(untouched, (SvgPaint{.kind = SvgPaint::Kind::kCurrent, .gray = 7, .alpha = 8, .server = true})) << text;
  }
}

TEST(SvgValues, ReadsHowSolidAsAPartOf255) {
  const auto solid = [](std::string_view text) {
    std::uint8_t opacity = 99;
    return ParseSvgOpacity(text, opacity) ? opacity : -1;
  };
  EXPECT_EQ(solid("0"), 0);
  EXPECT_EQ(solid("1"), 255);
  EXPECT_EQ(solid("0.5"), 128);
  EXPECT_EQ(solid(".25"), 64);
  EXPECT_EQ(solid(" 0.75 "), 191);
  EXPECT_EQ(solid("50%"), 128);
  EXPECT_EQ(solid("100%"), 255);
  // Beyond the ends, it is held to them.
  EXPECT_EQ(solid("2"), 255);
  EXPECT_EQ(solid("-1"), 0);
  EXPECT_EQ(solid("250%"), 255);
  for (const std::string_view text : {"", "half", "0.5x", "50 %", "%", "1 2"}) {
    EXPECT_EQ(solid(text), -1) << text;
  }
}

// ---- Boxes ----

TEST(SvgValues, ReadsAViewBox) {
  SvgBox box;
  bool clamped = false;
  ASSERT_TRUE(ParseSvgViewBox("0 0 100 50", box, clamped));
  EXPECT_EQ(box, (SvgBox{.x = 0, .y = 0, .width = Units(100), .height = Units(50)}));
  ASSERT_TRUE(ParseSvgViewBox(" -1.5,2 , 3.25\n4e1 ", box, clamped));
  EXPECT_EQ(box, (SvgBox{.x = Units(-1.5), .y = Units(2), .width = Units(3.25), .height = Units(40)}));
  // Numbers run together as path data has them.
  ASSERT_TRUE(ParseSvgViewBox("0 0 10-5", box, clamped));
  EXPECT_EQ(box.height, Units(-5));
  // A box of no size is read as it is written.
  ASSERT_TRUE(ParseSvgViewBox("0 0 0 0", box, clamped));
  EXPECT_EQ(box.width, 0);
  EXPECT_FALSE(clamped);
  ASSERT_TRUE(ParseSvgViewBox("0 0 9999999 5", box, clamped));
  EXPECT_TRUE(clamped);

  for (const std::string_view text :
       {"", "0 0 100", "0 0 100 50 6", "a b c d", "0 0 100 50px", "0,,0 100 50", "0 0 100,"}) {
    SvgBox untouched = {.x = 1, .y = 2, .width = 3, .height = 4};
    EXPECT_FALSE(ParseSvgViewBox(text, untouched, clamped)) << text;
    EXPECT_EQ(untouched, (SvgBox{.x = 1, .y = 2, .width = 3, .height = 4})) << text;
  }
}

TEST(SvgValues, ReadsHowABoxIsToBeFitted) {
  using Align = SvgFit::Align;
  SvgFit fit;
  EXPECT_EQ(fit, (SvgFit{.x = Align::kMid, .y = Align::kMid, .stretch = false, .slice = false}));
  ASSERT_TRUE(ParseSvgFit("none", fit));
  EXPECT_TRUE(fit.stretch);
  ASSERT_TRUE(ParseSvgFit("xMinYMax slice", fit));
  EXPECT_EQ(fit, (SvgFit{.x = Align::kMin, .y = Align::kMax, .stretch = false, .slice = true}));
  ASSERT_TRUE(ParseSvgFit("  xMaxYMin   meet ", fit));
  EXPECT_EQ(fit, (SvgFit{.x = Align::kMax, .y = Align::kMin, .stretch = false, .slice = false}));
  ASSERT_TRUE(ParseSvgFit("defer xMidYMid", fit));
  EXPECT_EQ(fit, SvgFit());
  ASSERT_TRUE(ParseSvgFit("none slice", fit));
  EXPECT_TRUE(fit.stretch);

  for (const std::string_view text : {"", "xmidymid", "xMidYMid fill", "meet", "xMidYMid meet slice", "xMidyMid",
                                      "xMidYMiddle", "defer", "xMid YMid"}) {
    SvgFit untouched = {.x = Align::kMax, .y = Align::kMax, .stretch = true, .slice = true};
    EXPECT_FALSE(ParseSvgFit(text, untouched)) << text;
    EXPECT_EQ(untouched, (SvgFit{.x = Align::kMax, .y = Align::kMax, .stretch = true, .slice = true})) << text;
  }
}

// Where the matrix that fits `box` into `space` puts the box's corners:
// left, top, right and bottom, in pixels.
struct Placed {
  double left;
  double top;
  double right;
  double bottom;
};

Placed Fitted(const SvgBox& box, const SvgBox& space, std::string_view how) {
  SvgFit fit;
  EXPECT_TRUE(ParseSvgFit(how, fit)) << how;
  bool clamped = false;
  const Matrix matrix = FitBox(box, space, fit, clamped);
  EXPECT_FALSE(clamped);
  EXPECT_EQ(matrix.b, 0);
  EXPECT_EQ(matrix.c, 0);
  const Point from = bn::draw::Apply(matrix, {.x = box.x, .y = box.y}, clamped);
  const Point to = bn::draw::Apply(matrix, {.x = box.x + box.width, .y = box.y + box.height}, clamped);
  return {.left = static_cast<double>(from.x) / kFixedOne,
          .top = static_cast<double>(from.y) / kFixedOne,
          .right = static_cast<double>(to.x) / kFixedOne,
          .bottom = static_cast<double>(to.y) / kFixedOne};
}

void ExpectPlaced(const Placed& placed, double left, double top, double right, double bottom, double within = 0.002) {
  EXPECT_NEAR(placed.left, left, within);
  EXPECT_NEAR(placed.top, top, within);
  EXPECT_NEAR(placed.right, right, within);
  EXPECT_NEAR(placed.bottom, bottom, within);
}

TEST(SvgValues, FitsABoxIntoASpaceAsItIsTold) {
  // A box twice as wide as it is high, into a square: it fits by its
  // width, and what is left over is above and below it.
  const SvgBox box = {.x = Units(10), .y = Units(20), .width = Units(100), .height = Units(50)};
  const SvgBox space = {.x = Units(5), .y = Units(7), .width = Units(200), .height = Units(200)};
  ExpectPlaced(Fitted(box, space, "xMidYMid meet"), 5, 57, 205, 157);
  ExpectPlaced(Fitted(box, space, "xMidYMin"), 5, 7, 205, 107);
  ExpectPlaced(Fitted(box, space, "xMinYMax meet"), 5, 107, 205, 207);
  // Made to cover the square, it is as high as it and twice as wide, and
  // what hangs over is to the left and the right.
  ExpectPlaced(Fitted(box, space, "xMidYMid slice"), -95, 7, 305, 207);
  ExpectPlaced(Fitted(box, space, "xMinYMid slice"), 5, 7, 405, 207);
  ExpectPlaced(Fitted(box, space, "xMaxYMax slice"), -195, 7, 205, 207);
  // Stretched, it is the square.
  ExpectPlaced(Fitted(box, space, "none"), 5, 7, 205, 207);

  // A tall box into a wide space fits by its height.
  const SvgBox tall = {.x = 0, .y = 0, .width = Units(10), .height = Units(40)};
  const SvgBox wide = {.x = 0, .y = 0, .width = Units(300), .height = Units(100)};
  ExpectPlaced(Fitted(tall, wide, "xMidYMid"), 137.5, 0, 162.5, 100);
  ExpectPlaced(Fitted(tall, wide, "xMaxYMid"), 275, 0, 300, 100);

  // A box thousands of units across, as a book's drawing can be, comes
  // out where it should to a fiftieth of a pixel: a factor is held to a
  // part in a million, and this one is multiplied by 31,976.
  const SvgBox large = {.x = 0, .y = 0, .width = Units(31976), .height = Units(20000)};
  const SvgBox page = {.x = 0, .y = 0, .width = Units(480), .height = Units(800)};
  ExpectPlaced(Fitted(large, page, "xMidYMid"), 0, 400 - (240.0 * 20000 / 31976), 480, 400 + (240.0 * 20000 / 31976),
               0.02);
}

TEST(SvgValues, TakesSpaceOffTheEndsAndTellsAWordInAnyCase) {
  EXPECT_EQ(bn::draw::WithoutSpace("  a b \t\r\n"), "a b");
  EXPECT_EQ(bn::draw::WithoutSpace(""), "");
  EXPECT_EQ(bn::draw::WithoutSpace(" \n "), "");
  EXPECT_TRUE(bn::draw::IsWord("None", "none"));
  EXPECT_TRUE(bn::draw::IsWord("currentColor", "currentcolor"));
  EXPECT_FALSE(bn::draw::IsWord("non", "none"));
  EXPECT_FALSE(bn::draw::IsWord("nonee", "none"));
  EXPECT_FALSE(bn::draw::IsWord("n0ne", "none"));
}

}  // namespace
