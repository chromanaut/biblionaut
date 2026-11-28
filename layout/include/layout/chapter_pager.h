#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "epub/segment.h"
#include "layout/hyphenator.h"
#include "layout/page_chooser.h"
#include "layout/paragraph_setter.h"

// A chapter of a bundle in pages.
//
// This is where the reader of a chapter's segment (epub/segment.h), the
// paragraph setter and the page chooser meet. It reads a block, has it set
// in lines, and gives the lines to a chooser, a block after another, until
// a page is known: the page that begins with a line, filled down; the page
// that ends above a line, filled up; or the page a chapter entered at some
// place is shown.
//
// A line is known by its block and by the character of the block it begins
// with, and a page by its first line. Characters are counted as the
// segment counts them: a byte of text is one, and so is a picture among
// the words.
//
// What it does not know is what a block's record comes to on a page: how
// wide its lines are, how high, what space is above and below, which face
// a run is in. Whoever makes one gives it a BlockSetting to ask, and the
// TextMeasure the setter measures text with.
//
// How a chapter is read:
//
//   - A block is found by the chapter's index and read from its head, and
//     the block after it is read on from there. A block past the last the
//     index has is come to by reading on from that one.
//   - To set a block, two things are wanted of the block before it: whether
//     that one is kept with the next, and the space after it. They are
//     remembered for the last few blocks whose heads were read, and
//     otherwise the head of the block before is read for them.
//   - Nothing else is kept of a block that has been set. A page that
//     begins part of the way down a block has the block set from its
//     start, and the lines above the page's first passed over. A page
//     filled up is set twice: up from its end, to find where it begins,
//     and then down from there for its lines; where filling it up comes
//     to a line that starts a page, it is that line's page, filled down.
//   - A picture among the words is given to the setter as one byte,
//     kPictureByte, in the style of the text about it: the setter puts
//     nothing but text in a line, and this stands for the picture until it
//     does.
//   - In a block that is a picture (SegmentBlockKind::kPicture), if the
//     picture comes before any text, the lines are as high as the setting
//     says the picture is.
//   - A block that comes to no line (a rule, a block of nothing but
//     spaces) is one line with nothing on it.
//   - A line is a pixel high at the least, whatever the setting says.
//
// A page is given as its lines, in records the caller provides, and not as
// their text: whoever draws the page reads the text again. A page has no
// more lines than there are records. One filled down ends where they run
// out, as it would where the page was full; one filled up begins with the
// first line there is a record for; and both say so (ChapterPage::cutShort).
//
// The work is done a step at a time. A step makes one call of the reader
// that reads, or one call of the setter that sets, and takes in the lines
// that call gives out, which are no more than the breaker has nodes: for
// each of them one call of a chooser. What those calls do is theirs to bound
// (a reader's call reads storage twice at the most; ParagraphSetter::
// MostWorkFor(); PageFromBottom goes over no more entries than it was
// given). LastStep() says what a step did.
//
// The reader may be used for anything else between one piece of work and
// the next, and not during one. Used by one task.

namespace bn::layout {

// How a piece of the pager's work has gone.
enum class PagerStatus : std::uint8_t {
  kOk = 0,
  kMisused,      // Asked for out of turn: while a piece of work is under way, or of a reader that has no chapter open.
  kNoMemory,     // Fewer records for lines than kMinLines, or fewer entries than the chooser works with.
  kBadHeight,    // A page of no height.
  kNoSuchBlock,  // A block the chapter has not.
  kReadFailed,   // The reader could not read the chapter: ReaderStatus() says how.
  kSetFailed,    // The setter would not begin a block: SetterStatus() says why.
};

// The name of a status, for diagnostics: "ok", "misused", ...
const char* PagerStatusName(PagerStatus status) noexcept;

// A place in a chapter: a block, and so many characters into it. A line is
// the place it begins at. The block after a chapter's last, with no
// characters, is the chapter's end.
struct ChapterPlace {
  std::uint32_t block = 0;
  std::uint32_t into = 0;

