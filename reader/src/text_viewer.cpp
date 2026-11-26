#include "reader/text_viewer.h"

#include <algorithm>
#include <new>

#include "core/utf8.h"
#include "hyphenation/american_english.h"
#include "log_line.h"

namespace bn::reader {

namespace {

constexpr std::uint16_t kRegular = 0;
constexpr std::uint16_t kItalicStyle = 1;
constexpr std::uint32_t kReplacement = 0xFFFD;  // What a byte that is not UTF-8 is drawn as.
// On a panel of two levels, a pixel is black with this much ink of three,
// or more.
constexpr std::uint32_t kBlackInk = 2;
// The levels of ink the gray pass makes gray: light, and dark.
constexpr std::uint32_t kLightInk = 1;
constexpr std::uint32_t kDarkInk = 2;

}  // namespace

void TextViewer::LineKeeper::OnLine(const layout::TextLine& line) noexcept {
  if (into != nullptr) {
    into->push_back(line);
  }
}

TextViewer::TextViewer(core::Display& display, core::Clock& clock, core::Storage& storage, core::TraceStream& stream,
                       core::ByteMap& fonts)
    : display_(display), clock_(clock), storage_(storage), stream_(stream), fontMap_(fonts) {}

TextViewer::~TextViewer() = default;

void TextViewer::Start() noexcept {
  if (state_ != State::kNotStarted) {
    return;
  }
  const draw::FontStatus status = fonts_.Open(fontMap_);
  if (status != draw::FontStatus::kOk) {
    LogLine line;
    line.Text("text: the fonts image is no good (").Text(draw::FontStatusName(status)).Text(").");
    stream_.SendText(line.View());
    Fail();
    return;
  }
  // The sizes: those the serif or the sans has in regular.
  for (std::size_t i = 0; i < fonts_.Faces(); ++i) {
    const draw::Face face = fonts_.FaceAt(i);
    if ((face.Role() == draw::FaceRole::kSerif || face.Role() == draw::FaceRole::kSans) && face.Style() == 0 &&
        std::find(sizes_.begin(), sizes_.end(), face.Size()) == sizes_.end()) {
      sizes_.push_back(face.Size());
    }
  }
  std::sort(sizes_.begin(), sizes_.end());
  if (sizes_.empty()) {
    stream_.SendText("text: the fonts image has no serif or sans in regular.");
    Fail();
    return;
  }
  // The middle size, which is the one text is first read in; a tour goes
  // from the smallest.
  sizeIndex_ = tour_ ? 0 : sizes_.size() / 2;
  if (!fonts_.Find(draw::FaceRole::kSerif, 0, sizes_[sizeIndex_]).Valid()) {
    role_ = draw::FaceRole::kSans;
  }
  if (hyphenator_.Open(hyphenation::AmericanEnglish().table) != layout::HyphenStatus::kOk) {
    stream_.SendText("text: the built-in hyphenation table would not open.");
  }

  if (!display_.Start()) {
    Fail();
    return;
  }
  grays_ = display_.Levels() >= 4;
  const core::DisplaySize size = display_.Size();
  const core::PanelTurn turn = display_.ReadingTurn();
  const bool quarter = turn == core::PanelTurn::kClockwise || turn == core::PanelTurn::kAnticlockwise;
  pageWidth_ = quarter ? size.height : size.width;
  pageHeight_ = quarter ? size.width : size.height;
  inkRowBytes_ = (static_cast<std::size_t>(size.width) + 3) / 4;
  placed_.reset(new (std::nothrow) Placed[kMostGlyphs]);
  ink_.reset(new (std::nothrow) std::uint8_t[inkRowBytes_ * kBandRows]);
  bits_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * kBandRows]);
  gray_.reset(new (std::nothrow) std::uint8_t[size.RowBytes() * kBandRows]);
  nodes_.reset(new (std::nothrow) layout::LineBreaker::Node[layout::LineBreaker::kUsualNodes]);
  records_.reset(new (std::nothrow) layout::ParagraphSetter::Record[layout::ParagraphSetter::kUsualRecords]);
  if (!placed_ || !ink_ || !bits_ || !gray_ || !nodes_ || !records_) {
    stream_.SendText("text: no memory to set it in");
    Fail();
    return;
  }
  setter_.reset(new (std::nothrow) layout::ParagraphSetter({nodes_.get(), layout::LineBreaker::kUsualNodes},
                                                           {records_.get(), layout::ParagraphSetter::kUsualRecords}));
  if (!setter_) {
    stream_.SendText("text: no memory to set it in");
    Fail();
    return;
  }
  setter_->UseHyphenator(hyphenator_.Status() == layout::HyphenStatus::kOk ? &hyphenator_ : nullptr);

  const core::StorageStatus opened = file_.Open(storage_, kTextPath, core::OpenMode::kRead);
  if (opened != core::StorageStatus::kOk) {
    LogLine line;
    line.Text("text: no text at ").Text(kTextPath).Text(" (").Text(core::StorageStatusName(opened)).Text(").");
    stream_.SendText(line.View());
  }
  state_ = State::kReady;
  UseFaces();
  pages_.assign(1, Place{});
  page_ = 0;
  pageWanted_ = true;
  phaseStartedUs_ = clock_.NowUs();
}

