#include "reader/pattern_viewer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/metrics.h"
#include "display_fakes.h"
#include "draw/test_pattern.h"
#include "reader/trace_ids.h"
#include "trace_fakes.h"

namespace {

using bn::core::AfterRefresh;
using bn::core::Button;
using bn::core::DisplayPlane;
using bn::core::InputEventKind;
using bn::core::RefreshMode;
using bn::reader::PatternViewer;
using bn::test::FakeDisplay;
using Op = bn::test::FakeDisplay::Op;

// One picture of the test pattern, as a whole plane.
std::vector<std::uint8_t> Picture(const FakeDisplay& display, int picture) {
  std::vector<std::uint8_t> bits(display.size.RowBytes() * display.size.height);
  bn::draw::TestPattern(display.size.width, display.size.height, static_cast<std::uint8_t>(picture))
      .RenderRows(0, bits);
  return bits;
}

struct ViewerFixture {
  ViewerFixture() { display.present = true; }

  // Starts the viewer, lets its first refresh finish, and lets it write the
  // next picture ahead.
  void StartAndSettle() {
    viewer.Start();
    FinishBusy();
    seen = display.calls.size();
  }

  void Press(Button button) { viewer.OnInput({.timeUs = clock.now, .button = button, .kind = InputEventKind::kPress}); }

  // Lets whatever the panel is doing finish and gives the viewer a turn.
  void FinishBusy() {
    display.busy = false;
    viewer.Tick();
  }

  // Gives the viewer turns until it asks nothing more of the panel.
  void Rest() {
    for (int turn = 0; turn < 8; ++turn) {
      const std::size_t before = display.calls.size();
      viewer.Tick();
      if (display.calls.size() == before) {
        return;
      }
    }
  }

  // Lets the two seconds pass, the voltages go off, and the viewer do
  // whatever it does about that.
  void Pause() {
    clock.now += PatternViewer::kPowerOffAfterUs;
    viewer.Tick();
    FinishBusy();
    Rest();
  }

  // The calls made since the last time this was asked.
  std::vector<FakeDisplay::Call> NewCalls() {
    std::vector<FakeDisplay::Call> calls(display.calls.begin() + static_cast<std::ptrdiff_t>(seen),
                                         display.calls.end());
    seen = display.calls.size();
    return calls;
  }

  bn::test::FakeClock clock;
  FakeDisplay display;
  PatternViewer viewer{display, clock};
  std::size_t seen = 0;
};

// Expects `calls` to be the whole picture in 80-row bands, to one plane.
void ExpectSweep(std::span<const FakeDisplay::Call> calls, DisplayPlane plane) {
  ASSERT_EQ(calls.size(), 6u);
  for (std::size_t band = 0; band < calls.size(); ++band) {
    EXPECT_EQ(calls[band].op, Op::kWrite) << band;
    EXPECT_EQ(calls[band].plane, plane) << band;
    EXPECT_EQ(calls[band].firstRow, band * 80) << band;
    EXPECT_EQ(calls[band].rowCount, 80u) << band;
  }
}

// Expects `calls` to be the whole picture, each band to both planes in turn.
void ExpectSweepOfBothPlanes(std::span<const FakeDisplay::Call> calls) {
  ASSERT_EQ(calls.size(), 12u);
  for (std::size_t i = 0; i < calls.size(); ++i) {
    EXPECT_EQ(calls[i].op, Op::kWrite) << i;
    EXPECT_EQ(calls[i].plane, i % 2 == 0 ? DisplayPlane::kNew : DisplayPlane::kOld) << i;
    EXPECT_EQ(calls[i].firstRow, (i / 2) * 80) << i;
    EXPECT_EQ(calls[i].rowCount, 80u) << i;
  }
}

// Expects `calls` to be nothing but one refresh.
void ExpectOnlyARefresh(std::span<const FakeDisplay::Call> calls, RefreshMode mode) {
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].op, Op::kRefresh);
  EXPECT_EQ(calls[0].mode, mode);
  EXPECT_EQ(calls[0].after, AfterRefresh::kStayPowered);
}

