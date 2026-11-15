#include "draw/svg_number.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>

namespace {

using bn::draw::AngleOfDegrees;
using bn::draw::CoefficientOf;
using bn::draw::FixedOf;
using bn::draw::kFixedMax;
using bn::draw::kFixedMin;
using bn::draw::kNumberOne;
using bn::draw::kQuarterTurn;
using bn::draw::Number;
using bn::draw::ScanNumber;
using bn::draw::SvgNumber;

// What reading the start of `text` as a number gives.
struct Reading {
  bool found = false;
  double value = 0;
  std::size_t length = 0;  // How many characters the number took.
  bool clamped = false;
};

Reading Read(std::string_view text) {
  Reading reading;
  Number number = 0;
  reading.found = ScanNumber(text, reading.length, number, reading.clamped);
  reading.value = static_cast<double>(number) / static_cast<double>(kNumberOne);
  return reading;
}

// A Number is in 1/2^32ths, so this is as near as one can be.
constexpr double kNearest = 1.0 / 4294967296.0;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

TEST(SvgNumber, ReadsWholeNumbersFractionsAndSigns) {
  const struct {
    const char* text;
    double value;
  } cases[] = {{"0", 0},
               {"7", 7},
               {"12", 12},
               {"-12", -12},
               {"+12", 12},
               {"3.25", 3.25},
               {"-3.25", -3.25},
               {".5", 0.5},
               {"-.5", -0.5},
               {"+.5", 0.5},
               {"1.", 1},
               {"-1.", -1},
               {"0.1", 0.1},
               {"100.001", 100.001},
               {"0.000244140625", 1.0 / 4096},
               {"524287.75", 524287.75},
               {"007", 7},
               {"-0", 0}};
  for (const auto& c : cases) {
    const Reading reading = Read(c.text);
    EXPECT_TRUE(reading.found) << c.text;
    EXPECT_NEAR(reading.value, c.value, 4 * kNearest) << c.text;
    EXPECT_EQ(reading.length, std::string_view(c.text).size()) << c.text;
    EXPECT_FALSE(reading.clamped) << c.text;
  }
}

TEST(SvgNumber, ReadsANumberWrittenWithAnExponent) {
  const struct {
    const char* text;
    double value;
  } cases[] = {{"1e3", 1000}, {"1E3", 1000}, {"1e+3", 1000}, {"2.5e-2", 0.025}, {"-2.5E-2", -0.025},
               {".5e1", 5},   {"1.e2", 100}, {"12e0", 12},   {"1e-3", 0.001},   {"123456e-3", 123.456},
               {"1e-40", 0},  {"0e99", 0},   {"0.01e2", 1}};
  for (const auto& c : cases) {
    const Reading reading = Read(c.text);
    EXPECT_TRUE(reading.found) << c.text;
    // A number is rounded to what a Number can hold before its exponent is
    // applied, so these are a little less exact than a number written out
    // plainly: by a 2^32th for each power of ten.
    EXPECT_NEAR(reading.value, c.value, 1e-7) << c.text;
    EXPECT_EQ(reading.length, std::string_view(c.text).size()) << c.text;
    EXPECT_FALSE(reading.clamped) << c.text;
  }
}

TEST(SvgNumber, StopsAtTheFirstCharacterThatIsNotPartOfTheNumber) {
  const struct {
    const char* text;
    double value;
    std::size_t length;
  } cases[] = {{"1.5.5", 1.5, 3}, {"10-5", 10, 2},     {"1 2", 1, 1},    {"1,2", 1, 1}, {"-1-2", -1, 2}, {"3+4", 3, 1},
               {"12px", 12, 2},   {"1e5x", 100000, 3}, {".5.5", 0.5, 2}, {"1L2", 1, 1}, {"1.e", 1, 2}};
  for (const auto& c : cases) {
    const Reading reading = Read(c.text);
    if (std::string_view(c.text) == "1.e") {
      // The "e" is taken for the start of an exponent that never comes, and
      // then there is no number: the reader cannot give the "e" back.
      EXPECT_FALSE(reading.found);
      continue;
    }
    EXPECT_TRUE(reading.found) << c.text;
    EXPECT_NEAR(reading.value, c.value, 4 * kNearest) << c.text;
    EXPECT_EQ(reading.length, c.length) << c.text;
  }
}

TEST(SvgNumber, SaysWhatIsNotANumberAndLeavesItsPlaceAlone) {
  for (const char* text :
       {"", "-", "+", ".", "-.", "+.", "e5", "E5", "1e", "1e-", "1e+", "--1", "+-1", "abc", " 1", ",1"}) {
    Number number = 99;
    std::size_t at = 0;
    bool clamped = false;
    EXPECT_FALSE(ScanNumber(text, at, number, clamped)) << "\"" << text << "\"";
    EXPECT_EQ(at, 0u) << text;
    EXPECT_EQ(number, 99) << text;
  }
}

TEST(SvgNumber, ReadsFromWhereItIsToldTo) {
  const std::string_view text = "ab12.5cd";
  std::size_t at = 2;
  Number number = 0;
  bool clamped = false;
  ASSERT_TRUE(ScanNumber(text, at, number, clamped));
  EXPECT_EQ(at, 6u);
  EXPECT_EQ(number, 12 * kNumberOne + (kNumberOne / 2));
  at = text.size();
  EXPECT_FALSE(ScanNumber(text, at, number, clamped));
}

TEST(SvgNumber, HoldsANumberTooLargeAtTheLargestAndSaysSo) {
  constexpr double kLargest = 1073741824.0;
  for (const char* text : {"17394593339182234", "7014632285136346", "1073741825", "99999999999999999999999999999999",
                           "1e10", "5e300", "1e99999", "123456789012.5"}) {
    const Reading reading = Read(text);
    EXPECT_TRUE(reading.found) << text;
    EXPECT_TRUE(reading.clamped) << text;
    EXPECT_EQ(reading.value, kLargest) << text;
    EXPECT_EQ(reading.length, std::string_view(text).size()) << text;
  }
  const Reading negative = Read("-17394593339182234");
  EXPECT_TRUE(negative.clamped);
  EXPECT_EQ(negative.value, -kLargest);
  // Just inside is not clamped.
  const Reading inside = Read("1073741823.5");
  EXPECT_FALSE(inside.clamped);
  EXPECT_EQ(inside.value, 1073741823.5);
}

TEST(SvgNumber, DropsDigitsAfterThePointThatCannotMatter) {
  const Reading reading = Read("0.12345678901234567890123456789");
  EXPECT_TRUE(reading.found);
  EXPECT_NEAR(reading.value, 0.123456789012345678, 4 * kNearest);
  EXPECT_EQ(reading.length, 31u);
  const Reading tiny = Read("0.00000000000000000000000000001");
  EXPECT_TRUE(tiny.found);
  EXPECT_EQ(tiny.value, 0);
}

TEST(SvgNumber, ReadsWhatAProgramWouldWrite) {
  std::mt19937 random = Repeatable(7);
  std::uniform_real_distribution<double> anywhere(-600000.0, 600000.0);
  std::uniform_real_distribution<double> small(-2.0, 2.0);
  std::array<char, 64> text{};
  for (int i = 0; i < 50000; ++i) {
    const double value = (i % 2 == 0) ? anywhere(random) : small(random);
    const int places = static_cast<int>(random() % 10);
    std::snprintf(text.data(), text.size(), "%.*f", places, value);
    const Reading plain = Read(text.data());
    ASSERT_TRUE(plain.found) << text.data();
    ASSERT_NEAR(plain.value, std::strtod(text.data(), nullptr), 4 * kNearest) << text.data();

    std::snprintf(text.data(), text.size(), "%.*e", places, value);
    const Reading scientific = Read(text.data());
    ASSERT_TRUE(scientific.found) << text.data();
    // Dividing by ten a few times loses a little at each.
    ASSERT_NEAR(scientific.value, std::strtod(text.data(), nullptr), 1e-7 + std::abs(value) * 1e-9) << text.data();
  }
}

TEST(SvgNumber, CanBeUsedAgainAfterAReset) {
  SvgNumber number;
  bool clamped = false;
  EXPECT_FALSE(number.Valid());
  for (const char c : std::string_view("-12.5")) {
    EXPECT_TRUE(number.Take(c));
  }
  EXPECT_TRUE(number.Valid());
  EXPECT_FALSE(number.Take(' '));
  EXPECT_EQ(number.Value(clamped), -12 * kNumberOne - (kNumberOne / 2));
  number.Reset();
  EXPECT_FALSE(number.Valid());
  EXPECT_TRUE(number.Take('3'));
  EXPECT_EQ(number.Value(clamped), 3 * kNumberOne);
  EXPECT_FALSE(clamped);
}

TEST(SvgNumber, BecomesALengthAFactorOrAnAngle) {
  bool clamped = false;
  const auto number = [](const char* text) {
    Number value = 0;
    std::size_t at = 0;
    bool held = false;
    EXPECT_TRUE(ScanNumber(text, at, value, held)) << text;
    return value;
  };
  // A length is in 4096ths, and is rounded to the nearest.
  EXPECT_EQ(FixedOf(number("1.5"), clamped), 6144);
  EXPECT_EQ(FixedOf(number("-1.5"), clamped), -6144);
  EXPECT_EQ(FixedOf(number("0.0001"), clamped), 0);
  EXPECT_EQ(FixedOf(number("0.0002"), clamped), 1);
  EXPECT_EQ(FixedOf(number("524287.9997"), clamped), 2147483647);
  // A factor is in 1,048,576ths, so a small one keeps its precision.
  EXPECT_EQ(CoefficientOf(number("1"), clamped), 1048576);
  EXPECT_EQ(CoefficientOf(number("0.015"), clamped), 15729);
  EXPECT_EQ(CoefficientOf(number("-3.532"), clamped), -3703570);
  EXPECT_FALSE(clamped);

  EXPECT_EQ(FixedOf(number("524288"), clamped), kFixedMax);
  EXPECT_TRUE(clamped);
  clamped = false;
  EXPECT_EQ(FixedOf(number("-600000"), clamped), kFixedMin);
  EXPECT_TRUE(clamped);
  clamped = false;
  EXPECT_EQ(CoefficientOf(number("2048"), clamped), kFixedMax);
  EXPECT_TRUE(clamped);

  // An angle is given in degrees, and whole turns make no difference.
  EXPECT_EQ(AngleOfDegrees(number("0")), 0u);
  EXPECT_EQ(AngleOfDegrees(number("90")), kQuarterTurn);
  EXPECT_EQ(AngleOfDegrees(number("180")), 2 * kQuarterTurn);
  EXPECT_EQ(AngleOfDegrees(number("-90")), 3 * kQuarterTurn);
  EXPECT_EQ(AngleOfDegrees(number("360")), 0u);
  EXPECT_EQ(AngleOfDegrees(number("450")), kQuarterTurn);
  EXPECT_EQ(AngleOfDegrees(number("-270")), kQuarterTurn);
  EXPECT_NEAR(static_cast<double>(AngleOfDegrees(number("45.5"))), 45.5 / 360.0 * 4294967296.0, 2.0);
}

}  // namespace