void TextViewer::UseFaces() noexcept {
  const std::uint16_t size = sizes_[sizeIndex_];
  // The glyphs of the page before are of the faces given up here.
  placedCount_ = 0;
  fonts_.ReleaseFaces();
  draw::Face regular = fonts_.Nearest(role_, 0, size);
  draw::Face italic = fonts_.Nearest(role_, draw::kItalic, size);
  draw::Face fallback = fonts_.Find(draw::FaceRole::kFallback, 0, size);
  std::size_t refused = 0;
  for (draw::Face* face : {&regular, &italic, &fallback}) {
    if (face->Valid() && !fonts_.Hold(*face)) {
      ++refused;
    }
  }
  measure_.SetFace(kRegular, regular);
  measure_.SetFace(kItalicStyle, italic);
  measure_.SetFallback(fallback);
  lineHeight_ = std::max<std::uint16_t>(regular.LineHeight(), 1);
  ascent_ = static_cast<std::uint16_t>(std::max<std::int16_t>(regular.Ascent(), 0));
  paragraphSet_ = false;

  LogLine line;
  line.Text("text: ")
      .Text(role_ == draw::FaceRole::kSerif ? "serif " : "sans ")
      .Number(size)
      .Text(" px: ")
      .Count(fonts_.Held(), " face", " faces")
      .Text(" held, ")
      .Number(fonts_.HeldBytes() / 1024)
      .Text(" KB of the image; ")
      .Number(fontMap_.MappedBytes() / 1024)
      .Text(" KB mapped");
  if (refused > 0) {
    line.Text("; no room for ").Count(refused, " face", " faces");
  }
  stream_.SendText(line.View());
}

void TextViewer::TourOn() noexcept {
  ++tourPages_;
  if (tourPages_ < kTourPages && !atEnd_) {
    asked_ = Asked::kNext;
    return;
  }
  tourPages_ = 0;
  const draw::FaceRole other = role_ == draw::FaceRole::kSerif ? draw::FaceRole::kSans : draw::FaceRole::kSerif;
  if (!tourOtherFace_ && fonts_.Find(other, 0, sizes_[sizeIndex_]).Valid()) {
    tourOtherFace_ = true;
    asked_ = Asked::kFace;
    return;
  }
  tourOtherFace_ = false;
  ++tourSizes_;
  if (tourSizes_ < sizes_.size()) {
    asked_ = Asked::kSize;
    return;
  }
  tourDone_ = true;
  stream_.SendText("text: the tour is done.");
}

void TextViewer::OnInput(const core::InputEvent& event) noexcept {
  if (state_ != State::kReady || sleepAsked_ || event.kind != core::InputEventKind::kPress) {
    return;
  }
  switch (event.button) {
    case core::Button::kDown:
    case core::Button::kRight:
      asked_ = Asked::kNext;
      break;
    case core::Button::kUp:
    case core::Button::kLeft:
      asked_ = Asked::kPrevious;
      break;
    case core::Button::kConfirm:
      asked_ = Asked::kSize;
      break;
    case core::Button::kBack:
      asked_ = Asked::kFace;
      break;
    default:
      break;
  }
}

