#include "reader/panel_check.h"

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
using bn::core::DisplayPlane;
using bn::core::RefreshMode;
using bn::reader::PanelCheck;
using bn::test::FakeDisplay;
using Op = bn::test::FakeDisplay::Op;

// The picture of one step, as a whole plane.
std::vector<std::uint8_t> Picture(const FakeDisplay& display, std::uint8_t step) {
  std::vector<std::uint8_t> bits(display.size.RowBytes() * display.size.height);
  bn::draw::TestPattern(display.size.width, display.size.height, step).RenderRows(0, bits);
  return bits;
}

struct CheckFixture {
  CheckFixture() { display.present = true; }

  // Lets the refresh in progress finish and gives the check a turn.
  void FinishRefresh() {
    display.busy = false;
    check.Tick();
  }

  // Lets the pause between steps run out and gives the check a turn.
  void FinishPause() {
    clock.now += PanelCheck::kPauseUs;
    check.Tick();
  }

  // The calls made since `from`, which is then moved to the end.
  std::vector<FakeDisplay::Call> CallsSince(std::size_t& from) {
    std::vector<FakeDisplay::Call> calls(display.calls.begin() + static_cast<std::ptrdiff_t>(from),
                                         display.calls.end());
    from = display.calls.size();
    return calls;
  }

  bn::test::FakeClock clock;
  FakeDisplay display;
  PanelCheck check{display, clock};
};

// Expects `calls` to be one sweep of the whole picture in 80-row bands, to
// `plane`; returns how many calls that was.
void ExpectSweep(std::span<const FakeDisplay::Call> calls, DisplayPlane plane) {
  ASSERT_EQ(calls.size(), 6u);
  for (std::size_t band = 0; band < calls.size(); ++band) {
    EXPECT_EQ(calls[band].op, Op::kWrite) << band;
    EXPECT_EQ(calls[band].plane, plane) << band;
    EXPECT_EQ(calls[band].firstRow, band * 80) << band;
    EXPECT_EQ(calls[band].rowCount, 80u) << band;
  }
}

TEST(PanelCheck, DoesNothingUntilStartedAndFailsIfThePanelCannotBeSetUp) {
  CheckFixture fixture;
  fixture.display.present = false;
  fixture.check.Tick();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kNotStarted);
  EXPECT_TRUE(fixture.display.calls.empty());

  fixture.check.Start();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kFailed);
  EXPECT_EQ(fixture.check.Step(), 0);
  ASSERT_EQ(fixture.display.calls.size(), 1u);
  EXPECT_EQ(fixture.display.calls[0].op, Op::kStart);

  // And it stays that way.
  fixture.clock.now += 60'000'000;
  fixture.check.Tick();
  fixture.check.Start();
  EXPECT_EQ(fixture.display.calls.size(), 1u);
}

TEST(PanelCheck, StepOneWritesBothPlanesInTurnAndRefreshesInFull) {
  CheckFixture fixture;
  fixture.check.Start();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kRunning);
  EXPECT_EQ(fixture.check.Step(), 1);

  const std::span<const FakeDisplay::Call> calls(fixture.display.calls);
  ASSERT_EQ(calls.size(), 14u);
  EXPECT_EQ(calls[0].op, Op::kStart);
  ExpectSweep(calls.subspan(1, 6), DisplayPlane::kNew);
  ExpectSweep(calls.subspan(7, 6), DisplayPlane::kOld);
  EXPECT_EQ(calls[13].op, Op::kRefresh);
  EXPECT_EQ(calls[13].mode, RefreshMode::kFull);
  EXPECT_EQ(calls[13].after, AfterRefresh::kStayPowered);

  ASSERT_EQ(fixture.display.shownAtRefresh.size(), 1u);
  EXPECT_EQ(fixture.display.shownAtRefresh[0], Picture(fixture.display, 1));
  EXPECT_EQ(fixture.display.oldAtRefresh[0], Picture(fixture.display, 1));
}