TEST(PatternViewer, DoesNothingUntilStartedAndFailsIfThePanelCannotBeSetUp) {
  ViewerFixture fixture;
  fixture.display.present = false;
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kNotStarted);
  EXPECT_TRUE(fixture.display.calls.empty());

  fixture.viewer.Start();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kFailed);
  EXPECT_EQ(fixture.viewer.Picture(), 0);
  ASSERT_EQ(fixture.display.calls.size(), 1u);
  EXPECT_EQ(fixture.display.calls[0].op, Op::kStart);

  // And it stays that way, whatever is pressed.
  fixture.clock.now += 60'000'000;
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.viewer.Start();
  EXPECT_EQ(fixture.display.calls.size(), 1u);
}

TEST(PatternViewer, StartsWithTheFirstPictureByAFullRefreshAndGetsTheSecondReady) {
  ViewerFixture fixture;
  fixture.viewer.Start();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kReady);
  EXPECT_EQ(fixture.viewer.Picture(), 1);

  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 14u);
  EXPECT_EQ(calls[0].op, Op::kStart);
  ExpectSweepOfBothPlanes(std::span(calls).subspan(1, 12));
  EXPECT_EQ(calls[13].op, Op::kRefresh);
  EXPECT_EQ(calls[13].mode, RefreshMode::kFull);
  EXPECT_EQ(calls[13].after, AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 1));

  // Nothing while the panel works. Then the picture Down would ask for,
  // into the new-image plane; the old one already holds the first.
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());
  fixture.FinishBusy();
  ExpectSweep(fixture.NewCalls(), DisplayPlane::kNew);
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kNew), Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kOld), Picture(fixture.display, 1));
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty()) << "once";
}

TEST(PatternViewer, DownHasOnlyToStartTheRefresh) {
  ViewerFixture fixture;
  fixture.StartAndSettle();

  fixture.Press(Button::kDown);
  EXPECT_TRUE(fixture.NewCalls().empty()) << "nothing happens until the next tick";
  fixture.viewer.Tick();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 2));

  // Nothing more while the panel works.
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  // Afterwards the picture after that, and nothing to the old-image plane:
  // the panel has made the picture it shows the old image by itself.
  fixture.FinishBusy();
  ExpectSweep(fixture.NewCalls(), DisplayPlane::kNew);
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kNew), Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kOld), Picture(fixture.display, 2));

  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, TheFourDirectionButtonsStepForwardAndBackAndWrapRound) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  const auto pressAndSettle = [&fixture](Button button) {
    fixture.Press(button);
    fixture.viewer.Tick();
    fixture.FinishBusy();
    return fixture.viewer.Picture();
  };
  const auto shows = [&fixture](int picture) { return fixture.display.shown == Picture(fixture.display, picture); };
  EXPECT_EQ(pressAndSettle(Button::kRight), 2);
  EXPECT_TRUE(shows(2));
  EXPECT_EQ(pressAndSettle(Button::kDown), 3);
  EXPECT_TRUE(shows(3));
  EXPECT_EQ(pressAndSettle(Button::kDown), 4);
  EXPECT_TRUE(shows(4));
  EXPECT_EQ(pressAndSettle(Button::kDown), 1);  // Round to the start.
  EXPECT_TRUE(shows(1));
  EXPECT_EQ(pressAndSettle(Button::kUp), 4);  // And back the other way.
  EXPECT_TRUE(shows(4));
  EXPECT_EQ(pressAndSettle(Button::kLeft), 3);
  EXPECT_TRUE(shows(3));
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 7u);
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, APictureThatIsNotReadyIsWrittenWhenItIsAskedFor) {
  ViewerFixture fixture;
  fixture.StartAndSettle();

  // Up: the picture got ready was the one Down would have asked for.
  fixture.Press(Button::kUp);
  fixture.viewer.Tick();
  const std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 7u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kNew);
  EXPECT_EQ(calls[6].mode, RefreshMode::kFast);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 4));
}

TEST(PatternViewer, GoingStraightBackNeedsNothingWritten) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  (void)fixture.NewCalls();

  // Up, pressed while the panel is still on its way to picture 2. After a
  // fast refresh the new-image plane holds the picture before, which is
  // the one wanted.
  fixture.Press(Button::kUp);
  fixture.FinishBusy();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  EXPECT_EQ(fixture.viewer.Picture(), 1);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 1));

  // And now it holds picture 2 again, which is the one after: nothing to
  // get ready either.
  fixture.FinishBusy();
  EXPECT_TRUE(fixture.NewCalls().empty());
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, ConfirmAndBackShowTheSamePictureAgainByHalfAndFullRefreshes) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  (void)fixture.NewCalls();

  fixture.Press(Button::kConfirm);
  fixture.viewer.Tick();
  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  ExpectSweepOfBothPlanes(std::span(calls).first(12));
  EXPECT_EQ(calls[12].mode, RefreshMode::kHalf);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 2));

  // Afterwards the next picture is got ready again: the redraw wrote over it.
  fixture.FinishBusy();
  ExpectSweep(fixture.NewCalls(), DisplayPlane::kNew);
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kNew), Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kOld), Picture(fixture.display, 2));

  fixture.Press(Button::kBack);
  fixture.viewer.Tick();
  calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  EXPECT_EQ(calls[12].mode, RefreshMode::kFull);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 2));

  // And a step after a redraw is right.
  fixture.FinishBusy();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 3));
}

