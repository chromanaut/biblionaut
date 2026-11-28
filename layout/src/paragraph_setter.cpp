#include "layout/paragraph_setter.h"

#include <algorithm>

#include "core/metrics.h"

namespace bn::layout {

namespace {

// Cuts made as a last resort, and lines ended where they stood for want of
// records.
core::Counter wordsCut{"words_cut"};
core::Counter linesCutShort{"lines_cut_short"};

// A record is where in the text its item begins, with the kind of item in
// the two bits above.
constexpr std::uint32_t kKindShift = 30;
constexpr std::uint32_t kPlaceMask = (1U << kKindShift) - 1U;
static_assert(ParagraphSetter::kMaxText <= kPlaceMask);

constexpr std::uint32_t kWidestItem = 0xFFFF;

constexpr std::uint8_t Byte(char ch) noexcept { return static_cast<std::uint8_t>(ch); }

// A byte that goes on with a character of UTF-8 and does not begin one.
constexpr bool GoesOn(char ch) noexcept { return (Byte(ch) & 0xC0U) == 0x80U; }

constexpr bool IsSpace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r'; }

// The third byte of U+2010, U+2012, U+2013 or U+2014, whose first two are
// E2 80: a hyphen or a dash that a line may end after.
constexpr bool IsDashByte(std::uint8_t third) noexcept {
  return third == 0x90 || third == 0x92 || third == 0x93 || third == 0x94;
}

// How many bytes the hyphen or dash that begins `text` has: 0 if it begins
// with neither.
std::size_t DashAt(std::string_view text) noexcept {
  if (text.empty()) {
    return 0;
  }
  if (text[0] == '-') {
    return 1;
  }
  if (text.size() >= 3 && Byte(text[0]) == 0xE2 && Byte(text[1]) == 0x80 && IsDashByte(Byte(text[2]))) {
    return 3;
  }
  return 0;
}

// Whether `text` begins with a word joiner (U+2060) or U+FEFF, which a book
// puts where no line is to end.
bool JoinerAt(std::string_view text) noexcept {
  if (text.size() < 3) {
    return false;
  }
  const std::uint8_t first = Byte(text[0]);
  const std::uint8_t second = Byte(text[1]);
  const std::uint8_t third = Byte(text[2]);
  return (first == 0xE2 && second == 0x81 && third == 0xA0) || (first == 0xEF && second == 0xBB && third == 0xBF);
}

// Whether `text` begins with a mark that is set over or under the letter
// before it (U+0300 to U+036F), which no cut should part from that letter.
bool MarkAt(std::string_view text) noexcept {
  if (text.size() < 2) {
    return false;
  }
  const std::uint8_t first = Byte(text[0]);
  const std::uint8_t second = Byte(text[1]);
  return (first == 0xCC && second >= 0x80 && second <= 0xBF) || (first == 0xCD && second >= 0x80 && second <= 0xAF);
}

// The bytes of `text` from `from` up to `end`, which are there.
std::string_view Part(std::string_view text, std::size_t from, std::size_t end) noexcept {
  return {text.data() + from, end - from};
}

}  // namespace

const char* ParagraphStatusName(ParagraphStatus status) noexcept {
  switch (status) {
    case ParagraphStatus::kOk:
      return "ok";
    case ParagraphStatus::kNotStarted:
      return "not started";
    case ParagraphStatus::kNoMemory:
      return "no memory";
    case ParagraphStatus::kBadWidths:
      return "bad widths";
  }
  return "unknown";
}

ParagraphSetter::ParagraphSetter(std::span<LineBreaker::Node> nodes, std::span<Record> records) noexcept
    : breaker_(nodes), records_(records) {}

ParagraphStatus ParagraphSetter::Begin(const ParagraphSettings& settings, TextMeasure& measure,
                                       TextLineSink& sink) noexcept {
  status_ = ParagraphStatus::kNoMemory;
  if (records_.size() < kMinRecords) {
    return status_;
  }
  const BreakStatus begun = breaker_.Begin(settings.widths, *this);
  if (begun != BreakStatus::kOk) {
    status_ = begun == BreakStatus::kNoMemory ? ParagraphStatus::kNoMemory : ParagraphStatus::kBadWidths;
    return status_;
  }
  measure_ = &measure;
  sink_ = &sink;
  hyphenPenalty_ = settings.hyphenPenalty;
  narrowest_ = settings.widths.rest;
  for (const std::uint16_t width : settings.widths.first) {
    narrowest_ = std::min<std::uint32_t>(narrowest_, width);
  }

  at_ = 0;
  textLeftOut_ = false;
  work_ = 0;
  mostWork_ = 0;
  linesCutShort_ = 0;
  lastResortCuts_ = 0;
  wordsAsked_ = 0;
  run_ = 0;
  boxEnd_ = 0;
  afterBox_ = false;
  lineStart_ = true;
  boxOnLine_ = false;
  room_ = Room::kSettle;
  spaceWaiting_ = false;
  wordSize_ = 0;
  wordWhole_ = false;
  wordCut_ = false;
  wordGoesOn_ = false;
  status_ = ParagraphStatus::kOk;
  return status_;
}

void ParagraphSetter::CallDone() noexcept { mostWork_ = std::max(mostWork_, work_); }

std::size_t ParagraphSetter::AddText(std::string_view text, std::uint16_t style) noexcept {
  if (status_ != ParagraphStatus::kOk) {
    return text.size();
  }
  work_ = 0;
  std::size_t taken = 0;
  if (!Step()) {
    taken = Gather(text, style);
  }
  CallDone();
  return taken;
}

bool ParagraphSetter::EndLine() noexcept {
  if (status_ != ParagraphStatus::kOk) {
    return true;
  }
  work_ = 0;
  if (wordSize_ != 0 && !wordWhole_) {
    WordIsWhole(false);
  }
  bool done = false;
  if (!Step() && Ready()) {
    GiveLineEnd(at_);
    wordGoesOn_ = false;
    done = true;
  }
  CallDone();
  return done;
}

bool ParagraphSetter::Finish() noexcept {
  if (status_ != ParagraphStatus::kOk) {
    return true;
  }
  work_ = 0;
  if (wordSize_ != 0 && !wordWhole_) {
    WordIsWhole(false);
  }
  bool done = false;
  if (!Step()) {
    // Spaces that nothing followed are on no line.
    (void)breaker_.Finish();
    status_ = ParagraphStatus::kNotStarted;
    done = true;
  }
  CallDone();
  return done;
}

// ---- Taking text ----

std::size_t ParagraphSetter::Gather(std::string_view text, std::uint16_t style) noexcept {
  std::size_t taken = 0;
  while (taken < text.size() && taken < kWordBufferBytes) {
    if (at_ >= kMaxText) {
      // No more is taken. A word in hand ends here.
      textLeftOut_ = true;
      if (wordSize_ != 0) {
        WordIsWhole(false);
        return taken;
      }
      return text.size();
    }
    const char ch = text[taken];
    ++work_;
    if (IsSpace(ch) || ch == '\n') {
      if (wordSize_ != 0) {
        // The word is over, and goes to the breaker before what ends it is
        // taken.
        WordIsWhole(false);
        break;
      }
      wordGoesOn_ = false;
      if (ch == '\n') {
        if (Ready()) {
          GiveLineEnd(at_);
          ++taken;
          ++at_;
        }
        break;
      }
      if (!lineStart_) {
        if (!spaceWaiting_) {
          spaceWaiting_ = true;
          spaceWidth_ = 0;
        }
        spaceWidth_ = std::min<std::uint32_t>(spaceWidth_ + measure_->SpaceWidth(style), kWidestItem);
      }
      ++taken;
      ++at_;
      continue;
    }

    // A byte that goes on with a character is in the style of the byte
    // before it. So a word changes style only where a character begins, and
    // one in too many styles is not cut inside a character.
    const std::uint16_t styleHere = GoesOn(ch) && (wordSize_ != 0 || wordGoesOn_) ? styleBefore_ : style;
    if (wordSize_ == 0) {
      wordAt_ = at_;
      styleCount_ = 1;
      styleFrom_[0] = 0;
      styles_[0] = styleHere;
    } else if (styles_[styleCount_ - 1] != styleHere) {
      if (styleCount_ == kMaxStyles) {
        WordIsWhole(true);
        break;
      }
      styleFrom_[styleCount_] = static_cast<std::uint8_t>(wordSize_);
      styles_[styleCount_] = styleHere;
      ++styleCount_;
    }
    if (wordSize_ >= kMaxWordBytes && (!GoesOn(ch) || wordSize_ == kWordBufferBytes)) {
      WordIsWhole(true);
      break;
    }
    word_[wordSize_] = ch;
    ++wordSize_;
    styleBefore_ = styleHere;
    ++taken;
    ++at_;
  }
  return taken;
}

void ParagraphSetter::WordIsWhole(bool cut) noexcept {
  wordWhole_ = true;
  wordCut_ = cut;
  wordAsked_ = false;
  // In what follows a cut, the word's first character came long ago.
  letterSeen_ = wordGoesOn_;
  breakCount_ = 0;
  breakNext_ = 0;
  pieceCount_ = 0;
  pieceNext_ = 0;
  piecesEnd_ = 0;
  pieceBoxed_ = false;
  breakBeforeRest_ = false;
  byCharacters_ = false;
}

// ---- The word in hand ----

bool ParagraphSetter::Step() noexcept {
  if (wordSize_ == 0 || !wordWhole_) {
    return false;
  }
  if (!wordAsked_) {
    AskHyphenator();
    PreparePieces();
    return true;
  }
  if (pieceNext_ < pieceCount_) {
    FeedWord();
    return true;
  }
  if (piecesEnd_ < wordSize_) {
    PreparePieces();
    return true;
  }
  // All of it has gone to the breaker.
  wordGoesOn_ = wordCut_;
  wordSize_ = 0;
  wordWhole_ = false;
  wordCut_ = false;
  return false;
}

void ParagraphSetter::AskHyphenator() noexcept {
  wordAsked_ = true;
  // Not about a piece of something longer than a word.
  if (hyphenator_ == nullptr || wordCut_ || wordGoesOn_) {
    return;
  }
  // Nor about a word whose breaks the book has given.
  const std::string_view word(word_.data(), wordSize_);
  work_ += static_cast<std::uint32_t>(wordSize_);
  for (std::size_t i = 0; i + 1 < wordSize_; ++i) {
    if (Byte(word[i]) == 0xC2 && Byte(word[i + 1]) == 0xAD) {
      return;
    }
  }
  breakCount_ = hyphenator_->Find(word, breaks_);
  ++wordsAsked_;
}

std::uint16_t ParagraphSetter::StyleAt(std::size_t at) const noexcept {
  std::size_t which = 0;
  while (which + 1 < styleCount_ && styleFrom_[which + 1] <= at) {
    ++which;
  }
  return styles_[which];
}

void ParagraphSetter::NextCut(std::size_t from, std::size_t& end, std::size_t& next, After& after) noexcept {
  const std::string_view word(word_.data(), wordSize_);
  // The nearest break the hyphenator gave, and the nearest change of style:
  // the piece goes no further than either.
  while (breakNext_ < breakCount_ && breaks_[breakNext_] <= from) {
    ++breakNext_;
  }
  std::size_t stop = wordSize_;
  bool stopIsBreak = false;
  if (breakNext_ < breakCount_ && breaks_[breakNext_] < stop) {
    stop = breaks_[breakNext_];
    stopIsBreak = true;
  }
  for (std::size_t style = 1; style < styleCount_; ++style) {
    if (styleFrom_[style] > from) {
      if (styleFrom_[style] < stop) {
        stop = styleFrom_[style];
        stopIsBreak = false;
      }
      break;
    }
  }

  // Before that, a mark in the text itself may end the piece.
  std::size_t i = from;
  while (i < stop) {
    ++work_;
    const std::uint8_t byte = Byte(word[i]);
    if (byte == 0xC2 && i + 1 < wordSize_ && Byte(word[i + 1]) == 0xAD) {
      // A soft hyphen: the piece ends before it and the next begins after.
      end = i;
      next = i + 2;
      after = After::kHyphen;
      return;
    }
    if (byte == 0xE2 && i + 2 < wordSize_ && Byte(word[i + 1]) == 0x80 && Byte(word[i + 2]) == 0x8B) {
      // A zero width space, likewise.
      end = i;
      next = i + 3;
      after = After::kFree;
      return;
    }
    const std::size_t dash = DashAt(Part(word, i, wordSize_));
    if (dash == 0) {
      letterSeen_ = true;
      ++i;
      continue;
    }
    const std::size_t past = i + dash;
    if (past > stop) {
      break;
    }
    // A line may end after it, if the word has begun, something follows
    // that is neither another nor a joiner, and it is not a minus before a
    // figure.
    const bool minusBeforeFigure = dash == 1 && past < wordSize_ && word[past] >= '0' && word[past] <= '9';
    const std::string_view rest = Part(word, past, wordSize_);
    const bool follows = past < wordSize_ && DashAt(rest) == 0 && !JoinerAt(rest);
    if (letterSeen_ && follows && !minusBeforeFigure) {
      end = past;
      next = past;
      after = After::kFree;
      return;
    }
    i = past;
  }
  end = stop;
  next = stop;
  after = stopIsBreak ? After::kHyphen : After::kNothing;
}

void ParagraphSetter::PreparePieces() noexcept {
  pieceCount_ = 0;
  pieceNext_ = 0;
  pieceBoxed_ = false;
  breakBeforeRest_ = false;
  byCharacters_ = false;
  std::size_t from = piecesEnd_;
  while (from < wordSize_ && pieceCount_ < kMaxPieces) {
    std::size_t end = from;
    std::size_t next = from;
    After after = After::kNothing;
    NextCut(from, end, next, after);
    if (end > from) {
      Piece& piece = pieces_[pieceCount_];
      piece.begin = static_cast<std::uint8_t>(from);
      piece.end = static_cast<std::uint8_t>(end);
      piece.after = after;
      piece.width = 0;
      piece.hyphen = 0;
      ++pieceCount_;
    } else if (pieceCount_ != 0 && pieces_[pieceCount_ - 1].after == After::kNothing) {
      // A mark straight after a change of style: it follows the piece
      // before.
      pieces_[pieceCount_ - 1].after = after;
    }
    // A cut always moves on, but nothing here depends on that.
    from = std::max(next, from + 1);
  }
  piecesEnd_ = std::min(from, wordSize_);

  // Each stretch of pieces that lie end to end in one style is measured as
  // one.
  for (std::size_t first = 0; first < pieceCount_;) {
    const std::uint16_t style = StyleAt(pieces_[first].begin);
    std::size_t end = first + 1;
    while (end < pieceCount_ && pieces_[end].begin == pieces_[end - 1].end && StyleAt(pieces_[end].begin) == style) {
      ++end;
    }
    MeasurePieces(first, end);
    first = end;
  }
}

void ParagraphSetter::MeasurePieces(std::size_t first, std::size_t end) noexcept {
  std::array<std::uint8_t, kMaxPieces> ends{};
  std::array<std::uint16_t, kMaxPieces> widths{};
  std::array<std::uint16_t, kMaxPieces> hyphens{};
  const std::size_t count = end - first;
  const std::size_t base = pieces_[first].begin;
  for (std::size_t i = 0; i < count; ++i) {
    ends[i] = static_cast<std::uint8_t>(pieces_[first + i].end - base);
  }
  const std::string_view text = Part({word_.data(), wordSize_}, base, pieces_[end - 1].end);
  work_ += static_cast<std::uint32_t>(text.size());
  measure_->Measure(text, StyleAt(base), std::span<const std::uint8_t>(ends).first(count),
                    std::span<std::uint16_t>(widths).first(count), std::span<std::uint16_t>(hyphens).first(count));
  for (std::size_t i = 0; i < count; ++i) {
    pieces_[first + i].width = widths[i];
    pieces_[first + i].hyphen = hyphens[i];
  }
}

void ParagraphSetter::FeedWord() noexcept {
  // One item goes to the breaker, or the records are seen to. A piece that
  // nothing follows takes no item, and the next is looked at.
  while (pieceNext_ < pieceCount_) {
    Piece& piece = pieces_[pieceNext_];
    const std::uint32_t begins = wordAt_ + piece.begin;

    if (breakBeforeRest_) {
      // The piece is going a character at a time. A line may end before
      // the next of them.
      if (Ready()) {
        GiveBreak(After::kLastResort, 0, begins);
        breakBeforeRest_ = false;
      }
      return;
    }

    if (pieceBoxed_) {
      // The piece has gone. What follows it?
      if (piece.after != After::kNothing) {
        if (!Ready()) {
          return;
        }
        GiveBreak(piece.after, piece.hyphen, wordAt_ + piece.end);
      }
      const bool gave = piece.after != After::kNothing;
      pieceBoxed_ = false;
      ++pieceNext_;
      if (gave) {
        return;
      }
      continue;
    }

    if (!Ready()) {
      return;
    }
    if (spaceWaiting_) {
      GiveSpace();
      return;
    }
    // A stretch with nowhere to end a line is kept no wider than the
    // narrowest line.
    const std::uint32_t room = narrowest_ > run_ ? narrowest_ - run_ : 0;
    const bool whole = !byCharacters_ && !wordCut_ && !wordGoesOn_;
    if (whole && piece.width <= room) {
      GiveBox(piece.width, begins, wordAt_ + piece.end);
      pieceBoxed_ = true;
      return;
    }
    if (whole && piece.width <= narrowest_) {
      // It would fit a line of its own, and does not fit after what has
      // come since the latest place a line could end.
      GiveBreak(After::kLastResort, 0, begins);
      return;
    }
    // It is wider than a line, or part of a run of bytes too long to be a
    // word. It goes a character at a time, and a line may end after any of
    // them.
    byCharacters_ = true;
    FeedCharacterOf(piece, room);
    return;
  }
}

void ParagraphSetter::FeedCharacterOf(Piece& piece, std::uint32_t room) noexcept {
  const std::string_view word(word_.data(), wordSize_);
  // The piece's first character: the bytes that go on with it are part of
  // it, and so is a mark set over it.
  std::size_t to = piece.begin + 1U;
  while (to < piece.end) {
    if (GoesOn(word[to])) {
      ++to;
    } else if (MarkAt(Part(word, to, piece.end))) {
      to += 2;
    } else {
      break;
    }
  }
  const std::array<std::uint8_t, 1> ends = {static_cast<std::uint8_t>(to - piece.begin)};
  std::array<std::uint16_t, 1> widths{};
  std::array<std::uint16_t, 1> hyphens{};  // No hyphen is shown between characters.
  work_ += static_cast<std::uint32_t>(2 * (to - piece.begin));
  measure_->Measure(Part(word, piece.begin, to), StyleAt(piece.begin), ends, widths, hyphens);

  const std::uint32_t begins = wordAt_ + piece.begin;
  if (run_ != 0 && widths[0] > room) {
    // Not after what has come since the latest place a line could end.
    GiveBreak(After::kLastResort, 0, begins);
    return;
  }
  // By itself it goes on a line however wide it is.
  GiveBox(widths[0], begins, wordAt_ + static_cast<std::uint32_t>(to));
  if (to == piece.end) {
    pieceBoxed_ = true;
    byCharacters_ = false;
    return;
  }
  piece.begin = static_cast<std::uint8_t>(to);
  breakBeforeRest_ = true;
}

// ---- What the breaker is given ----

bool ParagraphSetter::Ready() noexcept {
  const std::uint32_t held = breaker_.Items() - breaker_.Pending();
  if (held + 1U < records_.size()) {
    room_ = Room::kSettle;
    return true;
  }
  // Every record but one is in use, and that one is kept for a line end.
  switch (room_) {
    case Room::kSettle: {
      const std::uint32_t pending = breaker_.Pending();
      breaker_.Settle(breaker_.Items());
      if (breaker_.Pending() == pending) {
        // Everything held would go on one line: it has to end here.
        room_ = Room::kEndLine;
      }
      break;
    }
    case Room::kEndLine:
      Remember(Kind::kOther, boxEnd_);
      breaker_.AddForcedBreak();
      ++linesCutShort_;
      linesCutShort.Add();
      spaceWaiting_ = false;
      run_ = 0;
      afterBox_ = false;
      lineStart_ = true;
      boxOnLine_ = false;
      room_ = Room::kGiveOut;
      break;
    case Room::kGiveOut:
      breaker_.Settle(breaker_.Items());
      room_ = Room::kSettle;
      break;
  }
  return false;
}

void ParagraphSetter::Remember(Kind kind, std::uint32_t at) noexcept {
  records_[breaker_.Items() % records_.size()].value =
      (std::min(at, kPlaceMask)) | (static_cast<std::uint32_t>(kind) << kKindShift);
}

std::uint32_t ParagraphSetter::PlaceOf(std::uint32_t item) const noexcept {
  return records_[item % records_.size()].value & kPlaceMask;
}

ParagraphSetter::Kind ParagraphSetter::KindOf(std::uint32_t item) const noexcept {
  return static_cast<Kind>(records_[item % records_.size()].value >> kKindShift);
}

void ParagraphSetter::GiveBox(std::uint16_t width, std::uint32_t at, std::uint32_t endAt) noexcept {
  Remember(Kind::kBox, at);
  breaker_.AddBox(width);
  run_ += width;
  boxEnd_ = endAt;
  afterBox_ = true;
  lineStart_ = false;
  boxOnLine_ = true;
}

void ParagraphSetter::GiveSpace() noexcept {
  // A line that ends at it ends with the word before it.
  Remember(Kind::kSpace, boxEnd_);
  breaker_.AddSpace(static_cast<std::uint16_t>(spaceWidth_));
  spaceWaiting_ = false;
  run_ = 0;
  afterBox_ = false;
}

void ParagraphSetter::GiveBreak(After after, std::uint16_t hyphen, std::uint32_t at) noexcept {
  // A place to end a line that no box comes before is none.
  if (!afterBox_) {
    return;
  }
  if (after == After::kHyphen && run_ + hyphen > narrowest_) {
    // With the hyphen after it, what has come since the latest place is
    // too wide for the narrowest line. A line may end here without one, if
    // it must.
    after = After::kLastResort;
  }
  switch (after) {
    case After::kNothing:
      return;
    case After::kHyphen:
      Remember(Kind::kHyphen, at);
      breaker_.AddBreak(hyphen, hyphenPenalty_);
      break;
    case After::kFree:
      Remember(Kind::kOther, at);
      breaker_.AddBreak(0, 0);
      break;
    case After::kLastResort:
      Remember(Kind::kOther, at);
      breaker_.AddBreak(0, kLastResortPenalty);
      ++lastResortCuts_;
      wordsCut.Add();
      break;
  }
  // A line that ends here holds no more than the narrowest line does.
  run_ = 0;
  afterBox_ = false;
}

void ParagraphSetter::GiveLineEnd(std::uint32_t at) noexcept {
  // The line ends with its last word: spaces before the line end are not on
  // it. An empty line is where the line end itself is.
  Remember(Kind::kOther, boxOnLine_ ? boxEnd_ : at);
  breaker_.AddForcedBreak();
  spaceWaiting_ = false;
  run_ = 0;
  afterBox_ = false;
  lineStart_ = true;
  boxOnLine_ = false;
}

// ---- The lines that come back ----

void ParagraphSetter::OnLine(const Line& line) noexcept {
  TextLine text;
  const bool atEnd = line.endItem >= breaker_.Items();
  text.begin = PlaceOf(line.firstItem);
  // A line that ends with the paragraph ends with its last word.
  text.end = atEnd ? boxEnd_ : PlaceOf(line.endItem);
  text.width = line.width;
  text.number = line.number;
  text.hyphen = !atEnd && KindOf(line.endItem) == Kind::kHyphen;
  text.last = line.last;
  text.overfull = line.overfull;
  std::uint32_t spaces = 0;
  for (std::uint32_t item = line.firstItem; item < line.endItem; ++item) {
    spaces += KindOf(item) == Kind::kSpace ? 1U : 0U;
  }
  work_ += line.endItem - line.firstItem;
  text.spaces = static_cast<std::uint16_t>(std::min(spaces, kWidestItem));
  sink_->OnLine(text);
}

}  // namespace bn::layout
