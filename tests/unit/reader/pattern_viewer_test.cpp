#include "reader/pattern_viewer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
std::vector<std::uint8_t> Picture(const FakeDisplay& display, std::uint8_t picture) {
  std::vector<std::uint8_t> bits(display.size.RowBytes() * display.size.height);
  bn::draw::TestPattern(display.size.width, display.size.height, picture).RenderRows(0, bits);
  return bits;
}

struct ViewerFixture {
  ViewerFixture() { display.present = true; }

  // Starts the viewer and lets its first refresh finish.
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

TEST(PatternViewer, StartsWithTheFirstPictureByAFullRefresh) {
  ViewerFixture fixture;
  fixture.viewer.Start();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kReady);
  EXPECT_EQ(fixture.viewer.Picture(), 1);

  const std::span<const FakeDisplay::Call> calls(fixture.display.calls);
  ASSERT_EQ(calls.size(), 14u);
  EXPECT_EQ(calls[0].op, Op::kStart);
  ExpectSweepOfBothPlanes(calls.subspan(1, 12));
  EXPECT_EQ(calls[13].op, Op::kRefresh);
  EXPECT_EQ(calls[13].mode, RefreshMode::kFull);
  EXPECT_EQ(calls[13].after, AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.display.shownAtRefresh[0], Picture(fixture.display, 1));
  EXPECT_EQ(fixture.display.oldAtRefresh[0], Picture(fixture.display, 1));

  // Both planes were written before it, so nothing follows it.
  fixture.FinishBusy();
  EXPECT_EQ(fixture.display.calls.size(), 14u);
}

TEST(PatternViewer, DownShowsTheNextPictureByAFastRefreshThenBringsTheOldPlaneIntoLine) {
  ViewerFixture fixture;
  fixture.StartAndSettle();

  fixture.Press(Button::kDown);
  EXPECT_TRUE(fixture.NewCalls().empty()) << "nothing happens until the next tick";
  fixture.viewer.Tick();
  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 7u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kNew);
  EXPECT_EQ(calls[6].mode, RefreshMode::kFast);
  EXPECT_EQ(calls[6].after, AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  // At the refresh the new plane holds the next picture and the old plane
  // still holds the one on the panel.
  EXPECT_EQ(fixture.display.shownAtRefresh[1], Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.oldAtRefresh[1], Picture(fixture.display, 1));

  // Nothing more while the panel works.
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  fixture.FinishBusy();
  calls = fixture.NewCalls();
  ExpectSweep(calls, DisplayPlane::kOld);
  EXPECT_EQ(fixture.display.oldPlane, Picture(fixture.display, 2));

  // So a second fast refresh compares against the right thing.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.display.oldAtRefresh[2], Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.shownAtRefresh[2], Picture(fixture.display, 3));
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
  EXPECT_EQ(pressAndSettle(Button::kRight), 2);
  EXPECT_EQ(pressAndSettle(Button::kDown), 3);
  EXPECT_EQ(pressAndSettle(Button::kDown), 4);
  EXPECT_EQ(pressAndSettle(Button::kDown), 1);  // Round to the start.
  EXPECT_EQ(pressAndSettle(Button::kUp), 4);    // And back the other way.
  EXPECT_EQ(pressAndSettle(Button::kLeft), 3);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 7u);
  EXPECT_EQ(fixture.display.newPlane, Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.oldPlane, Picture(fixture.display, 3));
}

TEST(PatternViewer, ConfirmAndBackShowTheSamePictureAgainByHalfAndFullRefreshes) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  fixture.FinishBusy();
  fixture.NewCalls();

  fixture.Press(Button::kConfirm);
  fixture.viewer.Tick();
  std::vector<FakeDisplay::Call> calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  ExpectSweepOfBothPlanes(std::span(calls).first(12));
  EXPECT_EQ(calls[12].mode, RefreshMode::kHalf);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  fixture.FinishBusy();
  EXPECT_TRUE(fixture.NewCalls().empty()) << "the old plane is already right";

  fixture.Press(Button::kBack);
  fixture.viewer.Tick();
  calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 13u);
  EXPECT_EQ(calls[12].mode, RefreshMode::kFull);
  EXPECT_EQ(fixture.viewer.Picture(), 2);
  EXPECT_EQ(fixture.display.shownAtRefresh.back(), Picture(fixture.display, 2));
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
  EXPECT_EQ(fixture.display.shownAtRefresh[1], Picture(fixture.display, 3));
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
  EXPECT_EQ(fixture.display.shownAtRefresh.back(), Picture(fixture.display, 2));
  fixture.FinishBusy();
  fixture.NewCalls();

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

  // Once is enough, however long nothing happens.
  fixture.FinishBusy();
  fixture.clock.now += 60'000'000;
  fixture.viewer.Tick();
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  // A press brings it back; the planes were kept, so only the new one is
  // written. Two seconds after that refresh it is switched off again.
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  calls = fixture.NewCalls();
  ASSERT_EQ(calls.size(), 7u);
  EXPECT_EQ(calls[6].mode, RefreshMode::kFast);
  fixture.clock.now += 10'000'000;  // The refresh takes a long time...
  fixture.FinishBusy();
  fixture.NewCalls();
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

