#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "layout/hyphenator.h"
#include "layout/line_breaker.h"

// Setting a paragraph's text in lines.
//
// The line breaker chooses where lines end, and knows a paragraph only as
// widths. This stands between it and the text. It takes a paragraph's text
// as it comes, finds the words, asks where each may be broken and how wide
// its pieces are, gives the breaker all of that, and says of each line the
// breaker gives back which of the paragraph's text is on it.
//
// The text is UTF-8, given in runs, each in a style: a number that means
// something to whoever measures text and nothing here. A word may change
// style part way through. A character is in one style, the one its first
// byte came in: where a run ends part way through a character, the bytes
// that go on with it are taken in that style, whatever the next run's is.
// Places in the text are counted in bytes from the start of the paragraph,
// over all its runs.
//
// How wide text is, is asked of a TextMeasure, in whatever unit the lines'
// widths are given in. Nothing here knows of fonts or pixels.
//
// What is made of the text:
//
//   - A space or a tab is a place where a line may end. Several in a row are
//     one space as wide as all of them. Those at the start of the paragraph,
//     and those after a line end the text asks for, are left out.
//   - A line feed is a line end the text asks for. A carriage return is a
//     space.
//   - Everything else is words: any other character, a no-break space among
//     them, is part of the word it is in.
//   - A word may be broken where the hyphenator says, at a price, with a
//     hyphen shown at the end of the line.
//   - A soft hyphen (U+00AD) is such a place as well, put there by whoever
//     made the book. It is never shown otherwise: it is on no line, and is
//     not measured. A word that has one is broken at those and not where
//     the hyphenator would have broken it.
//   - A line may end after a hyphen or a dash written in a word (U+002D,
//     U+2010, U+2012, U+2013, U+2014) at no price, with nothing more shown:
//     but not before the word's first character that is neither, not
//     between two of them, not between a hyphen-minus and a figure, and not
//     where a word joiner (U+2060) or U+FEFF comes straight after it, which
//     a book puts where no line is to end: on either side of the dash
//     between two years, say. A joiner keeps a line from ending nowhere
//     else: one may still end at a space beside it.
//   - A line may end at a zero width space (U+200B) the same way, which is
//     on no line either.
//   - A stretch with nowhere to end a line that is wider than the
//     paragraph's narrowest line may be ended between any two of its
//     characters, as a last resort: the lines it runs over are then filled
//     as lines of words are. The same is done where a word may be broken
//     and the narrowest line has no room for the hyphen: a line may end
//     there without one. So no line is too wide unless one character is
//     wider than a line.
//
// Bytes that are not UTF-8 are part of their word, and are given to the
// measure as they are.
//
// What it does not do: put anything in a line but text (a picture among
// the words); keep the spaces at the start of a line, as text set just as
// it is written wants; break lines by the rules of scripts that are not
// written with spaces between words; or spread a line out to its full
// width, though it says what whoever does that needs.
//
// Its memory is the caller's: the breaker's nodes, and a record of 4 bytes
// for each of the paragraph's items that is not yet on a line given out.
// When the records run out, lines are given out early, as the breaker's
// Settle() gives them. If that frees none, because there are more items on
// one line than there are records, the line is ended where it stands, as
// one the text asked to end would be.
//
// No call does much, whatever the text. Each call looks at no more than a
// word's worth of bytes, asks the hyphenator about one word at the most,
// has the measure go over no more than a word's worth of bytes, and gives
// the breaker one item at the most, which may make the breaker give out
// lines: MostWorkFor(). So a paragraph takes many calls, and AddText() need
// not take all it is given.

