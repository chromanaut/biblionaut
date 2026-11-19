#include "reader/drawing_viewer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "core/metrics.h"
#include "core/trace.h"
#include "draw/fixed.h"
#include "log_line.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

// Drawings shown in full, and the time the last took, from the press (or
// the start) to its refresh beginning.
core::Counter drawingsShown{"drawings_shown"};
core::Gauge drawingMs{"drawing_ms"};

bool EndsWith(std::string_view name, std::string_view ending) noexcept {
  if (name.size() <= ending.size()) {
    return false;
  }
  for (std::size_t at = 0; at < ending.size(); ++at) {
    char c = name[name.size() - ending.size() + at];
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (c != ending[at]) {
      return false;
    }
  }
  return true;
}

bool IsDrawing(std::string_view name) noexcept { return EndsWith(name, ".svg"); }
bool IsJpeg(std::string_view name) noexcept { return EndsWith(name, ".jpg") || EndsWith(name, ".jpeg"); }
bool IsPicture(std::string_view name) noexcept { return EndsWith(name, ".png") || IsJpeg(name); }

// The buffer a picture's file is read through.
constexpr std::size_t kFileBufferBytes = 4096;

// Where pixel (x, y) of a page `pageWidth` by `pageHeight`, the way the
// device is read, lies on the panel: the same turn back as Unturned().
struct PanelPixel {
  std::size_t x;
  std::size_t y;
};

PanelPixel OnPanel(core::PanelTurn turn, std::size_t pageWidth, std::size_t pageHeight, std::size_t x,
                   std::size_t y) noexcept {
  switch (turn) {
    case core::PanelTurn::kNone:
      break;
    case core::PanelTurn::kClockwise:
      return {.x = y, .y = pageWidth - 1 - x};
    case core::PanelTurn::kHalf:
      return {.x = pageWidth - 1 - x, .y = pageHeight - 1 - y};
    case core::PanelTurn::kAnticlockwise:
      return {.x = pageHeight - 1 - y, .y = x};
  }
  return {.x = x, .y = y};
}

// Copies `count` pixels, a bit each, the leftmost the top bit of a byte:
// from `from`, starting at pixel `fromAt`, to `to` at `toAt`.
void CopyPixels(std::span<const std::uint8_t> from, std::size_t fromAt, std::span<std::uint8_t> to, std::size_t toAt,
                std::size_t count) noexcept {
  for (std::size_t at = 0; at < count; ++at) {
    const std::size_t source = fromAt + at;
    const std::size_t target = toAt + at;
    const auto mask = static_cast<std::uint8_t>(0x80U >> (target % 8));
    if ((from[source / 8] & (0x80U >> (source % 8))) != 0) {
      to[target / 8] = static_cast<std::uint8_t>(to[target / 8] | mask);
    } else {
      to[target / 8] = static_cast<std::uint8_t>(to[target / 8] & ~std::uint32_t{mask});
    }
  }
}

// From a page drawn the way the device is read, `page` across and down, to
// the panel's rows: the turn back.
draw::Matrix Unturned(core::PanelTurn turn, draw::Fixed pageWidth, draw::Fixed pageHeight) noexcept {
  constexpr draw::Coefficient kOne = draw::kCoefficientOne;
  switch (turn) {
    case core::PanelTurn::kNone:
      break;
    case core::PanelTurn::kClockwise:
      // The panel's picture is turned clockwise to be read: its left edge
      // is the page's top.
      return {.a = 0, .b = -kOne, .c = kOne, .d = 0, .e = 0, .f = pageWidth};
    case core::PanelTurn::kHalf:
      return {.a = -kOne, .b = 0, .c = 0, .d = -kOne, .e = pageWidth, .f = pageHeight};
    case core::PanelTurn::kAnticlockwise:
      return {.a = 0, .b = kOne, .c = -kOne, .d = 0, .e = pageHeight, .f = 0};
  }
  return {};
}

}  // namespace

DrawingViewer::DrawingViewer(core::Display& display, core::Clock& clock, core::Storage& storage,
                             core::TraceStream& stream, std::string_view folder)
    : display_(display), clock_(clock), storage_(storage), stream_(stream), folder_(folder) {}

DrawingViewer::~DrawingViewer() { CloseFile(); }

void DrawingViewer::Start() noexcept {
  if (state_ != State::kNotStarted) {
    return;
  }
  if (!display_.Start()) {
    Fail();
    return;
  }
  // The memory for drawing, for a panel of this width.
  const core::DisplaySize size = display_.Size();
  const std::size_t width = size.width;
  // A picture's rows run across the page, which is the panel's height
  // across when the panel is turned a quarter: the ditherer's memory is for
  // the longer side.
  const std::size_t widest = std::max<std::size_t>(size.width, size.height);
  renderer_.reset(new (std::nothrow) draw::SvgRenderer());
  gray_.reset(new (std::nothrow) std::uint8_t[widest * kBandRows]);
  cells_.reset(new (std::nothrow) std::int16_t[draw::Filler::CellsFor(width, kBandRows)]);
  errors_.reset(new (std::nothrow) std::int16_t[draw::Ditherer::ErrorsFor(widest)]);
  bits_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * kBandRows]);
  definitions_.reset(new (std::nothrow) std::uint8_t[kDefinitionBytes]);
  chunk_.reset(new (std::nothrow) std::uint8_t[kReadBytes]);
  cacheRows_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * kBandRows]);
  if (!renderer_ || !gray_ || !cells_ || !errors_ || !bits_ || !definitions_ || !chunk_ || !cacheRows_) {
    stream_.SendText("drawings: no memory to draw them in");
    Fail();
    return;
  }
  renderer_->KeepDefinitionsIn({definitions_.get(), kDefinitionBytes});
  renderer_->SetThinnestStroke(kThinnestStroke);
  spool_.reset(new (std::nothrow) draw::SpoolWriter());
  links_.reset(new (std::nothrow) std::uint16_t[kSpoolChunks]);
  state_ = State::kReady;

  // The drawings: every file in the folder named as one, in the order of
  // their names.
  core::DirectoryHandle directory = core::kNoDirectory;
  if (storage_.OpenDirectory(folder_, directory) == core::StorageStatus::kOk) {
    std::array<char, kLongestName + 1> name{};
    std::size_t length = 0;
    core::FileInfo info;
    while (names_.size() < kMostDrawings &&
           storage_.NextInDirectory(directory, name, length, info) == core::StorageStatus::kOk && length > 0) {
      const std::string_view found(name.data(), std::min(length, name.size()));
      // A name that begins with a dot is hidden, as the `._` files a Mac
      // writes beside each file it copies to a card are.
      if (!info.isDirectory && !found.starts_with('.') && (IsDrawing(found) || IsPicture(found))) {
        names_.emplace_back(found);
      }
    }
    storage_.CloseDirectory(directory);
  }
  std::sort(names_.begin(), names_.end());
  if (names_.empty()) {
    LogLine line;
    line.Text("drawings: none in ").Text(folder_);
    stream_.SendText(line.View());
    return;
  }
  drawingStartedUs_ = clock_.NowUs();
  Begin(0);
}