TEST(PanelCheck, WaitsForTheRefreshAndThenForThePauseBeforeTheNextStep) {
  CheckFixture fixture;
  fixture.check.Start();
  const std::size_t afterStepOne = fixture.display.calls.size();

  // While the panel is busy, time passing changes nothing.
  fixture.clock.now += 5'000'000;
  fixture.check.Tick();
  EXPECT_EQ(fixture.display.calls.size(), afterStepOne);

  // The refresh ends; the pause starts from then, not from when it began.
  fixture.FinishRefresh();
  fixture.clock.now += PanelCheck::kPauseUs - 1;
  fixture.check.Tick();
  EXPECT_EQ(fixture.display.calls.size(), afterStepOne);
  EXPECT_EQ(fixture.check.Step(), 1);

  fixture.clock.now += 1;
  fixture.check.Tick();
  EXPECT_EQ(fixture.check.Step(), 2);
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PanelCheck, RunsAllFourStepsAndPutsThePanelToSleep) {
  CheckFixture fixture;
  std::size_t seen = 0;
  fixture.check.Start();
  fixture.CallsSince(seen);

  // Step 2: only the new-image plane before a fast refresh, so that the
  // old-image plane still holds what is on the panel.
  fixture.FinishRefresh();
  EXPECT_EQ(fixture.display.calls.size(), seen) << "step 1 needs nothing after its refresh";
  fixture.FinishPause();
  std::vector<FakeDisplay::Call> calls = fixture.CallsSince(seen);
  ASSERT_EQ(calls.size(), 7u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kNew);
  EXPECT_EQ(calls[6].mode, RefreshMode::kFast);
  EXPECT_EQ(calls[6].after, AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.display.shownAtRefresh[1], Picture(fixture.display, 2));
  EXPECT_EQ(fixture.display.oldAtRefresh[1], Picture(fixture.display, 1));

  // Once it is over, the old-image plane is brought into line.
  fixture.FinishRefresh();
  calls = fixture.CallsSince(seen);
  ExpectSweep(calls, DisplayPlane::kOld);
  EXPECT_EQ(fixture.display.oldPlane, Picture(fixture.display, 2));

  // Step 3: both planes band by band in turn, then a half refresh.
  fixture.FinishPause();
  calls = fixture.CallsSince(seen);
  ASSERT_EQ(calls.size(), 13u);
  for (std::size_t i = 0; i < 12; ++i) {
    EXPECT_EQ(calls[i].op, Op::kWrite) << i;
    EXPECT_EQ(calls[i].plane, i % 2 == 0 ? DisplayPlane::kNew : DisplayPlane::kOld) << i;
    EXPECT_EQ(calls[i].firstRow, (i / 2) * 80) << i;
    EXPECT_EQ(calls[i].rowCount, 80u) << i;
  }
  EXPECT_EQ(calls[12].mode, RefreshMode::kHalf);
  EXPECT_EQ(calls[12].after, AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.display.shownAtRefresh[2], Picture(fixture.display, 3));
  EXPECT_EQ(fixture.display.oldAtRefresh[2], Picture(fixture.display, 3));

  // Step 4: a fast refresh again, switching the panel off afterwards.
  fixture.FinishRefresh();
  EXPECT_EQ(fixture.display.calls.size(), seen);
  fixture.FinishPause();
  calls = fixture.CallsSince(seen);
  ASSERT_EQ(calls.size(), 7u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kNew);
  EXPECT_EQ(calls[6].mode, RefreshMode::kFast);
  EXPECT_EQ(calls[6].after, AfterRefresh::kPowerOff);
  EXPECT_EQ(fixture.display.oldAtRefresh[3], Picture(fixture.display, 3));
  EXPECT_EQ(fixture.check.Step(), 4);
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kRunning);

  // Then the old-image plane, and sleep.
  fixture.FinishRefresh();
  calls = fixture.CallsSince(seen);
  ASSERT_EQ(calls.size(), 7u);
  ExpectSweep(std::span(calls).first(6), DisplayPlane::kOld);
  EXPECT_EQ(calls[6].op, Op::kSleep);
  EXPECT_EQ(fixture.display.oldPlane, Picture(fixture.display, 4));
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kDone);

  // Nothing more happens.
  fixture.clock.now += 60'000'000;
  fixture.check.Tick();
  fixture.check.Tick();
  EXPECT_EQ(fixture.display.calls.size(), seen);
  EXPECT_EQ(fixture.display.refused, 0);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 4u);
  EXPECT_EQ(fixture.display.Count(Op::kStart), 1u);
}

