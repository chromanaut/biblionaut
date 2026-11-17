#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/block_file.h"
#include "core/clock.h"
#include "core/storage.h"
#include "core/trace_stream.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/line_digest.h"
#include "layout/made_up_face.h"
#include "layout/paragraph_setter.h"
#include "reader/device_check.h"

namespace bn::reader {

class LogLine;

// Times the layout code on a device: the line breaker, the hyphenator and
// the paragraph setter. Their bounds are stated in work, and this says what
// that work is in time. It writes nothing to the card.
//
//   1. times the made-up face alone (layout/made_up_face.h), over 64 KB of
//      text held in memory, as the book check times its checksum: the face
//      is what the text is measured in, and a real face will cost more;
//   2. breaks the paragraph that brings one of the breaker's calls closest
//      to its bound (LineBreaker::MostWorkFor()) with 64, 256 and 1,024
//      nodes, timing every call;
//   3. reads the hyphenation table at kTablePath, if there is one, and
//      otherwise takes the one it was given, if it was given one (the
//      application gives it the table for American English that is built
//      in);
//   4. sets text made to be awkward for the setter, timing every call, and
//      has the hyphenator find the places in some long words;
//   5. sets the text at kTextPath (paragraphs.txt, made from books on a
//      computer by tools/layout-text.py) as `bn-lines` sets a file: an
//      empty line ends a paragraph, a line end inside one is a line the
//      text ends, and what is between two asterisks is in the second
//      style. It is set three times: hyphenated, then not, then
//      hyphenated again with every call timed (with no table at all, not
//      hyphenated, then again with every call timed);
//   6. says what it found, in lines of text on the diagnostic stream.
//
// The text is set in lines kWidth wide, with kNodes nodes and kRecords
// records, at kHyphenPenalty for a hyphen: the figures `bn-lines` prints for
// the same file and table with its defaults (`--figures`) are the ones this
// prints for each setting, the checksum of the lines (layout/line_digest.h)
// among them. So a device can be shown to set the same lines the desktop
// does.
//
// Times are counted apart: the card's, reading the text; setting it, which
// is everything the setter, the breaker, the hyphenator and the face do;
// and the face's share of that, worked out from the bytes it measured and
// the rate found in step 1. The first two settings are timed a step at a
// time, so that timing costs them nothing to speak of; the third reads the
// clock round every call, which costs it something, and finds the longest
// call.
//
// It works for about 20 ms each Tick() and then lets the application carry
// on, as the book check does. A step is reading a block of the text, or
// setting a kilobyte of it, no further than the end of a paragraph; or a
// few calls of the other kinds. The breaker's dearest call with 1,024 nodes
// is a step by itself, and the longest there is.
//
// Used by one task. It is about 93 KB, most of it the breaker's nodes and
// room for the table: too big for a stack.
class LayoutCheck final : public DeviceCheck {
 public:
  enum class State : std::uint8_t {
    kIdle,  // Never run.
    kRunning,
    kPassed,  // The text was set every way, in the same lines each time it was hyphenated alike.
    kFailed,
  };

  // Where the text and the table are on the card.
  static constexpr std::string_view kTextPath = "/layout/paragraphs.txt";
  static constexpr std::string_view kTablePath = "/layout/hyphenation.bnhy";
  // The largest table there is room for. American English's is 38 KB.
  static constexpr std::size_t kTableBytes = 49152;
  // How the text is set: as `bn-lines` sets it by default.
  static constexpr std::uint16_t kWidth = 1760;
  static constexpr std::uint32_t kHyphenPenalty = 12100;
  static constexpr std::size_t kNodes = layout::LineBreaker::kUsualNodes;
  static constexpr std::size_t kRecords = layout::ParagraphSetter::kUsualRecords;
  // The most nodes the breaker is timed with.
  static constexpr std::size_t kMostNodes = 1024;
  // How many lines a page is taken to have, for the time of a page's worth.
  static constexpr std::uint32_t kPageLines = 30;
  // How much text the face is timed over.
  static constexpr std::size_t kFaceBytes = 65536;
  // How much of the text is read at a time, and how much of that is set at
  // one step.
  static constexpr std::size_t kChunkBytes = 4096;
  static constexpr std::size_t kStepBytes = 1024;
  // How long one Tick() works for before it lets go.
  static constexpr std::uint64_t kSliceUs = 20'000;

  // How one of the dearest calls went: for the breaker with some number of
  // nodes, for the setter with awkward text, or for the hyphenator.
  struct Dearest {
    std::uint64_t calls = 0;
    std::uint64_t allUs = 0;        // All of them.
    std::uint64_t longestUs = 0;    // The longest call.
    std::uint32_t mostWork = 0;     // The most work a call did, as the piece counts it,
    std::uint32_t bound = 0;        // and the most it says one may.
    std::uint32_t breakerWork = 0;  // For the setter: the most the breaker did in one of its calls.
  };