void DrawingViewer::OnInput(const core::InputEvent& event) noexcept {
  if (state_ != State::kReady || sleepAsked_ || names_.empty() || event.kind != core::InputEventKind::kPress) {
    return;
  }
  const std::size_t count = names_.size();
  switch (event.button) {
    case core::Button::kDown:
    case core::Button::kRight:
      wanted_ = (wanted_ + 1) % count;
      break;
    case core::Button::kUp:
    case core::Button::kLeft:
      wanted_ = (wanted_ + count - 1) % count;
      break;
    case core::Button::kConfirm:
    case core::Button::kBack:
      redrawWanted_ = true;
      redrawMode_ = event.button == core::Button::kBack ? core::RefreshMode::kFull : core::RefreshMode::kHalf;
      break;
    default:
      return;
  }
  drawingStartedUs_ = event.timeUs;
  // One being drawn that is no longer wanted is given up.
  if ((phase_ == Phase::kSizing || phase_ == Phase::kRecording || phase_ == Phase::kDrawing ||
       phase_ == Phase::kReplaying || phase_ == Phase::kPicturing || phase_ == Phase::kWriting ||
       phase_ == Phase::kFromCache) &&
      wanted_ != current_) {
    CloseFile();
    phase_ = Phase::kIdle;
  }
}

bool DrawingViewer::WorkWaiting() const noexcept {
  if (state_ != State::kReady) {
    return false;
  }
  switch (phase_) {
    case Phase::kSizing:
    case Phase::kRecording:
    case Phase::kDrawing:
    case Phase::kReplaying:
    case Phase::kPicturing:
      return true;
    case Phase::kWriting:
    case Phase::kFromCache:
      return !display_.Busy();
    case Phase::kIdle:
      return !sleepAsked_ && !names_.empty() && (wanted_ != current_ || redrawWanted_) && !display_.Busy();
    case Phase::kRefreshing:
      break;
  }
  return false;
}

void DrawingViewer::Sleep() noexcept {
  sleepAsked_ = true;
  if (phase_ == Phase::kSizing || phase_ == Phase::kRecording || phase_ == Phase::kDrawing ||
      phase_ == Phase::kReplaying || phase_ == Phase::kPicturing || phase_ == Phase::kWriting ||
      phase_ == Phase::kFromCache) {
    CloseFile();
    phase_ = Phase::kIdle;
  }
}

void DrawingViewer::Tick() noexcept {
  tickStartedUs_ = clock_.NowUs();
  TickSome();
  turnsUs_ += clock_.NowUs() - tickStartedUs_;
}

void DrawingViewer::TickSome() noexcept {
  if (state_ != State::kReady) {
    return;
  }
  const std::uint64_t now = clock_.NowUs();
  switch (phase_) {
    case Phase::kIdle:
      if (display_.Busy()) {
        if (now - phaseStartedUs_ > kGiveUpUs) {
          Fail();
        }
        return;
      }
      if (sleepAsked_) {
        if (!display_.Sleep()) {
          Fail();
          return;
        }
        state_ = State::kAsleep;
        return;
      }
      if (!names_.empty() && (wanted_ != current_ || redrawWanted_)) {
        Begin(wanted_);
      }
      return;

    case Phase::kSizing:
      if (!Feed(now + kSliceUs)) {
        return;
      }
      Place();
      if (FindInCache()) {
        return;
      }
      if (!StartBands()) {
        Fail();
      }
      return;

    case Phase::kRecording: {
      if (!Feed(now + kSliceUs)) {
        return;
      }
      static_cast<void>(renderer_->Finish());
      draw::SpoolStatus kept = spool_->Finish();
      // What is gathered goes to the card before the band of grays is
      // wanted for the bands.
      if (kept == draw::SpoolStatus::kOk && !spoolStore_.Flush()) {
        kept = draw::SpoolStatus::kStoreFailed;
      }
      spoolStore_.Gather({});
      spooling_ = kept == draw::SpoolStatus::kOk;
      if (spooling_) {
        phase_ = Phase::kReplaying;
        if (!StartReplay()) {
          Fail();
        }
        return;
      }
      // Not kept whole: drawn as before.
      spoolFailure_ = draw::SpoolStatusName(kept);
      spoolStore_.Close();
      phase_ = Phase::kDrawing;
      if (!StartPass(&filler_)) {
        Fail();
      }
      return;
    }

    case Phase::kReplaying:
      if (!Replay(now + kSliceUs)) {
        return;
      }
      phase_ = Phase::kWriting;
      phaseStartedUs_ = now;
      return;

    case Phase::kDrawing:
      if (!Feed(now + kSliceUs)) {
        return;
      }
      static_cast<void>(renderer_->Finish());
      phase_ = Phase::kWriting;
      phaseStartedUs_ = now;
      [[fallthrough]];

    case Phase::kWriting: {
      if (display_.Busy()) {
        if (now - phaseStartedUs_ > kGiveUpUs) {
          Fail();
        }
        return;
      }
      if (picture_) {
        if (!WritePicture()) {
          Fail();
          return;
        }
        Refresh(now);
        return;
      }
      waitingUs_ += now - phaseStartedUs_;
      if (!WriteBand()) {
        Fail();
        return;
      }
      bandTop_ += kBandRows;
      if (bandTop_ < display_.Size().height) {
        phase_ = spooling_ ? Phase::kReplaying : Phase::kDrawing;
        if (!(spooling_ ? StartReplay() : StartPass(&filler_))) {
          Fail();
        }
        return;
      }
      // The whole drawing is in both planes.
      Refresh(now);
      return;
    }

    case Phase::kPicturing:
      if (!FeedPicture(now + kSliceUs)) {
        return;
      }
      phase_ = Phase::kWriting;
      phaseStartedUs_ = now;
      return;

    case Phase::kFromCache:
      if (display_.Busy()) {
        if (now - phaseStartedUs_ > kGiveUpUs) {
          Fail();
        }
        return;
      }
      if (FromCache(now + kSliceUs)) {
        Refresh(now);
      }
      return;

    case Phase::kRefreshing:
      if (display_.Busy()) {
        if (now - phaseStartedUs_ > kGiveUpUs) {
          Fail();
        }
        return;
      }
      ++shown_;
      drawingsShown.Add();
      SayTimes(now - phaseStartedUs_);
      phase_ = Phase::kIdle;
      TourOn(now);
      phaseStartedUs_ = now;
      return;
  }
}