namespace bn::layout {

// How wide text is.
class TextMeasure {
 public:
  // Measures a stretch of text in one style: a word, or as much of one as is
  // in that style. `ends` is where the stretch is cut into pieces: how many
  // bytes of it come before the end of each piece, in order, the last being
  // all of it. `widths`, which is as long as `ends`, is to be given the
  // width of each piece, and `hyphens`, as long again, what a hyphen shown
  // after each comes to.
  //
  // The widths of pieces that are put on one line are added up. A stretch
  // need not be as wide in two parts on two lines as it is whole: a measure
  // may move two letters together, or draw two as one glyph, and can do
  // neither with a line's end between them. So the widths are to be those
  // of the text on from a cut: the pieces from any one to the last add up to
  // how wide the stretch is from that piece on, set by itself, which from
  // the first piece is all of it. And a piece's number in `hyphens` is what
  // a line that ends after the piece with a hyphen shown is wider by than
  // the widths of the pieces on it: the hyphen, and whatever the text before
  // the cut gains or loses by being set with a hyphen after it and not with
  // the text that follows. Every cut comes before the first byte of a
  // character, if the text is UTF-8.
  virtual void Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
                       std::span<std::uint16_t> widths, std::span<std::uint16_t> hyphens) noexcept = 0;
  // How wide a space between two words is.
  virtual std::uint16_t SpaceWidth(std::uint16_t style) noexcept = 0;

 protected:
  ~TextMeasure() = default;  // Never destroyed through the interface.
};

// One line of a paragraph, as text.
struct TextLine {
  // The text on the line: from `begin` bytes into the paragraph up to, and
  // not including, `end`. It begins with the first character of its first
  // word and ends with the last of its last, so whatever the line ended at
  // (a space, a line feed, a soft hyphen) is not on it. A soft hyphen or a
  // zero width space further in is among its bytes, and is not to be shown.
  // An empty line, which two line feeds together make, has the two the
  // same.
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  // How wide what is on the line is, as the measure has it: its words, the
  // spaces between them, and the hyphen if one is to be shown, as wide as
  // the measure said a hyphen there is.
  std::uint32_t width = 0;
  // Which line of the paragraph: 0 for the first.
  std::uint32_t number = 0;
  // How many places between words the line has: where it can be stretched
  // to make it as wide as the others. A run of spaces is one.
  std::uint16_t spaces = 0;
  // A hyphen is to be shown after the last character: the line ends where
  // the hyphenator, or a soft hyphen, let the word be broken.
  bool hyphen = false;
  // It ends with the paragraph or where the text asked for a line to end.
  // Such a line is not spread out to the full width when the others are.
  bool last = false;
  // It is wider than the line allows.
  bool overfull = false;
};

// What is told of a paragraph's lines as they are given out.
class TextLineSink {
 public:
  // Called for each line, in order. It must not call the setter.
  virtual void OnLine(const TextLine& line) noexcept = 0;

 protected:
  ~TextLineSink() = default;  // Never destroyed through the interface.
};

// How setting a paragraph has gone.
enum class ParagraphStatus : std::uint8_t {
  kOk = 0,
  kNotStarted,  // Begin() has not been called, or the paragraph is finished.
  kNoMemory,    // Fewer nodes than the breaker works with, or fewer records than kMinRecords.
  kBadWidths,   // The breaker would not have the lines' widths.
};

// The name of a status, for diagnostics: "ok", "not started", ...
const char* ParagraphStatusName(ParagraphStatus status) noexcept;

// What a paragraph is set by.
struct ParagraphSettings {
  // How wide its lines may be.
  LineWidths widths;
  // What it costs to end a line inside a word with a hyphen. A line costs
  // the square of the space left on it, so this is the square of the gap at
  // the end of a line that is as bad as a hyphen: with 0 a word is broken
  // wherever that leaves the lines more even, and the larger it is the
  // wider a gap is left sooner than break one.
  std::uint32_t hyphenPenalty = 0;
};

// Sets one paragraph after another.
//
// Used by one task.
class ParagraphSetter final : private LineSink {
 public:
  // What it remembers of an item until the line it is on is given out. The
  // setter's own business: the caller only provides an array of them.
  struct Record {
    std::uint32_t value = 0;
  };

