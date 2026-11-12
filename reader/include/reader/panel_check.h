#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/clock.h"
#include "core/display.h"

namespace bn::reader {

// Until the reader has pages to show, this is what the application puts on
// the panel when it starts: the test pattern, four times over, each step by
// a different kind of refresh. It shows that pictures reach the panel
// intact, and the trace of it gives each refresh's duration.
//
//   step 1  full refresh. Both planes written, one after the other.
//   step 2  fast refresh. Only the new-image plane is written first; the
//           old-image plane is brought into line once the refresh is over.
//   step 3  half refresh. The two planes written band by band in turn.
//   step 4  fast refresh again, with the panel switched off afterwards,
//           then the panel is put to sleep.
//
// The second fast refresh is there because the half refresh before it leaves
// the panel's controller believing it is warm, which may shorten what
// follows; the two fast refreshes can be compared.
//
// Nothing here waits: Tick() does at most one piece of work and returns.
class PanelCheck {
 public:
  enum class State : std::uint8_t {
    kNotStarted,
    kRunning,
    kDone,    // All four steps shown and the panel asleep.
    kFailed,  // The panel could not be set up, refused something, or never finished a refresh.
  };

  PanelCheck(core::Display& display, core::Clock& clock) noexcept : display_(display), clock_(clock) {}

  // Sets the panel up and shows step 1.
  void Start() noexcept;

  // Moves on when the panel and the clock allow. Call often.
  void Tick() noexcept;

  State CurrentState() const noexcept { return state_; }
  // The step most recently sent to the panel: 0 before the first.
  std::uint8_t Step() const noexcept { return step_; }

  // How long each step stays up before the next is drawn.
  static constexpr std::uint64_t kPauseUs = 1'500'000;
  // A refresh still running after this long is taken to have failed.
  static constexpr std::uint64_t kGiveUpUs = 20'000'000;

 private:
  // Which planes a step's picture is written to before its refresh.
  enum class Planes : std::uint8_t { kNewOnly, kOldOnly, kNewThenOld, kInterleaved };
  enum class Phase : std::uint8_t { kRefreshing, kPausing };

  bool ShowStep(std::uint8_t step) noexcept;
  // Renders the picture of `step` and writes it to the planes named.
  bool Sweep(std::uint8_t step, Planes planes) noexcept;
  // One pass over the picture, band by band, each band going to the
  // new-image plane, the old-image plane, or one and then the other.
  bool WriteBands(std::uint8_t step, bool toNew, bool toOld) noexcept;
  void Fail() noexcept;

  // One band of the picture: 80 rows of the widest panel supported.
  static constexpr std::size_t kBandBytes = 8000;
  static constexpr std::size_t kBandRows = 80;

  core::Display& display_;
  core::Clock& clock_;
  State state_ = State::kNotStarted;
  Phase phase_ = Phase::kRefreshing;
  std::uint8_t step_ = 0;
  std::uint64_t phaseStartedUs_ = 0;
  std::array<std::uint8_t, kBandBytes> band_{};
};

}  // namespace bn::reader