void DrawingViewer::TourOn(std::uint64_t now) noexcept {
  if (!touring_) {
    return;
  }
  if (current_ + 1 < names_.size()) {
    wanted_ = current_ + 1;
    drawingStartedUs_ = now;
    return;
  }
  if (!secondLap_) {
    secondLap_ = true;
    wanted_ = 0;
    if (wanted_ == current_) {
      redrawWanted_ = true;
      redrawMode_ = core::RefreshMode::kHalf;
    }
    drawingStartedUs_ = now;
    return;
  }
  LogLine line;
  line.Text("drawings: the tour is over, ").Count(shown_, " shown", " shown");
  stream_.SendText(line.View());
  touring_ = false;
}

void DrawingViewer::Refresh(std::uint64_t now) noexcept {
  FinishCaching();
  pressToRefreshUs_ = clock_.NowUs() - drawingStartedUs_;
  turnsAtRefreshUs_ = turnsUs_ + (clock_.NowUs() - tickStartedUs_);
  CloseFile();
  const core::RefreshMode mode = firstRefresh_   ? core::RefreshMode::kFull
                                 : redrawWanted_ ? redrawMode_
                                                 : core::RefreshMode::kHalf;
  if (!display_.StartRefresh(mode, core::AfterRefresh::kPowerOff)) {
    Fail();
    return;
  }
  BN_TRACE_TOP_SPAN_SINCE(kSpanDrawing, static_cast<std::uint32_t>(drawingStartedUs_),
                          static_cast<std::uint32_t>(current_ + 1));
  drawingMs.Set(
      static_cast<std::uint32_t>(std::min<std::uint64_t>((clock_.NowUs() - drawingStartedUs_) / 1000, UINT32_MAX)));
  if (picture_) {
    SayPicture();
  } else {
    Say();
  }
  SayCache();
  firstRefresh_ = false;
  redrawWanted_ = false;
  phase_ = Phase::kRefreshing;
  phaseStartedUs_ = now;
}

void DrawingViewer::Begin(std::size_t index) noexcept {
  CloseFile();
  current_ = index;
  wanted_ = index;
  picture_ = IsPicture(names_[index]);
  jpeg_ = IsJpeg(names_[index]);
  // What it would be kept in the cache as, so far: its file.
  {
    const std::string path = folder_ + "/" + names_[index];
    core::FileInfo info;
    known_ = storage_.Stat(path, info) == core::StorageStatus::kOk;
    key_ = {.source = draw::PictureKey::PathHash(path),
            .sourceSize = info.size,
            .sourceModified = info.modified,
            .width = 0,
            .height = 0,
            .levels = static_cast<std::uint16_t>(levels_),
            .turn = static_cast<std::uint8_t>(display_.ReadingTurn())};
    fromCache_ = false;
    caching_ = false;
    cacheKept_ = false;
    cacheNote_ = nullptr;
    cacheUs_ = 0;
    cacheBytes_ = 0;
  }
  if (picture_) {
    if (!BeginPicture(index)) {
      CloseFile();
      redrawWanted_ = false;
      phase_ = Phase::kIdle;
      TourOn(clock_.NowUs());
    }
    return;
  }
  const std::string path = folder_ + "/" + names_[index];
  if (storage_.Open(path, core::OpenMode::kRead, file_) != core::StorageStatus::kOk ||
      storage_.Size(file_, fileSize_) != core::StorageStatus::kOk) {
    LogLine line;
    line.Text("drawing ").Number(index + 1).Text(": cannot read ").Text(names_[index]);
    stream_.SendText(line.View());
    CloseFile();
    redrawWanted_ = false;
    phase_ = Phase::kIdle;
    TourOn(clock_.NowUs());
    return;
  }
  readingUs_ = 0;
  renderingUs_ = 0;
  ditheringUs_ = 0;
  writingUs_ = 0;
  waitingUs_ = 0;
  bytesRead_ = 0;
  passes_ = 0;
  spooling_ = false;
  spoolFailure_ = nullptr;
  replayingUs_ = 0;
  turnsUs_ = 0;
  spoolStore_.writingUs = 0;
  spoolStore_.writes = 0;
  spoolStore_.readingUs = 0;
  spoolStore_.bytesWritten = 0;
  spoolStore_.bytesRead = 0;
  phase_ = Phase::kSizing;
  phaseStartedUs_ = clock_.NowUs();
  // The size first: with nothing to draw into, the renderer reads only as
  // far as the drawing's first tag before this stops it.
  static_cast<void>(StartPass(nullptr));
}

bool DrawingViewer::StartBands() noexcept {
  bandTop_ = 0;
  if (!ditherer_.Begin(display_.Size().width, static_cast<std::uint32_t>(levels_),
                       {errors_.get(), draw::Ditherer::ErrorsFor(display_.Size().width)})) {
    return false;
  }
  if (StartRecording()) {
    phase_ = Phase::kRecording;
    return true;
  }
  phase_ = Phase::kDrawing;
  return StartPass(&filler_);
}

bool DrawingViewer::StartPass(draw::Filler* filler) noexcept {
  offset_ = 0;
  chunkAt_ = 0;
  chunkSize_ = 0;
  fileFailed_ = false;
  if (filler == nullptr) {
    renderer_->Begin(nullptr, draw::kFixedOne, draw::kFixedOne);
    return true;
  }
  const core::DisplaySize size = display_.Size();
  const std::size_t width = size.width;
  const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - bandTop_);
  const std::span<std::uint8_t> band(gray_.get(), width * rows);
  std::fill(band.begin(), band.end(), std::uint8_t{255});
  if (!filler->Begin(band, width, static_cast<std::int32_t>(bandTop_),
                     {cells_.get(), draw::Filler::CellsFor(width, kBandRows)})) {
    return false;
  }
  renderer_->Begin(filler, drawWidth_, drawHeight_);
  return true;
}

bool DrawingViewer::Feed(std::uint64_t until) noexcept {
  const std::uint64_t started = clock_.NowUs();
  const std::uint64_t readBefore = readingUs_;
  const std::uint64_t spooledBefore = spoolStore_.writingUs;
  const bool over = FeedSome(until);
  renderingUs_ += (clock_.NowUs() - started) - (readingUs_ - readBefore) - (spoolStore_.writingUs - spooledBefore);
  return over;
}