TEST(PatternViewer, APressWhileThePanelIsSwitchingOffWaitsForItToFinish) {
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.clock.now += PatternViewer::kPowerOffAfterUs;
  fixture.viewer.Tick();
  ASSERT_EQ(fixture.NewCalls().size(), 1u);
  ASSERT_TRUE(fixture.display.busy);

  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_TRUE(fixture.NewCalls().empty());

  fixture.FinishBusy();
  EXPECT_EQ(fixture.viewer.Picture(), 2);
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
  ViewerFixture fixture;
  fixture.StartAndSettle();
  fixture.display.failWrites = true;
  fixture.Press(Button::kDown);
  fixture.viewer.Tick();
  EXPECT_EQ(fixture.viewer.CurrentState(), PatternViewer::State::kFailed);
  EXPECT_EQ(fixture.viewer.Picture(), 1) << "the next picture never reached the panel";
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 1u);

  ViewerFixture atStart;
  atStart.display.failWrites = true;
  atStart.viewer.Start();
  EXPECT_EQ(atStart.viewer.CurrentState(), PatternViewer::State::kFailed);
  EXPECT_EQ(atStart.display.Count(Op::kRefresh), 0u);
}

TEST(PatternViewer, CoversAPanelOfAnotherShapeInWholeBands) {
  // 792 x 528: rows of 99 bytes, and a last band that is not a full one.
  ViewerFixture fixture;
  fixture.display.size = {.width = 792, .height = 528};
  const std::size_t planeBytes = fixture.display.size.RowBytes() * fixture.display.size.height;
  fixture.display.newPlane.assign(planeBytes, 0xFF);
  fixture.display.oldPlane.assign(planeBytes, 0xFF);
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
  EXPECT_EQ(fixture.display.shownAtRefresh[0], Picture(fixture.display, 1));
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
  (void)trace.tracer.Drain(records);  // The first picture: no button was involved.

  // Pressed at 1.000 s, while... nothing was in the way; acted on at 1.006 s.
  trace.clock.now = 1'000'000;
  viewer.OnInput({.timeUs = trace.clock.now, .button = Button::kDown, .kind = InputEventKind::kPress});
  trace.clock.now = 1'006'000;
  viewer.Tick();

  const std::size_t count = trace.tracer.Drain(records);
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(records[0].id, bn::reader::kSpanPanelSweep);
  EXPECT_EQ(records[0].arg, 2u);
  EXPECT_EQ(records[1].id, bn::reader::kSpanPressToRefresh);
  EXPECT_EQ(records[1].startUs, 1'000'000u);
  EXPECT_EQ(records[1].durationUs, 6'000u);
  EXPECT_EQ(records[1].arg, static_cast<std::uint32_t>(Button::kDown));

  // Of several presses acted on together, the first is the one measured.
  trace.clock.now = 2'000'000;
  viewer.OnInput({.timeUs = 1'500'000, .button = Button::kRight, .kind = InputEventKind::kPress});
  viewer.OnInput({.timeUs = 1'700'000, .button = Button::kConfirm, .kind = InputEventKind::kPress});
  display.busy = false;
  viewer.Tick();
  const std::size_t more = trace.tracer.Drain(records);
  ASSERT_GE(more, 1u);
  EXPECT_EQ(records[more - 1].id, bn::reader::kSpanPressToRefresh);
  EXPECT_EQ(records[more - 1].startUs, 1'500'000u);
  EXPECT_EQ(records[more - 1].arg, static_cast<std::uint32_t>(Button::kRight));
}

}  // namespace