  bool operator==(const ChapterPlace&) const noexcept = default;
};

// What is true of a line (ChapterLine::flags).
inline constexpr std::uint8_t kLineKept = 1U << 0U;      // It is kept with the line before it.
inline constexpr std::uint8_t kLineStarts = 1U << 1U;    // It starts a page, whatever is above it.
inline constexpr std::uint8_t kLineHyphen = 1U << 2U;    // A hyphen is shown at its end.
inline constexpr std::uint8_t kLineLast = 1U << 3U;      // It ends its paragraph, or where the text asked.
inline constexpr std::uint8_t kLineOverfull = 1U << 4U;  // It is wider than its block's lines may be.

// One line of a chapter. 28 bytes.
struct ChapterLine {
  std::uint32_t block = 0;
  // The characters of the block that are on it: from `begin` up to, and
  // not including, `end`, as TextLine has them.
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  // How wide they are, as the measure has it.
  std::uint32_t width = 0;
  // On a page: where the line begins, down from the top of the page.
  std::uint32_t top = 0;
  std::uint16_t height = 0;
  // The space that belongs above it, whether or not the page has it there.
  std::uint16_t gap = 0;
  // How many places between words it has.
  std::uint16_t spaces = 0;
  std::uint8_t flags = 0;

  ChapterPlace Place() const noexcept { return {.block = block, .into = begin}; }
  bool Is(std::uint8_t flag) const noexcept { return (flags & flag) != 0; }
};

// A page.
struct ChapterPage {
  // There is such a page. There is none above a chapter's first line, and
  // none that begins at a chapter's end.
  bool found = false;
  // It was filled down from its first line; false, up from its last.
  bool filledDown = true;
  // It holds fewer lines than it might, for want of records or of entries.
  bool cutShort = false;
  // Its first line, and the line after its last.
  ChapterPlace first;
  ChapterPlace next;
  // How high its lines stand, from the top of the page to the bottom of
  // the last; and how many there are, in the records.
  std::uint32_t height = 0;
  std::uint32_t lines = 0;
};

// What a block's record comes to. Widths are in the measure's unit, and
// heights and spaces in the unit a page's height is given in.
struct BlockMeasures {
  // How wide the block's first line may be, and every line after it.
  std::uint16_t firstWidth = 0;
  std::uint16_t restWidth = 0;
  std::uint16_t lineHeight = 0;
  // The space above the block's first line, and below its last.
  std::uint16_t spaceBefore = 0;
  std::uint16_t spaceAfter = 0;
  // What ending a line inside a word costs (ParagraphSettings).
  std::uint32_t hyphenPenalty = 0;
};

// What a run's style comes to: the measure's number for it, and what
// breaks its words, or null.
struct RunMeasures {
  std::uint16_t style = 0;
  Hyphenator* hyphenator = nullptr;
};

// What the pager asks of whoever knows how a book is to look.
class BlockSetting {
 public:
  // What the block whose head this is comes to.
  virtual void MeasureBlock(const epub::SegmentBlock& block, BlockMeasures& measures) noexcept = 0;
  // What a run of the block's text comes to. Asked where a block's text
  // begins, of a style that says nothing, and at every style item.
  virtual void MeasureRun(const epub::SegmentBlock& block, const epub::RunStyle& run,
                          RunMeasures& measures) noexcept = 0;
  // How high the lines of a block that is a picture are: `width` and
  // `height` are the picture's size as the document declares it, and
  // `picture` what the chapter says of the file, or null if it says
  // nothing.
  virtual std::uint16_t PictureHeight(const epub::SegmentBlock& block, const epub::StyleLength& width,
                                      const epub::StyleLength& height,
                                      const epub::SegmentPicture* picture) noexcept = 0;

 protected:
  ~BlockSetting() = default;  // Never destroyed through the interface.
};

// What is told of a block's lines (ChapterPager::BeginLines()).
class ChapterLineSink {
 public:
  // Called for each line, in order. It must not call the pager.
  virtual void OnChapterLine(const ChapterLine& line) noexcept = 0;

 protected:
  ~ChapterLineSink() = default;  // Never destroyed through the interface.
};

class ChapterPager final : private TextLineSink {
 public:
  // The byte that stands for a picture among a block's characters.
  static constexpr char kPictureByte = 0x1A;
  // The fewest records for lines it will work with.
  static constexpr std::size_t kMinLines = 8;
  // How many blocks it remembers what the block after needs of.
  static constexpr std::size_t kBlocksRemembered = 4;

  // What a step did.
  struct StepWork {
    std::uint8_t readerCalls = 0;  // Calls of the reader that read.
    std::uint8_t setterCalls = 0;  // Calls of the setter that set.
    std::uint32_t lines = 0;       // Lines taken in.
  };

