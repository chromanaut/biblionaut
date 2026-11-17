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
bool IsPicture(std::string_view name) noexcept { return EndsWith(name, ".png"); }

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
  if (!renderer_ || !gray_ || !cells_ || !errors_ || !bits_ || !definitions_ || !chunk_) {
    stream_.SendText("drawings: no memory to draw them in");
    Fail();
    return;
  }
  renderer_->KeepDefinitionsIn({definitions_.get(), kDefinitionBytes});
  renderer_->SetThinnestStroke(kThinnestStroke);
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
      if (!info.isDirectory && (IsDrawing(found) || IsPicture(found))) {
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
  if ((phase_ == Phase::kSizing || phase_ == Phase::kDrawing || phase_ == Phase::kPicturing ||
       phase_ == Phase::kWriting) &&
      wanted_ != current_) {
    CloseFile();
    phase_ = Phase::kIdle;
  }
}

void DrawingViewer::Sleep() noexcept {
  sleepAsked_ = true;
  if (phase_ == Phase::kSizing || phase_ == Phase::kDrawing || phase_ == Phase::kPicturing ||
      phase_ == Phase::kWriting) {
    CloseFile();
    phase_ = Phase::kIdle;
  }
}

void DrawingViewer::Tick() noexcept {
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
      bandTop_ = 0;
      if (!ditherer_.Begin(display_.Size().width, static_cast<std::uint32_t>(levels_),
                           {errors_.get(), draw::Ditherer::ErrorsFor(display_.Size().width)})) {
        Fail();
        return;
      }
      phase_ = Phase::kDrawing;
      if (!StartPass(&filler_)) {
        Fail();
      }
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
        if (!WritePictureBand()) {
          Fail();
          return;
        }
        bandTop_ += kPictureBandRows;
        if (bandTop_ < display_.Size().height) {
          phase_ = Phase::kPicturing;
          if (!StartPicturePass()) {
            Fail();
          }
          return;
        }
        Refresh(now);
        return;
      }
      if (!WriteBand()) {
        Fail();
        return;
      }
      bandTop_ += kBandRows;
      if (bandTop_ < display_.Size().height) {
        phase_ = Phase::kDrawing;
        if (!StartPass(&filler_)) {
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

    case Phase::kRefreshing:
      if (display_.Busy()) {
        if (now - phaseStartedUs_ > kGiveUpUs) {
          Fail();
        }
        return;
      }
      ++shown_;
      drawingsShown.Add();
      phase_ = Phase::kIdle;
      phaseStartedUs_ = now;
      return;
  }
}

void DrawingViewer::Refresh(std::uint64_t now) noexcept {
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
  if (picture_) {
    if (!BeginPicture(index)) {
      CloseFile();
      redrawWanted_ = false;
      phase_ = Phase::kIdle;
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
    return;
  }
  phase_ = Phase::kSizing;
  phaseStartedUs_ = clock_.NowUs();
  // The size first: with nothing to draw into, the renderer reads only as
  // far as the drawing's first tag before this stops it.
  static_cast<void>(StartPass(nullptr));
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
      if (storage_.ReadAt(file_, offset_, {chunk_.get(), kReadBytes}, read) != core::StorageStatus::kOk || read == 0) {
        fileFailed_ = true;
        return true;
      }
      offset_ += static_cast<std::uint32_t>(read);
      chunkAt_ = 0;
      chunkSize_ = read;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes read, as the characters they are.
    const std::string_view bytes(reinterpret_cast<const char*>(chunk_.get()) + chunkAt_, chunkSize_ - chunkAt_);
    chunkAt_ += renderer_->Step(bytes);
  }
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
  const draw::Matrix middle = draw::Translation((pageWidth - drawWidth_) / 2, (pageHeight - drawHeight_) / 2);
  renderer_->PlaceOn(draw::Multiply(Unturned(turn, pageWidth, pageHeight), middle, clamped));
}

