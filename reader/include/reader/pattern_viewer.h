#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/clock.h"
#include "core/display.h"
#include "core/input.h"

namespace bn::reader {

// Until the reader has pages to show, this is what the application does with
// the panel and the buttons: it shows the test pattern, and the buttons step
// through the pattern's four pictures the way they will turn pages.
//
//   at start         picture 1, by a full refresh
//   Down or Right    the next picture, by a fast refresh
//   Up or Left       the previous picture, by a fast refresh
//   Confirm          the same picture again, by a half refresh
//   Back             the same picture again, by a full refresh
//
// It is the smallest thing that goes all the way from a button to the panel,
// so the trace of it is the first measure of how long that takes: each press
// that leads to a refresh is recorded from the moment the button went down
// to the moment the refresh was started, and the panel's own time follows.
//
// A press made while the panel is busy is remembered and acted on when the
// panel is free; several of them are added up and acted on together. Two
// seconds after the last refresh the panel's drive voltages are switched
// off. Nothing here waits: Tick() does at most one piece of work and
// returns.
class PatternViewer {
 public:
  enum class State : std::uint8_t {
    kNotStarted,
    kReady,
    kFailed,  // The panel could not be set up, refused something, or never finished what it was doing.
  };

  PatternViewer(core::Display& display, core::Clock& clock) noexcept : display_(display), clock_(clock) {}

  // Sets the panel up and shows the first picture.
  void Start() noexcept;

  // Takes a button event. Only presses matter.
  void OnInput(const core::InputEvent& event) noexcept;

  // Moves on when the panel and the clock allow. Call often.
  void Tick() noexcept;

  State CurrentState() const noexcept { return state_; }
  // The picture most recently sent to the panel, 1 to 4: 0 before the first.
  std::uint8_t Picture() const noexcept { return picture_; }

  // How long after a refresh the panel's drive voltages are switched off.
  static constexpr std::uint64_t kPowerOffAfterUs = 2'000'000;
  // A panel still busy after this long is taken to have failed.
  static constexpr std::uint64_t kGiveUpUs = 20'000'000;

 private:
  enum class Phase : std::uint8_t { kIdle, kRefreshing, kPoweringOff };

  // What the buttons have asked for that has not been done yet.
  struct Request {
    bool waiting = false;
    int steps = 0;        // Pictures forward, less pictures back.
    bool redraw = false;  // Show the picture again by `redrawMode`, whether or not it changed.
    core::RefreshMode redrawMode = core::RefreshMode::kHalf;
    std::uint64_t pressedAtUs = 0;              // When the first of the presses was made,
    core::Button button = core::Button::kBack;  // and which button it was.
  };

  void Act(const Request& request) noexcept;
  // Renders `picture` band by band, each band going to the new-image plane,
  // the old-image plane, or one and then the other.
  bool WriteBands(std::uint8_t picture, bool toNew, bool toOld) noexcept;
  void Fail() noexcept;

  // One band of the picture: 80 rows of the widest panel supported.
  static constexpr std::size_t kBandBytes = 8000;
  static constexpr std::size_t kBandRows = 80;

  core::Display& display_;
  core::Clock& clock_;
  State state_ = State::kNotStarted;
  Phase phase_ = Phase::kIdle;
  std::uint8_t picture_ = 0;
  bool oldPlaneBehind_ = false;  // The old-image plane does not hold the picture being refreshed to.
  bool powered_ = false;         // The panel's drive voltages are on.
  std::uint64_t phaseStartedUs_ = 0;
  Request request_;
  std::array<std::uint8_t, kBandBytes> band_{};
};

}  // namespace bn::reader