bool DrawingViewer::FeedSome(std::uint64_t until) noexcept {
  const bool sizing = phase_ == Phase::kSizing;
  for (std::uint32_t calls = 0;; ++calls) {
    // The clock is read every few calls: a call does little.
    if (calls % 16 == 15 && clock_.NowUs() >= until) {
      return false;
    }
    if (sizing && renderer_->Size().known) {
      return true;
    }
    if (chunkAt_ == chunkSize_) {
      if (fileFailed_ || offset_ >= fileSize_) {
        return true;
      }
      std::size_t read = 0;
      const std::uint64_t reading = clock_.NowUs();
      const bool readOk =
          storage_.ReadAt(file_, offset_, {chunk_.get(), kReadBytes}, read) == core::StorageStatus::kOk && read != 0;
      readingUs_ += clock_.NowUs() - reading;
      if (!readOk) {
        fileFailed_ = true;
        return true;
      }
      bytesRead_ += read;
      offset_ += static_cast<std::uint32_t>(read);
      chunkAt_ = 0;
      chunkSize_ = read;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes read, as the characters they are.
    const std::string_view bytes(reinterpret_cast<const char*>(chunk_.get()) + chunkAt_, chunkSize_ - chunkAt_);
    chunkAt_ += renderer_->Step(bytes);
  }
}

bool DrawingViewer::StartRecording() noexcept {
  if (!spool_ || !links_) {
    spoolFailure_ = "no memory";
    return false;
  }
  if (!spoolStore_.Open(kSpoolPath)) {
    spoolFailure_ = "the card would not take it";
    return false;
  }
  // The filler's cells are not wanted until the file has been read: they
  // hold what each band has in hand meanwhile.
  const core::DisplaySize size = display_.Size();
  const std::size_t cells = draw::Filler::CellsFor(size.width, kBandRows);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the cells' memory, as bytes.
  const std::span<std::uint8_t> buffers(reinterpret_cast<std::uint8_t*>(cells_.get()), cells * sizeof(std::int16_t));
  if (!spool_->Begin(static_cast<std::int32_t>(size.width), static_cast<std::int32_t>(size.height),
                     static_cast<std::int32_t>(kBandRows), buffers, {links_.get(), kSpoolChunks}, spoolStore_)) {
    spoolFailure_ = "not a picture it can keep";
    spoolStore_.Close();
    return false;
  }
  // The band of grays is not wanted either: what is kept is gathered there,
  // to be written a few kilobytes at a time.
  const std::size_t grays = std::max<std::size_t>(size.width, size.height) * kBandRows;
  const std::size_t gather =
      std::min(kMostGathered, (grays / draw::SpoolWriter::kChunkBytes) * draw::SpoolWriter::kChunkBytes);
  spoolStore_.Gather({gray_.get(), gather});
  offset_ = 0;
  chunkAt_ = 0;
  chunkSize_ = 0;
  fileFailed_ = false;
  renderer_->Begin(spool_.get(), drawWidth_, drawHeight_);
  return true;
}

bool DrawingViewer::StartReplay() noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t width = size.width;
  const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - bandTop_);
  const std::span<std::uint8_t> band(gray_.get(), width * rows);
  std::fill(band.begin(), band.end(), std::uint8_t{255});
  return filler_.Begin(band, width, static_cast<std::int32_t>(bandTop_),
                       {cells_.get(), draw::Filler::CellsFor(width, kBandRows)}) &&
         spool_->BeginBand(bandTop_ / kBandRows, {chunk_.get(), kReadBytes});
}

bool DrawingViewer::Replay(std::uint64_t until) noexcept {
  const std::uint64_t started = clock_.NowUs();
  const std::uint64_t readBefore = spoolStore_.readingUs;
  bool over = false;
  for (std::uint32_t calls = 0;; ++calls) {
    if (calls % 16 == 15 && clock_.NowUs() >= until) {
      break;
    }
    if (!spool_->ReplayStep(filler_)) {
      over = true;
      break;
    }
  }
  replayingUs_ += (clock_.NowUs() - started) - (spoolStore_.readingUs - readBefore);
  if (over && spool_->Status() != draw::SpoolStatus::kOk) {
    // What was kept could not be read back: this band and the rest are
    // drawn as before.
    spoolFailure_ = draw::SpoolStatusName(spool_->Status());
    spooling_ = false;
    spoolStore_.Close();
    phase_ = Phase::kDrawing;
    if (!StartPass(&filler_)) {
      Fail();
    }
    return false;
  }
  return over;
}

bool DrawingViewer::CardSpool::Open(std::string_view path) noexcept {
  Close();
  size_ = 0;
  return storage_.Open(path, core::OpenMode::kCreate, file_) == core::StorageStatus::kOk;
}

void DrawingViewer::CardSpool::Close() noexcept {
  if (file_ != core::kNoFile) {
    storage_.Close(file_);
    file_ = core::kNoFile;
  }
  gather_ = {};
  gathered_ = 0;
}

void DrawingViewer::CardSpool::Gather(std::span<std::uint8_t> buffer) noexcept {
  gather_ = buffer;
  gathered_ = 0;
}

bool DrawingViewer::CardSpool::Flush() noexcept {
  if (gathered_ == 0) {
    return true;
  }
  const std::span<const std::uint8_t> pending(gather_.data(), gathered_);
  gathered_ = 0;
  return Write(pending);
}

bool DrawingViewer::CardSpool::Append(std::span<const std::uint8_t> bytes) noexcept {
  if (gather_.empty() || bytes.size() > gather_.size()) {
    return Flush() && Write(bytes);
  }
  if (gathered_ + bytes.size() > gather_.size() && !Flush()) {
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), gather_.subspan(gathered_, bytes.size()).begin());
  gathered_ += bytes.size();
  bytesWritten += bytes.size();
  return gathered_ < gather_.size() || Flush();
}

bool DrawingViewer::CardSpool::Write(std::span<const std::uint8_t> bytes) noexcept {
  const std::uint64_t started = clock_.NowUs();
  const bool written = file_ != core::kNoFile && storage_.WriteAt(file_, size_, bytes) == core::StorageStatus::kOk;
  writingUs += clock_.NowUs() - started;
  if (written) {
    size_ += static_cast<std::uint32_t>(bytes.size());
    ++writes;
  }
  return written;
}

bool DrawingViewer::CardSpool::ReadAt(std::uint32_t offset, std::span<std::uint8_t> bytes) noexcept {
  if (!Flush()) {
    return false;
  }
  const std::uint64_t started = clock_.NowUs();
  std::size_t read = 0;
  const bool ok = file_ != core::kNoFile && storage_.ReadAt(file_, offset, bytes, read) == core::StorageStatus::kOk &&
                  read == bytes.size();
  readingUs += clock_.NowUs() - started;
  bytesRead += read;
  return ok;
}