bool DrawingViewer::WriteBand() noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t width = size.width;
  const std::size_t rowBytes = size.RowBytes();
  const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - bandTop_);
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
  const std::span<const std::uint8_t> bits(bits_.get(), rows * rowBytes);
  const auto top = static_cast<std::uint16_t>(bandTop_);
  return display_.WriteRows(core::DisplayPlane::kNew, top, bits) &&
         display_.WriteRows(core::DisplayPlane::kOld, top, bits);
}

void DrawingViewer::Say() noexcept {
  const draw::SvgReport& report = renderer_->Report();
  LogLine line;
  line.Text("drawing ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
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

void DrawingViewer::Fail() noexcept {
  CloseFile();
  state_ = State::kFailed;
}

void DrawingViewer::CloseFile() noexcept {
  if (file_ != core::kNoFile) {
    storage_.Close(file_);
    file_ = core::kNoFile;
  }
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
  fileBuffer_.reset(new (std::nothrow) std::uint8_t[kFileBufferBytes]);
  window_.reset(new (std::nothrow) std::uint8_t[draw::PngReader::kWindowBytes]);
  pngRows_.reset(new (std::nothrow) std::uint8_t[kPictureRowBytes]);
  sums_.reset(new (std::nothrow) std::uint32_t[draw::Scaler::SumsFor(widest)]);
  scaledRow_.reset(new (std::nothrow) std::uint8_t[widest]);
  bandBits_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * kPictureBandRows]);
  if (fileBuffer_ && window_ && pngRows_ && sums_ && scaledRow_ && bandBits_) {
    pictureFile_.reset(new (std::nothrow) core::BlockFile({fileBuffer_.get(), kFileBufferBytes}));
    png_.reset(new (std::nothrow)
                   draw::PngReader({window_.get(), draw::PngReader::kWindowBytes}, {pngRows_.get(), kPictureRowBytes}));
    scaler_.reset(new (std::nothrow)
                      draw::Scaler({sums_.get(), draw::Scaler::SumsFor(widest)}, {scaledRow_.get(), widest}));
  }
  if (!pictureFile_ || !png_ || !scaler_) {
    // All of it, or none: what was had is given back.
    png_.reset();
    scaler_.reset();
    pictureFile_.reset();
    fileBuffer_.reset();
    window_.reset();
    pngRows_.reset();
    sums_.reset();
    scaledRow_.reset();
    bandBits_.reset();
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
  // The header alone, which says how large it is: a few steps.
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
  if (draw::PngReader::RowBytesFor(header_) > kPictureRowBytes) {
    LogLine why;
    why.Text("its rows need ").Number(draw::PngReader::RowBytesFor(header_)).Text(" bytes");
    SayNotShown(why.View());
    return false;
  }
  // As large as fits on the page the way the device is read, keeping its
  // shape, and in the middle of it.
  const core::DisplaySize size = display_.Size();
  const core::PanelTurn turn = display_.ReadingTurn();
  const bool quarter = turn == core::PanelTurn::kClockwise || turn == core::PanelTurn::kAnticlockwise;
  pageWidth_ = quarter ? size.height : size.width;
  pageHeight_ = quarter ? size.width : size.height;
  const std::uint64_t width = header_.width;
  const std::uint64_t height = header_.height;
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
  passes_ = (size.height + kPictureBandRows - 1) / kPictureBandRows;
  readingUs_ = 0;
  scalingUs_ = 0;
  ditheringUs_ = 0;
  writingUs_ = 0;
  bandTop_ = 0;
  if (!StartPicturePass()) {
    SayNotShown("it cannot be scaled");
    return false;
  }
  phase_ = Phase::kPicturing;
  phaseStartedUs_ = clock_.NowUs();
  return true;
}

bool DrawingViewer::StartPicturePass() noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t rows = std::min<std::size_t>(kPictureBandRows, size.height - bandTop_);
  // White, until the picture's dark pixels are set.
  const std::span<std::uint8_t> band(bandBits_.get(), rows * size.RowBytes());
  std::fill(band.begin(), band.end(), std::uint8_t{0xFF});
  if (!pictureFile_->Seek(0)) {
    return false;
  }
  png_->Begin(*pictureFile_, &timed_);
  const std::size_t widest = std::max<std::size_t>(size.width, size.height);
  return scaler_->Begin(header_.width, header_.height, shownWidth_, shownHeight_, pictureRows_) ==
             draw::ScaleStatus::kOk &&
         ditherer_.Begin(shownWidth_, static_cast<std::uint32_t>(levels_),
                         {errors_.get(), draw::Ditherer::ErrorsFor(widest)});
}

