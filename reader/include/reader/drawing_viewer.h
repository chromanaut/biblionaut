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
#include "draw/jpeg.h"
#include "draw/picture_cache.h"
#include "draw/png.h"
#include "draw/row_sink.h"
#include "draw/scale.h"
#include "draw/spool.h"
#include "draw/svg_render.h"
#include "reader/screen.h"

namespace bn::reader {

// What the application shows in place of the test pattern when it is given
// a folder of drawings (App::ViewDrawings()): the SVG, PNG and JPEG files in it,
// one at a time, drawn as the firmware draws a drawing in a book, and a
// picture read, scaled and dithered as the firmware will show one.
//
//   at start         the first, by a full refresh
//   Down or Right    the next, by a half refresh
//   Up or Left       the one before, by a half refresh
//   Confirm, Back    the same one again, by a half or a full refresh
//
// The files are those in the folder whose names end in `.svg`, `.png`,
// `.jpg` or `.jpeg`, in the order of their names; going past the last comes round to the
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
// A picture is fitted to the page in the same way, and read through once.
// Its rows are read (draw::PngReader, or draw::JpegReader: a JPEG at the
// most it can be made smaller as it is read and still be as large as it is
// shown, and a progressive one from its first scan, at an eighth), made the
// size it is shown at (draw::Scaler) and made the panel's levels
// (draw::Ditherer), and written to both planes as they come, a strip at a
// time: the page's rows that fall in one byte of the panel's, which on a
// panel turned a quarter is a window one byte wide and the panel's height
// (core::Display::WriteWindow()), and otherwise eight of the panel's rows.
// The strips of the page above and below the picture are written white. So
// no copy of the panel is kept.
//
// A picture is made as many levels as the panel shows (core::Display::
// Levels()). On a panel with grays, such as the X4's, it is four: it is
// shown first in black and white, each of its grays black, by a half or
// full refresh; then it is read back from the cache, its two grays are
// marked in the two planes, a strip at a time as before, and the gray pass
// (core::Display::StartGrayRefresh()) brings them out. The drive voltages
// stay up from the one refresh to the other. A picture with no grays has no
// gray pass, and one that could not be kept is left in black and white,
// and a line says why. A picture whose rows need more than
// kPictureRowBytes, an interlaced PNG, a JPEG of a kind that is not read,
// and a file that is not a picture are not shown, and a line says why.
//
// Nothing here waits. Tick() draws for about kSliceUs and returns, so the
// buttons and the rest of the application go on around it, a drawing of
// megabytes taking many turns of the loop. A press while one drawing is
// being drawn begins the one it asks for. The panel's drive voltages are
// switched off as each refresh ends, but before a gray pass: with both
// planes written each time, nothing is gained by keeping them on. For each drawing, a line of log
// text says what it was, what it took, and what the renderer left out; for
// each picture, what it was, the size it was shown at, and how long the
// reading, the scaling, the dithering and the writing took, and the gray
// pass.
//
// A drawing is read once and kept on the card (kSpoolPath), and each band
// drawn from what was kept; if it cannot be kept, it is read again for each
// band, and a line says why.
//
// What is shown is kept as well, in the cache of pictures on the card
// (draw::PictureCache, in kCacheFolder), the size it is shown and in its
// levels, a bit a pixel for a drawing and two for a picture in four levels:
// a drawing as it lies on the panel, in the
// order its bands are written; a picture as it lies on the page, in the
// order its rows come (so its key says it is not turned). Shown again,
// while its file has the same size and time, it is read from there a band
// at a time and written to the panel as it was the first time, with nothing
// drawn. What the cache does not have, or has damaged, is drawn as above,
// and kept as it is written; a line says what the cache did.
//
// The memory for the bands, for the renderer, for what a drawing defines
// and for the links of what is kept is taken from the heap at Start():
// about 94 KB for the X4's panel, most of it the filler's cells, the
// definitions and the links. What a picture needs besides is taken the
// first time one is shown: about 73 KB, PNG's window and the readers' rows
// the most of it.
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
  // RowBytesFor(), draw::JpegReader::RowBytesFor()). A picture that needs
  // more is not shown.
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
  // Where what is shown is kept, to be shown again: a hidden folder at the
  // top of the card.
  static constexpr std::string_view kCacheFolder = "/.picture-cache";
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
  // Goes round the drawings twice by itself, for timing them with nobody at
  // the buttons, the second time from the cache: once each is shown, the
  // next is begun as if Down had been pressed then, and after the last the
  // first, or a line saying the tour is over. One that cannot be shown is
  // passed over in the same way, once its line has said why. Call before
  // Start().
  void Tour() noexcept { touring_ = true; }