TEST(PatternViewer, PressesMadeWhileThePanelIsBusyAreAddedUpAndActedOnTogether) {
  ViewerFixture fixture;
  fixture.viewer.Start();  // The first refresh is still running.
  const std::size_t afterStart = fixture.display.calls.size();
  fixture.Press(Button::kDown);
  fixture.Press(Button::kDown);
  fixture.Press(Button::kDown);
  fixture.Press(Button::kUp);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.calls.size(), afterStart);

  // Three forward and one back: two on, in one refresh.
  fixture.FinishBusy();
  EXPECT_EQ(fixture.viewer.Picture(), 3);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 2u);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, PressesThatCancelOutLeadToNothing) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.Press(Button::kUp);
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());
  EXPECT_EQ(fixture.viewer.Picture(), 1);

  // Four forward is all the way round: nothing to show either.
  for (int i = 0; i < 4; ++i) {
    fixture.Press(Button::kRight);
  }
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());
}

TEST(PatternViewer, ARedrawAskedForWithAStepShowsTheNewPictureByThatRefresh) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.Press(Button::kConfirm);
  fixture.viewer.Tick();
  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  EXPECT_EQ(calls[12].mode, RefreshMode::kHalf);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 2));
  fixture.FinishBusy();
  (void)fixture.NewCalls();

  // Back asks for more than Confirm does, whichever came first.
  fixture.Press(Button::kBack);
  fixture.Press(Button::kConfirm);
  fixture.viewer.Tick();
  calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  EXPECT_EQ(calls[12].mode, RefreshMode::kFull);
}

TEST(PatternViewer, OnlyPressesOfTheSixButtonsItUsesMatter) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kPower);
  fixture.viewer.OnInput({.timeUs = 5, .button = Button::kDown, .kind = InputEventKind::kRelease});
  fixture.viewer.OnInput({.timeUs = 6, .button = Button::kDown, .kind = InputEventKind::kHold});
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());
  EXPECT_EQ(fixture.viewer.Picture(), 1);
}