bool TextViewer::TakeAsked() noexcept {
  const Asked asked = asked_;
  asked_ = Asked::kNothing;
  bool again = false;  // The pages are set again, from the one shown.
  switch (asked) {
    case Asked::kNothing:
      break;
    case Asked::kNext:
      // The last page of the text has none after it in the list.
      if (page_ + 1 < pages_.size()) {
        ++page_;
        pageWanted_ = true;
      }
      break;
    case Asked::kPrevious:
      if (page_ > 0) {
        --page_;
        pageWanted_ = true;
      }
      break;
    case Asked::kSize:
      if (sizes_.size() > 1) {
        sizeIndex_ = (sizeIndex_ + 1) % sizes_.size();
        // The face read in may not be had at every size.
        if (!fonts_.Find(role_, 0, sizes_[sizeIndex_]).Valid()) {
          role_ = role_ == draw::FaceRole::kSerif ? draw::FaceRole::kSans : draw::FaceRole::kSerif;
        }
        again = true;
      }
      break;
    case Asked::kFace: {
      const draw::FaceRole other = role_ == draw::FaceRole::kSerif ? draw::FaceRole::kSans : draw::FaceRole::kSerif;
      if (fonts_.Find(other, 0, sizes_[sizeIndex_]).Valid()) {
        role_ = other;
        again = true;
      }
      break;
    }
  }
  if (again) {
    UseFaces();
    const Place shown = pages_[page_];
    pages_.assign(1, shown);
    page_ = 0;
    pageWanted_ = true;
  }
  return pageWanted_;
}

bool TextViewer::WorkWaiting() const noexcept {
  if (state_ != State::kReady) {
    return false;
  }
  switch (phase_) {
    case Phase::kSetting:
    case Phase::kWriting:
    case Phase::kGraying:
      return true;
    case Phase::kIdle:
      return pageWanted_ || asked_ != Asked::kNothing;
    case Phase::kRefreshing:
    case Phase::kGrayRefreshing:
      return false;
  }
  return false;
}

bool TextViewer::PanelBusy(std::uint64_t now) noexcept {
  if (!display_.Busy()) {
    return false;
  }
  if (now - phaseStartedUs_ > kGiveUpUs) {
    Fail();
  }
  return true;
}

