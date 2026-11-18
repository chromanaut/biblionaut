#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/clock.h"
#include "core/display.h"
#include "core/input.h"
#include "core/storage.h"
#include "core/trace_stream.h"
#include "draw/dither.h"
#include "draw/fill.h"
#include "draw/png.h"
#include "draw/row_sink.h"
#include "draw/scale.h"
#include "draw/spool.h"
#include "draw/svg_render.h"
#include "reader/screen.h"

namespace bn::reader {

// What the application shows in place of the test pattern when it is given
// a folder of drawings (App::ViewDrawings()): the SVG and PNG files in it,
// one at a time, drawn as the firmware draws a drawing in a book, and a
// picture read, scaled and dithered as the firmware will show one.
//
//   at start         the first, by a full refresh
//   Down or Right    the next, by a half refresh
//   Up or Left       the one before, by a half refresh
//   Confirm, Back    the same one again, by a half or a full refresh
//
// The files are those in the folder whose names end in `.svg` or `.png`,
// in the order of their names; going past the last comes round to the
// first.
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
// A picture is fitted to the page in the same way. Its rows come from the
// file in the order the page is read, which is not the panel's when the
// panel is turned: so the whole panel is kept, a bit a pixel, while the
// file is read through once. Its rows are read (draw::PngReader), made the
// size it is shown at (draw::Scaler) and made the panel's levels
// (draw::Ditherer), and each pixel set where it falls on the panel; then
// the whole panel is written to both planes. A picture whose rows need more
// than kPictureRowBytes, an interlaced one, and a file that is not a
// picture are not shown, and a line says why.
//
// Nothing here waits. Tick() draws for about kSliceUs and returns, so the
// buttons and the rest of the application go on around it, a drawing of
// megabytes taking many turns of the loop. A press while one drawing is
// being drawn begins the one it asks for. The panel's drive voltages are
// switched off as each refresh ends: with both planes written each time,
// nothing is gained by keeping them on. For each drawing, a line of log
// text says what it was, what it took, and what the renderer left out; for
// each picture, what it was, the size it was shown at, and how long the
// reading, the scaling, the dithering and the writing took.
//
// A drawing is read once and kept on the card (kSpoolPath), and each band
// drawn from what was kept; if it cannot be kept, it is read again for each
// band, and a line says why.
//
// The memory for the bands, for the renderer, for what a drawing defines
// and for the links of what is kept is taken from the heap at Start():
// about 94 KB for the X4's panel, most of it the filler's cells, the
// definitions and the links. What a picture
// needs besides is taken the first time one is shown: about 110 KB, the
// panel's bits, the reader's window and its rows the most of it.
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
  // The memory for a picture's rows as it is read (draw::PngReader::
  // RowBytesFor()). A picture that needs more is not shown.
  static constexpr std::size_t kPictureRowBytes = 16384;
  // About how long one Tick() draws for.
  static constexpr std::uint64_t kSliceUs = 20'000;
  // Bytes of a file read at a time (the card reads 4 KB at once half again
  // as fast as a kilobyte), and kept for what a drawing defines.
  static constexpr std::size_t kReadBytes = 8192;
  static constexpr std::size_t kDefinitionBytes = 24576;
  // The thinnest a stroke is drawn: a pixel, in 4,096ths. One much thinner
  // would be a light gray, which black and white show as a broken line.
  static constexpr draw::Fixed kThinnestStroke = 4096;
  // Where a drawing is kept while it is shown, once read, to be drawn a
  // band at a time (draw/spool.h): a hidden file at the top of the card,
  // made again for each drawing. And how many chunks it may have: 4 MB.
  static constexpr std::string_view kSpoolPath = "/.drawing-spool";
  static constexpr std::size_t kSpoolChunks = 8192;
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
  // A drawing or picture being read, drawn or kept, a band to write to a
  // panel that is free, or another to begin.
  bool WorkWaiting() const noexcept override;
  bool Resting() const noexcept override { return state_ == State::kAsleep || state_ == State::kFailed; }

