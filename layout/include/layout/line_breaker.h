#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Breaking a paragraph into lines.
//
// The lines of a paragraph are chosen together, not one at a time. Filling
// each line as full as it will go leaves some lines tight and others with a
// great gap at the end; looking at the whole paragraph, a word can be taken
// down early so that the lines after it come out even. What is made as small
// as possible is the sum, over the lines, of the square of the space left on
// each, so one line with a large gap costs more than several with small ones.
// The last line of a paragraph is not counted: it is allowed to be short.
//
// A paragraph is given as a run of items, in the order of its text:
//
//   - a box: something that is never broken, a word or a piece of one;
//   - a space: a place where a line may end. A space that ends a line is on
//     neither that line nor the next;
//   - a break: a place inside a word where a line may end, at a price, with
//     something more shown at the end of the line if it does (a hyphen);
//   - a forced break: where the line ends whatever else is true.
//
// Widths are whole numbers in whatever unit the caller measures text in. The
// breaker knows nothing of fonts, text or pixels.
//
// The first lines of a paragraph may each have a width of their own, and the
// rest share one. That is how a paragraph's first line is indented, and how
// room is left beside a large first letter that stands several lines high.
//
// How even the lines can be made depends on the whole paragraph: one more
// word at its end can shift a little of every line before. So the best
// breaking is only known when the paragraph is over, and nothing is given
// out before then unless something makes it. Two things can. The breaker has
// room for only so many places where a line could end, and the caller may
// have room for only so many items. When the breaker's room runs out, the
// lines of the earlier half of what it holds are settled as they would be
// if the paragraph ended there; when the caller's does, as many as it asks
// for are. Those lines are given out, and the rest is worked out again from
// where they end. A paragraph that fits is broken the cheapest way there
// is; a longer one, the cheapest way within a stretch that moves along it.

namespace bn::layout {

// How breaking a paragraph has gone.
enum class BreakStatus : std::uint8_t {
  kOk = 0,
  kNotStarted,  // Begin() has not been called, or the paragraph is finished.
  kNoMemory,    // Fewer nodes than LineBreaker::kMinNodes.
  kBadWidths,   // A line's width is 0 or over kMaxWidth, or there are more first widths than kMaxFirstLines.
};

// The name of a status, for diagnostics: "ok", "not started", ...
const char* BreakStatusName(BreakStatus status) noexcept;

// How wide the lines of a paragraph may be.
struct LineWidths {
  // The paragraph's first lines, in order: as many of them as differ from
  // the rest. None, for a paragraph whose lines are all one width.
  std::span<const std::uint16_t> first;
  // Every line after those.
  std::uint16_t rest = 0;
};

// One line of a paragraph.
struct Line {
  // The items on the line: from `firstItem` up to, and not including,
  // `endItem`, counting the paragraph's items from 0 in the order they were
  // added. `endItem` is the space or the break the line ended at, or the
  // count of items if it ended with the paragraph. Spaces and breaks inside
  // the line are among its items; the space it ended at is not.
  std::uint32_t firstItem = 0;
  std::uint32_t endItem = 0;
  // How wide what is on the line is: its boxes and the spaces between them
  // at the widths they were given, and the hyphen if it ends at a break.
  std::uint32_t width = 0;
  // Which line of the paragraph: 0 for the first.
  std::uint32_t number = 0;
  // It ends at a break inside a word, and whatever that break shows (its
  // width is counted in `width`) is to be drawn after the last box.
  bool hyphenated = false;
  // It ends at a forced break or at the paragraph's end. Such a line is not
  // spread out to the full width when the others are.
  bool last = false;
  // It is wider than the line allows. That happens where a stretch of boxes
  // with nowhere to break is wider than the line it falls on, and the line
  // then holds that stretch and nothing else.
  bool overfull = false;
};

// What is told of a paragraph's lines as they are given out.
class LineSink {
 public:
  // Called for each line, in order. It must not call the breaker.
  virtual void OnLine(const Line& line) noexcept = 0;

