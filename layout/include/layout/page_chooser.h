#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Choosing which lines go on a page.
//
// Breaking a paragraph into lines does not depend on where a page ends, so
// a chapter is one run of lines whatever pages they fall on. A page is then
// known by its first line, and choosing pages is only a matter of how many
// lines fit. That can be done from either end. Going forward, a page is
// filled down from its first line (PageFromTop). Going back to a page that
// has not been laid out before, it is filled up from its last line, the one
// above the first line of the page being left (PageFromBottom). And where a
// chapter is entered at some line in the middle of it, the page that shows
// that line begins with it, or a little above it (PageHolding).
//
// Nothing here knows what a line holds. A line is a height, the gap that
// belongs between it and the line before, and what is true of the place
// between those two: that a page may end there, may not, or has to.
//
//   - The gap before a line is only there while the line before is on the
//     same page. Above the first line of a page it is left out. The one
//     exception is a line that starts a page whatever comes before it (the
//     first of a chapter, or one after a break the book asks for): there it
//     is the space the book wants above that line, and it is kept.
//   - A line kept with the one before it does not begin a page, and the one
//     before it does not end one. Where the lines kept together are more
//     than a page can hold, that cannot be done, and the page ends, or
//     begins, where it is full.
//   - A line that starts a page always begins one. Nothing above it is on
//     its page, kept with it or not.
//   - A line taller than the page has a page to itself.
//
// Both ways of filling follow the same rules. So a page filled up to where
// a page filled down ends holds that page's lines, and perhaps more above
// them; unless the first of them was a line that should not have begun a
// page, and then it may begin lower.

namespace bn::layout {

// How starting a page has gone.
enum class PageStatus : std::uint8_t {
  kOk = 0,
  kNotStarted,  // Begin() has not been called.
  kNoMemory,    // Fewer entries than PageFromBottom::kMinEntries.
  kBadHeight,   // A page of no height.
};

// The name of a status, for diagnostics: "ok", "not started", ...
const char* PageStatusName(PageStatus status) noexcept;

// One line, as far as choosing pages goes. Heights and gaps are whole
// numbers in whatever unit the page's height is in.
struct PageLine {
  std::uint16_t height = 0;
  // The space between this line and the one before it.
  std::uint16_t gapBefore = 0;
  // No page ends between the line before and this one, if it can be helped.
  bool keepWithPrevious = false;
  // This line begins a page, whatever is above it.
  bool startsPage = false;
  // Whatever the caller knows the line by: where its text begins, say. It
  // is given back for the line a page filled up begins with.
  std::uint32_t tag = 0;
};

// Fills a page down from its first line.
//
// The lines are offered in order, starting with the one the page begins
// with, until one is refused. The page is then the first Lines() of those
// offered, which can be fewer than were accepted: lines that are kept
// together and do not all fit are left for the next page together. The next
// page begins with the line after the last one on this.
//
// Each call does a fixed amount of work. Used by one task.
class PageFromTop final {
 public:
  // Starts a page `height` high.
  [[nodiscard]] PageStatus Begin(std::uint16_t height) noexcept;

  // Offers the next line. True if it goes on the page, as far as is known;
  // false if the page is over, and then no more are taken. Before Begin()
  // it is false.
  [[nodiscard]] bool Add(const PageLine& line) noexcept;

  // How the last Begin() went.
  PageStatus Status() const noexcept { return status_; }
  // How many of the lines offered are on the page.
  std::uint32_t Lines() const noexcept { return lines_; }
  // How high those lines stand: from the top of the page to the bottom of
  // the last of them.
  std::uint32_t Height() const noexcept { return height_; }
  // Where the line last accepted begins, measured down from the top of the
  // page.
  std::uint32_t Top() const noexcept { return top_; }
  // A line has been refused: the page is settled.
  bool Over() const noexcept { return over_; }
  // The page holds one line, and that line is taller than the page.
  bool TooTall() const noexcept { return tooTall_; }