  // The fewest records it will work with, and a sensible number to go with
  // LineBreaker::kUsualNodes: a place where a line could end is two items
  // or more, the place and what is between it and the one before.
  static constexpr std::size_t kMinRecords = 64;
  static constexpr std::size_t kUsualRecords = 768;
  // The most bytes of a word that are taken as one: a longer run of bytes
  // with no space in it is taken in parts of this many. The hyphenator is
  // asked about none of them, and a line may end between any two of its
  // characters as a last resort. (A character that is under way at this
  // many bytes is finished first.) It is as much as the hyphenator's longest
  // word can come to.
  static constexpr std::size_t kMaxWordBytes = 4 * Hyphenator::kMaxLetters;
  // The most styles one word may be in. One in more is taken in parts, as a
  // word too long is.
  static constexpr std::size_t kMaxStyles = 8;
  // The most text a paragraph may have, in bytes. What comes after is left
  // out, and TextLeftOut() says so.
  static constexpr std::uint32_t kMaxText = (1U << 30U) - 1U;
  // What ending a line at a cut made as a last resort costs: more than the
  // square of any gap on a line 4,000 wide.
  static constexpr std::uint32_t kLastResortPenalty = 1U << 24U;

  // The most work one call does with `records` records, whatever the text.
  // Work is counted in bytes of text looked at, bytes given to the measure,
  // and records gone over for the lines that are given out. A call looks at
  // a word's worth of bytes twice at the most and has as many measured
  // once; and no more records are gone over than there are.
  //
  // What the hyphenator and the breaker do is not counted here, and is
  // theirs to bound: a call asks the hyphenator about one word at the most
  // and gives the breaker one item at the most, or has it settle lines
  // once.
  static constexpr std::uint32_t MostWorkFor(std::size_t records) noexcept {
    return static_cast<std::uint32_t>((3 * kWordBufferBytes) + records);
  }

  // `nodes` is the breaker's working memory and `records` the setter's own.
  // Both are the caller's to provide.
  ParagraphSetter(std::span<LineBreaker::Node> nodes, std::span<Record> records) noexcept;

  ParagraphSetter(const ParagraphSetter&) = delete;
  ParagraphSetter& operator=(const ParagraphSetter&) = delete;
  ~ParagraphSetter() = default;

  // Starts a paragraph. Its text is measured by `measure` and its lines are
  // given to `sink`; both must stay alive until Finish() has returned true.
  // A paragraph that was being set is abandoned. The hyphenator in use
  // stays in use.
  [[nodiscard]] ParagraphStatus Begin(const ParagraphSettings& settings, TextMeasure& measure,
                                      TextLineSink& sink) noexcept;

  // The hyphenator for the words that end from now on: null for none. It
  // may be changed in the middle of a paragraph, where the text changes
  // language. It must stay alive while it is in use.
  void UseHyphenator(Hyphenator* hyphenator) noexcept { hyphenator_ = hyphenator; }

  // Gives the paragraph more text, in the style `style`. Returns how many
  // bytes of it were taken, from its start: the rest is to be given again.
  // It can take none, when it has work left over from what it took before;
  // it will have taken everything after a number of calls that depends only
  // on how much text there is. Without a paragraph begun, it takes all it
  // is given and does nothing with it.
  [[nodiscard]] std::size_t AddText(std::string_view text, std::uint16_t style) noexcept;

  // Ends the line where the text now stands, as a line feed in the text
  // does, but without being a byte of the text. Returns true when that is
  // done, and false if it has to be called again.
  [[nodiscard]] bool EndLine() noexcept;

  // Ends the paragraph: every line not yet given out is given out. Returns
  // true when the paragraph is finished, and false if it has to be called
  // again.
  [[nodiscard]] bool Finish() noexcept;