void TextViewer::Tick() noexcept {
  if (state_ != State::kReady) {
    return;
  }
  const std::uint64_t now = clock_.NowUs();
  switch (phase_) {
    case Phase::kIdle:
      if (PanelBusy(now)) {
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
      if (TakeAsked()) {
        BeginPage(now);
      }
      return;

    case Phase::kSetting: {
      bool set = false;
      do {
        set = SetStep();
      } while (!set && clock_.NowUs() < now + kSliceUs);
      const std::uint64_t after = clock_.NowUs();
      setUs_ += after - now;
      if (set) {
        PageSet(after);
      }
      return;
    }

    case Phase::kWriting: {
      // Something else asked for: this page is given up for it.
      if (asked_ != Asked::kNothing && TakeAsked()) {
        BeginPage(now);
        return;
      }
      const bool written = WriteBands(false, now + kSliceUs);
      const std::uint64_t after = clock_.NowUs();
      planesUs_ += after - now;
      if (written) {
        StartRefresh(after);
      }
      return;
    }

    case Phase::kRefreshing:
      if (PanelBusy(now)) {
        return;
      }
      refreshUs_ = now - phaseStartedUs_;
      if (grayNext_) {
        grayNext_ = false;
        // Something else asked for while the refresh went on is not kept
        // waiting for this page's grays.
        if (asked_ != Asked::kNothing || sleepAsked_) {
          WithoutGrays("something else was asked for", now);
          return;
        }
        bandTop_ = 0;
        phase_ = Phase::kGraying;
        phaseStartedUs_ = now;
        return;
      }
      Finished(now);
      return;

    case Phase::kGraying: {
      if (asked_ != Asked::kNothing || sleepAsked_) {
        WithoutGrays("something else was asked for", now);
        return;
      }
      const bool written = WriteBands(true, now + kSliceUs);
      const std::uint64_t after = clock_.NowUs();
      grayPlanesUs_ += after - now;
      if (!written) {
        return;
      }
      if (!display_.StartGrayRefresh(core::AfterRefresh::kPowerOff)) {
        WithoutGrays("the panel would not start the gray pass", after);
        return;
      }
      phase_ = Phase::kGrayRefreshing;
      phaseStartedUs_ = after;
      return;
    }

    case Phase::kGrayRefreshing:
      if (PanelBusy(now)) {
        return;
      }
      grayPassUs_ = now - phaseStartedUs_;
      grayShown_ = true;
      Finished(now);
      return;
  }
}

void TextViewer::Sleep() noexcept {
  if (state_ == State::kReady) {
    sleepAsked_ = true;
  }
}

// ---- Setting a page ----

void TextViewer::BeginPage(std::uint64_t now) noexcept {
  pageWanted_ = false;
  at_ = pages_[page_];
  top_ = kMargin;
  lineCount_ = 0;
  placedCount_ = 0;
  leftOut_ = 0;
  making_ = Making::kFinding;
  atEnd_ = false;
  graySeen_ = false;
  grayNote_ = nullptr;
  setUs_ = 0;
  planesUs_ = 0;
  refreshUs_ = 0;
  grayPlanesUs_ = 0;
  grayPassUs_ = 0;
  phase_ = Phase::kSetting;
  phaseStartedUs_ = now;
}

bool TextViewer::SetStep() noexcept {
  switch (making_) {
    case Making::kFinding:
      if (paragraphSet_ && paragraph_.offset == at_.offset) {
        making_ = Making::kPlacing;  // The one already set: a page that begins inside it.
        return false;
      }
      paragraph_ = Paragraph{};
      paragraph_.offset = at_.offset;
      paragraph_.next = at_.offset;
      paragraphSet_ = false;
      readStyle_ = kRegular;
      readLineHasText_ = false;
      readLineBegun_ = false;
      readCrWaiting_ = false;
      if (!file_.IsOpen() || !file_.Seek(at_.offset)) {
        paragraphSet_ = true;  // There is none.
        making_ = Making::kPlacing;
        return false;
      }
      making_ = Making::kReading;
      return false;

    case Making::kReading: {
      if (!ReadStep()) {
        return false;
      }
      layout::ParagraphSettings settings;
      settings.widths.rest = static_cast<std::uint16_t>(
          std::min<std::int32_t>(std::max<std::int32_t>((pageWidth_ - (2 * kMargin)) * 4, 4), 0xFFFF));
      settings.hyphenPenalty = kHyphenPenalty;
      lines_.into = &paragraph_.lines;
      if (!paragraph_.found || setter_->Begin(settings, measure_, lines_) != layout::ParagraphStatus::kOk) {
        lines_.into = nullptr;
        paragraphSet_ = true;
        making_ = Making::kPlacing;
        return false;
      }
      feedAt_ = 0;
      feedStyleAt_ = 0;
      feedBreakAt_ = 0;
      feedStyle_ = kRegular;
      making_ = Making::kFeeding;
      return false;
    }

    case Making::kFeeding:
      if (FeedStep()) {
        making_ = Making::kFinishing;
      }
      return false;

    case Making::kFinishing:
      if (setter_->Finish()) {
        lines_.into = nullptr;
        paragraphSet_ = true;
        making_ = Making::kPlacing;
      }
      return false;

    case Making::kPlacing:
      if (!paragraph_.found) {
        atEnd_ = true;
        return true;
      }
      if (at_.line < paragraph_.lines.size()) {
        // A page has one line at the least, however tall a line is.
        if (lineCount_ > 0 && top_ + lineHeight_ > pageHeight_ - kMargin) {
          return true;
        }
        PlaceLine(paragraph_.lines[at_.line], top_ + ascent_);
        top_ += lineHeight_;
        ++lineCount_;
        ++at_.line;
        return false;
      }
      // All of it is on the page: the next, a third of a line down.
      at_ = Place{.offset = paragraph_.next, .line = 0};
      top_ += lineHeight_ / 3;
      making_ = Making::kFinding;
      return false;
  }
  return true;
}

bool TextViewer::ReadStep() noexcept {
  for (std::size_t read = 0; read < kReadBytes; ++read) {
    std::uint8_t one = 0;
    if (file_.Read({&one, 1}) != 1) {
      paragraph_.next = file_.Position();
      return true;  // The end of the file.
    }
    const char byte = static_cast<char>(one);
    if (byte == '\r') {
      readCrWaiting_ = true;
      continue;
    }
    if (readCrWaiting_ && byte != '\n') {
      // A carriage return by itself is a character of the text.
      if (paragraph_.text.size() < kMostParagraphBytes) {
        paragraph_.text.push_back('\r');
      }
      readLineHasText_ = true;
    }
    readCrWaiting_ = false;
    if (byte == '\n') {
      if (!readLineHasText_) {
        if (paragraph_.found) {
          paragraph_.next = file_.Position();
          return true;  // An empty line ends the paragraph.
        }
        continue;  // Empty lines before it are passed over.
      }
      readLineHasText_ = false;
      readLineBegun_ = true;
      continue;
    }
    paragraph_.found = true;
    if (!readLineHasText_ && readLineBegun_) {
      // A line end inside the paragraph: a line the text ends.
      paragraph_.breaks.push_back(static_cast<std::uint32_t>(paragraph_.text.size()));
    }
    readLineHasText_ = true;
    if (byte == '*') {
      readStyle_ = readStyle_ == kRegular ? kItalicStyle : kRegular;
      paragraph_.styles.emplace_back(static_cast<std::uint32_t>(paragraph_.text.size()), readStyle_);
      continue;
    }
    if (paragraph_.text.size() < kMostParagraphBytes) {
      paragraph_.text.push_back(byte);
    }
  }
  return false;
}

bool TextViewer::FeedStep() noexcept {
  const std::string_view text = paragraph_.text;
  while (feedStyleAt_ < paragraph_.styles.size() && paragraph_.styles[feedStyleAt_].first <= feedAt_) {
    feedStyle_ = paragraph_.styles[feedStyleAt_].second;
    ++feedStyleAt_;
  }
  if (feedBreakAt_ < paragraph_.breaks.size() && paragraph_.breaks[feedBreakAt_] <= feedAt_) {
    if (setter_->EndLine()) {
      ++feedBreakAt_;
    }
    return false;
  }
  if (feedAt_ >= text.size()) {
    return true;
  }
  // As far as the style next changes, or the text next ends a line.
  std::size_t end = text.size();
  if (feedStyleAt_ < paragraph_.styles.size()) {
    end = std::min<std::size_t>(end, paragraph_.styles[feedStyleAt_].first);
  }
  if (feedBreakAt_ < paragraph_.breaks.size()) {
    end = std::min<std::size_t>(end, paragraph_.breaks[feedBreakAt_]);
  }
  feedAt_ += setter_->AddText(text.substr(feedAt_, end - feedAt_), feedStyle_);
  return false;
}

void TextViewer::PageSet(std::uint64_t now) noexcept {
  if (!atEnd_ && page_ + 1 >= pages_.size()) {
    pages_.push_back(at_);
  }
  bandTop_ = 0;
  phase_ = Phase::kWriting;
  phaseStartedUs_ = now;
}

void TextViewer::PlaceLine(const layout::TextLine& line, std::int32_t baseline) noexcept {
  // Everything here is in whole pixels, as the measure counted the line:
  // a glyph is drawn where the pen is, and the pen moves on by the glyph's
  // step, so two letters are as far apart here as anywhere else.
  const std::int32_t available = pageWidth_ - (2 * kMargin);
  const auto width = static_cast<std::int32_t>(line.width / layout::FaceMeasure::kUnitsToAPixel);
  // What is over, on a line to be spread, goes between its words: the same
  // to each place, and a pixel more to as many places as there are pixels
  // still left, taken evenly along the line.
  std::int32_t extra = 0;
  std::int32_t leftOver = 0;
  if (!line.last && !line.overfull && line.spaces > 0 && width < available) {
    extra = (available - width) / line.spaces;
    leftOver = (available - width) % line.spaces;
  }
  const std::string_view text = paragraph_.text;
  std::size_t nextStyle = 0;
  std::uint16_t style = kRegular;
  std::int32_t pen = kMargin;
  // Of the pixels left over, how much is owed so far, in parts of a pixel,
  // `line.spaces` to one. It begins at a half, so that one pixel over goes
  // to the middle of the line and not to an end of it.
  std::int32_t owed = line.spaces / 2;
  bool inSpace = false;
  for (std::size_t at = line.begin; at < line.end && at < text.size();) {
    while (nextStyle < paragraph_.styles.size() && paragraph_.styles[nextStyle].first <= at) {
      style = paragraph_.styles[nextStyle].second;
      ++nextStyle;
    }
    if (text[at] == ' ') {
      // A run of spaces is one place between words.
      if (!inSpace) {
        // The pixels left over are owed a share at each place, and a place
        // has one when what is owed has come to a whole pixel.
        std::int32_t more = 0;
        owed += leftOver;
        if (leftOver > 0 && owed >= line.spaces) {
          owed -= line.spaces;
          more = 1;
        }
        pen += (measure_.SpaceWidth(style) / layout::FaceMeasure::kUnitsToAPixel) + extra + more;
        inSpace = true;
      }
      ++at;
      continue;
    }
    inSpace = false;
    const core::Character character = core::CharacterAt(text, at);
    at += character.bytes;
    draw::Glyph glyph;
    if (measure_.GlyphFor(style, character.good ? character.codePoint : kReplacement, glyph)) {
      PlaceGlyph(glyph, pen, baseline);
      pen += layout::FaceMeasure::PenStep(glyph);
    }
  }
  draw::Glyph hyphen;
  if (line.hyphen && measure_.HyphenGlyph(style, hyphen)) {
    PlaceGlyph(hyphen, pen, baseline);
  }
}

void TextViewer::PlaceGlyph(const draw::Glyph& glyph, std::int32_t pen, std::int32_t baseline) noexcept {
  if (glyph.picture.empty()) {
    return;  // A space, or the like: nothing to draw.
  }
  const std::int32_t left = pen + glyph.left;
  const std::int32_t top = baseline - glyph.top;
  if (left >= pageWidth_ || top >= pageHeight_ || left + glyph.width <= 0 || top + glyph.height <= 0 ||
      left > INT16_MAX || top > INT16_MAX) {
    return;  // None of it is on the page.
  }
  if (placedCount_ == kMostGlyphs) {
    ++leftOut_;
    return;
  }
  placed_[placedCount_++] = Placed{.x = static_cast<std::int16_t>(left),
                                   .y = static_cast<std::int16_t>(top),
                                   .width = glyph.width,
                                   .height = glyph.height,
                                   .picture = glyph.pictureAt};
}

// ---- The panel's rows ----

std::size_t TextViewer::MakeBand(std::size_t top) noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t rows = std::min<std::size_t>(kBandRows, size.height - top);
  std::fill(ink_.get(), ink_.get() + (rows * inkRowBytes_), std::uint8_t{0});
  // The part of the page the band shows: all of it one way, and `rows` of
  // it the other, which way depending on how the panel is turned.
  const core::PanelTurn turn = display_.ReadingTurn();
  const auto first = static_cast<std::int32_t>(top);
  const auto count = static_cast<std::int32_t>(rows);
  std::int32_t x0 = 0;
  std::int32_t x1 = pageWidth_;
  std::int32_t y0 = 0;
  std::int32_t y1 = pageHeight_;
  switch (turn) {
    case core::PanelTurn::kNone:
      y0 = first;
      y1 = first + count;
      break;
    case core::PanelTurn::kHalf:
      y0 = pageHeight_ - first - count;
      y1 = pageHeight_ - first;
      break;
    case core::PanelTurn::kClockwise:
      x0 = pageWidth_ - first - count;
      x1 = pageWidth_ - first;
      break;
    case core::PanelTurn::kAnticlockwise:
      x0 = first;
      x1 = first + count;
      break;
  }
  for (std::size_t i = 0; i < placedCount_; ++i) {
    const Placed& placed = placed_[i];
    const std::int32_t left = std::max<std::int32_t>(placed.x, x0);
    const std::int32_t right = std::min<std::int32_t>(placed.x + placed.width, x1);
    const std::int32_t above = std::max<std::int32_t>(placed.y, y0);
    const std::int32_t below = std::min<std::int32_t>(placed.y + placed.height, y1);
    if (left >= right || above >= below) {
      continue;
    }
    draw::Glyph glyph;
    glyph.width = placed.width;
    glyph.height = placed.height;
    glyph.picture = fonts_.Bytes(placed.picture, draw::PictureBytes(placed.width, placed.height));
    if (glyph.picture.empty()) {
      continue;  // Its face is no longer held: nothing of it can be read.
    }
    for (std::int32_t y = above; y < below; ++y) {
      for (std::int32_t x = left; x < right; ++x) {
        const std::uint32_t ink =
            glyph.Ink(static_cast<std::uint32_t>(x - placed.x), static_cast<std::uint32_t>(y - placed.y));
        if (ink == 0) {
          continue;
        }
        // The pixel of the panel that shows this pixel of the page.
        std::int32_t panelX = x;
        std::int32_t panelY = y;
        switch (turn) {
          case core::PanelTurn::kNone:
            break;
          case core::PanelTurn::kHalf:
            panelX = pageWidth_ - 1 - x;
            panelY = pageHeight_ - 1 - y;
            break;
          case core::PanelTurn::kClockwise:
            panelX = y;
            panelY = pageWidth_ - 1 - x;
            break;
          case core::PanelTurn::kAnticlockwise:
            panelX = pageHeight_ - 1 - y;
            panelY = x;
            break;
        }
        // The darker, where glyphs meet.
        const std::size_t at =
            (static_cast<std::size_t>(panelY - first) * inkRowBytes_) + (static_cast<std::size_t>(panelX) / 4);
        const unsigned shift = 6U - ((static_cast<unsigned>(panelX) % 4) * 2);
        if (ink > ((std::uint32_t{ink_[at]} >> shift) & 3U)) {
          ink_[at] = static_cast<std::uint8_t>((ink_[at] & ~(3U << shift)) | (ink << shift));
        }
      }
    }
  }
  return rows;
}