 private:
  PageStatus status_ = PageStatus::kNotStarted;
  bool over_ = false;
  bool tooTall_ = false;
  std::uint16_t pageHeight_ = 0;
  std::uint32_t lines_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t top_ = 0;
  // The latest place among the lines accepted where the page could end: how
  // many lines are before it, and how high they stand. No lines if there is
  // no such place.
  std::uint32_t endLines_ = 0;
  std::uint32_t endHeight_ = 0;
};

// Fills a page up from its last line.
//
// The lines above a page are only known by breaking their paragraphs from
// the start, so they cannot be offered from the bottom up one at a time.
// They are offered a block at a time instead: first the block whose last
// line is the page's last, then the block above it, and so on up, the lines
// of each block in their own order from its first. EndBlock() says when no
// more are wanted. A block here is whatever the caller reads lines in: a
// paragraph, usually. Where the page's last line is in the middle of one,
// only the lines down to it are given.
//
// The page ends where it was told to. It is not filled from below: it
// begins with the highest line that lets everything down to its last line
// fit, and where a line may not begin a page, with the first below it that
// may.
//
// A long block is not held. Only the lines that could still be the page's
// first are remembered, in entries the caller provides, twelve bytes each:
// one for each line of the block being given that would fit on the page as
// things stand. With fewer entries than a page has lines, the page is the
// lines there were entries for, and says so (CutShort()).
//
// One call goes over no more entries than it was given. Used by one task.
class PageFromBottom final {
 public:
  // A line that could still be the page's first. The filler's own business:
  // the caller only provides an array of them.
  struct Entry {
    std::uint32_t top = 0;  // Where it begins, counted down its block.
    std::uint32_t tag = 0;
    std::uint16_t gap = 0;  // The gap before it.
    std::uint8_t flags = 0;
  };

  // The fewest entries it will work with, and enough for any ordinary page:
  // one for each line that could be on a page.
  static constexpr std::size_t kMinEntries = 8;
  static constexpr std::size_t kUsualEntries = 128;

  // `entries` is the filler's working memory, and the caller's to provide.
  explicit PageFromBottom(std::span<Entry> entries) noexcept;

  PageFromBottom(const PageFromBottom&) = delete;
  PageFromBottom& operator=(const PageFromBottom&) = delete;
  ~PageFromBottom() = default;

  // Starts a page `height` high, which ends where the first block given
  // ends.
  [[nodiscard]] PageStatus Begin(std::uint16_t height) noexcept;

  // A block's lines follow, in order. Before Begin(), and once the page's
  // first line is found, these three do nothing.
  void BeginBlock() noexcept;
  void Add(const PageLine& line) noexcept;
  // The block is over. True if the page's first line is found, and no more
  // blocks are wanted; false if the block above is. If there is no block
  // above, the page is what has been found so far.
  [[nodiscard]] bool EndBlock() noexcept;

  // How the last Begin() went.
  PageStatus Status() const noexcept { return status_; }

  // What has been found, as of the last block ended.

  // The page holds a line.
  bool Found() const noexcept { return Chosen().lines != 0; }
  // The tag of the line the page begins with, and which of the blocks
  // given that line is in: 0 for the first given, 1 for the one given after
  // it, which is the one above it, and so on. A block with no lines is
  // counted as any other is.
  std::uint32_t FirstTag() const noexcept { return Chosen().tag; }
  std::uint32_t FirstBlock() const noexcept { return Chosen().block; }
  // How many lines are on the page, and how high they stand: from the top
  // of the page to the bottom of the last.
  std::uint32_t Lines() const noexcept { return Chosen().lines; }
  std::uint32_t Height() const noexcept { return Chosen().height; }
  // The page begins with a line that starts a page, so it stopped there,
  // and may have room to spare below its last line. A caller that wants
  // such a page full fills it down from that line instead.
  bool AtPageStart() const noexcept { return Chosen().starts; }
  // The page holds one line, and that line is taller than the page.
  bool TooTall() const noexcept { return tooTall_; }
  // The entries ran out, so the page may hold fewer lines than would fit.
  bool CutShort() const noexcept { return cutShort_; }

 private:
  // A line the page could begin with, and what the page would then be.
  struct Start {
    std::uint32_t tag = 0;
    std::uint32_t block = 0;  // Which of the blocks given it is in.
    std::uint32_t lines = 0;  // None if there is no such line.
    std::uint32_t height = 0;
    bool starts = false;
  };

  const Start& Chosen() const noexcept { return mayBegin_.lines != 0 ? mayBegin_ : highest_; }
  Entry& At(std::size_t n) noexcept { return entries_[(head_ + n) % entries_.size()]; }
  void DropOldest() noexcept;

  std::span<Entry> entries_;
  PageStatus status_ = PageStatus::kNotStarted;
  bool done_ = false;
  bool tooTall_ = false;
  bool cutShort_ = false;
  std::uint16_t pageHeight_ = 0;
  std::uint32_t blocksEnded_ = 0;