bool DrawingViewer::FeedPicture(std::uint64_t until) noexcept {
  for (;;) {
    // A step of the reader, with the scaler's share of it taken out; then
    // the new rows it made ready, with the dithering's share taken out.
    const std::uint64_t started = clock_.NowUs();
    timedUs_ = 0;
    const bool more = png_->Step();
    const std::uint64_t stepped = clock_.NowUs();
    readingUs_ += (stepped - started) - timedUs_;
    scalingUs_ += timedUs_;
    const std::uint64_t dithered = ditheringUs_;
    while (scaler_->Step()) {
    }
    const std::uint64_t drained = clock_.NowUs();
    scalingUs_ += (drained - stepped) - (ditheringUs_ - dithered);
    if (!more) {
      pictureStatus_ = png_->Status();
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
    const core::DisplaySize size = display_.Size();
    const std::size_t rowBytes = size.RowBytes();
    const std::size_t rows = std::min<std::size_t>(kPictureBandRows, size.height - bandTop_);
    const core::PanelTurn turn = display_.ReadingTurn();
    // A clear bit is black, the leftmost pixel the top bit of the first byte.
    for (std::size_t x = 0; x < row.size(); ++x) {
      if (row[x] >= 128) {
        continue;
      }
      const PanelPixel at = OnPanel(turn, pageWidth_, pageHeight_, left_ + x, top_ + y);
      if (at.y >= bandTop_ && at.y < bandTop_ + rows) {
        std::uint8_t& byte = bandBits_[((at.y - bandTop_) * rowBytes) + (at.x / 8)];
        byte = static_cast<std::uint8_t>(byte & ~(0x80U >> (at.x % 8)));
      }
    }
  }
  ditheringUs_ += clock_.NowUs() - started;
}

bool DrawingViewer::WritePictureBand() noexcept {
  const std::uint64_t started = clock_.NowUs();
  const core::DisplaySize size = display_.Size();
  const std::size_t rows = std::min<std::size_t>(kPictureBandRows, size.height - bandTop_);
  const std::span<const std::uint8_t> bits(bandBits_.get(), rows * size.RowBytes());
  const auto top = static_cast<std::uint16_t>(bandTop_);
  const bool written = display_.WriteRows(core::DisplayPlane::kNew, top, bits) &&
                       display_.WriteRows(core::DisplayPlane::kOld, top, bits);
  writingUs_ += clock_.NowUs() - started;
  return written;
}

void DrawingViewer::SayPicture() noexcept {
  LogLine line;
  line.Text("picture ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
  line.Text(", ").Text(draw::PngStatusName(pictureStatus_)).Text(", ");
  line.Number(header_.width).Text("x").Number(header_.height).Text(" ").Text(draw::PngColourName(header_.colour));
  line.Text(" shown ").Number(shownWidth_).Text("x").Number(shownHeight_).Text(", ");
  line.Count(passes_, " pass", " passes").Text(": reading ").Number(readingUs_ / 1000);
  line.Text(" ms, scaling ").Number(scalingUs_ / 1000).Text(" ms, dithering ").Number(ditheringUs_ / 1000);
  line.Text(" ms, writing ").Number(writingUs_ / 1000).Text(" ms; ");
  line.Number((clock_.NowUs() - drawingStartedUs_) / 1000).Text(" ms");
  stream_.SendText(line.View());
}

void DrawingViewer::SayNotShown(std::string_view why) noexcept {
  LogLine line;
  line.Text("picture ").Number(current_ + 1).Text(" of ").Number(names_.size()).Text(": ").Text(names_[current_]);
  line.Text(", not shown: ").Text(why);
  stream_.SendText(line.View());
}

}  // namespace bn::reader