  // What setting the text one way came to.
  struct Pass {
    bool hyphenated = false;
    bool callByCall = false;  // Every call was timed.
    std::uint64_t paragraphs = 0;
    std::uint64_t bytes = 0;  // Of text, as the setter was given it.
    std::uint64_t lines = 0;
    std::uint64_t hyphens = 0;
    std::uint64_t tooWide = 0;
    std::uint32_t checksum = 0;  // Of the lines (layout/line_digest.h).
    std::uint64_t wordsAsked = 0;
    std::uint64_t lastResortCuts = 0;
    std::uint64_t settledEarly = 0;
    std::uint64_t paragraphsSettledEarly = 0;
    std::uint64_t linesCutShort = 0;
    std::uint64_t bytesMeasured = 0;
    std::size_t mostNodes = 0;
    std::uint32_t mostSetterWork = 0;
    std::uint32_t mostBreakerWork = 0;
    std::uint32_t mostHyphenatorWork = 0;
    std::uint64_t calls = 0;
    std::uint64_t setUs = 0;   // Setting the text.
    std::uint64_t cardUs = 0;  // Reading it from the card,
    std::uint32_t reads = 0;   // in this many reads.
    // The paragraph that took longest, from its start.
    std::uint64_t longestParagraphUs = 0;
    std::uint64_t longestParagraphBytes = 0;
    std::uint64_t longestParagraphLines = 0;
    std::uint64_t longestCallUs = 0;  // When every call was timed.
  };

  // `builtIn` is the table to use when the card has none, or none that
  // can be used. It has to stay where it is for as long as the check does.
  LayoutCheck(core::Storage& storage, core::Clock& clock, core::TraceStream& stream,
              std::span<const std::uint8_t> builtIn = {}) noexcept;

  LayoutCheck(const LayoutCheck&) = delete;
  LayoutCheck& operator=(const LayoutCheck&) = delete;
  ~LayoutCheck() override = default;

  // Starts a run from the beginning, unless one is under way.
  void Begin() noexcept override;

  // Does the next pieces of a run, if one is under way.
  void Tick() noexcept override;

  State CurrentState() const noexcept { return state_; }

  // Of the last run, or of the one under way so far.
  std::uint64_t FaceUs() const noexcept { return faceUs_; }  // Measuring kFaceBytes.
  // The breaker with 64, 256 and 1,024 nodes.
  const Dearest& Breaker(std::size_t which) const noexcept { return breakerRuns_[which]; }
  const Dearest& Setter() const noexcept { return setterRun_; }
  const Dearest& Words() const noexcept { return wordRun_; }
  // Whether a table was found and opened,
  bool Hyphenating() const noexcept { return tableOpen_; }
  // and whether it was the one the check was given rather than the card's.
  bool HyphenatingBuiltIn() const noexcept { return tableBuiltIn_; }
  // The settings of the text so far, in the order they are made.
  std::size_t Passes() const noexcept { return passCount_; }
  const Pass& PassFigures(std::size_t which) const noexcept { return passes_[which]; }
  // How long the longest step took, and what it was doing.
  std::uint64_t LongestStepUs() const noexcept { return longestStepUs_; }
  const char* LongestStepName() const noexcept;

 private:
  enum class Step : std::uint8_t {
    kNone,
    kTimeFace,
    kBreak,
    kOpenTable,
    kReadTable,
    kSetAwkward,
    kFindWords,
    kOpenText,
    kReadText,
    kSetText,
    kEndPass,
  };

  // Counts the lines of the text as they are given out.
  class Lines final : public layout::TextLineSink {
   public:
    void OnLine(const layout::TextLine& line) noexcept override;
    void Clear() noexcept { *this = Lines(); }

    layout::LineDigest digest;
    std::uint64_t hyphens = 0;
    std::uint64_t tooWide = 0;
  };

  // Counts the breaker's lines in the dearest calls, and nothing else.
  class BrokenLines final : public layout::LineSink {
   public:
    void OnLine(const layout::Line& /*line*/) noexcept override { ++count; }
    std::uint64_t count = 0;
  };

  void DoStep() noexcept;
  void TimeFace() noexcept;
  void Break() noexcept;
  void OpenTable() noexcept;
  void ReadTable() noexcept;
  // The card's table cannot be used: opens the one the check was given, if
  // any, and finishes `line` with what came of that.
  void OpenBuiltIn(LogLine& line) noexcept;
  void SetAwkward() noexcept;
  void FindWords() noexcept;
  void OpenText() noexcept;
  void ReadText() noexcept;
  void SetText() noexcept;
  void EndPass() noexcept;

