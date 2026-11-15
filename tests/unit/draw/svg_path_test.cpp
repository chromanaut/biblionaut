#include "draw/svg_path.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>

namespace {

using bn::draw::Angle;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::PathSink;
using bn::draw::Point;
using bn::draw::SvgPathReader;
using bn::draw::SvgPathStatus;
using bn::draw::SvgPathStatusName;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// Writes down what it is told, a command at a time: "M 1 2", "L 3 4",
// "C 1 2 3 4 5 6", "Q 1 2 3 4", "A 5 6 30 1 0 7 8" (two radii, the turn in
// degrees, the two flags, the place) and "Z", with "|" between them.
class Written final : public PathSink {
 public:
  void MoveTo(Point to) noexcept override { Add("M", {to}); }
  void LineTo(Point to) noexcept override { Add("L", {to}); }
  void QuadTo(Point control, Point to) noexcept override { Add("Q", {control, to}); }
  void CubicTo(Point control1, Point control2, Point to) noexcept override { Add("C", {control1, control2, to}); }
  void ArcTo(Fixed radiusX, Fixed radiusY, Angle turn, bool largeArc, bool sweep, Point to) noexcept override {
    Separate();
    text_ += "A " + Text(radiusX) + " " + Text(radiusY) + " ";
    text_ += Shortest(static_cast<double>(turn) / 4294967296.0 * 360.0);
    text_ += largeArc ? " 1" : " 0";
    text_ += sweep ? " 1 " : " 0 ";
    text_ += Text(to.x) + " " + Text(to.y);
  }
  void Close() noexcept override {
    Separate();
    text_ += 'Z';
  }

  const std::string& All() const { return text_; }
  // How many things it has been told.
  std::size_t Told() const { return told_; }

 private:
  static std::string Shortest(double value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.6g", value);
    return text.data();
  }
  static std::string Text(Fixed value) { return Shortest(static_cast<double>(value) / kFixedOne); }
  void Separate() {
    if (!text_.empty()) {
      text_ += '|';
    }
    ++told_;
  }
  void Add(const char* letter, std::initializer_list<Point> points) {
    Separate();
    text_ += letter;
    for (const Point point : points) {
      text_ += " " + Text(point.x) + " " + Text(point.y);
    }
  }