bool TextViewer::WriteBands(bool grays, std::uint64_t until) noexcept {
  const core::DisplaySize size = display_.Size();
  const std::size_t rowBytes = size.RowBytes();
  // For black and white: black wherever there is ink, if grays are to
  // follow, and where there is more ink than not if they are not.
  const std::uint32_t blackInk = grays_ ? kLightInk : kBlackInk;
  do {
    if (bandTop_ >= size.height) {
      return true;
    }
    const std::size_t rows = MakeBand(bandTop_);
    const std::span<std::uint8_t> bits(bits_.get(), rows * rowBytes);
    const std::span<std::uint8_t> either(gray_.get(), rows * rowBytes);
    // A set bit is white, in black and white, and marks a gray among the
    // marks: in the new-image plane the dark grays, in the old every gray.
    std::fill(bits.begin(), bits.end(), grays ? std::uint8_t{0} : std::uint8_t{0xFF});
    std::fill(either.begin(), either.end(), std::uint8_t{0});
    for (std::size_t row = 0; row < rows; ++row) {
      const std::uint8_t* inkRow = ink_.get() + (row * inkRowBytes_);
      const std::size_t rowAt = row * rowBytes;
      for (std::size_t four = 0; four < inkRowBytes_; ++four) {
        if (inkRow[four] == 0) {
          continue;  // Four pixels of paper, as most are.
        }
        for (unsigned pixel = 0; pixel < 4; ++pixel) {
          const std::uint32_t ink = (std::uint32_t{inkRow[four]} >> (6U - (pixel * 2))) & 3U;
          const std::size_t x = (four * 4) + pixel;
          if (ink == 0 || x >= size.width) {
            continue;
          }
          const auto mask = static_cast<std::uint8_t>(0x80U >> (x % 8));
          const std::size_t at = rowAt + (x / 8);
          const bool gray = ink == kLightInk || ink == kDarkInk;
          if (!grays) {
            graySeen_ = graySeen_ || gray;
            if (ink >= blackInk) {
              bits[at] = static_cast<std::uint8_t>(std::uint32_t{bits[at]} & ~std::uint32_t{mask});
            }
          } else if (gray) {
            either[at] = static_cast<std::uint8_t>(either[at] | mask);
            if (ink == kDarkInk) {
              bits[at] = static_cast<std::uint8_t>(bits[at] | mask);
            }
          }
        }
      }
    }
    const auto first = static_cast<std::uint16_t>(bandTop_);
    // In black and white both planes are written, so that a half or full
    // refresh, which ignores the old-image plane, leaves both right.
    if (!display_.WriteRows(core::DisplayPlane::kNew, first, bits) ||
        !display_.WriteRows(core::DisplayPlane::kOld, first, grays ? either : bits)) {
      Fail();
      return false;
    }
    bandTop_ += rows;
  } while (clock_.NowUs() < until);
  return bandTop_ >= size.height;
}