  // Puts the next of the awkward text into chunk_: as much as there is
  // room for or as is left.
  void FillAwkward() noexcept;
  // Starts the next setting of the text, or ends the run.
  void BeginPass() noexcept;
  // Gives `text` to the setter in the style in hand, taking as many calls
  // as it takes.
  void Feed(std::string_view text) noexcept;
  // A byte of the text is beginning a paragraph's text, or going on with it.
  void BeginParagraphIfNone() noexcept;
  // The paragraph in hand is over: has the setter finish it.
  void EndParagraph() noexcept;
  // Calls the setter's AddText() and Finish(), timed when every call is.
  std::size_t TimedAdd(std::string_view text) noexcept;
  bool TimedFinish() noexcept;
  // A call that was timed took `us`.
  void CallTook(std::uint64_t us) noexcept;
  void Fail(std::string_view why) noexcept;
  void MoveTo(Step step) noexcept;
  void Finish() noexcept;
  void SayDearest() noexcept;
  void SayPass(const Pass& pass) noexcept;
  const char* PassName(const Pass& pass) const noexcept;

  core::Storage& storage_;
  core::Clock& clock_;
  core::TraceStream& stream_;

  State state_ = State::kIdle;
  Step step_ = Step::kNone;
  std::size_t count_ = 0;  // Pieces done within the step.
  bool failed_ = false;
  std::uint64_t longestStepUs_ = 0;
  Step longestStep_ = Step::kNone;
  std::uint32_t beganTrace_ = 0;
  std::uint32_t passBeganTrace_ = 0;

  // Steps 1 and 2.
  layout::MadeUpFace face_;
  std::uint64_t faceUs_ = 0;
  std::array<Dearest, 3> breakerRuns_{};
  std::optional<layout::LineBreaker> breaker_;
  BrokenLines brokenLines_;
  std::size_t breakerItems_ = 0;  // How many items the paragraph in hand has been given.

  // Steps 3 and 4.
  std::span<const std::uint8_t> builtIn_;
  bool tableOpen_ = false;
  bool tableBuiltIn_ = false;
  std::uint32_t tableSize_ = 0;
  std::uint64_t tableUs_ = 0;  // Reading it.
  layout::Hyphenator hyphenator_;
  Dearest setterRun_{};
  Dearest wordRun_{};
  std::uint32_t awkwardAt_ = 0;  // How far into the awkward text in hand.

  // Step 5.
  std::array<Pass, 3> passes_{};
  std::size_t passCount_ = 0;
  std::size_t passIndex_ = 0;  // Which of passes_ is under way, or was last.
  Pass& Current() noexcept { return passes_[passIndex_]; }
  bool textOpen_ = false;
  std::size_t chunkLength_ = 0;  // What chunk_ holds of the text,
  std::size_t chunkAt_ = 0;      // and how much of that has been set.
  bool textEnded_ = false;
  // The paragraph in hand, and where the reading of the file stands.
  bool paragraphOpen_ = false;
  bool lineHasText_ = false;  // The line of the file in hand has text on it.
  bool feedWaiting_ = false;  // A line of the file has ended inside the paragraph.
  bool crWaiting_ = false;    // A carriage return has been read, and what follows it has not.
  std::uint16_t style_ = 0;
  // The time the paragraph in hand has taken so far, and the lines before
  // it. When it is done, how many bytes and lines it came to, for the turn
  // to set beside the longest once its last step has been timed.
  std::uint64_t paragraphUs_ = 0;
  std::uint64_t paragraphLinesBefore_ = 0;
  bool paragraphDone_ = false;
  std::uint64_t lastParagraphBytes_ = 0;
  std::uint64_t lastParagraphLines_ = 0;
  std::uint64_t measuredBefore_ = 0;  // What the face had measured when the setting began.

  // The setter and what it works in, for both the awkward text and the text
  // on the card.
  std::array<layout::LineBreaker::Node, kMostNodes> nodes_{};
  std::array<layout::ParagraphSetter::Record, kRecords> records_{};
  layout::ParagraphSetter setter_;
  layout::ParagraphSettings settings_;
  Lines lines_;
  bool timeEveryCall_ = false;
  Dearest* timing_ = nullptr;  // Where every call's time goes, when it is timed.

  // Aligned, and a multiple of four long, so that a device's card driver
  // takes them as they are.
  alignas(4) std::array<std::uint8_t, kChunkBytes> fileBuffer_{};
  alignas(4) std::array<std::uint8_t, kChunkBytes> chunk_{};
  alignas(4) std::array<std::uint8_t, kTableBytes> table_{};
  core::BlockFile file_;
};

}  // namespace bn::reader