TEST(PatternViewer, SwitchesThePanelOffTwoSecondsAfterTheLastRefresh) {
  ViewerFixture fixture;
  fixture.StartAndSettle();

  fixture.clock.now += PatternViewer::kPowerOffAfterUs - 1;
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  fixture.clock.now += 1;
  fixture.viewer.Tick();
  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].op, Op::kPowerOff);
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty()) << "nothing while they go off";

  // Once is enough, however long nothing happens.
  fixture.FinishBusy();
  fixture.Rest();
  (void)fixture.NewCalls();
  fixture.clock.now += 60'000'000;
  fixture.viewer.Tick();
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  // A press brings the voltages back. Two seconds after that refresh they
  // are switched off again.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  fixture.clock.now += 10'000'000;  // The refresh takes a long time...
  fixture.FinishBusy();
  (void)fixture.NewCalls();
  fixture.clock.now += PatternViewer::kPowerOffAfterUs - 1;  // ...and the two seconds count from its end.
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());
  fixture.clock.now += 1;
  fixture.viewer.Tick();
  calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].op, Op::kPowerOff);
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, WritesBothPlanesAgainOnceTheVoltagesAreOff) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  (void)fixture.NewCalls();

  // One fast refresh since the voltages came on: switching them off puts
  // the planes back in their first places, which is the wrong way round.
  // The picture on the panel is in the new-image plane, and the one got
  // ready for later is in the old.
  fixture.clock.now += PatternViewer::kPowerOffAfterUs;
  fixture.viewer.Tick();
  ASSERT_EQ(fixture.NewCalls().size(), 1u);
  fixture.display.busy = false;
  ASSERT_EQ(fixture.display.Plane(DisplayPlane::kNew), Picture(fixture.display, 2));
  ASSERT_EQ(fixture.display.Plane(DisplayPlane::kOld), Picture(fixture.display, 3));

  // So the old-image plane is written with what the panel shows, and then
  // the picture after it is got ready again.
  fixture.viewer.Tick();
  ExpectSweep(fixture.NewCalls(), DisplayPlane::kOld);
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kOld), Picture(fixture.display, 2));
  fixture.viewer.Tick();
  ExpectSweep(fixture.NewCalls(), DisplayPlane::kNew);
  EXPECT_EQ(fixture.display.Plane(DisplayPlane::kNew), Picture(fixture.display, 3));
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  // And the next step, which only has to start the refresh, is right.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  ExpectOnlyARefresh(fixture.NewCalls(), RefreshMode::kFast);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.oldAtRefresh.back(), Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, ShowsTheRightPictureHoweverThePressesFallAroundASwitchOff) {
  // Every run of steps between two switch-offs, from none to four, forward
  // and back: an odd number leaves the planes one way round and an even
  // number the other.
  struct Step {
    Button button;
    bool pauseAfter;
  };
  const std::array<Step, 20> steps = {{
      {Button::kDown, true},  {Button::kDown, true},   {Button::kDown, false}, {Button::kDown, true},
      {Button::kUp, true},    {Button::kUp, false},    {Button::kUp, false},   {Button::kDown, true},
      {Button::kDown, false}, {Button::kDown, false},  {Button::kDown, false}, {Button::kUp, true},
      {Button::kUp, true},    {Button::kRight, false}, {Button::kLeft, true},  {Button::kDown, true},
      {Button::kDown, true},  {Button::kDown, true},   {Button::kUp, false},   {Button::kUp, true},
  }};
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Pause();  // Also with no step at all since the voltages came on.
  int expected = 1;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const bool forward = steps[i].button == Button::kDown || steps[i].button == Button::kRight;
    expected = forward ? (expected % 4) + 1 : ((expected + 2) % 4) + 1;
    fixture.Press(steps[i].button);
    fixture.viewer.Tick();
    ASSERT_EQ(fixture.viewer.Picture(), expected) << "step " << i;
    EXPECT_TRUE(fixture.display.shown == Picture(fixture.display, expected)) << "step " << i;
    fixture.FinishBusy();
    fixture.Rest();
    if (steps[i].pauseAfter) {
      fixture.Pause();
    }
  }
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kReady);
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, APressWhileTheVoltagesGoOffWaitsAndThenForTheOldPlane) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  fixture.clock.now += PatternViewer::kPowerOffAfterUs;
  fixture.viewer.Tick();
  (void)fixture.NewCalls();
  ASSERT_TRUE(fixture.display.busy);

  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  // The old-image plane first, then the picture, then the refresh.
  fixture.FinishBusy();
  const std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kOld);
  ExpectSweep(std::span(calls).subspan(6, 6), DisplayPlane::kNew);
  EXPECT_EQ(calls[12].op, Op::kRefresh);
  EXPECT_EQ(fixture.viewer.Picture(), 3);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, GivesUpOnAPanelThatNeverFinishes) {
  ViewerFixture fixture;
  fixture.viewer.Start();
  fixture.clock.now += PatternViewer::kGiveUpUs;
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kReady);

  fixture.clock.now += 1;
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kFailed);

  // Even if the panel comes back, it is over.
  const std::size_t calls = fixture.display.calls.size();
  fixture.Press(Button::kDown);
  fixture.FinishBusy();
  EXPECT_EQ(fixture.display.calls.size(), calls);
}

TEST(PatternViewer, FailsIfThePanelRefusesAWrite) {
  // The picture asked for.
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.display.failWrites = true;
  fixture.Press(Button::kUp);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kFailed);
  EXPECT_EQ(fixture.viewer.Picture(), 1) << "the picture never reached the panel";
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 1u);

  // The picture got ready ahead of time.
  ViewerFixture ahead;
  ahead.viewer.Start();
  ahead.display.failWrites = true;
  ahead.FinishBusy();
  EXPECT_EQ(ahead.viewer.CurrentState(), PatternViewer::State::kFailed);

  // The old-image plane, once the voltages are off.
  ViewerFixture afterSwitchOff;
  afterSwitchOff.StartAndSettle();
  afterSwitchOff.clock.now += PatternViewer::kPowerOffAfterUs;
  afterSwitchOff.viewer.Tick();
  afterSwitchOff.display.failWrites = true;
  afterSwitchOff.FinishBusy();
  EXPECT_EQ(afterSwitchOff.viewer.CurrentState(), PatternViewer::State::kFailed);

  // The first picture.
  ViewerFixture atStart;
  atStart.display.failWrites = true;
  atStart.viewer.Start();
  EXPECT_EQ(atStart.viewer.CurrentState(), PatternViewer::State::kFailed);
  EXPECT_EQ(atStart.display.Count(Op::kRefresh), 0u);
}