void DrawingViewer::Place() noexcept {
  const core::DisplaySize size = display_.Size();
  const core::PanelTurn turn = display_.ReadingTurn();
  const bool quarter = turn == core::PanelTurn::kClockwise || turn == core::PanelTurn::kAnticlockwise;
  // The page, the way the device is read.
  const draw::Fixed pageWidth = static_cast<draw::Fixed>((quarter ? size.height : size.width) * draw::kFixedOne);
  const draw::Fixed pageHeight = static_cast<draw::Fixed>((quarter ? size.width : size.height) * draw::kFixedOne);
  draw::Fixed width = 0;
  draw::Fixed height = 0;
  renderer_->Size().Natural(width, height);
  if (width <= 0 || height <= 0) {
    // Nothing to fit: the drawing draws nothing, whatever size it is given.
    width = pageWidth;
    height = pageHeight;
  }
  // As large as fits, keeping its shape.
  if (std::int64_t{width} * pageHeight >= std::int64_t{height} * pageWidth) {
    drawWidth_ = pageWidth;
    drawHeight_ = static_cast<draw::Fixed>((std::int64_t{height} * pageWidth) / width);
  } else {
    drawHeight_ = pageHeight;
    drawWidth_ = static_cast<draw::Fixed>((std::int64_t{width} * pageHeight) / height);
  }
  bool clamped = false;
  const draw::Fixed left = (pageWidth - drawWidth_) / 2;
  const draw::Fixed top = (pageHeight - drawHeight_) / 2;
  const draw::Matrix middle = draw::Translation(left, top);
  renderer_->PlaceOn(draw::Multiply(Unturned(turn, pageWidth, pageHeight), middle, clamped));
  // The pixels it covers, a part or all of each, which nothing outside of
  // is drawn on.
  pageWidth_ = quarter ? size.height : size.width;
  pageHeight_ = quarter ? size.width : size.height;
  const auto from = [](draw::Fixed at) { return static_cast<std::size_t>(at / draw::kFixedOne); };
  const auto to = [](draw::Fixed at) {
    return static_cast<std::size_t>((std::int64_t{at} + draw::kFixedOne - 1) / draw::kFixedOne);
  };
  const std::size_t x = from(left);
  const std::size_t y = from(top);
  KeyFor(x, y, std::min(to(left + drawWidth_), pageWidth_) - x, std::min(to(top + drawHeight_), pageHeight_) - y);
}

bool DrawingViewer::WriteBand() noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t width = size.width;
  const std::size_t rowBytes = size.RowBytes();
  const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - bandTop_);
  const std::uint64_t dithering = clock_.NowUs();
  for (std::size_t row = 0; row < rows; ++row) {
    const std::span<std::uint8_t> grays(gray_.get() + (row * width), width);
    if (!ditherer_.Row(grays)) {
      return false;
    }
    // A set bit is white, the leftmost pixel the top bit of the first byte.
    const std::span<std::uint8_t> out(bits_.get() + (row * rowBytes), rowBytes);
    std::fill(out.begin(), out.end(), std::uint8_t{0});
    for (std::size_t x = 0; x < width; ++x) {
      if (grays[x] >= 128) {
        out[x / 8] = static_cast<std::uint8_t>(out[x / 8] | (0x80U >> (x % 8)));
      }
    }
  }
  const std::uint64_t writing = clock_.NowUs();
  ditheringUs_ += writing - dithering;
  const std::span<const std::uint8_t> bits(bits_.get(), rows * rowBytes);
  const auto top = static_cast<std::uint16_t>(bandTop_);
  const bool written = display_.WriteRows(core::DisplayPlane::kNew, top, bits) &&
                       display_.WriteRows(core::DisplayPlane::kOld, top, bits);
  writingUs_ += clock_.NowUs() - writing;
  ++passes_;
  CacheRows(bits, bandTop_, rows);
  return written;
}

void DrawingViewer::Say() noexcept {
  const draw::SvgReport& report = renderer_->Report();
  LogLine line;
  line.Text("drawing ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
  if (fromCache_) {
    line.Text(", from the cache, ").Number((clock_.NowUs() - drawingStartedUs_) / 1000).Text(" ms");
    stream_.SendText(line.View());
    return;
  }
  line.Text(", ").Text(draw::SvgStatusName(renderer_->Status())).Text(", ");
  line.Count(report.shapes, " shape", " shapes").Text(", ");
  line.Count(report.strokes, " stroke", " strokes").Text(", ");
  line.Number((clock_.NowUs() - drawingStartedUs_) / 1000).Text(" ms");
  if (fileFailed_) {
    line.Text(", the file could not all be read");
  }
  if (report.leftOut != 0) {
    line.Text("; left out:");
    for (unsigned bit = 0; bit < 32; ++bit) {
      if ((report.leftOut & (1U << bit)) != 0) {
        line.Text(" ").Text(draw::SvgLeftOutName(static_cast<draw::SvgLeftOut>(1U << bit))).Text(",");
      }
    }
  }
  stream_.SendText(line.View());
}

void DrawingViewer::SayTimes(std::uint64_t refreshUs) noexcept {
  LogLine line;
  if (picture_) {
    line.Text("picture ").Number(current_ + 1).Text(" timed: refreshing ").Number(refreshUs / 1000).Text(" ms");
  } else {
    line.Text("drawing ").Number(current_ + 1).Text(" timed: ");
    line.Count(passes_, " band", " bands").Text(", ").Number(bytesRead_ / 1024).Text(" KB read in ");
    line.Number(readingUs_ / 1000).Text(" ms; rendering ").Number(renderingUs_ / 1000);
    line.Text(" ms, dithering ").Number(ditheringUs_ / 1000).Text(" ms, writing ").Number(writingUs_ / 1000);
    line.Text(" ms, waiting ").Number(waitingUs_ / 1000);
    // From the press to the refresh, what the viewer did not spend on these
    // is the loop's other turns.
    // Of the rest, what went in the viewer's own turns, and what between them.
    const std::uint64_t untilRefresh = pressToRefreshUs_;
    const std::uint64_t spent = readingUs_ + renderingUs_ + ditheringUs_ + writingUs_ + waitingUs_ +
                                spoolStore_.writingUs + spoolStore_.readingUs + replayingUs_ + cacheUs_;
    const std::uint64_t inTurns = std::max(turnsAtRefreshUs_, spent);
    line.Text(" ms, other ").Number((inTurns - spent) / 1000);
    line.Text(" ms, between turns ").Number((untilRefresh > inTurns ? untilRefresh - inTurns : 0) / 1000);
    line.Text(" ms, refreshing ").Number(refreshUs / 1000).Text(" ms");
  }
  stream_.SendText(line.View());
  if (picture_ || (!spooling_ && spoolFailure_ == nullptr)) {
    return;
  }
  LogLine spooled;
  spooled.Text("drawing ").Number(current_ + 1).Text(" timed: ");
  if (!spooling_) {
    spooled.Text("not spooled: ").Text(spoolFailure_);
  } else {
    spooled.Text("spool ").Number(spoolStore_.bytesWritten / 1024).Text(" KB written in ");
    spooled.Number(spoolStore_.writingUs / 1000).Text(" ms (").Count(spoolStore_.writes, " write", " writes");
    spooled.Text("), ").Number(spoolStore_.bytesRead / 1024);
    spooled.Text(" KB read in ").Number(spoolStore_.readingUs / 1000).Text(" ms; replaying ");
    spooled.Number(replayingUs_ / 1000).Text(" ms");
  }
  stream_.SendText(spooled.View());
}

void DrawingViewer::Fail() noexcept {
  CloseFile();
  state_ = State::kFailed;
}

void DrawingViewer::CloseFile() noexcept {
  if (file_ != core::kNoFile) {
    storage_.Close(file_);
    file_ = core::kNoFile;
  }
  // A picture not yet all kept is left so that it is found damaged.
  cache_.Close();
  caching_ = false;
  spoolStore_.Close();
  if (pictureFile_) {
    pictureFile_->Close();
  }
}

// ---- Pictures ----

bool DrawingViewer::PictureMemory() noexcept {
  if (png_) {
    return true;
  }
  const core::DisplaySize size = display_.Size();
  const auto widest = static_cast<std::uint32_t>(std::max<std::size_t>(size.width, size.height));
  // The largest first, while the heap has the most room in one piece.
  panelBits_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * size.height]);
  window_.reset(new (std::nothrow) std::uint8_t[draw::PngReader::kWindowBytes]);
  pngRows_.reset(new (std::nothrow) std::uint8_t[kPictureRowBytes]);
  fileBuffer_.reset(new (std::nothrow) std::uint8_t[kFileBufferBytes]);
  sums_.reset(new (std::nothrow) std::uint32_t[draw::Scaler::SumsFor(widest)]);
  scaledRow_.reset(new (std::nothrow) std::uint8_t[widest]);
  if (fileBuffer_ && window_ && pngRows_ && sums_ && scaledRow_ && panelBits_) {
    pictureFile_.reset(new (std::nothrow) core::BlockFile({fileBuffer_.get(), kFileBufferBytes}));
    png_.reset(new (std::nothrow)
                   draw::PngReader({window_.get(), draw::PngReader::kWindowBytes}, {pngRows_.get(), kPictureRowBytes}));
    jpegReader_.reset(new (std::nothrow) draw::JpegReader({pngRows_.get(), kPictureRowBytes}));
    scaler_.reset(new (std::nothrow)
                      draw::Scaler({sums_.get(), draw::Scaler::SumsFor(widest)}, {scaledRow_.get(), widest}));
  }
  if (!pictureFile_ || !png_ || !jpegReader_ || !scaler_) {
    // All of it, or none: what was had is given back.
    png_.reset();
    jpegReader_.reset();
    scaler_.reset();
    pictureFile_.reset();
    fileBuffer_.reset();
    window_.reset();
    pngRows_.reset();
    sums_.reset();
    scaledRow_.reset();
    panelBits_.reset();
    return false;
  }
  return true;
}

