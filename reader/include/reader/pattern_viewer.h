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
//
// It is also the first thing to drive the panel's two planes the way
// core/display.h says they have to be driven:
//
//  - Once a picture is showing, the one after it is written to the
//    new-image plane straight away, so that Down has only to start the
//    refresh.
//  - After a fast refresh, with the voltages on, the old-image plane is
//    left alone: the panel has put the picture there itself.
//  - Once the voltages have been switched off, nothing written before is
//    relied on. The old-image plane is written with the picture on the
//    panel, and the picture after it is written again.
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

  // Has both planes read back out of the panel whenever it comes to rest:
  // after a refresh and whatever is written in its wake, and again after
  // the voltages are switched off. What each holds is recorded in the
  // trace and compared with what it should hold (the picture after this
  // one, and this one), with a count kept of the times it did not.
  //
  // This is for finding out, on a device, whether the planes are what this
  // code believes. Reading takes about a second, a band each Tick(). A
  // press during it is acted on at once, and the reading starts again when
  // the panel is next at rest. The voltages stay on for their usual two
  // seconds after a reading, not after the refresh before it. Call before
  // Start().
  void CheckPlanes() noexcept { checkingPlanes_ = true; }

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
  // Works out what each picture looks like, whole and band by band, to
  // recognise them by when the planes are read back.
  void PrepareChecks() noexcept;
  // Asks for the planes to be read back, now that the panel is at rest.
  void BeginCheck() noexcept;
  // Reads one band of one plane back and notes which picture it is; after
  // the last, records what both planes hold.
  void CheckNextBand() noexcept;
  void Fail() noexcept;

  // One band of the picture: 80 rows of the widest panel supported.
  static constexpr std::size_t kBandBytes = 8000;
  static constexpr std::size_t kBandRows = 80;
  // The most bands a panel may have for its planes to be read back.
  static constexpr std::size_t kMaxCheckBands = 8;
  // A blank plane, and the four pictures.
  static constexpr std::size_t kCheckPictures = 5;

  core::Display& display_;
  core::Clock& clock_;
  State state_ = State::kNotStarted;
  Phase phase_ = Phase::kIdle;
  std::uint8_t picture_ = 0;  // On the panel, or on its way there.
  // The picture the new-image plane holds: 0 if that is not known.
  std::uint8_t inNewPlane_ = 0;
  // The old-image plane does not hold the picture on the panel, or is not
  // known to, and is to be written with it.
  bool oldPlaneDue_ = false;
  bool powered_ = false;  // The panel's drive voltages are on.
  std::uint64_t phaseStartedUs_ = 0;
  Request request_;
  std::array<std::uint8_t, kBandBytes> band_{};

  // Reading the planes back.
  bool checkingPlanes_ = false;
  bool checkDue_ = false;
  std::uint8_t checkStep_ = 0;  // Two for each band: the new-image plane, then the old.
  std::size_t checkBands_ = 0;  // 0 until PrepareChecks() has run, or if this panel cannot be checked.
  // What has been read so far of each plane (the new-image plane first):
  // its checksum, and a digit for each band.
  std::array<std::uint32_t, 2> readChecksum_{};
  std::array<std::uint32_t, 2> readBands_{};
  // What to recognise them by: the checksum of each picture (and of a
  // blank plane, first), whole and band by band.
  std::array<std::uint32_t, kCheckPictures> pictureChecksums_{};
  std::array<std::array<std::uint32_t, kMaxCheckBands>, kCheckPictures> bandChecksums_{};
};

}  // namespace bn::reader
