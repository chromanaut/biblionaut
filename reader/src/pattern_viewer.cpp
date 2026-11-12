#include "reader/pattern_viewer.h"

#include <algorithm>
#include <utility>

#include "core/checksum.h"
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
// Times both planes were read back, and how many of those found a plane
// holding something other than it should.
core::Counter planeChecks{"plane_checks"};
core::Counter planesUnexpected{"planes_unexpected"};

constexpr int kPictureCount = draw::TestPattern::kStepCount;

// The picture after `picture`, going round from the last to the first.
std::uint8_t NextPicture(std::uint8_t picture) noexcept {
  return static_cast<std::uint8_t>((picture % kPictureCount) + 1);
}

// What a plane read back is recorded as, when it is not one of the
// pictures.
constexpr std::uint32_t kHoldsBlank = 8;
constexpr std::uint32_t kHoldsSomethingElse = 9;

// How a picture is cut into bands for a panel: `count` is 0 if it cannot be.
struct BandLayout {
  std::size_t rowBytes = 0;
  std::size_t rows = 0;  // In every band but, perhaps, the last.
  std::size_t count = 0;
};

BandLayout LayoutFor(core::DisplaySize size, std::size_t bandBytes, std::size_t bandRows) noexcept {
  const std::size_t rowBytes = size.RowBytes();
  if (rowBytes == 0 || rowBytes > bandBytes) {
    return {};
  }
  const std::size_t rows = std::min(bandRows, bandBytes / rowBytes);
  return {.rowBytes = rowBytes, .rows = rows, .count = (size.height + rows - 1) / rows};
}

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
  if (checkingPlanes_ && state_ == State::kReady) {
    PrepareChecks();  // While the panel is busy with that.
  }
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
      return;  // Not this viewer's to act on.
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
    if (phase_ == Phase::kPoweringOff) {
      // The planes are back in the places they had before the voltages
      // came on, which may not be where they were a moment ago. Nothing
      // written to either since then is relied on.
      oldPlaneDue_ = true;
      inNewPlane_ = 0;
    }
    phase_ = Phase::kIdle;
    phaseStartedUs_ = now;
    BeginCheck();
  }

  // The next fast refresh compares against the old-image plane: it has to
  // hold what the panel shows.
  if (oldPlaneDue_) {
    if (!WriteBands(picture_, false, true)) {
      Fail();
      return;
    }
    oldPlaneDue_ = false;
    if (!request_.waiting) {
      return;  // One sweep a turn, unless a press is waiting on this one.
    }
  }

  if (request_.waiting) {
    Act(std::exchange(request_, Request{}));
    return;
  }

  if (inNewPlane_ != NextPicture(picture_)) {
    // Nothing is waiting: get the picture Down would ask for ready.
    if (!WriteBands(NextPicture(picture_), true, false)) {
      Fail();
      return;
    }
    inNewPlane_ = NextPicture(picture_);
    return;
  }

  if (checkDue_) {
    CheckNextBand();
    return;
  }

  if (powered_ && now - phaseStartedUs_ >= kPowerOffAfterUs) {
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
  std::uint8_t newPlaneAfter = 0;
  if (request.redraw) {
    // A full or half refresh ignores the old-image plane and leaves both
    // planes where they are, so both can be written together and the old
    // one is right for whatever comes next.
    started =
        WriteBands(target, true, true) && display_.StartRefresh(request.redrawMode, core::AfterRefresh::kStayPowered);
    newPlaneAfter = target;
  } else {
    // A fast refresh compares the planes, and Tick() has seen to it that
    // the old one holds what the panel shows. The new one may already hold
    // the picture wanted.
    started = (inNewPlane_ == target || WriteBands(target, true, false)) &&
              display_.StartRefresh(core::RefreshMode::kFast, core::AfterRefresh::kStayPowered);
    // Afterwards the planes have changed places: the picture shown is in
    // the old-image plane without anything being written, and the
    // new-image plane holds the picture shown before.
    newPlaneAfter = picture_;
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
  inNewPlane_ = newPlaneAfter;
  oldPlaneDue_ = false;
  checkDue_ = false;  // The planes are about to change: they are read once the panel is at rest again.
  panelPicture.Set(target);
  powered_ = true;
  phase_ = Phase::kRefreshing;
  phaseStartedUs_ = clock_.NowUs();
}

bool PatternViewer::WriteBands(std::uint8_t picture, bool toNew, bool toOld) noexcept {
  BN_TRACE_TOP_SCOPE_ARG(kSpanPanelSweep, picture);
  const core::DisplaySize size = display_.Size();
  const BandLayout layout = LayoutFor(size, kBandBytes, kBandRows);
  if (layout.count == 0) {
    return false;
  }
  const draw::TestPattern pattern(size.width, size.height, picture);
  for (std::size_t row = 0; row < size.height; row += layout.rows) {
    // Each band is rendered once, then written wherever it is wanted.
    const std::size_t rows = std::min<std::size_t>(layout.rows, size.height - row);
    const std::span<std::uint8_t> band = std::span(band_).first(rows * layout.rowBytes);
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

void PatternViewer::PrepareChecks() noexcept {
  const core::DisplaySize size = display_.Size();
  const BandLayout layout = LayoutFor(size, kBandBytes, kBandRows);
  if (layout.count == 0 || layout.count > kMaxCheckBands) {
    return;
  }
  // Entry 0 is a blank plane, entries 1 to 4 the pictures.
  for (std::size_t picture = 0; picture < kCheckPictures; ++picture) {
    std::uint32_t whole = 0;
    for (std::size_t index = 0; index < layout.count; ++index) {
      const std::size_t row = index * layout.rows;
      const std::size_t rows = std::min<std::size_t>(layout.rows, size.height - row);
      const std::span<std::uint8_t> band = std::span(band_).first(rows * layout.rowBytes);
      if (picture == 0) {
        std::fill(band.begin(), band.end(), std::uint8_t{0xFF});
      } else {
        draw::TestPattern(size.width, size.height, static_cast<std::uint8_t>(picture))
            .RenderRows(static_cast<std::uint16_t>(row), band);
      }
      bandChecksums_[picture][index] = core::Crc32(band);
      whole = core::Crc32(band, whole);
    }
    pictureChecksums_[picture] = whole;
  }
  checkBands_ = layout.count;
}

void PatternViewer::BeginCheck() noexcept {
  checkDue_ = checkingPlanes_ && checkBands_ > 0;
  checkStep_ = 0;
  readChecksum_ = {};
  readBands_ = {};
}

void PatternViewer::CheckNextBand() noexcept {
  const core::DisplaySize size = display_.Size();
  const BandLayout layout = LayoutFor(size, kBandBytes, kBandRows);
  const std::size_t index = checkStep_ / 2U;
  const std::size_t plane = checkStep_ % 2U;  // The new-image plane, then the old.
  const std::size_t row = index * layout.rows;
  const std::size_t rows = std::min<std::size_t>(layout.rows, size.height - row);
  const std::span<std::uint8_t> band = std::span(band_).first(rows * layout.rowBytes);
  if (!display_.ReadRows(plane == 0 ? core::DisplayPlane::kNew : core::DisplayPlane::kOld,
                         static_cast<std::uint16_t>(row), band)) {
    // This panel cannot be read, or would not be: stop asking.
    BN_TRACE_INSTANT(kInstantPlaneUnreadable, static_cast<std::uint32_t>(plane));
    checkingPlanes_ = false;
    checkDue_ = false;
    return;
  }

  // Which of the things it might be this is: the first of them is the
  // blank plane.
  const auto recognise = [](std::span<const std::uint32_t> known, std::uint32_t checksum) noexcept {
    const auto found = std::find(known.begin(), known.end(), checksum);
    if (found == known.end()) {
      return kHoldsSomethingElse;
    }
    return found == known.begin() ? kHoldsBlank : static_cast<std::uint32_t>(found - known.begin());
  };
  std::array<std::uint32_t, kCheckPictures> bandOfEach{};
  for (std::size_t picture = 0; picture < kCheckPictures; ++picture) {
    bandOfEach[picture] = bandChecksums_[picture][index];
  }
  readBands_[plane] = (readBands_[plane] * 10) + recognise(bandOfEach, core::Crc32(band));
  readChecksum_[plane] = core::Crc32(band, readChecksum_[plane]);

  ++checkStep_;
  if (checkStep_ < checkBands_ * 2) {
    return;
  }
  const std::uint32_t newHolds = recognise(pictureChecksums_, readChecksum_[0]);
  const std::uint32_t oldHolds = recognise(pictureChecksums_, readChecksum_[1]);
  BN_TRACE_INSTANT(kInstantNewPlaneHolds, newHolds);
  if (newHolds == kHoldsSomethingElse) {
    BN_TRACE_INSTANT(kInstantNewPlaneBands, readBands_[0]);
  }
  BN_TRACE_INSTANT(kInstantOldPlaneHolds, oldHolds);
  if (oldHolds == kHoldsSomethingElse) {
    BN_TRACE_INSTANT(kInstantOldPlaneBands, readBands_[1]);
  }
  // By now Tick() has written whatever it meant to: the old-image plane
  // should hold the picture on the panel, and the new one what this code
  // last knew it to hold.
  planeChecks.Add();
  if (newHolds != inNewPlane_ || oldHolds != picture_) {
    planesUnexpected.Add();
  }
  checkDue_ = false;
  // The voltages stay on for their usual time from here, not from when
  // the reading began: whoever is watching for these lines before pressing
  // again still has that long to do it with the voltages on.
  phaseStartedUs_ = clock_.NowUs();
}

void PatternViewer::Fail() noexcept {
  state_ = State::kFailed;
  panelOk.Set(0);
}

}  // namespace bn::reader