TEST(PanelCheck, GivesUpOnARefreshThatNeverFinishes) {
  CheckFixture fixture;
  fixture.check.Start();
  fixture.clock.now += PanelCheck::kGiveUpUs;
  fixture.check.Tick();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kRunning);

  fixture.clock.now += 1;
  fixture.check.Tick();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kFailed);

  // Even if the panel comes back, the check is over.
  const std::size_t calls = fixture.display.calls.size();
  fixture.FinishRefresh();
  fixture.FinishPause();
  EXPECT_EQ(fixture.display.calls.size(), calls);
}

TEST(PanelCheck, FailsIfThePanelRefusesAWrite) {
  CheckFixture fixture;
  fixture.check.Start();
  fixture.FinishRefresh();
  fixture.display.failWrites = true;
  fixture.FinishPause();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kFailed);
  EXPECT_EQ(fixture.check.Step(), 1) << "step 2 never reached the panel";
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 1u);
}

TEST(PanelCheck, FailsAtTheStartIfTheFirstPictureCannotBeWritten) {
  CheckFixture fixture;
  fixture.display.failWrites = true;
  fixture.check.Start();
  EXPECT_EQ(fixture.check.CurrentState(), PanelCheck::State::kFailed);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 0u);
}

TEST(PanelCheck, CoversAPanelOfAnotherShapeInWholeBands) {
  // 792 x 528: rows of 99 bytes, and a last band that is not a full one.
  CheckFixture fixture;
  fixture.display.size = {.width = 792, .height = 528};
  const std::size_t planeBytes = fixture.display.size.RowBytes() * fixture.display.size.height;
  fixture.display.newPlane.assign(planeBytes, 0xFF);
  fixture.display.oldPlane.assign(planeBytes, 0xFF);
  fixture.check.Start();

  std::size_t rowsToNew = 0;
  std::size_t nextRow = 0;
  for (const FakeDisplay::Call& call : fixture.display.calls) {
    if (call.op == Op::kWrite && call.plane == DisplayPlane::kNew) {
      EXPECT_EQ(call.firstRow, nextRow);
      EXPECT_LE(call.rowCount, 80u);
      nextRow += call.rowCount;
      rowsToNew += call.rowCount;
    }
  }
  EXPECT_EQ(rowsToNew, 528u);
  EXPECT_EQ(fixture.display.shownAtRefresh[0], Picture(fixture.display, 1));
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(PanelCheck, TracesEverySweepOfThePicture) {
  bn::test::TraceFixture<64> trace;
  FakeDisplay display;
  display.present = true;
  PanelCheck check(display, trace.clock);
  check.Start();
  for (int step = 2; step <= 4; ++step) {
    display.busy = false;
    check.Tick();
    trace.clock.now += PanelCheck::kPauseUs;
    check.Tick();
  }
  display.busy = false;
  check.Tick();
  ASSERT_EQ(check.CurrentState(), PanelCheck::State::kDone);

  // One sweep before each refresh, and one after each fast refresh.
  std::array<bn::core::TraceRecord, 64> records{};
  const std::size_t count = trace.tracer.Drain(records);
  std::vector<std::uint32_t> steps;
  for (std::size_t i = 0; i < count; ++i) {
    if (records[i].id == bn::reader::kSpanPanelSweep) {
      steps.push_back(records[i].arg);
    }
  }
  EXPECT_EQ(steps, (std::vector<std::uint32_t>{1, 2, 2, 3, 4, 4}));
}

}  // namespace