  // `reader` has a chapter open while any work is asked for. `setter` is
  // what blocks are set with, with the memory it was given, and `measure`
  // what it measures text with; `setting` says what a block comes to.
  // `entries` are for the chooser that fills a page up, and `lines` the
  // records a page's lines are given in. All of them are the caller's,
  // alive for as long as this is in use, and the setter and the records
  // left alone while a piece of work is under way.
  ChapterPager(epub::SegmentReader& reader, ParagraphSetter& setter, TextMeasure& measure, BlockSetting& setting,
               std::span<PageFromBottom::Entry> entries, std::span<ChapterLine> lines) noexcept;

  ChapterPager(const ChapterPager&) = delete;
  ChapterPager& operator=(const ChapterPager&) = delete;
  ~ChapterPager() = default;

  // Each of these begins a piece of work; then Step(), and Page() or the
  // sink has what was asked for. `pageHeight` is how high a page is.

  // The page that begins with the line holding a place: the last line of
  // the block that begins at or before so many characters into it.
  [[nodiscard]] PagerStatus BeginDown(ChapterPlace first, std::uint16_t pageHeight) noexcept;
  // The page that ends with the last line that begins before a place: for
  // the place a line begins at, the line above it; for the chapter's end,
  // the chapter's last line.
  [[nodiscard]] PagerStatus BeginUp(ChapterPlace before, std::uint16_t pageHeight) noexcept;
  // The page a chapter entered at a place is shown: it begins with the
  // line holding the place, or a little above it where that line is kept
  // with those before (PageHolding).
  [[nodiscard]] PagerStatus BeginAbout(ChapterPlace place, std::uint16_t pageHeight) noexcept;
  // The lines of one block, each told to `sink` as it is known, which
  // must stay alive until the work is over. Their `top` says nothing.
  [[nodiscard]] PagerStatus BeginLines(std::uint32_t block, ChapterLineSink& sink) noexcept;

  // Does the next piece of whatever was begun. Returns true if there is
  // more to do, false once it is over: Status() says how it ended.
  [[nodiscard]] bool Step() noexcept;
  PagerStatus Status() const noexcept { return status_; }
  // How the reader failed, after kReadFailed, and the setter after
  // kSetFailed.
  epub::SegmentStatus ReaderStatus() const noexcept { return readerStatus_; }
  ParagraphStatus SetterStatus() const noexcept { return setterStatus_; }

  // The page that was asked for, once the work has ended kOk, and its
  // lines. They stand until the next piece of work is begun.
  const ChapterPage& Page() const noexcept { return page_; }
  std::span<const ChapterLine> Lines() const noexcept { return lines_.first(page_.lines); }

  // The chapter's end, as a place.
  ChapterPlace End() const noexcept { return {.block = reader_.Blocks(), .into = 0}; }

  // The reader has another chapter open, or the setting says other things
  // than it did: what was remembered of blocks is forgotten.
  void Forget() noexcept;

  // What the last step did.
  const StepWork& LastStep() const noexcept { return step_; }
  // What has been done since this was made: blocks given to the setter;
  // heads read only for what a block gives the one after it; blocks gone
  // to by the index; and blocks read through on the way to another.
  std::uint32_t BlocksSet() const noexcept { return blocksSet_; }
  std::uint32_t HeadsRead() const noexcept { return headsRead_; }
  std::uint32_t BlocksFound() const noexcept { return blocksFound_; }
  std::uint32_t BlocksPassed() const noexcept { return blocksPassed_; }

 private:
  enum class Job : std::uint8_t {
    kNone,
    kDown,       // Filling a page down.
    kUpFind,     // Filling a page up: finding where it begins,
    kUpFill,     // and then its lines.
    kAboutFind,  // Finding where the page about a place begins.
    kLines,
  };
  // Where the reading and setting of the block in hand stands.
  enum class Feed : std::uint8_t {
    kIdle,
    kFinding,     // Going to a block: the index is being looked in,
    kPassHead,    // or a block before it is being read through.
    kPassItems,   //
    kBeforeHead,  // The head of the block before, for what it gives this one.
    kHead,        // This block's head.
    kItems,       // Its items, one after another.
    kText,        // Text of it is going to the setter.
    kPictureSize,
    kPictureByte,
    kFinish,  // The setter is giving out the last lines.
  };
  // What a block gives the one after it.
  struct Remembered {
    bool known = false;
    bool keepWithNext = false;
    std::uint16_t spaceAfter = 0;
    std::uint32_t block = 0;
  };

  void OnLine(const TextLine& line) noexcept override;

  PagerStatus Begin(Job job, std::uint16_t pageHeight) noexcept;
  PagerStatus Fail(PagerStatus status) noexcept;
  void FailRead(epub::SegmentStatus status) noexcept;
  void Done() noexcept;