TEST(PatternViewer, CoversAPanelOfAnotherShapeInWholeBands) {
  // 792 x 528: rows of 99 bytes, and a last band that is not a full one.
  ViewerFixture fixture;
  fixture.display.Resize({.width = 792, .height = 528});
  fixture.viewer.Start();

  std::size_t rowsToNew = 0;
  for (const FakeDisplay::Call& call : fixture.display.calls) {
    if (call.op == Op::kWrite && call.plane == DisplayPlane::kNew) {
      EXPECT_EQ(call.firstRow, rowsToNew);
      EXPECT_LE(call.rowCount, 80u);
      rowsToNew += call.rowCount;
    }
  }
  EXPECT_EQ(rowsToNew, 528u);
  EXPECT_EQ(fixture.display.shown, Picture(fixture.display, 1));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PatternViewer, TracesEachPressFromTheButtonToTheRefreshBeingStarted) {
  bn::test::TraceFixture<64> trace;
  FakeDisplay display;
  display.present = true;
  PatternViewer viewer(display, trace.clock);
  viewer.Start();
  display.busy = false;
  viewer.Tick();
  std::array<bn::core::TraceRecord, 64> records{};
  (void)trace.tracer.Drain(records);  // The first picture, and the second got ready: no button was involved.

  // Pressed at 1.000 s, with nothing in the way; acted on at 1.006 s. The
  // picture was ready, so there is no sweep.
  trace.clock.now = 1'000'000;
  viewer.OnInput({.timeUs = trace.clock.now, .button = Button::kDown, .kind = InputEventKind::kPress});
  trace.clock.now = 1'006'000;
  viewer.Tick();

  std::size_t count = trace.tracer.Drain(records);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(records[0].id, bn::reader::kSpanPressToRefresh);
  EXPECT_EQ(records[0].startUs, 1'000'000u);
  EXPECT_EQ(records[0].durationUs, 6'000u);
  EXPECT_EQ(records[0].arg, static_cast<std::uint32_t>(Button::kDown));

  // Of several presses acted on together, the first is the one measured.
  trace.clock.now = 2'000'000;
  viewer.OnInput({.timeUs = 1'500'000, .button = Button::kRight, .kind = InputEventKind::kPress});
  viewer.OnInput({.timeUs = 1'700'000, .button = Button::kConfirm, .kind = InputEventKind::kPress});
  display.busy = false;
  viewer.Tick();
  count = trace.tracer.Drain(records);
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(records[0].id, bn::reader::kSpanPanelSweep);
  EXPECT_EQ(records[0].arg, 3u);
  EXPECT_EQ(records[1].id, bn::reader::kSpanPressToRefresh);
  EXPECT_EQ(records[1].startUs, 1'500'000u);
  EXPECT_EQ(records[1].arg, static_cast<std::uint32_t>(Button::kRight));
}

// ---- Reading the planes back ----

// The present value of one of the counters the viewer keeps.
std::uint32_t CounterNow(const std::string& name) {
  std::array<char, 2048> line{};
  bn::core::DefaultMetrics().FormatLine(line);
  const std::string text(line.data());
  const std::size_t at = text.find(name + "=");
  EXPECT_NE(at, std::string::npos) << text;
  return at == std::string::npos
             ? 0
             : static_cast<std::uint32_t>(std::strtoul(text.c_str() + at + name.size() + 1, nullptr, 10));
}

// A viewer told to check the planes, and a tracer to see what it records.
struct CheckFixture {
  CheckFixture() {
    display.present = true;
    viewer.CheckPlanes();
  }

  void Press(Button button) {
    viewer.OnInput({.timeUs = trace.clock.now, .button = button, .kind = InputEventKind::kPress});
  }

  void FinishBusy() {
    display.busy = false;
    viewer.Tick();
  }

  // Starts the viewer and lets it get the second picture ready.
  void StartAndSettle() {
    viewer.Start();
    FinishBusy();
  }

  // Enough turns to read both planes: a band of one plane each turn.
  void ReadThrough() {
    for (int turn = 0; turn < 12; ++turn) {
      viewer.Tick();
    }
  }

  // Everything recorded under `id` since Forget() was last called, in order.
  std::vector<std::uint32_t> Recorded(bn::core::SpanId id) {
    std::array<bn::core::TraceRecord, 64> records{};
    const std::size_t count = trace.tracer.Drain(records);
    for (std::size_t i = 0; i < count; ++i) {
      seen.push_back(records[i]);
    }
    std::vector<std::uint32_t> numbers;
    for (const bn::core::TraceRecord& record : seen) {
      if (record.id == id) {
        numbers.push_back(record.arg);
      }
    }
    return numbers;
  }
  void Forget() {
    (void)Recorded(bn::reader::kInstantNewPlaneHolds);
    seen.clear();
  }

  bn::test::TraceFixture<64> trace;
  FakeDisplay display;
  PatternViewer viewer{display, trace.clock};
  std::vector<bn::core::TraceRecord> seen;
};

using Numbers = std::vector<std::uint32_t>;

TEST(PatternViewer, ReadsNothingBackUnlessToldTo) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Rest();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  fixture.Rest();
  fixture.Pause();
  EXPECT_EQ(fixture.display.reads, 0);
}