  // The lines taken from the blocks that are over.
  Start highest_;             // The highest line that fits, and the page from it down.
  Start mayBegin_;            // The highest that fits and may begin a page.
  std::uint16_t topGap_ = 0;  // The gap before the highest line.

  // The block being given. Its entries are the lines that fit so far, in
  // order, kept in a ring.
  bool inBlock_ = false;
  bool cut_ = false;      // A line of it does not fit, or may not be on the page: the block above is not wanted.
  std::size_t head_ = 0;  // The oldest entry.
  std::size_t size_ = 0;  // How many there are.
  std::uint32_t blockLines_ = 0;
  std::uint32_t bottom_ = 0;  // Where the last line given ends, counted down the block.
  std::int32_t room_ = 0;     // How high the lines of this block may stand. Less than nothing if there is none.
  PageLine last_;             // The last line given.
};

// Finds where the page that is to show a given line begins, when a chapter
// is entered at that line and not at a page already known.
//
// That is the line itself, if a page may begin with it. If it is kept with
// the line before, the page begins with the nearest line above it that a
// page may begin with, so that lines kept together are shown together. If
// that line is so far up that the line asked for would not be on its page,
// the page begins with the line asked for after all.
//
// Lines are given as they are to PageFromBottom: the block the line is in,
// from its first line down to the line itself, then the block above, and so
// on up, until EndBlock() says no more are wanted. Nearly always the first
// block is enough.
//
// Each call does a fixed amount of work. Used by one task.
class PageHolding final {
 public:
  // The page is `height` high.
  [[nodiscard]] PageStatus Begin(std::uint16_t height) noexcept;

  // A block's lines follow, in order. Before Begin(), and once the page's
  // first line is found, these three do nothing.
  void BeginBlock() noexcept;
  void Add(const PageLine& line) noexcept;
  // The block is over. True if the page's first line is found, and no more
  // blocks are wanted; false if the block above is. If there is no block
  // above, the page begins with the highest line given.
  [[nodiscard]] bool EndBlock() noexcept;

  // How the last Begin() went.
  PageStatus Status() const noexcept { return status_; }
  // A line has been given.
  bool Found() const noexcept { return Chosen().lines != 0; }
  // The tag of the line the page begins with, and which of the blocks
  // given that line is in, counted as PageFromBottom counts them.
  std::uint32_t FirstTag() const noexcept { return Chosen().tag; }
  std::uint32_t FirstBlock() const noexcept { return Chosen().block; }
  // How many lines there are from that one to the line asked for, counting
  // both, and how high they stand.
  std::uint32_t Lines() const noexcept { return Chosen().lines; }
  std::uint32_t Height() const noexcept { return Chosen().height; }

 private:
  // A line the page could begin with, and what is on the page from it down
  // to the line asked for.
  struct Start {
    std::uint32_t tag = 0;
    std::uint32_t block = 0;  // Which of the blocks given it is in.
    std::uint32_t lines = 0;  // None if there is no such line.
    std::uint32_t height = 0;
  };

  const Start& Chosen() const noexcept { return done_ ? chosen_ : highest_; }

  PageStatus status_ = PageStatus::kNotStarted;
  bool done_ = false;
  std::uint16_t pageHeight_ = 0;
  std::uint32_t blocksEnded_ = 0;
  Start asked_;    // The line asked for, by itself.
  Start highest_;  // The highest line given in the blocks that are over.
  Start chosen_;
  std::uint16_t topGap_ = 0;  // The gap before the highest line.

  // The block being given. Its heights are added up no further than far
  // more than any page: past that, all that matters is that it is too much.
  bool inBlock_ = false;
  std::uint32_t blockLines_ = 0;
  std::uint32_t blockHeight_ = 0;  // How high its lines stand so far.
  std::uint32_t firstTag_ = 0;     // Its first line,
  std::uint16_t firstGap_ = 0;     // and the gap before that.
  bool mayBegin_ = false;          // A page may begin with one of its lines. The lowest such:
  std::uint32_t beginTag_ = 0;     // its tag,
  std::uint32_t beginLine_ = 0;    // how many lines of the block are above it,
  std::uint32_t sinceBegin_ = 0;   // and how high the lines from it down stand.
  PageLine last_;                  // The last line given.
};

}  // namespace bn::layout