void TextViewer::StartRefresh(std::uint64_t now) noexcept {
  const core::RefreshMode mode = firstRefresh_ ? core::RefreshMode::kFull : core::RefreshMode::kHalf;
  // The voltages are kept up for a gray pass to follow, which a page with
  // no gray in it has no need of.
  grayNext_ = grays_ && graySeen_;
  if (!display_.StartRefresh(mode, grayNext_ ? core::AfterRefresh::kStayPowered : core::AfterRefresh::kPowerOff)) {
    Fail();
    return;
  }
  firstRefresh_ = false;
  grayShown_ = false;
  phase_ = Phase::kRefreshing;
  phaseStartedUs_ = now;
}

void TextViewer::WithoutGrays(const char* why, std::uint64_t now) noexcept {
  grayNote_ = why;
  // The voltages were kept up for the gray pass.
  if (!display_.PowerOff()) {
    Fail();
    return;
  }
  Finished(now);
}

void TextViewer::Finished(std::uint64_t now) noexcept {
  ++shown_;
  LogLine line;
  line.Text("text: page ")
      .Number(page_ + 1)
      .Text(", ")
      .Text(role_ == draw::FaceRole::kSerif ? "serif " : "sans ")
      .Number(sizes_[sizeIndex_])
      .Text(" px, ")
      .Count(lineCount_, " line, ", " lines, ")
      .Count(placedCount_, " glyph", " glyphs")
      .Text(": set ")
      .Number(setUs_ / 1000)
      .Text(" ms, planes ")
      .Number(planesUs_ / 1000)
      .Text(" ms, refresh ")
      .Number(refreshUs_ / 1000)
      .Text(" ms");
  if (grayShown_) {
    line.Text(", gray planes ")
        .Number(grayPlanesUs_ / 1000)
        .Text(" ms, gray pass ")
        .Number(grayPassUs_ / 1000)
        .Text(" ms");
  } else if (grayNote_ != nullptr) {
    line.Text("; no grays: ").Text(grayNote_);
  }
  if (leftOut_ > 0) {
    line.Text("; ").Count(leftOut_, " glyph", " glyphs").Text(" left out");
  }
  if (atEnd_) {
    line.Text("; the end of the text");
  }
  stream_.SendText(line.View());
  phase_ = Phase::kIdle;
  phaseStartedUs_ = now;
  if (tour_ && !tourDone_) {
    TourOn();
  }
}

void TextViewer::Fail() noexcept {
  file_.Close();
  state_ = State::kFailed;
}

}  // namespace bn::reader
