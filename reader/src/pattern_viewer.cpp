#include "reader/pattern_viewer.h"

#include <algorithm>
#include <utility>

#include "core/metrics.h"
#include "core/trace.h"
#include "draw/test_pattern.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

// 1 once the panel has been set up, 0 if that or anything after it failed.
core::Gauge panelOk{"panel_ok"};
// The picture of the test pattern now on the panel.
core::Gauge panelPicture{"panel_picture"};
core::Counter buttonPresses{"button_presses"};

constexpr int kPictureCount = draw::TestPattern::kStepCount;

}  // namespace

void PatternViewer::Start() noexcept {
  if (state_ != State::kNotStarted) {
    return;
  }
  if (!display_.Start()) {
    Fail();
    return;
  }
  state_ = State::kReady;
  panelOk.Set(1);
  // The same as Back on picture 1: both planes, then a full refresh.
  Act(Request{.waiting = true, .redraw = true, .redrawMode = core::RefreshMode::kFull});
}

void PatternViewer::OnInput(const core::InputEvent& event) noexcept {
  if (event.kind != core::InputEventKind::kPress) {
    return;
  }
  buttonPresses.Add();
  if (state_ != State::kReady) {
    return;
  }
  switch (event.button) {
    case core::Button::kDown:
    case core::Button::kRight:
      ++request_.steps;
      break;
    case core::Button::kUp:
    case core::Button::kLeft:
      --request_.steps;
      break;
    case core::Button::kConfirm:
      // A full refresh already asked for does everything a half one would.
      if (!request_.redraw) {
        request_.redrawMode = core::RefreshMode::kHalf;
      }
      request_.redraw = true;
      break;
    case core::Button::kBack:
      request_.redraw = true;
      request_.redrawMode = core::RefreshMode::kFull;
      break;
    case core::Button::kPower:
      return;
  }
  if (!request_.waiting) {
    request_.waiting = true;
    request_.pressedAtUs = event.timeUs;
    request_.button = event.button;
  }
}

void PatternViewer::Tick() noexcept {
  if (state_ != State::kReady) {
    return;
  }
  const std::uint64_t now = clock_.NowUs();

  if (phase_ != Phase::kIdle) {
    if (display_.Busy()) {
      if (now - phaseStartedUs_ > kGiveUpUs) {
        Fail();
      }
      return;
    }
    // After a fast refresh the old-image plane still holds the picture
    // before: bring it into line, or the next fast refresh would compare
    // against the wrong thing.
    if (phase_ == Phase::kRefreshing && oldPlaneBehind_) {
      if (!WriteBands(picture_, false, true)) {
        Fail();
        return;
      }
      oldPlaneBehind_ = false;
    }
    phase_ = Phase::kIdle;
    phaseStartedUs_ = now;
  }

  if (request_.waiting) {
    Act(std::exchange(request_, Request{}));
  } else if (powered_ && now - phaseStartedUs_ >= kPowerOffAfterUs) {
    if (!display_.PowerOff()) {
      Fail();
      return;
    }
    powered_ = false;
    phase_ = Phase::kPoweringOff;
    phaseStartedUs_ = now;
  }
}

void PatternViewer::Act(const Request& request) noexcept {
  // Pictures run 1 to kPictureCount and wrap round both ways.
  const int current = picture_ == 0 ? 0 : picture_ - 1;
  const int wrapped = (((current + request.steps) % kPictureCount) + kPictureCount) % kPictureCount;
  const auto target = static_cast<std::uint8_t>(wrapped + 1);
  if (!request.redraw && target == picture_) {
    return;  // Forward and back again: nothing to show.
  }

  bool started = false;
  if (request.redraw) {
    // A full or half refresh ignores the old-image plane, so both can be
    // written together and the old one is right for whatever comes next.
    started =
        WriteBands(target, true, true) && display_.StartRefresh(request.redrawMode, core::AfterRefresh::kStayPowered);
    oldPlaneBehind_ = false;
  } else {
    // A fast refresh compares the planes: the old one must stay as it is
    // until the refresh is over.
    started = WriteBands(target, true, false) &&
              display_.StartRefresh(core::RefreshMode::kFast, core::AfterRefresh::kStayPowered);
    oldPlaneBehind_ = true;
  }
  if (!started) {
    Fail();
    return;
  }
  if (request.pressedAtUs != 0) {
    // From the button going down to the refresh being under way. Trace
    // times are the low half of the clock the press was timed on.
    BN_TRACE_TOP_SPAN_SINCE(kSpanPressToRefresh, static_cast<std::uint32_t>(request.pressedAtUs),
                            static_cast<std::uint32_t>(request.button));
  }
  picture_ = target;
  panelPicture.Set(target);
  powered_ = true;
  phase_ = Phase::kRefreshing;
  phaseStartedUs_ = clock_.NowUs();
}

bool PatternViewer::WriteBands(std::uint8_t picture, bool toNew, bool toOld) noexcept {
  BN_TRACE_TOP_SCOPE_ARG(kSpanPanelSweep, picture);
  const core::DisplaySize size = display_.Size();
  const std::size_t rowBytes = size.RowBytes();
  if (rowBytes == 0 || rowBytes > kBandBytes) {
    return false;
  }
  const draw::TestPattern pattern(size.width, size.height, picture);
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

void PatternViewer::Fail() noexcept {
  state_ = State::kFailed;
  panelOk.Set(0);
}

}  // namespace bn::reader