  ParagraphStatus Status() const noexcept { return status_; }
  // How many bytes of text the paragraph has been given so far.
  std::uint32_t Text() const noexcept { return at_; }
  // Text was given past kMaxText bytes, and left out.
  bool TextLeftOut() const noexcept { return textLeftOut_; }
  // How many lines of this paragraph have been given out.
  std::uint32_t Lines() const noexcept { return breaker_.Lines(); }
  // How many of them were ended where they stood for want of records.
  std::uint32_t LinesCutShort() const noexcept { return linesCutShort_; }
  // How many places to end a line were made as a last resort, whether or
  // not a line came to end at them.
  std::uint32_t LastResortCuts() const noexcept { return lastResortCuts_; }
  // How many words the hyphenator was asked about.
  std::uint32_t WordsAsked() const noexcept { return wordsAsked_; }
  // The most work one call has done on this paragraph, counted as
  // MostWorkFor() counts it, and never more than that says.
  std::uint32_t MostWork() const noexcept { return mostWork_; }
  // The breaker, for what it has to say of the paragraph: how many nodes it
  // used, how many lines were given out early or were too wide.
  const LineBreaker& Breaker() const noexcept { return breaker_; }

 private:
  // Room for a word, and for the character that was under way when a word
  // came to kMaxWordBytes.
  static constexpr std::size_t kWordBufferBytes = kMaxWordBytes + 3;
  // The most pieces of a word that are measured and held at once. A word
  // with more is gone through that many at a time.
  static constexpr std::size_t kMaxPieces = 64;

  // What follows a piece of a word.
  enum class After : std::uint8_t {
    kNothing,     // The next piece, or the end of the word: no line ends here.
    kHyphen,      // A line may end here, at a price, and shows a hyphen if it does.
    kFree,        // A line may end here at no price, with nothing more shown.
    kLastResort,  // A line may end here if there is nothing else for it.
  };

  // What kind of item a record is of.
  enum class Kind : std::uint8_t { kBox = 0, kSpace = 1, kHyphen = 2, kOther = 3 };

  // What is done about records that have run out, a step to a call.
  enum class Room : std::uint8_t { kSettle, kEndLine, kGiveOut };

  // One piece of a word: bytes `begin` up to `end` of it.
  struct Piece {
    std::uint8_t begin = 0;
    std::uint8_t end = 0;
    After after = After::kNothing;
    std::uint16_t width = 0;
    std::uint16_t hyphen = 0;  // What a hyphen shown after it comes to.
  };

  void OnLine(const Line& line) noexcept override;

  // Does one step of whatever is left to do with the text already taken.
  // False if there was nothing.
  bool Step() noexcept;
  // A call that did work is over: its work is set beside the most so far.
  void CallDone() noexcept;
  // Takes bytes from the front of `text` until a word is whole or a line is
  // ended, and returns how many.
  std::size_t Gather(std::string_view text, std::uint16_t style) noexcept;
  // The word in hand is whole: nothing more is added to it. `cut` if that is
  // because it could take no more, and not because it ended.
  void WordIsWhole(bool cut) noexcept;
  // Asks the hyphenator about the word in hand, if it is one to ask about.
  void AskHyphenator() noexcept;
  // Finds and measures the next pieces of the word in hand.
  void PreparePieces() noexcept;
  // Where the piece that begins at byte `from` of the word ends, where the
  // next begins, and what comes between.
  void NextCut(std::size_t from, std::size_t& end, std::size_t& next, After& after) noexcept;
  // Has the measure give the widths of pieces `first` up to `end`, which are
  // one stretch of the word in one style.
  void MeasurePieces(std::size_t first, std::size_t end) noexcept;
  // Gives the breaker the next item of the word in hand, if there is one.
  void FeedWord() noexcept;
  // Gives the breaker the first character of `piece`, or a place to end a
  // line before it if it is wider than `room` and something has come since
  // the latest such place.
  void FeedCharacterOf(Piece& piece, std::uint32_t room) noexcept;
  // The style of byte `at` of the word in hand.
  std::uint16_t StyleAt(std::size_t at) const noexcept;
  // Whether there is a record free for one more item, with one to spare. If
  // not, does one step of making room and returns false.
  bool Ready() noexcept;