TEST(PatternViewer, ReadsBothPlanesBackWhenThePanelComesToRest) {
  CheckFixture fixture;
  const std::uint32_t checksBefore = CounterNow("plane_checks");
  const std::uint32_t unexpectedBefore = CounterNow("planes_unexpected");
  fixture.viewer.Start();
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.reads, 0) << "not while the panel is busy";
  fixture.FinishBusy();
  EXPECT_EQ(fixture.display.reads, 0) << "the next picture is got ready first";

  // A band of one plane each turn: twelve turns for the two planes.
  for (int turn = 1; turn <= 12; ++turn) {
    EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantNewPlaneHolds).empty()) << turn;
    fixture.viewer.Tick();
    EXPECT_EQ(fixture.display.reads, turn);
  }
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{2}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{1}));
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.reads, 12) << "once";
  fixture.Forget();

  // After a step: the picture after the new one, and the new one, which
  // the panel has made the old image by itself.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  fixture.ReadThrough();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{3}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{2}));
  fixture.Forget();

  // And again once the voltages have been switched off and both planes
  // written again.
  fixture.trace.clock.now += PatternViewer::kPowerOffAfterUs;
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.Count(Op::kPowerOff), 1u);
  fixture.FinishBusy();   // The old-image plane.
  fixture.viewer.Tick();  // The next picture.
  fixture.ReadThrough();
  EXPECT_EQ(fixture.display.reads, 36);
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{3}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{2}));
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantNewPlaneBands).empty());
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantOldPlaneBands).empty());

  // Three readings, and every plane what it should have been.
  EXPECT_EQ(CounterNow("plane_checks"), checksBefore + 3);
  EXPECT_EQ(CounterNow("planes_unexpected"), unexpectedBefore);
}

TEST(PatternViewer, FindsThePlanesRightAfterGoingStraightBack) {
  // The one case where it writes nothing and relies on what the panel
  // left in the new-image plane.
  CheckFixture fixture;
  const std::uint32_t unexpectedBefore = CounterNow("planes_unexpected");
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.Press(Button::kUp);
  fixture.FinishBusy();
  ASSERT_EQ(fixture.viewer.Picture(), 1);
  fixture.FinishBusy();
  fixture.ReadThrough();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{2}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{1}));
  EXPECT_EQ(CounterNow("planes_unexpected"), unexpectedBefore);
}

TEST(PatternViewer, TheVoltagesStayOnForTheirUsualTimeAfterThePlanesHaveBeenRead) {
  CheckFixture fixture;
  fixture.StartAndSettle();
  // The reading takes its time...
  fixture.trace.clock.now += PatternViewer::kPowerOffAfterUs;
  for (int turn = 0; turn < 12; ++turn) {
    fixture.viewer.Tick();
    EXPECT_EQ(fixture.display.Count(Op::kPowerOff), 0u) << turn;
  }
  EXPECT_EQ(fixture.display.reads, 12);
  // ...and the two seconds run from the end of it.
  fixture.trace.clock.now += PatternViewer::kPowerOffAfterUs - 1;
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.Count(Op::kPowerOff), 0u);
  fixture.trace.clock.now += 1;
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.Count(Op::kPowerOff), 1u);
}