  State CurrentState() const noexcept { return state_; }
  // The drawings found, and which is on the panel or on its way: 0 for the
  // first. The number of drawings fully shown so far.
  std::size_t Drawings() const noexcept { return names_.size(); }
  std::size_t Current() const noexcept { return current_; }
  std::size_t Shown() const noexcept { return shown_; }
  // Goes round the drawings once by itself, for timing them with nobody at
  // the buttons: once each is shown, the next is begun as if Down had been
  // pressed then, and after the last a line says the tour is over. One that
  // cannot be shown is passed over in the same way, once its line has said
  // why. Call before Start().
  void TourOnce() noexcept { touring_ = true; }

 private:
  enum class Phase : std::uint8_t {
    kIdle,        // A drawing is on the panel, or there is none.
    kSizing,      // Reading the file's first tag, for its size.
    kRecording,   // Reading the file once, and keeping what it draws for each band.
    kDrawing,     // Drawing a band from the file, when it was not kept.
    kReplaying,   // Drawing a band from what was kept.
    kPicturing,   // Reading a picture through, into the panel's bits.
    kWriting,     // A band or a picture drawn, to be written once the panel is free.
    kRefreshing,  // The panel is showing what was drawn.
  };

  // Where a picture's rows go: made the panel's levels, and set into the
  // panel's bits.
  class PictureRows final : public draw::RowSink {
   public:
    explicit PictureRows(DrawingViewer& viewer) noexcept : viewer_(viewer) {}
    void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override { viewer_.TakeRow(y, gray); }

   private:
    DrawingViewer& viewer_;
  };

  // Between the reader and the scaler: times what the scaler does with
  // each row it is given.
  class Timed final : public draw::RowSink {
   public:
    explicit Timed(DrawingViewer& viewer) noexcept : viewer_(viewer) {}
    void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override;

   private:
    DrawingViewer& viewer_;
  };

  // Begins drawing `index`, from reading its size.
  void Begin(std::size_t index) noexcept;
  // The same for a picture: reads its header, fits it to the page, and
  // begins reading it through. False if it cannot be shown.
  bool BeginPicture(std::size_t index) noexcept;
  bool PictureMemory() noexcept;
  // Reads on until the time left: true when the picture has been read
  // through, or reading it failed.
  bool FeedPicture(std::uint64_t until) noexcept;
  // A row of the picture at the size shown: made the panel's levels, and
  // its dark pixels cleared in the panel's bits.
  void TakeRow(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept;
  // The panel's bits, written to both planes.
  bool WritePicture() noexcept;
  // On a tour, once a file has been shown or found not to be showable: the
  // next is asked for, or after the last a line says the tour is over.
  void TourOn(std::uint64_t now) noexcept;
  // Everything is in both planes: the refresh, and the line saying so.
  void Refresh(std::uint64_t now) noexcept;
  void SayPicture() noexcept;
  void SayNotShown(std::string_view why) noexcept;
  // Gives the renderer the file for up to the time left: true when the
  // file is over, or reading it failed. FeedSome() does it, and Feed()
  // times it.
  bool Feed(std::uint64_t until) noexcept;
  bool FeedSome(std::uint64_t until) noexcept;
  // Tick() times TickSome(), which does the work.
  void TickSome() noexcept;
  // The file read once into the spool: false, and why in spoolFailure_, if
  // it cannot be. A band given back from it, for up to the time left: true
  // when the band is all there.
  bool StartRecording() noexcept;
  bool StartReplay() noexcept;
  bool Replay(std::uint64_t until) noexcept;
  // Once the refresh is over, a second line for what was shown: where the
  // time went, the refresh's included.
  void SayTimes(std::uint64_t refreshUs) noexcept;
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
  bool touring_ = false;
  std::uint64_t phaseStartedUs_ = 0;
  std::uint64_t drawingStartedUs_ = 0;
  std::uint64_t pressToRefreshUs_ = 0;  // From the press to the refresh's start: a press during it moves the first.

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

  // The spool: a file on the card, timed as it is written and read.
  class CardSpool final : public draw::SpoolStore {
   public:
    CardSpool(core::Storage& storage, core::Clock& clock) noexcept : storage_(storage), clock_(clock) {}
    [[nodiscard]] bool Open(std::string_view path) noexcept;
    void Close() noexcept;
    // Gathers what is added in `buffer`, and writes it when it is full, or
    // at Flush(): the card writes a few kilobytes at once faster than the
    // same a chunk at a time. Empty, each chunk is written as it comes.
    void Gather(std::span<std::uint8_t> buffer) noexcept;
    [[nodiscard]] bool Flush() noexcept;
    [[nodiscard]] bool Append(std::span<const std::uint8_t> bytes) noexcept override;
    [[nodiscard]] bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> bytes) noexcept override;
    std::uint64_t writingUs = 0;
    std::uint32_t writes = 0;  // Writes to the card.
    std::uint64_t readingUs = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t bytesRead = 0;

   private:
    core::Storage& storage_;
    core::Clock& clock_;
    // Writes `bytes` to the card, after what is there, and times it.
    [[nodiscard]] bool Write(std::span<const std::uint8_t> bytes) noexcept;
    core::FileHandle file_ = core::kNoFile;
    std::uint32_t size_ = 0;  // Written to the card,
    std::span<std::uint8_t> gather_;
    std::size_t gathered_ = 0;  // and gathered after it.
  };
  // The most a write of the spool gathers.
  static constexpr std::size_t kMostGathered = 8192;
  CardSpool spoolStore_{storage_, clock_};
  std::unique_ptr<draw::SpoolWriter> spool_;
  std::unique_ptr<std::uint16_t[]> links_;
  bool spooling_ = false;               // The drawing in hand is drawn from the spool.
  const char* spoolFailure_ = nullptr;  // Why it is not, if it was tried.
  std::uint64_t replayingUs_ = 0;
  // The viewer's own turns: when the one in hand began, how long they have
  // taken since the drawing began, and that when its refresh began.
  std::uint64_t tickStartedUs_ = 0;
  std::uint64_t turnsUs_ = 0;
  std::uint64_t turnsAtRefreshUs_ = 0;