 private:
  enum class Phase : std::uint8_t {
    kIdle,        // A drawing is on the panel, or there is none.
    kSizing,      // Reading the file's first tag, for its size.
    kRecording,   // Reading the file once, and keeping what it draws for each band.
    kDrawing,     // Drawing a band from the file, when it was not kept.
    kReplaying,   // Drawing a band from what was kept.
    kPicturing,   // Reading a picture through, and writing it a strip at a time.
    kFromCache,   // Writing bands read from the cache.
    kWriting,     // A band or a picture drawn, to be written once the panel is free.
    kRefreshing,  // The panel is showing what was drawn.
    kGraying,     // A picture's grays read back from the cache and written to the planes.
  };

  // Where a picture's rows go: made the panel's levels, kept, and written.
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
  // Once a drawing has been placed: its bands begun, read once and kept,
  // or read for each. False if they cannot be.
  bool StartBands() noexcept;
  // Once a picture's size is known: reading it through begun. False if it
  // cannot be.
  bool StartPictureRead() noexcept;

  // The cache. What the drawing or picture in hand is kept as, from where
  // it lies on the page, the way the device is read.
  void KeyFor(std::size_t left, std::size_t top, std::size_t width, std::size_t height) noexcept;
  // Shows it from the cache, if the cache has it: true if it does. If not,
  // it is kept as it is drawn.
  bool FindInCache() noexcept;
  // Bands read from the cache and written, for up to the time left: true
  // when they all are. If what is kept turns out damaged, it is drawn
  // instead, and the phase is that of drawing it. A picture's rows go
  // through the strips, as they did when it was read.
  bool FromCache(std::uint64_t until) noexcept;
  bool PictureFromCache(std::uint64_t until) noexcept;
  // After a picture's black and white refresh: whether a gray pass follows,
  // and why not if not (into grayNote_); its beginning, reading the picture
  // back from the cache; and its strips written from what is read, for up
  // to the time left, true when they all are and the pass has begun. False
  // from StartGrays() or GrayStrips() leaves the voltages to be switched
  // off and the picture in black and white.
  bool GraysWanted() noexcept;
  bool StartGrays() noexcept;
  bool GrayStrips(std::uint64_t until) noexcept;
  // The picture's grays not shown, for `why`: the voltages switched off,
  // and the refresh's line said.
  void WithoutGrays(const char* why, std::uint64_t now) noexcept;
  // The refresh, or the gray pass after it, over: the lines said, and on to
  // the next.
  void Finished(std::uint64_t now) noexcept;
  // What was read from the cache is not what was written: drawn instead,
  // and kept again.
  void DrawInstead(draw::PictureCacheStatus read) noexcept;
  // Rows of the panel, `rows` of them from `firstRow`, a bit a pixel: the
  // part that is the drawing or picture, kept.
  void CacheRows(std::span<const std::uint8_t> panelRows, std::size_t firstRow, std::size_t rows) noexcept;
  void FinishCaching() noexcept;
  void SayCache() noexcept;
  // The same for a picture: reads its header, fits it to the page, and
  // begins reading it through. False if it cannot be shown.
  bool BeginPicture(std::size_t index) noexcept;
  bool PictureMemory() noexcept;
  // Its header, a PNG's or a JPEG's, and the size it is read at once it is
  // known how large it is shown. False, with a line saying why, if it
  // cannot be shown.
  bool ReadPictureHeader() noexcept;
  bool SizePictureRead() noexcept;
  // A step of whichever reader the picture has, and how its reading went.
  [[nodiscard]] bool StepPicture() noexcept;
  bool PictureReadWhole() const noexcept;
  const char* PictureStatusName() const noexcept;
  // Reads on until the time left: true when the picture has been read
  // through, or reading it failed.
  bool FeedPicture(std::uint64_t until) noexcept;
  // A row of the picture at the size shown: made its levels, kept, and its
  // white put in its strip.
  void TakeRow(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept;
  // The rest of the picture's strips written, and the rest of its rows
  // kept. False if the panel would not take a strip.
  bool WritePicture() noexcept;

  // The strips a picture is written in. The page's row `pageRow` is in
  // strip StripOf(pageRow); rows of one strip come one after another.
  std::size_t StripOf(std::size_t pageRow) const noexcept;
  // Back to the page's first row, nothing written.
  void ResetStrips() noexcept;
  // A row of the picture, `bits` a bit a pixel (a set bit white) from the
  // picture's left edge, put in its strip; each strip before it, white
  // where no row was put, is written first. For the gray pass a strip is
  // two, `which` saying which: 0 for the new-image plane's marks, 1 for the
  // old's, each given as a row where a clear bit is a pixel marked.
  void PutRow(std::size_t pageRow, std::span<const std::uint8_t> bits, std::size_t which = 0) noexcept;
  // Every strip up to and including that of `pageRow` begun, and those
  // before it written.
  void ReachRow(std::size_t pageRow) noexcept;
  void OpenStrip(std::size_t pageRow) noexcept;
  void FlushStrip() noexcept;
  // The strips to the page's end written: false if any was not.
  bool FinishStrips() noexcept;
  // A row of a picture, kept; and the rows gathered, written to the cache.
  // As many rows are gathered as cacheRows_ holds: a write to the card
  // costs a few milliseconds whatever its size.
  // The row is the dithered grays, each the gray of its level.
  void CachePictureRow(std::span<const std::uint8_t> grays) noexcept;
  void FlushCachedRows() noexcept;
  // On a tour, once a file has been shown or found not to be showable: the
  // next is asked for, or after the last the first, or a line saying the
  // tour is over.
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
  bool secondLap_ = false;
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
  std::size_t levels_ = 2;  // A drawing's.

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

  // A picture: whether it is a JPEG, what its header says, its size as it
  // is read (a JPEG's made smaller), and where it goes on the page.
  bool picture_ = false;
  bool jpeg_ = false;
  draw::PngHeader header_;
  draw::JpegHeader jpegHeader_;
  std::uint32_t readWidth_ = 0;
  std::uint32_t readHeight_ = 0;
  std::uint32_t shownWidth_ = 0;
  std::uint32_t shownHeight_ = 0;
  std::size_t pageWidth_ = 0;
  std::size_t pageHeight_ = 0;
  std::size_t left_ = 0;
  std::size_t top_ = 0;
  std::size_t passes_ = 0;  // The bands a drawing was drawn in.
  draw::PngStatus pictureStatus_ = draw::PngStatus::kOk;
  draw::JpegStatus jpegStatus_ = draw::JpegStatus::kOk;
  std::uint32_t pictureLevels_ = 2;  // The panel's: 2, or 4 with a gray pass.
  bool graysSeen_ = false;           // The picture has a gray.
  // Its gray pass: next after the refresh, the planes being written, or
  // begun; and how long each took, or why there was none.
  bool grayNext_ = false;
  bool grayPass_ = false;
  bool grayShown_ = false;
  std::uint64_t refreshUs_ = 0;
  std::uint64_t grayPlanesUs_ = 0;
  std::uint64_t grayPassUs_ = 0;
  const char* grayNote_ = nullptr;
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

  // The strips of a picture: the next of the page's rows not yet reached,
  // the strip open and where it goes on the panel, whether it is the gray
  // pass's two, and whether the panel refused one.
  std::size_t nextRow_ = 0;
  bool stripOpen_ = false;
  std::size_t strip_ = 0;
  core::DisplayWindow stripWindow_;
  bool stripGrays_ = false;
  bool stripFailed_ = false;
  std::size_t cachedRows_ = 0;  // A picture's rows gathered for the cache.

  // The cache: what the one in hand is kept as, where it lies on the
  // panel, its file's size and time, and what the cache has done with it.
  draw::PictureCache cache_{storage_, kCacheFolder};
  std::unique_ptr<std::uint8_t[]> cacheRows_;  // Rows of it: a band of the panel's.
  std::size_t cacheRowsBytes_ = 0;
  draw::PictureKey key_;
  std::size_t cacheX_ = 0;
  std::size_t cacheY_ = 0;
  bool known_ = false;               // Its file's size and time are known, so it can be kept.
  bool fromCache_ = false;           // It is shown from the cache,
  bool caching_ = false;             // or being kept as it is drawn,
  bool cacheKept_ = false;           // and has been,
  const char* cacheNote_ = nullptr;  // or why not, or what was wrong with what was there.
  std::uint64_t cacheUs_ = 0;        // Writing to the cache, or reading from it.
  std::uint64_t cacheBytes_ = 0;

  // The memory a picture takes, the first time one is shown.
  std::unique_ptr<std::uint8_t[]> fileBuffer_;
  std::unique_ptr<std::uint8_t[]> window_;
  std::unique_ptr<std::uint8_t[]> pngRows_;
  std::unique_ptr<std::uint32_t[]> sums_;
  std::unique_ptr<std::uint8_t[]> scaledRow_;
  std::unique_ptr<std::uint8_t[]> stripBits_;  // The strip being filled: two, for the gray pass.
  std::unique_ptr<std::uint8_t[]> rowBits_;    // A row of the picture, a bit a pixel: two, for the gray pass.
  std::unique_ptr<core::BlockFile> pictureFile_;
  std::unique_ptr<draw::PngReader> png_;
  std::unique_ptr<draw::JpegReader> jpegReader_;  // Its rows in pngRows_ too: one picture is read at a time.
  std::unique_ptr<draw::Scaler> scaler_;
  PictureRows pictureRows_{*this};
  Timed timed_{*this};
};

}  // namespace bn::reader