  std::string text_;
  std::size_t told_ = 0;
};

struct Outcome {
  std::string drawn;
  SvgPathStatus status = SvgPathStatus::kOk;
  bool clamped = false;
  std::uint32_t commands = 0;
  std::uint32_t errorOffset = 0;
};

// Reads `data` in pieces of `piece` bytes (all at once if 0).
Outcome Draw(std::string_view data, std::size_t piece = 0) {
  Written written;
  SvgPathReader reader(written);
  if (piece == 0) {
    reader.Feed(data);
  } else {
    for (std::size_t at = 0; at < data.size(); at += piece) {
      reader.Feed(std::string_view(data.data() + at, std::min(piece, data.size() - at)));
    }
  }
  Outcome outcome;
  outcome.status = reader.Finish();
  EXPECT_EQ(outcome.status, reader.Status());
  outcome.drawn = written.All();
  outcome.clamped = reader.Clamped();
  outcome.commands = reader.Commands();
  outcome.errorOffset = reader.ErrorOffset();
  return outcome;
}

// What a well-formed path draws.
std::string Drawn(std::string_view data) {
  const Outcome outcome = Draw(data);
  EXPECT_EQ(outcome.status, SvgPathStatus::kOk) << data;
  EXPECT_FALSE(outcome.clamped) << data;
  return outcome.drawn;
}

TEST(SvgPath, ReadsEachCommandWithPlacesAsWritten) {
  EXPECT_EQ(Drawn("M 10 20"), "M 10 20");
  EXPECT_EQ(Drawn("M 10 20 L 30 40"), "M 10 20|L 30 40");
  EXPECT_EQ(Drawn("M 10 20 H 30"), "M 10 20|L 30 20");
  EXPECT_EQ(Drawn("M 10 20 V 30"), "M 10 20|L 10 30");
  EXPECT_EQ(Drawn("M 0 0 C 1 2 3 4 5 6"), "M 0 0|C 1 2 3 4 5 6");
  EXPECT_EQ(Drawn("M 0 0 Q 1 2 3 4"), "M 0 0|Q 1 2 3 4");
  EXPECT_EQ(Drawn("M 0 0 A 5 6 30 1 0 7 8"), "M 0 0|A 5 6 30 1 0 7 8");
  EXPECT_EQ(Drawn("M 1 2 L 3 4 Z"), "M 1 2|L 3 4|Z");
}

TEST(SvgPath, ASmallLetterMeasuresFromWhereThePenIs) {
  EXPECT_EQ(Drawn("m 10 20"), "M 10 20");
  EXPECT_EQ(Drawn("M 10 20 m 1 1"), "M 10 20|M 11 21");
  EXPECT_EQ(Drawn("M 10 20 l 30 40"), "M 10 20|L 40 60");
  EXPECT_EQ(Drawn("M 10 20 h 30"), "M 10 20|L 40 20");
  EXPECT_EQ(Drawn("M 10 20 v -30"), "M 10 20|L 10 -10");
  // Every point of a curve is measured from where the curve starts.
  EXPECT_EQ(Drawn("M 10 20 c 1 2 3 4 5 6"), "M 10 20|C 11 22 13 24 15 26");
  EXPECT_EQ(Drawn("M 10 20 q 1 2 3 4"), "M 10 20|Q 11 22 13 24");
  EXPECT_EQ(Drawn("M 10 20 s 3 4 5 6"), "M 10 20|C 10 20 13 24 15 26");
  EXPECT_EQ(Drawn("M 10 20 t 5 6"), "M 10 20|Q 10 20 15 26");
  // The radii and the turn of an arc are not places, and stay as written.
  EXPECT_EQ(Drawn("M 10 20 a 5 6 30 0 1 7 8"), "M 10 20|A 5 6 30 0 1 17 28");
  EXPECT_EQ(Drawn("M 1 2 l 3 4 z"), "M 1 2|L 4 6|Z");
}

TEST(SvgPath, MoreNumbersAfterAMoveAreLines) {
  EXPECT_EQ(Drawn("M 1 2 3 4 5 6"), "M 1 2|L 3 4|L 5 6");
  EXPECT_EQ(Drawn("m 1 2 3 4 5 6"), "M 1 2|L 4 6|L 9 12");
}

TEST(SvgPath, ACommandWrittenOnceIsRepeatedForEachSetOfNumbers) {
  EXPECT_EQ(Drawn("M 0 0 L 1 2 3 4"), "M 0 0|L 1 2|L 3 4");
  EXPECT_EQ(Drawn("M 0 0 l 1 2 3 4"), "M 0 0|L 1 2|L 4 6");
  EXPECT_EQ(Drawn("M 0 0 H 1 2 3"), "M 0 0|L 1 0|L 2 0|L 3 0");
  EXPECT_EQ(Drawn("M 0 0 v 1 2 3"), "M 0 0|L 0 1|L 0 3|L 0 6");
  EXPECT_EQ(Drawn("M 0 0 C 1 1 2 2 3 3 4 4 5 5 6 6"), "M 0 0|C 1 1 2 2 3 3|C 4 4 5 5 6 6");
  EXPECT_EQ(Drawn("M 0 0 q 1 1 2 2 1 1 2 2"), "M 0 0|Q 1 1 2 2|Q 3 3 4 4");
  EXPECT_EQ(Drawn("M 0 0 a 1 1 0 0 0 2 2 1 1 0 1 1 2 2"), "M 0 0|A 1 1 0 0 0 2 2|A 1 1 0 1 1 4 4");
  EXPECT_EQ(Draw("M 0 0 L 1 2 3 4").commands, 3u);
}

TEST(SvgPath, AShortCurveMirrorsTheControlPointOfTheCurveBeforeIt) {
  // The cubic before ends at (30, 30) having been pulled towards (20, 40):
  // the next is pulled first towards the same distance the other side.
  EXPECT_EQ(Drawn("M 0 0 C 10 0 20 40 30 30 S 50 10 60 30"), "M 0 0|C 10 0 20 40 30 30|C 40 20 50 10 60 30");
  EXPECT_EQ(Drawn("M 0 0 c 10 0 20 40 30 30 s 20 -20 30 0"), "M 0 0|C 10 0 20 40 30 30|C 40 20 50 10 60 30");
  EXPECT_EQ(Drawn("M 0 0 S 5 5 10 0 S 15 -5 20 0 20 10 30 10"),
            "M 0 0|C 0 0 5 5 10 0|C 15 -5 15 -5 20 0|C 25 5 20 10 30 10");
  EXPECT_EQ(Drawn("M 0 0 Q 10 20 20 0 T 40 0"), "M 0 0|Q 10 20 20 0|Q 30 -20 40 0");
  EXPECT_EQ(Drawn("M 0 0 Q 10 20 20 0 T 40 0 60 0"), "M 0 0|Q 10 20 20 0|Q 30 -20 40 0|Q 50 20 60 0");
  EXPECT_EQ(Drawn("M 0 0 q 10 20 20 0 t 20 0"), "M 0 0|Q 10 20 20 0|Q 30 -20 40 0");
}

TEST(SvgPath, AShortCurveWithNoCurveOfItsKindBeforeItStartsFromThePen) {
  EXPECT_EQ(Drawn("M 5 5 S 10 10 20 5"), "M 5 5|C 5 5 10 10 20 5");
  EXPECT_EQ(Drawn("M 5 5 T 20 5"), "M 5 5|Q 5 5 20 5");
  // A line in between, or a curve of the other kind, is not a curve before.
  EXPECT_EQ(Drawn("M 0 0 C 1 1 2 2 3 3 L 5 5 S 6 6 7 7"), "M 0 0|C 1 1 2 2 3 3|L 5 5|C 5 5 6 6 7 7");
  EXPECT_EQ(Drawn("M 0 0 Q 1 1 3 3 S 6 6 7 7"), "M 0 0|Q 1 1 3 3|C 3 3 6 6 7 7");
  EXPECT_EQ(Drawn("M 0 0 C 1 1 2 2 3 3 T 7 7"), "M 0 0|C 1 1 2 2 3 3|Q 3 3 7 7");
  EXPECT_EQ(Drawn("M 0 0 C 1 1 2 2 3 3 Z S 6 6 7 7"), "M 0 0|C 1 1 2 2 3 3|Z|M 0 0|C 0 0 6 6 7 7");
}

TEST(SvgPath, NumbersNeedNothingBetweenThemWhereItIsClearWhereOneEnds) {
  EXPECT_EQ(Drawn("M1.5.5L10-5"), "M 1.5 0.5|L 10 -5");
  EXPECT_EQ(Drawn("M-1-2l-3-4"), "M -1 -2|L -4 -6");
  EXPECT_EQ(Drawn("M.5.5.5.5"), "M 0.5 0.5|L 0.5 0.5");
  EXPECT_EQ(Drawn("M0 0L1e1-1e1"), "M 0 0|L 10 -10");
  EXPECT_EQ(Drawn("M0,0L1,2,3,4"), "M 0 0|L 1 2|L 3 4");
  EXPECT_EQ(Drawn(" \t\nM \r 1 \f, 2 \n"), "M 1 2");
  EXPECT_EQ(Drawn("M1 2L3 4z"), "M 1 2|L 3 4|Z");
  EXPECT_EQ(Drawn("M1 2zM3 4"), "M 1 2|Z|M 3 4");
}

TEST(SvgPath, TheFlagsOfAnArcNeedNothingAfterThem) {
  EXPECT_EQ(Drawn("M0 0a1 1 0 011 1"), "M 0 0|A 1 1 0 0 1 1 1");
  EXPECT_EQ(Drawn("M0 0a1 1 0 1 0 5 5"), "M 0 0|A 1 1 0 1 0 5 5");
  EXPECT_EQ(Drawn("M0 0a1,1,0,1,0,5,5"), "M 0 0|A 1 1 0 1 0 5 5");
  EXPECT_EQ(Drawn("M0 0a1 1 0 10-5-5"), "M 0 0|A 1 1 0 1 0 -5 -5");
  EXPECT_EQ(Drawn("M0 0a1 1 0 00.5.5"), "M 0 0|A 1 1 0 0 0 0.5 0.5");
  EXPECT_EQ(Drawn("M0 0A8971.87 8971.87 0 0 0-23.70 5"), "M 0 0|A 8971.87 8971.87 0 0 0 -23.7 5");
  // And an arc written once repeats like any other command.
  EXPECT_EQ(Drawn("M0 0a1 1 0 011 1 1 1 0 101 1"), "M 0 0|A 1 1 0 0 1 1 1|A 1 1 0 1 0 2 2");
}

TEST(SvgPath, ARadiusWrittenWithAMinusSignIsTakenWithoutIt) {
  EXPECT_EQ(Drawn("M0 0A-5 -6 0 0 0 7 8"), "M 0 0|A 5 6 0 0 0 7 8");
  EXPECT_EQ(Drawn("M0 0A5 6 -90 0 0 7 8"), "M 0 0|A 5 6 270 0 0 7 8");
  EXPECT_EQ(Drawn("M0 0A5 6 450 0 0 7 8"), "M 0 0|A 5 6 90 0 0 7 8");
}

TEST(SvgPath, AfterClosingTheNextThingDrawnBeginsWhereTheOutlineBegan) {
  EXPECT_EQ(Drawn("M 1 1 l 1 0 0 1 z l 5 0"), "M 1 1|L 2 1|L 2 2|Z|M 1 1|L 6 1");
  EXPECT_EQ(Drawn("M 1 1 L 2 2 Z L 3 3"), "M 1 1|L 2 2|Z|M 1 1|L 3 3");
  EXPECT_EQ(Drawn("M 1 1 L 2 2 z h 4 z"), "M 1 1|L 2 2|Z|M 1 1|L 5 1|Z");
  // A move after closing says where the next outline begins by itself; a
  // small one is measured from where the closed one began.
  EXPECT_EQ(Drawn("M 1 1 L 2 2 z m 1 1 l 1 0"), "M 1 1|L 2 2|Z|M 2 2|L 3 2");
  EXPECT_EQ(Drawn("M 1 1 L 2 2 Z M 5 5 L 6 6"), "M 1 1|L 2 2|Z|M 5 5|L 6 6");
  // Closing twice draws nothing the second time.
  EXPECT_EQ(Drawn("M 1 1 L 2 2 Z Z z"), "M 1 1|L 2 2|Z");
  EXPECT_EQ(Draw("M 1 1 L 2 2 Z Z z").commands, 5u);
}

TEST(SvgPath, NoDataIsNoOutlineAndNoMistake) {
  for (const char* data : {"", " ", " \n\t "}) {
    const Outcome outcome = Draw(data);
    EXPECT_EQ(outcome.status, SvgPathStatus::kOk);
    EXPECT_EQ(outcome.drawn, "");
    EXPECT_EQ(outcome.commands, 0u);
  }
}

TEST(SvgPath, StopsAtAMistakeHavingDrawnEverythingBeforeIt) {
  const struct {
    const char* data;
    const char* drawn;
    std::uint32_t errorOffset;
  } cases[] = {
      // It has to start with a move.
      {"1 2", "", 0},
      {"L 1 2", "", 0},
      {"Z", "", 0},
      {"x", "", 0},
      // A letter that is no command.
      {"M 0 0 K 1 2", "M 0 0", 6},
      {"M 0 0 L 1 2 e", "M 0 0|L 1 2", 12},
      {"M 0 0 # L 1 2", "M 0 0", 6},
      // A command without all its numbers.
      {"M 0", "", 3},
      {"M 0 0 L 1", "M 0 0", 9},
      {"M 0 0 L 1 2 3", "M 0 0|L 1 2", 13},
      {"M 0 0 L 1 Z", "M 0 0", 10},
      {"M 0 0 C 1 2 3 4 5 L 6 6", "M 0 0", 18},
      {"M 0 0 L", "M 0 0", 7},
      {"M 0 0 L M 1 1", "M 0 0", 8},
      {"M M 1 1", "", 2},
      // Commas only between numbers, and one at a time.
      {"M,0 0", "", 1},
      {"M 0,,0", "", 4},
      {"M 0 0,", "M 0 0", 6},
      {"M 0 0,L 1 1", "M 0 0", 6},
      {",M 0 0", "", 0},
      // Closing takes no numbers.
      {"M 0 0 L 1 1 Z 2 2", "M 0 0|L 1 1|Z", 14},
      // A flag is 0 or 1 and nothing else.
      {"M 0 0 A 1 1 0 2 0 5 5", "M 0 0", 14},
      {"M 0 0 A 1 1 0 0 -1 5 5", "M 0 0", 16},
      {"M 0 0 A 1 1 0 0 .5 5 5", "M 0 0", 16},
      // Things that only begin like numbers.
      {"M 0 0 L - 1", "M 0 0", 9},
      {"M 0 0 L . 1", "M 0 0", 9},
      {"M 0 0 L 1e 2", "M 0 0", 10},
      {"M 0 0 L 1 2e", "M 0 0", 12},
      {"M 0 0 L +-1 2", "M 0 0", 9},
  };
  for (const auto& c : cases) {
    const Outcome outcome = Draw(c.data);
    EXPECT_EQ(outcome.status, SvgPathStatus::kBadData) << c.data;
    EXPECT_EQ(outcome.drawn, c.drawn) << c.data;
    EXPECT_EQ(outcome.errorOffset, c.errorOffset) << c.data;
  }
}

TEST(SvgPath, NothingIsDrawnAfterAMistake) {
  Written written;
  SvgPathReader reader(written);
  reader.Feed("M 0 0 L 1 1 ! L 2 2");
  reader.Feed(" L 3 3 Z M 9 9");
  EXPECT_EQ(reader.Finish(), SvgPathStatus::kBadData);
  EXPECT_EQ(written.All(), "M 0 0|L 1 1");
  EXPECT_EQ(reader.ErrorOffset(), 12u);
  EXPECT_EQ(reader.Commands(), 2u);
}

TEST(SvgPath, ReadsTheSameWhereverTheDataIsCut) {
  const std::string_view data =
      "M10.5-20.25l1e1,2.5.5.5H40v-3.75C1 2,3 4,5 6s1-1 2-2Q0 0 9 9t1 1,2 2"
      "a25.5 26.5-30 1 0 50-25A1 1 0 011 1zm1 1L-1.5e-1,+.25Z";
  const Outcome whole = Draw(data);
  ASSERT_EQ(whole.status, SvgPathStatus::kOk);
  ASSERT_GT(whole.commands, 15u);
  for (std::size_t piece = 1; piece <= data.size(); ++piece) {
    const Outcome cut = Draw(data, piece);
    ASSERT_EQ(cut.status, SvgPathStatus::kOk) << piece;
    ASSERT_EQ(cut.drawn, whole.drawn) << piece;
    ASSERT_EQ(cut.commands, whole.commands) << piece;
  }
  // And a mistake is found in the same place.
  const std::string_view broken = "M10.5-20.25l1e1,2.5.5.5H40v-3.75C1 2,3 4,5 6s1-1 2-2Q0 0 9 9 9";
  const Outcome wholeBroken = Draw(broken);
  ASSERT_EQ(wholeBroken.status, SvgPathStatus::kBadData);
  for (std::size_t piece = 1; piece <= broken.size(); ++piece) {
    const Outcome cut = Draw(broken, piece);
    ASSERT_EQ(cut.status, SvgPathStatus::kBadData) << piece;
    ASSERT_EQ(cut.drawn, wholeBroken.drawn) << piece;
    ASSERT_EQ(cut.errorOffset, wholeBroken.errorOffset) << piece;
  }
}

TEST(SvgPath, HoldsAPlaceTooFarOffAtTheEdgeAndSaysSo) {
  // An arc all but straight, with a radius of seventeen digits, as two
  // books have them.
  Outcome outcome = Draw("M0 0a17394593339182234 17394593339182234 0 0 0 5 5");
  EXPECT_EQ(outcome.status, SvgPathStatus::kOk);
  EXPECT_TRUE(outcome.clamped);
  EXPECT_EQ(outcome.drawn, "M 0 0|A 524288 524288 0 0 0 5 5");

  outcome = Draw("M0 0L600000 -600000");
  EXPECT_EQ(outcome.status, SvgPathStatus::kOk);
  EXPECT_TRUE(outcome.clamped);
  EXPECT_EQ(outcome.drawn, "M 0 0|L 524288 -524288");

  // Small steps that add up to too much.
  outcome = Draw("M400000 0 l100000 0 100000 0 h100000 v-300000 -300000");
  EXPECT_EQ(outcome.status, SvgPathStatus::kOk);
  EXPECT_TRUE(outcome.clamped);
  EXPECT_EQ(outcome.drawn, "M 400000 0|L 500000 0|L 524288 0|L 524288 0|L 524288 -300000|L 524288 -524288");

  // A control point mirrored to beyond the edge.
  outcome = Draw("M0 0C0 0-500000 0 500000 0S0 0 0 0");
  EXPECT_EQ(outcome.status, SvgPathStatus::kOk);
  EXPECT_TRUE(outcome.clamped);

  EXPECT_FALSE(Draw("M524287 -524287 L 1 1").clamped);
}

TEST(SvgPath, SmallDistancesAddUpToTheRightPlaceHoweverManyThereAre) {
  // A ten-thousandth of a unit is less than half the least a place can
  // show. Rounded one at a time, ten thousand of them would come to
  // nothing, where they should come to a whole unit.
  const auto ends = [](const std::string& step, const char* last) {
    std::string data = "M0 0";
    for (int i = 0; i < 10000; ++i) {
      data += step;
    }
    const std::string drawn = Drawn(data);
    const std::string tail = drawn.substr(drawn.rfind('|') + 1);
    EXPECT_EQ(tail, last) << step;
  };
  ends("l.0001 .0003", "L 1 3");
  ends("h.0001v-.0002", "L 1 -2");
  ends("t.0001 .0001", "Q 1 1 1 1");
  // The last curve's control points are where the one before it ended: a
  // step short of the end, which is the nearest a place can show to it.
  ends("c0 0 0 0 .00015 -.00005", "C 1.49976 -0.5 1.49976 -0.5 1.5 -0.5");
  // And a closed outline begins the next from exactly where it began.
  ends("l.0001 0zm.0001 0", "M 1 0");
}

// Reads `data` by Step(), asking for `commands` at a time, and checks each
// call against what a call may do.
Outcome DrawInSteps(std::string_view data, std::uint32_t commands) {
  Written written;
  SvgPathReader reader(written);
  std::size_t at = 0;
  while (at < data.size()) {
    const std::uint32_t before = reader.Commands();
    const std::size_t told = written.Told();
    const std::size_t taken = reader.Step(std::string_view(data.data() + at, data.size() - at), commands);
    // It gets somewhere, and never past the end.
    EXPECT_GT(taken, 0u);
    EXPECT_LE(taken, data.size() - at);
    if (taken == 0) {
      break;
    }
    // It gives out what it was asked for, or one more; and a command is
    // one thing told to the sink, or two.
    EXPECT_LE(reader.Commands() - before, commands + 1);
    EXPECT_LE(written.Told() - told, std::size_t{2} * (commands + 1));
    // Having stopped short, it had given out at least what was asked.
    if (taken < data.size() - at && reader.Status() == SvgPathStatus::kOk) {
      EXPECT_GE(reader.Commands() - before, commands);
    }
    at += taken;
  }
  Outcome outcome;
  outcome.status = reader.Finish();
  outcome.drawn = written.All();
  outcome.clamped = reader.Clamped();
  outcome.commands = reader.Commands();
  outcome.errorOffset = reader.ErrorOffset();
  return outcome;
}

TEST(SvgPath, AStepGivesOutNoMoreThanItWasAskedFor) {
  const std::string_view data =
      "M10.5-20.25l1e1,2.5.5.5H40v-3.75C1 2,3 4,5 6s1-1 2-2Q0 0 9 9t1 1,2 2"
      "a25.5 26.5-30 1 0 50-25A1 1 0 011 1zh5zv5zzl1 2Zm1 1L-1.5e-1,+.25Z";
  const Outcome whole = Draw(data);
  ASSERT_EQ(whole.status, SvgPathStatus::kOk);
  for (const std::uint32_t commands : {1u, 2u, 3u, 7u, 1000u}) {
    const Outcome stepped = DrawInSteps(data, commands);
    EXPECT_EQ(stepped.status, SvgPathStatus::kOk) << commands;
    EXPECT_EQ(stepped.drawn, whole.drawn) << commands;
    EXPECT_EQ(stepped.commands, whole.commands) << commands;
  }
  // The letter that closes an outline can end a number as well, and then
  // one byte is two commands: the one more that a step may give.
  Written written;
  SvgPathReader reader(written);
  EXPECT_EQ(reader.Step("M0 0L1 2Z", 1), 5u);
  EXPECT_EQ(written.All(), "M 0 0");
  EXPECT_EQ(reader.Step("1 2ZL9 9", 1), 4u);
  EXPECT_EQ(written.All(), "M 0 0|L 1 2|Z");
  // Asked for nothing, it takes nothing.
  EXPECT_EQ(reader.Step("L9 9", 0), 0u);
  EXPECT_EQ(reader.Commands(), 3u);
}

TEST(SvgPath, AStepIsBoundedWhateverBytesItIsGiven) {
  std::mt19937 random = Repeatable(15);
  const std::string_view alphabet = "MmLlHhVvCcSsQqTtAaZzzzZ0123456789+-.,eE \n01";
  for (int i = 0; i < 20000; ++i) {
    std::string data = (i % 2 == 0) ? "M0 0" : "";
    const std::size_t length = random() % 64;
    for (std::size_t at = 0; at < length; ++at) {
      data += (i % 4 == 3) ? static_cast<char>(random() % 256) : alphabet[random() % alphabet.size()];
    }
    const Outcome whole = Draw(data);
    const Outcome stepped = DrawInSteps(data, 1 + static_cast<std::uint32_t>(random() % 3));
    // And what is drawn is the same as when it is read all at once.
    ASSERT_EQ(stepped.status, whole.status) << data;
    ASSERT_EQ(stepped.drawn, whole.drawn) << data;
    ASSERT_EQ(stepped.errorOffset, whole.errorOffset) << data;
  }
}

TEST(SvgPath, AfterAMistakeAStepTakesTheRestUnread) {
  Written written;
  SvgPathReader reader(written);
  EXPECT_EQ(reader.Step("M0 0 L1 1 ! L2 2 L3 3", 1), 5u);
  EXPECT_EQ(reader.Step("L1 1 ! L2 2 L3 3", 1), 5u);
  EXPECT_EQ(reader.Step("! L2 2 L3 3", 1), 11u);
  EXPECT_EQ(reader.Status(), SvgPathStatus::kBadData);
  // So a caller that goes on giving it the data comes to the end of it.
  EXPECT_EQ(reader.Step("L4 4 L5 5", 1), 9u);
  EXPECT_EQ(reader.Finish(), SvgPathStatus::kBadData);
  EXPECT_EQ(written.All(), "M 0 0|L 1 1");
  EXPECT_EQ(reader.ErrorOffset(), 10u);
}

TEST(SvgPath, CanReadAnotherPathAfterAReset) {
  Written written;
  SvgPathReader reader(written);
  reader.Feed("M 1 2 L 3");
  EXPECT_EQ(reader.Finish(), SvgPathStatus::kBadData);
  reader.Reset();
  EXPECT_EQ(reader.Status(), SvgPathStatus::kOk);
  EXPECT_EQ(reader.Commands(), 0u);
  reader.Feed("m 5 5 l 1 1");
  EXPECT_EQ(reader.Finish(), SvgPathStatus::kOk);
  EXPECT_EQ(written.All(), "M 1 2|M 5 5|L 6 6");
}

TEST(SvgPath, EndsInAStatusWhateverBytesItIsGiven) {
  // For the sanitizers as much as for the answers: text made of the
  // characters path data is written in, and text made of any bytes at all.
  std::mt19937 random = Repeatable(8);
  const std::string_view alphabet = "MmLlHhVvCcSsQqTtAaZz0123456789+-.,eE \n01";
  std::size_t wellFormed = 0;
  for (int i = 0; i < 40000; ++i) {
    std::string data = (i % 2 == 0) ? "M0 0" : "";
    const std::size_t length = random() % 48;
    for (std::size_t at = 0; at < length; ++at) {
      data += (i % 4 == 3) ? static_cast<char>(random() % 256) : alphabet[random() % alphabet.size()];
    }
    Written written;
    SvgPathReader reader(written);
    reader.Feed(data);
    const SvgPathStatus status = reader.Finish();
    ASSERT_TRUE(status == SvgPathStatus::kOk || status == SvgPathStatus::kBadData);
    if (status == SvgPathStatus::kOk) {
      ++wellFormed;
    } else {
      ASSERT_LE(reader.ErrorOffset(), data.size());
    }
  }
  // Some of what was made up is real path data, or the test tries nothing.
  EXPECT_GT(wellFormed, 100u);
}

TEST(SvgPath, NamesItsStatuses) {
  EXPECT_STREQ(SvgPathStatusName(SvgPathStatus::kOk), "ok");
  EXPECT_STREQ(SvgPathStatusName(SvgPathStatus::kBadData), "bad data");
}

}  // namespace