bool DrawingViewer::BeginPicture(std::size_t index) noexcept {
  if (!PictureMemory()) {
    SayNotShown("no memory to show it in");
    return false;
  }
  const std::string path = folder_ + "/" + names_[index];
  if (pictureFile_->Open(storage_, path, core::OpenMode::kRead) != core::StorageStatus::kOk) {
    SayNotShown("cannot read it");
    return false;
  }
  if (!ReadPictureHeader()) {
    return false;
  }
  // As large as fits on the page the way the device is read, keeping its
  // shape, and in the middle of it.
  const core::DisplaySize size = display_.Size();
  const core::PanelTurn turn = display_.ReadingTurn();
  const bool quarter = turn == core::PanelTurn::kClockwise || turn == core::PanelTurn::kAnticlockwise;
  pageWidth_ = quarter ? size.height : size.width;
  pageHeight_ = quarter ? size.width : size.height;
  const std::uint64_t width = jpeg_ ? jpegHeader_.width : header_.width;
  const std::uint64_t height = jpeg_ ? jpegHeader_.height : header_.height;
  if (width * pageHeight_ >= height * pageWidth_) {
    shownWidth_ = static_cast<std::uint32_t>(pageWidth_);
    shownHeight_ =
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, ((2 * height * pageWidth_) + width) / (2 * width)));
  } else {
    shownHeight_ = static_cast<std::uint32_t>(pageHeight_);
    shownWidth_ =
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, ((2 * width * pageHeight_) + height) / (2 * height)));
  }
  left_ = (pageWidth_ - shownWidth_) / 2;
  top_ = (pageHeight_ - shownHeight_) / 2;
  if (!SizePictureRead()) {
    return false;
  }
  readingUs_ = 0;
  scalingUs_ = 0;
  ditheringUs_ = 0;
  writingUs_ = 0;
  waitingUs_ = 0;
  bytesRead_ = 0;
  passes_ = 0;
  turnsUs_ = 0;
  KeyFor(left_, top_, shownWidth_, shownHeight_);
  if (FindInCache()) {
    return true;
  }
  if (!StartPictureRead()) {
    SayNotShown("it cannot be scaled");
    return false;
  }
  return true;
}

bool DrawingViewer::ReadPictureHeader() noexcept {
  // The header alone, which says how large it is: a few steps.
  pictureStatus_ = draw::PngStatus::kOk;
  jpegStatus_ = draw::JpegStatus::kOk;
  if (jpeg_) {
    jpegReader_->Begin(*pictureFile_, nullptr);
    while (jpegReader_->Step()) {
    }
    jpegStatus_ = jpegReader_->Status();
    if (!jpegReader_->HeaderKnown() || jpegStatus_ != draw::JpegStatus::kOk) {
      SayNotShown(draw::JpegStatusName(jpegStatus_));
      return false;
    }
    jpegHeader_ = jpegReader_->Header();
    return true;
  }
  png_->Begin(*pictureFile_, nullptr);
  while (png_->Step()) {
  }
  pictureStatus_ = png_->Status();
  if (!png_->HeaderKnown() || pictureStatus_ != draw::PngStatus::kOk) {
    SayNotShown(draw::PngStatusName(pictureStatus_));
    return false;
  }
  header_ = png_->Header();
  if (header_.interlaced) {
    SayNotShown("interlaced");
    return false;
  }
  return true;
}

bool DrawingViewer::SizePictureRead() noexcept {
  std::uint64_t room = 0;
  if (jpeg_) {
    // As small as it can be read and still be as large as it is shown.
    const draw::JpegReduction reduction = draw::JpegReader::ReductionFor(jpegHeader_, shownWidth_, shownHeight_);
    readWidth_ = draw::JpegReader::WidthAt(jpegHeader_, reduction);
    readHeight_ = draw::JpegReader::HeightAt(jpegHeader_, reduction);
    room = draw::JpegReader::RowBytesFor(jpegHeader_, reduction);
  } else {
    readWidth_ = header_.width;
    readHeight_ = header_.height;
    room = draw::PngReader::RowBytesFor(header_);
  }
  if (room > kPictureRowBytes) {
    LogLine why;
    why.Text("its rows need ").Number(room).Text(" bytes");
    SayNotShown(why.View());
    return false;
  }
  return true;
}

bool DrawingViewer::StepPicture() noexcept { return jpeg_ ? jpegReader_->Step() : png_->Step(); }

bool DrawingViewer::PictureReadWhole() const noexcept {
  return jpeg_ ? jpegStatus_ == draw::JpegStatus::kOk : pictureStatus_ == draw::PngStatus::kOk;
}

