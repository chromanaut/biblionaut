#include "reader/panel_check.h"

#include <algorithm>

#include "core/metrics.h"
#include "core/trace.h"
#include "draw/test_pattern.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

// 1 once the panel has been set up, 0 if that or anything after it failed.
core::Gauge panelOk{"panel_ok"};
// The step of the check now on the panel.
core::Gauge panelStep{"panel_step"};

struct StepPlan {
  core::RefreshMode mode;
  core::AfterRefresh after;
  bool interleaved;     // Write both planes band by band in turn.
  bool oldPlaneBefore;  // Write the old-image plane before the refresh (after the new one).
  bool oldPlaneAfter;   // Write the old-image plane once the refresh is over.
};

// What each step does; see the description in the header.
constexpr std::array<StepPlan, draw::TestPattern::kStepCount> kSteps = {{
    {core::RefreshMode::kFull, core::AfterRefresh::kStayPowered, false, true, false},
    {core::RefreshMode::kFast, core::AfterRefresh::kStayPowered, false, false, true},
    {core::RefreshMode::kHalf, core::AfterRefresh::kStayPowered, true, false, false},
    {core::RefreshMode::kFast, core::AfterRefresh::kPowerOff, false, false, true},
}};

}  // namespace

void PanelCheck::Start() noexcept {
  if (state_ != State::kNotStarted) {
    return;
  }
  if (!display_.Start()) {
    Fail();
    return;
  }
  state_ = State::kRunning;
  panelOk.Set(1);
  if (!ShowStep(1)) {
    Fail();
  }
}

void PanelCheck::Tick() noexcept {
  if (state_ != State::kRunning) {
    return;
  }
  const std::uint64_t now = clock_.NowUs();

  if (phase_ == Phase::kRefreshing) {
    if (display_.Busy()) {
      if (now - phaseStartedUs_ > kGiveUpUs) {
        Fail();
      }
      return;
    }
    // The panel now shows this step. A fast refresh needs the old-image
    // plane to say so before the next one can be trusted.
    if (kSteps[step_ - 1U].oldPlaneAfter && !Sweep(step_, Planes::kOldOnly)) {
      Fail();
      return;
    }
    if (step_ == draw::TestPattern::kStepCount) {
      if (display_.Sleep()) {
        state_ = State::kDone;
      } else {
        Fail();
      }
      return;
    }
    phase_ = Phase::kPausing;
    phaseStartedUs_ = now;
    return;
  }

  if (now - phaseStartedUs_ >= kPauseUs && !ShowStep(static_cast<std::uint8_t>(step_ + 1))) {
    Fail();
  }
}

bool PanelCheck::ShowStep(std::uint8_t step) noexcept {
  const StepPlan& plan = kSteps[step - 1U];
  Planes planes = Planes::kNewOnly;
  if (plan.interleaved) {
    planes = Planes::kInterleaved;
  } else if (plan.oldPlaneBefore) {
    planes = Planes::kNewThenOld;
  }
  if (!Sweep(step, planes) || !display_.StartRefresh(plan.mode, plan.after)) {
    return false;
  }
  step_ = step;
  panelStep.Set(step);
  phase_ = Phase::kRefreshing;
  phaseStartedUs_ = clock_.NowUs();
  return true;
}

bool PanelCheck::Sweep(std::uint8_t step, Planes planes) noexcept {
  BN_TRACE_TOP_SCOPE_ARG(kSpanPanelSweep, step);
  switch (planes) {
    case Planes::kNewOnly:
      return WriteBands(step, true, false);
    case Planes::kOldOnly:
      return WriteBands(step, false, true);
    case Planes::kNewThenOld:
      return WriteBands(step, true, false) && WriteBands(step, false, true);
    case Planes::kInterleaved:
      return WriteBands(step, true, true);
  }
  return false;
}

bool PanelCheck::WriteBands(std::uint8_t step, bool toNew, bool toOld) noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t rowBytes = size.RowBytes();
  if (rowBytes == 0 || rowBytes > kBandBytes) {
    return false;
  }
  const draw::TestPattern pattern(size.width, size.height, step);
  const std::size_t bandRows = std::min(kBandRows, kBandBytes / rowBytes);
  for (std::size_t row = 0; row < size.height; row += bandRows) {
    // Each band is rendered once, then written wherever it is wanted.
    const std::size_t rows = std::min<std::size_t>(bandRows, size.height - row);
    const std::span<std::uint8_t> band = std::span(band_).first(rows * rowBytes);
    const auto firstRow = static_cast<std::uint16_t>(row);
    pattern.RenderRows(firstRow, band);
    if (toNew && !display_.WriteRows(core::DisplayPlane::kNew, firstRow, band)) {
      return false;
    }
    if (toOld && !display_.WriteRows(core::DisplayPlane::kOld, firstRow, band)) {
      return false;
    }
  }
  return true;
}

void PanelCheck::Fail() noexcept {
  state_ = State::kFailed;
  panelOk.Set(0);
}

}  // namespace bn::reader
