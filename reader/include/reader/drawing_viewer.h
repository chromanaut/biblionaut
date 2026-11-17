#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/clock.h"
#include "core/display.h"
#include "core/input.h"
#include "core/storage.h"
#include "core/trace_stream.h"
#include "draw/dither.h"
#include "draw/fill.h"
#include "draw/svg_render.h"
#include "reader/screen.h"

namespace bn::reader {

// What the application shows in place of the test pattern when it is given
// a folder of drawings (App::ViewDrawings()): the SVG files in it, one at a
// time, drawn as the firmware draws a drawing in a book.
//
//   at start         the first, by a full refresh
//   Down or Right    the next, by a half refresh
//   Up or Left       the one before, by a half refresh
//   Confirm, Back    the same one again, by a half or a full refresh
//
// The files are those in the folder whose names end in `.svg`, in the
// order of their names; going past the last comes round to the first.
//
// A drawing is fitted to the page the way the device is held to be read
// (core::Display::ReadingTurn()), as large as fits and in the middle of
// it, and turned into the panel's rows. It is drawn a band of kBandRows
// panel rows at a time, the file being read from its start for each, and
// each band is made the panel's levels (draw::Ditherer) and written to
// both planes. So a half or full refresh, which ignores the old-image
// plane, leaves both right. The panel is driven black and white, so it has
// two levels; a panel driven in grays would have more.
//
// Nothing here waits. Tick() draws for about kSliceUs and returns, so the
// buttons and the rest of the application go on around it, a drawing of
// megabytes taking many turns of the loop. A press while one drawing is
// being drawn begins the one it asks for. The panel's drive voltages are
// switched off as each refresh ends: with both planes written each time,
// nothing is gained by keeping them on. For each drawing, a line of log
// text says what it was, what it took, and what the renderer left out.
//
// The memory for the bands, for the renderer and for what a drawing
// defines is taken from the heap at Start(): about 77 KB for the X4's
// panel, most of it the filler's cells and the definitions.
class DrawingViewer final : public Screen {
 public:
  // Where on storage the drawings are looked for, by the firmware and the
  // desktop that ask for this viewer.
  static constexpr std::string_view kFolder = "/drawings";

  enum class State : std::uint8_t {
    kNotStarted,
    kReady,
    kFailed,  // The panel could not be set up, refused something, or never finished; or no memory.
    kAsleep,  // Asked to sleep, and the panel is asleep (Sleep()).
  };

  // Rows of the panel drawn at a time.
  static constexpr std::size_t kBandRows = 16;
  // About how long one Tick() draws for.
  static constexpr std::uint64_t kSliceUs = 20'000;
  // Bytes of a file read at a time, and kept for what a drawing defines.
  static constexpr std::size_t kReadBytes = 1024;
  static constexpr std::size_t kDefinitionBytes = 24576;
  // The most drawings in the folder that are shown, and the longest name.
  static constexpr std::size_t kMostDrawings = 256;
  static constexpr std::size_t kLongestName = 255;
  // As the pattern viewer: a panel still busy after this long is taken to
  // have failed.
  static constexpr std::uint64_t kGiveUpUs = 20'000'000;

  // `folder` is a path on storage, which has to be started before Start().
  DrawingViewer(core::Display& display, core::Clock& clock, core::Storage& storage, core::TraceStream& stream,
                std::string_view folder);
  ~DrawingViewer() override;
  DrawingViewer(const DrawingViewer&) = delete;
  DrawingViewer& operator=(const DrawingViewer&) = delete;

  // Sets the panel up, finds the drawings, and begins the first. With no
  // drawings to show, the panel is left white, and a line says why.
  void Start() noexcept override;
  // Takes a button event. Only presses matter.
  void OnInput(const core::InputEvent& event) noexcept override;
  // Draws, or moves on, when the panel and the clock allow. Call often.
  void Tick() noexcept override;
  // Puts the panel to sleep, as PatternViewer::Sleep() does: the drawing
  // being drawn is given up.
  void Sleep() noexcept override;
  bool Resting() const noexcept override { return state_ == State::kAsleep || state_ == State::kFailed; }

  State CurrentState() const noexcept { return state_; }
  // The drawings found, and which is on the panel or on its way: 0 for the
  // first. The number of drawings fully shown so far.
  std::size_t Drawings() const noexcept { return names_.size(); }
  std::size_t Current() const noexcept { return current_; }
  std::size_t Shown() const noexcept { return shown_; }

 private:
  enum class Phase : std::uint8_t {
    kIdle,        // A drawing is on the panel, or there is none.
    kSizing,      // Reading the file's first tag, for its size.
    kDrawing,     // Drawing a band.
    kWriting,     // A band drawn, to be written once the panel is free.
    kRefreshing,  // The panel is showing what was drawn.
  };

  // Begins drawing `index`, from reading its size.
  void Begin(std::size_t index) noexcept;
  // Gives the renderer the file for up to the time left: true when the
  // file is over, or reading it failed.
  bool Feed(std::uint64_t until) noexcept;
  // Starts a pass over the file: for its size if `filler` is null.
  bool StartPass(draw::Filler* filler) noexcept;
  // Fits the drawing to the page and puts it into the panel's rows.
  void Place() noexcept;
  // A band drawn: made the panel's levels and written to both planes.
  bool WriteBand() noexcept;
  void Say() noexcept;
  void Fail() noexcept;
  void CloseFile() noexcept;

  core::Display& display_;
  core::Clock& clock_;
  core::Storage& storage_;
  core::TraceStream& stream_;
  std::string folder_;
  std::vector<std::string> names_;

  State state_ = State::kNotStarted;
  Phase phase_ = Phase::kIdle;
  std::size_t current_ = 0;
  std::size_t shown_ = 0;
  std::size_t wanted_ = 0;     // What the buttons ask for: drawn next, if it is not `current_`.
  bool redrawWanted_ = false;  // The same again,
  core::RefreshMode redrawMode_ = core::RefreshMode::kHalf;  // by this.
  bool firstRefresh_ = true;
  bool sleepAsked_ = false;
  std::uint64_t phaseStartedUs_ = 0;
  std::uint64_t drawingStartedUs_ = 0;

  // The file being read.
  core::FileHandle file_ = core::kNoFile;
  std::uint32_t fileSize_ = 0;
  std::uint32_t offset_ = 0;  // Read up to here,
  std::size_t chunkAt_ = 0;   // and taken up to here of what was read.
  std::size_t chunkSize_ = 0;
  bool fileFailed_ = false;

  // Where the drawing goes: its size, fitted to the page, and the band of
  // the panel being drawn.
  draw::Fixed drawWidth_ = 0;
  draw::Fixed drawHeight_ = 0;
  std::size_t bandTop_ = 0;
  std::size_t levels_ = 2;

  // The memory taken at Start().
  std::unique_ptr<draw::SvgRenderer> renderer_;
  std::unique_ptr<std::uint8_t[]> gray_;
  std::unique_ptr<std::int16_t[]> cells_;
  std::unique_ptr<std::int16_t[]> errors_;
  std::unique_ptr<std::uint8_t[]> bits_;
  std::unique_ptr<std::uint8_t[]> definitions_;
  std::unique_ptr<std::uint8_t[]> chunk_;
  draw::Filler filler_;
  draw::Ditherer ditherer_;
};

}  // namespace bn::reader