const char* DrawingViewer::PictureStatusName() const noexcept {
  return jpeg_ ? draw::JpegStatusName(jpegStatus_) : draw::PngStatusName(pictureStatus_);
}

bool DrawingViewer::StartPictureRead() noexcept {
  const core::DisplaySize size = display_.Size();
  // White, until the picture's dark pixels are cleared; and the picture
  // read through from its start.
  std::fill_n(panelBits_.get(), size.RowBytes() * size.height, std::uint8_t{0xFF});
  if (!pictureFile_->Seek(0)) {
    return false;
  }
  if (jpeg_) {
    jpegReader_->Begin(*pictureFile_, &timed_, draw::JpegReader::ReductionFor(jpegHeader_, shownWidth_, shownHeight_));
  } else {
    png_->Begin(*pictureFile_, &timed_);
  }
  const std::size_t widest = std::max<std::size_t>(size.width, size.height);
  if (scaler_->Begin(readWidth_, readHeight_, shownWidth_, shownHeight_, pictureRows_) != draw::ScaleStatus::kOk ||
      !ditherer_.Begin(shownWidth_, static_cast<std::uint32_t>(levels_),
                       {errors_.get(), draw::Ditherer::ErrorsFor(widest)})) {
    return false;
  }
  phase_ = Phase::kPicturing;
  phaseStartedUs_ = clock_.NowUs();
  return true;
}

bool DrawingViewer::FeedPicture(std::uint64_t until) noexcept {
  for (;;) {
    // A step of the reader, with the scaler's share of it taken out; then
    // the new rows it made ready, with the dithering's share taken out.
    const std::uint64_t started = clock_.NowUs();
    timedUs_ = 0;
    const bool more = StepPicture();
    const std::uint64_t stepped = clock_.NowUs();
    readingUs_ += (stepped - started) - timedUs_;
    scalingUs_ += timedUs_;
    const std::uint64_t dithered = ditheringUs_;
    while (scaler_->Step()) {
    }
    const std::uint64_t drained = clock_.NowUs();
    scalingUs_ += (drained - stepped) - (ditheringUs_ - dithered);
    if (!more) {
      if (jpeg_) {
        jpegStatus_ = jpegReader_->Status();
      } else {
        pictureStatus_ = png_->Status();
      }
      return true;
    }
    if (drained >= until) {
      return false;
    }
  }
}

void DrawingViewer::Timed::Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept {
  const std::uint64_t started = viewer_.clock_.NowUs();
  viewer_.scaler_->Row(y, gray);
  viewer_.timedUs_ += viewer_.clock_.NowUs() - started;
}

void DrawingViewer::TakeRow(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept {
  const std::uint64_t started = clock_.NowUs();
  // The ditherer works in place.
  const std::span<std::uint8_t> row(gray_.get(), gray.size());
  std::copy(gray.begin(), gray.end(), row.begin());
  if (ditherer_.Row(row)) {
    const std::size_t rowBytes = display_.Size().RowBytes();
    const core::PanelTurn turn = display_.ReadingTurn();
    // A clear bit is black, the leftmost pixel the top bit of the first byte.
    for (std::size_t x = 0; x < row.size(); ++x) {
      if (row[x] >= 128) {
        continue;
      }
      const PanelPixel at = OnPanel(turn, pageWidth_, pageHeight_, left_ + x, top_ + y);
      std::uint8_t& byte = panelBits_[(at.y * rowBytes) + (at.x / 8)];
      byte = static_cast<std::uint8_t>(byte & ~(0x80U >> (at.x % 8)));
    }
  }
  ditheringUs_ += clock_.NowUs() - started;
}

bool DrawingViewer::WritePicture() noexcept {
  const std::uint64_t started = clock_.NowUs();
  const core::DisplaySize size = display_.Size();
  const std::span<const std::uint8_t> bits(panelBits_.get(), size.RowBytes() * size.height);
  const bool written =
      display_.WriteRows(core::DisplayPlane::kNew, 0, bits) && display_.WriteRows(core::DisplayPlane::kOld, 0, bits);
  writingUs_ += clock_.NowUs() - started;
  // A picture not read whole is not kept: the card may have failed.
  if (caching_ && !PictureReadWhole()) {
    cache_.Close();
    caching_ = false;
    cacheNote_ = PictureStatusName();
  }
  for (std::size_t row = 0; caching_ && row < size.height; row += kBandRows) {
    const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - row);
    CacheRows(bits.subspan(row * size.RowBytes(), rows * size.RowBytes()), row, rows);
  }
  return written;
}