  // A picture: what its header says, and where it goes on the page.
  bool picture_ = false;
  draw::PngHeader header_;
  std::uint32_t shownWidth_ = 0;
  std::uint32_t shownHeight_ = 0;
  std::size_t pageWidth_ = 0;
  std::size_t pageHeight_ = 0;
  std::size_t left_ = 0;
  std::size_t top_ = 0;
  std::size_t passes_ = 0;  // The bands a drawing was drawn in.
  draw::PngStatus pictureStatus_ = draw::PngStatus::kOk;
  // How long each stage took, over all the bands, a drawing's or a
  // picture's: reading the file, drawing or scaling what it holds, making
  // the panel's levels of it, writing it to the panel, and waiting for the
  // panel to be ready for a band. And how much of a drawing's file was read
  // in all.
  std::uint64_t readingUs_ = 0;
  std::uint64_t renderingUs_ = 0;
  std::uint64_t scalingUs_ = 0;
  std::uint64_t ditheringUs_ = 0;
  std::uint64_t writingUs_ = 0;
  std::uint64_t waitingUs_ = 0;
  std::uint64_t bytesRead_ = 0;
  std::uint64_t timedUs_ = 0;  // The scaler's share of the reader's last step.

  // The memory a picture takes, the first time one is shown.
  std::unique_ptr<std::uint8_t[]> fileBuffer_;
  std::unique_ptr<std::uint8_t[]> window_;
  std::unique_ptr<std::uint8_t[]> pngRows_;
  std::unique_ptr<std::uint32_t[]> sums_;
  std::unique_ptr<std::uint8_t[]> scaledRow_;
  std::unique_ptr<std::uint8_t[]> panelBits_;
  std::unique_ptr<core::BlockFile> pictureFile_;
  std::unique_ptr<draw::PngReader> png_;
  std::unique_ptr<draw::Scaler> scaler_;
  PictureRows pictureRows_{*this};
  Timed timed_{*this};
};

}  // namespace bn::reader
