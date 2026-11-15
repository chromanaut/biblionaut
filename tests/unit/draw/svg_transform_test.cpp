#include "draw/svg_transform.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string_view>

namespace {

using bn::draw::Apply;
using bn::draw::Fixed;
using bn::draw::kCoefficientOne;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::ParseSvgTransform;
using bn::draw::Point;

// The six numbers of a matrix, as SVG would write them.
struct Six {
  double a, b, c, d, e, f;
};

// Reads a transform that should be one.
Six Read(std::string_view text) {
  Matrix matrix;
  bool clamped = false;
  EXPECT_TRUE(ParseSvgTransform(text, matrix, clamped)) << text;
  EXPECT_FALSE(clamped) << text;
  return {static_cast<double>(matrix.a) / kCoefficientOne, static_cast<double>(matrix.b) / kCoefficientOne,
          static_cast<double>(matrix.c) / kCoefficientOne, static_cast<double>(matrix.d) / kCoefficientOne,
          static_cast<double>(matrix.e) / kFixedOne,       static_cast<double>(matrix.f) / kFixedOne};
}

void ExpectSix(const Six& got, const Six& want, std::string_view what) {
  EXPECT_NEAR(got.a, want.a, 2e-6) << what;
  EXPECT_NEAR(got.b, want.b, 2e-6) << what;
  EXPECT_NEAR(got.c, want.c, 2e-6) << what;
  EXPECT_NEAR(got.d, want.d, 2e-6) << what;
  EXPECT_NEAR(got.e, want.e, 0.001) << what;
  EXPECT_NEAR(got.f, want.f, 0.001) << what;
}

// Where a transform puts a point.
std::pair<double, double> Moved(std::string_view text, double x, double y) {
  Matrix matrix;
  bool clamped = false;
  EXPECT_TRUE(ParseSvgTransform(text, matrix, clamped)) << text;
  const Point moved =
      Apply(matrix,
            {.x = static_cast<Fixed>(std::lround(x * kFixedOne)), .y = static_cast<Fixed>(std::lround(y * kFixedOne))},
            clamped);
  EXPECT_FALSE(clamped) << text;
  return {static_cast<double>(moved.x) / kFixedOne, static_cast<double>(moved.y) / kFixedOne};
}

void ExpectMoved(std::string_view text, double x, double y, double wantX, double wantY) {
  const auto [gotX, gotY] = Moved(text, x, y);
  EXPECT_NEAR(gotX, wantX, 0.002) << text;
  EXPECT_NEAR(gotY, wantY, 0.002) << text;
}

TEST(SvgTransform, ReadsEachKind) {
  ExpectSix(Read("matrix(1 2 3 4 5 6)"), {1, 2, 3, 4, 5, 6}, "matrix");
  ExpectSix(Read("matrix(0.5,-0.25,1.75,-2,-30.5,40.25)"), {0.5, -0.25, 1.75, -2, -30.5, 40.25}, "matrix");
  ExpectSix(Read("translate(10 20)"), {1, 0, 0, 1, 10, 20}, "translate");
  ExpectSix(Read("scale(2 3)"), {2, 0, 0, 3, 0, 0}, "scale");
  const double sine = std::sin(30.0 / 180.0 * std::numbers::pi);
  const double cosine = std::cos(30.0 / 180.0 * std::numbers::pi);
  ExpectSix(Read("rotate(30)"), {cosine, sine, -sine, cosine, 0, 0}, "rotate");
  const double tangent = std::tan(30.0 / 180.0 * std::numbers::pi);
  ExpectSix(Read("skewX(30)"), {1, 0, tangent, 1, 0, 0}, "skewX");
  ExpectSix(Read("skewY(30)"), {1, tangent, 0, 1, 0, 0}, "skewY");
}

TEST(SvgTransform, NumbersThatAreLeftOutHaveTheirUsualMeaning) {
  ExpectSix(Read("translate(10)"), {1, 0, 0, 1, 10, 0}, "translate");
  ExpectSix(Read("scale(2)"), {2, 0, 0, 2, 0, 0}, "scale");
  ExpectSix(Read("scale(-1)"), {-1, 0, 0, -1, 0, 0}, "scale");
}

TEST(SvgTransform, TurnsAboutAPointWhenGivenOne) {
  ExpectMoved("rotate(90 10 10)", 10, 10, 10, 10);
  ExpectMoved("rotate(90 10 10)", 20, 10, 10, 20);
  ExpectMoved("rotate(90 10 10)", 10, 0, 20, 10);
  ExpectMoved("rotate(-90, 10, 10)", 20, 10, 10, 0);
  ExpectMoved("rotate(180 5 0)", 0, 0, 10, 0);
}

TEST(SvgTransform, TheLastInTheListIsDoneFirst) {
  // Made twice as large and then moved ten across.
  ExpectMoved("translate(10 0) scale(2)", 1, 1, 12, 2);
  // Moved ten across and then made twice as large.
  ExpectMoved("scale(2) translate(10 0)", 1, 1, 22, 2);
  ExpectMoved("translate(100 100) rotate(90) scale(2 1)", 3, 4, 96, 106);
  ExpectMoved("translate(1707.4,0)translate(0,-533)scale(3.532,1)", 100, 10, 2060.6, -523);
}

TEST(SvgTransform, TakesSpacesAndCommasBetweenThings) {
  const Six want = {2, 0, 0, 2, 1, 2};
  for (const char* text : {"translate(1 2) scale(2)", "translate(1,2),scale(2)", "translate(1 , 2) , scale(2)",
                           "  translate( 1 2 )\n\tscale( 2 )  ", "translate(1 2)scale(2)", "translate (1 2) scale (2)",
                           "translate(1\n2)scale(2,2)"}) {
    ExpectSix(Read(text), want, text);
  }
}

TEST(SvgTransform, NothingAtAllChangesNothing) {
  for (const char* text : {"", " ", "\n\t "}) {
    ExpectSix(Read(text), {1, 0, 0, 1, 0, 0}, "empty");
  }
}

TEST(SvgTransform, KeepsASmallFactorPreciseEnoughForAWideDrawing) {
  // A drawing 31,976 units wide brought down to 480 pixels: the far edge
  // has to land within a fiftieth of a pixel.
  const auto [farX, farY] = Moved("scale(0.015011258)", 31976, 31976);
  EXPECT_NEAR(farX, 480.0, 0.02);
  EXPECT_NEAR(farY, 480.0, 0.02);
  const auto [halfX, halfY] = Moved("scale(0.015011258)", 15988, 0);
  EXPECT_NEAR(halfX, 240.0, 0.01);
  EXPECT_EQ(halfY, 0.0);
}

TEST(SvgTransform, SaysWhatIsNotATransformAndLeavesTheMatrixAlone) {
  for (const char* text : {"foo(1)",
                           "translate",
                           "translate(1 2",
                           "translate 1 2)",
                           "translate(1 2 3)",
                           "translate()",
                           "scale()",
                           "scale(1 2 3)",
                           "matrix(1 2 3)",
                           "matrix(1 2 3 4 5 6 7)",
                           "translate(a)",
                           "translate(1 a)",
                           "rotate(1 2)",
                           "rotate(1 2 3 4)",
                           "skewX(1 2)",
                           "skewY()",
                           "translate(1) x",
                           "(1 2)",
                           "translate(1,,2)",
                           "translate(,1)",
                           "Translate(1 2)",
                           "skewx(10)",
                           "translate(1 2) scale(",
                           "1 2",
                           "translate(1e)"}) {
    Matrix matrix;
    matrix.e = 77;
    bool clamped = false;
    EXPECT_FALSE(ParseSvgTransform(text, matrix, clamped)) << text;
    EXPECT_EQ(matrix.e, 77) << text;
    EXPECT_EQ(matrix.a, kCoefficientOne) << text;
  }
}

TEST(SvgTransform, HoldsWhatIsTooLargeAtTheEdgeAndSaysSo) {
  for (const char* text : {"scale(5000)", "translate(600000 0)", "scale(1500) scale(1500)", "skewX(90)",
                           "matrix(1 0 0 1 17394593339182234 0)", "translate(400000 0) translate(400000 0)"}) {
    Matrix matrix;
    bool clamped = false;
    EXPECT_TRUE(ParseSvgTransform(text, matrix, clamped)) << text;
    EXPECT_TRUE(clamped) << text;
  }
  // A flag that was already set is left set.
  Matrix matrix;
  bool clamped = true;
  EXPECT_TRUE(ParseSvgTransform("scale(2)", matrix, clamped));
  EXPECT_TRUE(clamped);
}

}  // namespace