 protected:
  ~LineSink() = default;  // Never destroyed through the interface.
};

// Breaks one paragraph after another.
//
// What a line costs, which is what "cheapest" means here:
//
//   - a line that is not a last line costs the square of the space left on
//     it: the width it was allowed less the width of what is on it;
//   - a line that ends at a break costs that break's penalty as well;
//   - a last line costs nothing;
//   - a line is only wider than it is allowed where no line that fits can
//     end at that place, and then costs kOverfullCost and more.
//
// What it does not do: squeeze spaces to get more on a line, charge for two
// hyphenated lines in a row, or mind a last line of one short word.
//
// Used by one task.
class LineBreaker final {
 public:
  // A place where a line could end, and the cheapest way found of getting
  // to it. The breaker's own business: the caller only provides an array of
  // them. 32 bytes each.
  struct Node {
    std::uint32_t start = 0;     // The width of everything before the line that would begin here.
    std::uint32_t end = 0;       // The width of everything before the place itself.
    std::uint32_t endItem = 0;   // The item a line ending here ends at.
    std::uint32_t nextItem = 0;  // The first item of the line that would begin here.
    std::uint32_t cost = 0;      // Of the cheapest way to break everything before here.
    std::uint32_t penalty = 0;   // What ending a line here costs besides.
    std::uint16_t extra = 0;     // What a line ending here shows after its last box.
    std::uint16_t parent = 0;    // The node where, that cheapest way, the line ending here began.
    std::uint8_t line = 0;       // How many lines come before here, counted no further than the first widths.
    std::uint8_t flags = 0;
  };

  // The fewest nodes it will work with, and a sensible number for ordinary
  // text. A place where a line could end takes one node, and a few more
  // near the start of a paragraph whose first lines have widths of their
  // own. There should be enough for several lines' worth of places: with
  // fewer than one line's worth, lines are ended before they are full.
  static constexpr std::size_t kMinNodes = 32;
  static constexpr std::size_t kUsualNodes = 256;
  // The most first lines that may have widths of their own.
  static constexpr std::size_t kMaxFirstLines = 8;
  // The widest a line may be.
  static constexpr std::uint16_t kMaxWidth = 30000;
  // The largest penalty a break can carry: a larger one is taken as this.
  static constexpr std::uint32_t kMaxPenalty = 100'000'000;
  // What a line that is too wide costs before its excess is counted: more
  // than any line that fits can cost.
  static constexpr std::uint32_t kOverfullCost = 1U << 30U;

  // `nodes` is the breaker's working memory, and the caller's to provide.
  explicit LineBreaker(std::span<Node> nodes) noexcept;

  LineBreaker(const LineBreaker&) = delete;
  LineBreaker& operator=(const LineBreaker&) = delete;
  ~LineBreaker() = default;

  // Starts a paragraph whose lines may be as wide as `widths` says, and
  // whose lines are to be given to `sink`, which must stay alive until
  // Finish() returns. A paragraph that was being broken is abandoned.
  [[nodiscard]] BreakStatus Begin(const LineWidths& widths, LineSink& sink) noexcept;

  // The paragraph's items, in the order of its text. Each counts as one
  // item, whatever becomes of it. Before Begin() these do nothing.

  // Something `width` wide that is never broken.
  void AddBox(std::uint16_t width) noexcept;
  // A space `width` wide, where a line may end. Spaces are expected one at a
  // time; a space straight after another is taken with it as one wider
  // space. A space at the start of the paragraph, or straight after a
  // forced break, is dropped, as one at the paragraph's end is.
  void AddSpace(std::uint16_t width) noexcept;
  // A place inside a word where a line may end. A line that ends here costs
  // `penalty` more and shows something `width` wide after its last box. One
  // with no box straight before it, or none straight after, is ignored.
  void AddBreak(std::uint16_t width, std::uint32_t penalty) noexcept;
  // The line ends here.
  void AddForcedBreak() noexcept;

  // For a caller that can hold no more items. Gives out lines until `item`
  // is the first item not on a line given out, or as near to that as it can:
  // the line being added to cannot be given out, and nothing is while all
  // that is left would go on one line. The lines are the ones the paragraph
  // would have if it ended with what has been added so far. They were chosen
  // without knowing what follows, so ask for no more than is needed.
  void Settle(std::uint32_t item) noexcept;