TEST(PatternViewer, CountsAPlaneThatIsNotWhatItShouldBeAndSaysWhatItHoldsBandByBand) {
  CheckFixture fixture;
  fixture.StartAndSettle();
  const std::uint32_t checksBefore = CounterNow("plane_checks");
  const std::uint32_t unexpectedBefore = CounterNow("planes_unexpected");

  // Behind the viewer's back: the top of picture 1, a blank band, bands of
  // pictures 4 and 3 where the pictures differ, and one that is none of
  // them.
  const std::size_t bandBytes = 8000;
  const auto bandOf = [&](int picture, std::size_t band) {
    const std::vector<std::uint8_t> whole = Picture(fixture.display, picture);
    const auto begin = whole.begin() + static_cast<std::ptrdiff_t>(band * bandBytes);
    return std::vector<std::uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(bandBytes));
  };
  std::vector<std::uint8_t> mixed;
  for (const std::vector<std::uint8_t>& band :
       {bandOf(1, 0), bandOf(1, 1), std::vector<std::uint8_t>(bandBytes, 0xFF), bandOf(4, 3), bandOf(3, 4),
        std::vector<std::uint8_t>(bandBytes, 0x5A)}) {
    mixed.insert(mixed.end(), band.begin(), band.end());
  }
  ASSERT_EQ(mixed.size(), fixture.display.Plane(DisplayPlane::kOld).size());
  fixture.display.Plane(DisplayPlane::kOld) = mixed;

  fixture.ReadThrough();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{2}));
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantNewPlaneBands).empty());
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{9}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneBands), (Numbers{118439}));
  EXPECT_EQ(CounterNow("plane_checks"), checksBefore + 1);
  EXPECT_EQ(CounterNow("planes_unexpected"), unexpectedBefore + 1);
}

TEST(PatternViewer, RecognisesABlankPlaneAndAPictureInTheWrongPlane) {
  CheckFixture fixture;
  fixture.StartAndSettle();
  const std::uint32_t unexpectedBefore = CounterNow("planes_unexpected");
  std::vector<std::uint8_t>& oldPlane = fixture.display.Plane(DisplayPlane::kOld);
  oldPlane.assign(oldPlane.size(), 0xFF);
  fixture.ReadThrough();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{8}));
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantOldPlaneBands).empty());
  EXPECT_EQ(CounterNow("planes_unexpected"), unexpectedBefore + 1);
  fixture.Forget();

  // The two planes the wrong way round, as a switch-off can leave them.
  fixture.Press(Button::kConfirm);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  std::swap(fixture.display.Plane(DisplayPlane::kOld), fixture.display.Plane(DisplayPlane::kNew));
  fixture.ReadThrough();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{1}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{2}));
  EXPECT_EQ(CounterNow("planes_unexpected"), unexpectedBefore + 2);
}

TEST(PatternViewer, APressPutsOffReadingThePlanesBack) {
  CheckFixture fixture;
  fixture.StartAndSettle();
  fixture.viewer.Tick();
  fixture.viewer.Tick();
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.reads, 3);

  // The press is acted on at once, and the reading starts again from the
  // top when the panel is next at rest.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.reads, 3);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 2u);
  fixture.FinishBusy();  // The picture after.
  fixture.ReadThrough();
  EXPECT_EQ(fixture.display.reads, 15);
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantNewPlaneHolds), (Numbers{3}));
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantOldPlaneHolds), (Numbers{2}));
}

TEST(PatternViewer, GivesUpReadingAPanelThatCannotBeRead) {
  CheckFixture fixture;
  fixture.display.readable = false;
  fixture.StartAndSettle();
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.Recorded(bn::reader::kInstantPlaneUnreadable), (Numbers{0}));
  fixture.Forget();

  // It says so once, and carries on as a viewer.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  fixture.ReadThrough();
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantPlaneUnreadable).empty());
  EXPECT_TRUE(fixture.Recorded(bn::reader::kInstantNewPlaneHolds).empty());
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kReady);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.reads, 0);
}

}  // namespace