  // Begins the reading and setting of a block.
  void StartBlock(std::uint32_t block) noexcept;
  // Goes to a block, and then on to `then`.
  void GoTo(std::uint32_t block, Feed then) noexcept;
  void FeedStep() noexcept;
  void StepFinding() noexcept;
  void StepPassing() noexcept;
  void StepBeforeHead() noexcept;
  void StepHead() noexcept;
  void StepItems() noexcept;
  void GiveSetter(std::string_view& text) noexcept;
  void Remember(std::uint32_t block, bool keepWithNext, std::uint16_t spaceAfter) noexcept;
  const Remembered* Recall(std::uint32_t block) const noexcept;

  // A line of the block in hand; and the block is over, read to its end or
  // left where a line said to stop.
  void TakeLine(const ChapterLine& line) noexcept;
  void BlockOver(bool whole) noexcept;

  static PageLine PageLineOf(const ChapterLine& line, std::uint32_t tag) noexcept;
  void BeginFillingDown(ChapterPlace first) noexcept;
  void Offer(const ChapterLine& line) noexcept;
  void FinishDown() noexcept;
  void NextUpBlock() noexcept;
  void FinishUpFind() noexcept;
  void FinishUpFill() noexcept;
  void FinishAboutFind() noexcept;

  epub::SegmentReader& reader_;
  ParagraphSetter& setter_;
  TextMeasure& measure_;
  BlockSetting& setting_;
  std::span<ChapterLine> lines_;
  ChapterLineSink* sink_ = nullptr;
  PageFromTop top_;
  PageFromBottom bottom_;
  PageHolding holding_;

  PagerStatus status_ = PagerStatus::kOk;
  epub::SegmentStatus readerStatus_ = epub::SegmentStatus::kOk;
  ParagraphStatus setterStatus_ = ParagraphStatus::kOk;
  Job job_ = Job::kNone;
  StepWork step_;
  std::uint32_t blocksSet_ = 0;
  std::uint32_t headsRead_ = 0;
  std::uint32_t blocksFound_ = 0;
  std::uint32_t blocksPassed_ = 0;

  // Reading: which block the reader will read next, if that is known; and
  // the block being gone to.
  bool placed_ = false;
  std::uint32_t nextBlock_ = 0;
  std::uint32_t goingTo_ = 0;
  std::uint32_t passing_ = 0;
  Feed then_ = Feed::kIdle;
  std::array<Remembered, kBlocksRemembered> remembered_{};
  std::size_t rememberNext_ = 0;

  // The block in hand.
  Feed feed_ = Feed::kIdle;
  std::uint32_t block_ = 0;
  epub::SegmentBlock head_;
  BlockMeasures measures_;
  std::array<std::uint16_t, 1> firstWidth_{};
  bool keptWithBefore_ = false;
  std::uint16_t spaceAbove_ = 0;
  std::uint16_t lineHeight_ = 0;
  std::uint16_t style_ = 0;
  std::string_view text_;       // What is left of the text item in hand: it lies in the reader's page.
  std::string_view picture_;    // What is left of the byte that stands for a picture.
  bool somethingSeen_ = false;  // Text or a picture has come in the block.
  std::uint16_t pictureNumber_ = 0;
  epub::StyleLength pictureWidth_;
  epub::StyleLength pictureHeight_;
  std::uint32_t blockLines_ = 0;
  bool stop_ = false;  // A line said that no more of the block is wanted.

  // The page being made.
  ChapterPage page_;
  std::uint16_t pageHeight_ = 0;
  ChapterPlace asked_;            // The place the work was begun with.
  std::uint32_t count_ = 0;       // Records in use.
  bool seeking_ = false;          // The line holding a place is still being looked for:
  bool hasPending_ = false;       // the last line that began at or before it,
  ChapterLine pending_;           // so far.
  bool over_ = false;             // Filling down: a line was refused,
  bool hasRefused_ = false;       // which was this one,
  ChapterPlace refused_;          //
  bool forced_ = false;           // for want of a record.
  std::uint32_t firstGiven_ = 0;  // Going up: the first block given to the chooser,
  std::uint32_t limit_ = 0;       // and how much of the block in hand is given.
  bool given_ = false;
  ChapterPlace fillFirst_;    // Filling a page found by going up: its first line,
  std::uint32_t toPass_ = 0;  // how many lines there are no records for,
  std::uint32_t y_ = 0;       // and how high the lines so far stand.
};

}  // namespace bn::layout