  // Ends the paragraph: every line not yet given out is given out. Returns
  // kOk, or kNotStarted if there was no paragraph to end.
  [[nodiscard]] BreakStatus Finish() noexcept;

  BreakStatus Status() const noexcept { return status_; }
  // How many items the paragraph has been given so far.
  std::uint32_t Items() const noexcept { return items_; }
  // The first item that is not yet on a line that has been given out.
  // Everything before it can be let go of.
  std::uint32_t Pending() const noexcept;
  // How many lines of this paragraph have been given out.
  std::uint32_t Lines() const noexcept { return lines_; }
  // How many of those were too wide.
  std::uint32_t OverfullLines() const noexcept { return overfull_; }
  // How many of them were given out before the paragraph was over, for want
  // of nodes or because Settle() asked. While this is 0 the lines are a
  // cheapest breaking of the whole paragraph.
  std::uint32_t SettledEarly() const noexcept { return settledEarly_; }
  // The most nodes this paragraph has had in use at once.
  std::size_t MostNodes() const noexcept { return mostNodes_; }

 private:
  // A space or a break that has been added and not yet acted on: whether a
  // line can end there depends on what comes next.
  enum class Waiting : std::uint8_t { kNothing, kSpace, kBreak };

  // A place where a line could end.
  struct Place {
    std::uint32_t end = 0;       // The width of everything before it.
    std::uint32_t start = 0;     // The width of everything before the line that would begin after it.
    std::uint32_t endItem = 0;   // The item it is.
    std::uint32_t nextItem = 0;  // The first item after it.
    std::uint32_t penalty = 0;
    std::uint16_t extra = 0;
    std::uint8_t flags = 0;
  };

  std::uint16_t WidthOf(std::uint8_t line) const noexcept;
  // Makes the nodes for a place, no more than `room` of them.
  void AddPlace(const Place& place, std::size_t room) noexcept;
  // Acts on the space or the break that was waiting, now that a box follows.
  void PlaceWaiting() noexcept;
  // Sees that there is room for the nodes of one more place.
  void MakeRoom() noexcept;
  // The cheapest of the nodes at the latest place.
  std::size_t CheapestAtLatestPlace() const noexcept;
  // The node the last line would begin at, the cheapest way, if the
  // paragraph ended with what has been added so far. The first node only if
  // there is no other.
  std::size_t CheapestOpenEnd() const noexcept;
  // Turns the chain of parents from `leaf` back to the first node round, so
  // that `parent` leads from each node on it to the next towards `leaf`.
  void TurnChainRound(std::size_t leaf) noexcept;
  // Gives out the lines from the first node to `root` along a chain that has
  // been turned round, makes `root` the first node, and works out again the
  // cheapest way to every place after it.
  void SettleTo(std::size_t root) noexcept;
  void GiveOut(const Node& from, const Node& to) noexcept;

  std::span<Node> nodes_;
  std::size_t count_ = 0;      // Nodes in use. The first is where the lines not yet given out begin.
  std::size_t barrier_ = 0;    // No line can begin before this node: a forced break is there.
  std::size_t lastPlace_ = 0;  // The first node of the latest place a line could end.
  std::size_t mostNodes_ = 0;

  LineSink* sink_ = nullptr;
  BreakStatus status_ = BreakStatus::kNotStarted;
  std::array<std::uint16_t, kMaxFirstLines> first_{};
  std::uint8_t firstCount_ = 0;
  std::uint16_t rest_ = 0;
  std::uint16_t widest_ = 0;

  std::uint32_t total_ = 0;  // The width of everything added since the first node's place.
  std::uint32_t items_ = 0;
  bool boxSincePlace_ = false;  // A box has come since the latest place a line could end.

  Waiting waiting_ = Waiting::kNothing;
  std::uint32_t waitingEnd_ = 0;   // The width of everything before it.
  std::uint32_t waitingItem_ = 0;  // Which item it is: the first, of spaces in a row.
  std::uint16_t waitingExtra_ = 0;
  std::uint32_t waitingPenalty_ = 0;

  std::uint32_t lines_ = 0;
  std::uint32_t overfull_ = 0;
  std::uint32_t settledEarly_ = 0;
};

}  // namespace bn::layout