  void GiveBox(std::uint16_t width, std::uint32_t at, std::uint32_t endAt) noexcept;
  void GiveSpace() noexcept;
  // `hyphen` is what a hyphen shown there comes to, for a place that shows
  // one.
  void GiveBreak(After after, std::uint16_t hyphen, std::uint32_t at) noexcept;
  void GiveLineEnd(std::uint32_t at) noexcept;
  void Remember(Kind kind, std::uint32_t at) noexcept;
  std::uint32_t PlaceOf(std::uint32_t item) const noexcept;
  Kind KindOf(std::uint32_t item) const noexcept;

  LineBreaker breaker_;
  std::span<Record> records_;
  TextMeasure* measure_ = nullptr;
  TextLineSink* sink_ = nullptr;
  Hyphenator* hyphenator_ = nullptr;
  ParagraphStatus status_ = ParagraphStatus::kNotStarted;
  std::uint32_t hyphenPenalty_ = 0;
  std::uint32_t narrowest_ = 0;  // The width of the paragraph's narrowest line.

  std::uint32_t at_ = 0;  // Bytes of text taken so far.
  bool textLeftOut_ = false;
  std::uint32_t work_ = 0;  // Of the call under way.
  std::uint32_t mostWork_ = 0;
  std::uint32_t linesCutShort_ = 0;
  std::uint32_t lastResortCuts_ = 0;
  std::uint32_t wordsAsked_ = 0;

  // Where the paragraph stands, as the breaker has been told of it.
  std::uint32_t run_ = 0;      // The width of what has come since the latest place a line could end.
  std::uint32_t boxEnd_ = 0;   // Where in the text the latest box ends.
  bool afterBox_ = false;      // The latest item was a box.
  bool lineStart_ = true;      // Nothing has come since the paragraph began or a line was ended.
  bool boxOnLine_ = false;     // A box has come since then.
  Room room_ = Room::kSettle;  // The next step, if the records run out.

  // Spaces that have been taken and not yet given to the breaker: they are
  // one item, and only once a word follows them.
  bool spaceWaiting_ = false;
  std::uint32_t spaceWidth_ = 0;

  // The word in hand.
  std::array<char, kWordBufferBytes> word_{};
  std::size_t wordSize_ = 0;
  std::uint32_t wordAt_ = 0;  // Where in the text it begins.
  bool wordWhole_ = false;    // Nothing more is to be added to it.
  bool wordCut_ = false;      // It is whole only because it could take no more.
  bool wordGoesOn_ = false;   // It is what follows such a cut: part of a longer run.
  bool wordAsked_ = false;    // The hyphenator has had its say.
  bool letterSeen_ = false;   // A character that is no hyphen or dash has come in it.
  // Where its styles change: the first byte in each, and which style.
  std::array<std::uint8_t, kMaxStyles> styleFrom_{};
  std::array<std::uint16_t, kMaxStyles> styles_{};
  std::size_t styleCount_ = 0;
  // The style the last byte of a word was taken in, which a byte that goes
  // on with its character is taken in too.
  std::uint16_t styleBefore_ = 0;
  // Where the hyphenator would break it, as bytes before each break.
  std::array<std::uint8_t, Hyphenator::kMaxLetters> breaks_{};
  std::size_t breakCount_ = 0;
  std::size_t breakNext_ = 0;
  // The pieces that are measured and waiting to go to the breaker.
  std::array<Piece, kMaxPieces> pieces_{};
  std::size_t pieceCount_ = 0;
  std::size_t pieceNext_ = 0;
  std::size_t piecesEnd_ = 0;     // The byte of the word the pieces after these begin at.
  bool pieceBoxed_ = false;       // The next piece has gone to the breaker, and what follows it has not.
  bool byCharacters_ = false;     // It is going to the breaker a character at a time.
  bool breakBeforeRest_ = false;  // One of its characters has gone, and a break comes before the next.
};

}  // namespace bn::layout