void DrawingViewer::SayPicture() noexcept {
  LogLine line;
  line.Text("picture ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
  if (fromCache_) {
    line.Text(", from the cache, shown ").Number(shownWidth_).Text("x").Number(shownHeight_).Text("; ");
    line.Number((clock_.NowUs() - drawingStartedUs_) / 1000).Text(" ms");
    stream_.SendText(line.View());
    return;
  }
  line.Text(", ").Text(PictureStatusName()).Text(", ");
  if (jpeg_) {
    line.Number(jpegHeader_.width).Text("x").Number(jpegHeader_.height).Text(" ").Text(draw::JpegKindName(jpegHeader_));
    const draw::JpegReduction reduction = jpegReader_->Reduction();
    if (reduction != draw::JpegReduction::kNone) {
      line.Text(" read at 1/").Number(static_cast<unsigned>(reduction));
    }
  } else {
    line.Number(header_.width).Text("x").Number(header_.height).Text(" ").Text(draw::PngColourName(header_.colour));
  }
  line.Text(" shown ").Number(shownWidth_).Text("x").Number(shownHeight_);
  line.Text(": reading ").Number(readingUs_ / 1000);
  line.Text(" ms, scaling ").Number(scalingUs_ / 1000).Text(" ms, dithering ").Number(ditheringUs_ / 1000);
  line.Text(" ms, writing ").Number(writingUs_ / 1000).Text(" ms; ");
  line.Number((clock_.NowUs() - drawingStartedUs_) / 1000).Text(" ms");
  stream_.SendText(line.View());
}

// ---- The cache ----

void DrawingViewer::KeyFor(std::size_t left, std::size_t top, std::size_t width, std::size_t height) noexcept {
  // As it lies on the panel: its corners, turned.
  const core::PanelTurn turn = display_.ReadingTurn();
  const PanelPixel one = OnPanel(turn, pageWidth_, pageHeight_, left, top);
  const PanelPixel other = OnPanel(turn, pageWidth_, pageHeight_, left + std::max<std::size_t>(width, 1) - 1,
                                   top + std::max<std::size_t>(height, 1) - 1);
  cacheX_ = std::min(one.x, other.x);
  cacheY_ = std::min(one.y, other.y);
  key_.width = static_cast<std::uint16_t>(width == 0 ? 0 : std::max(one.x, other.x) - cacheX_ + 1);
  key_.height = static_cast<std::uint16_t>(height == 0 ? 0 : std::max(one.y, other.y) - cacheY_ + 1);
}

bool DrawingViewer::FindInCache() noexcept {
  if (!known_) {
    cacheNote_ = "its file's time is not known";
    return false;
  }
  const std::uint64_t started = clock_.NowUs();
  const draw::PictureCacheStatus found = cache_.BeginRead(key_);
  cacheUs_ += clock_.NowUs() - started;
  if (found == draw::PictureCacheStatus::kOk) {
    fromCache_ = true;
    bandTop_ = 0;
    phase_ = Phase::kFromCache;
    phaseStartedUs_ = clock_.NowUs();
    return true;
  }
  // Not there, or not as it should be: it is drawn, and kept as it is.
  if (found != draw::PictureCacheStatus::kMissing && found != draw::PictureCacheStatus::kOther) {
    cacheNote_ = draw::PictureCacheStatusName(found);
  }
  const std::uint64_t writing = clock_.NowUs();
  const draw::PictureCacheStatus begun = cache_.BeginWrite(key_);
  cacheUs_ += clock_.NowUs() - writing;
  caching_ = begun == draw::PictureCacheStatus::kOk;
  if (!caching_) {
    cacheNote_ = draw::PictureCacheStatusName(begun);
  }
  return false;
}

bool DrawingViewer::FromCache(std::uint64_t until) noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t rowBytes = size.RowBytes();
  const std::size_t keptBytes = draw::PictureCache::RowBytesFor(key_);
  for (;;) {
    const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - bandTop_);
    const std::span<std::uint8_t> band(bits_.get(), rows * rowBytes);
    std::fill(band.begin(), band.end(), std::uint8_t{0xFF});
    // The rows of it in this band.
    const std::size_t first = std::max(bandTop_, cacheY_);
    const std::size_t last = std::min(bandTop_ + rows, cacheY_ + key_.height);
    if (first < last) {
      const std::span<std::uint8_t> kept(cacheRows_.get(), (last - first) * keptBytes);
      const std::uint64_t reading = clock_.NowUs();
      const draw::PictureCacheStatus read = cache_.ReadRows(kept);
      cacheUs_ += clock_.NowUs() - reading;
      if (read != draw::PictureCacheStatus::kOk) {
        // What was kept is not what was written: drawn instead, and kept
        // again.
        fromCache_ = false;
        cacheNote_ = draw::PictureCacheStatusName(read);
        const std::uint64_t writing = clock_.NowUs();
        caching_ = cache_.BeginWrite(key_) == draw::PictureCacheStatus::kOk;
        cacheUs_ += clock_.NowUs() - writing;
        writingUs_ = 0;
        cacheBytes_ = 0;
        passes_ = 0;
        if (!(picture_ ? StartPictureRead() : StartBands())) {
          Fail();
        }
        return false;
      }
      cacheBytes_ += kept.size();
      for (std::size_t row = first; row < last; ++row) {
        CopyPixels(kept.subspan((row - first) * keptBytes, keptBytes), 0,
                   band.subspan((row - bandTop_) * rowBytes, rowBytes), cacheX_, key_.width);
      }
    }
    const std::uint64_t writing = clock_.NowUs();
    const auto top = static_cast<std::uint16_t>(bandTop_);
    if (!display_.WriteRows(core::DisplayPlane::kNew, top, band) ||
        !display_.WriteRows(core::DisplayPlane::kOld, top, band)) {
      Fail();
      return false;
    }
    writingUs_ += clock_.NowUs() - writing;
    ++passes_;
    bandTop_ += kBandRows;
    if (bandTop_ >= size.height) {
      return true;
    }
    if (clock_.NowUs() >= until) {
      return false;
    }
  }
}

void DrawingViewer::CacheRows(std::span<const std::uint8_t> panelRows, std::size_t firstRow,
                              std::size_t rows) noexcept {
  if (!caching_) {
    return;
  }
  const std::size_t rowBytes = display_.Size().RowBytes();
  const std::size_t keptBytes = draw::PictureCache::RowBytesFor(key_);
  const std::size_t first = std::max(firstRow, cacheY_);
  const std::size_t last = std::min(firstRow + rows, cacheY_ + key_.height);
  if (first >= last) {
    return;
  }
  const std::uint64_t started = clock_.NowUs();
  const std::span<std::uint8_t> kept(cacheRows_.get(), (last - first) * keptBytes);
  std::fill(kept.begin(), kept.end(), std::uint8_t{0});
  for (std::size_t row = first; row < last; ++row) {
    CopyPixels(panelRows.subspan((row - firstRow) * rowBytes, rowBytes), cacheX_,
               kept.subspan((row - first) * keptBytes, keptBytes), 0, key_.width);
  }
  const draw::PictureCacheStatus written = cache_.WriteRows(kept);
  cacheUs_ += clock_.NowUs() - started;
  cacheBytes_ += kept.size();
  if (written != draw::PictureCacheStatus::kOk) {
    cache_.Close();
    caching_ = false;
    cacheNote_ = draw::PictureCacheStatusName(written);
  }
}

void DrawingViewer::FinishCaching() noexcept {
  if (!caching_) {
    return;
  }
  caching_ = false;
  // A drawing whose file could not all be read is not kept: the card may
  // have failed, and the next time it may not.
  if (!picture_ && fileFailed_) {
    cache_.Close();
    cacheNote_ = "the file could not all be read";
    return;
  }
  const std::uint64_t started = clock_.NowUs();
  const draw::PictureCacheStatus finished = cache_.FinishWrite();
  cacheUs_ += clock_.NowUs() - started;
  if (finished != draw::PictureCacheStatus::kOk) {
    cacheNote_ = draw::PictureCacheStatusName(finished);
    return;
  }
  cacheKept_ = true;
}

void DrawingViewer::SayCache() noexcept {
  LogLine line;
  line.Text(picture_ ? "picture " : "drawing ").Number(current_ + 1).Text(" cache: ");
  if (fromCache_) {
    line.Text("read ").Number(cacheBytes_ / 1024).Text(" KB in ").Number(cacheUs_ / 1000).Text(" ms");
  } else if (cacheKept_) {
    line.Text("kept, ").Number(cacheBytes_ / 1024).Text(" KB in ").Number(cacheUs_ / 1000).Text(" ms");
    if (cacheNote_ != nullptr) {
      line.Text(", after it was found ").Text(cacheNote_);
    }
  } else {
    line.Text("not kept: ").Text(cacheNote_ != nullptr ? cacheNote_ : "no reason given");
  }
  stream_.SendText(line.View());
}

void DrawingViewer::SayNotShown(std::string_view why) noexcept {
  LogLine line;
  line.Text("picture ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
  line.Text(", not shown: ").Text(why);
  stream_.SendText(line.View());
}

}  // namespace bn::reader
