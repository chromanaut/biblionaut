#include "layout/line_breaker.h"

#include <algorithm>
#include <limits>

#include "core/metrics.h"

namespace bn::layout {

namespace {

// Lines given out; those of them that were too wide; and those given out
// before their paragraph was over.
core::Counter linesBroken{"lines_broken"};
core::Counter linesOverfull{"lines_overfull"};
core::Counter linesSettledEarly{"lines_settled_early"};

// In a node's `parent`: it has none.
constexpr std::uint16_t kNone = 0xFFFF;

// What a node's flags say of its place,
constexpr std::uint32_t kHyphen = 0x01;  // a line ending there ends at a break inside a word;
constexpr std::uint32_t kLast = 0x02;    // a line ending there ends at a forced break or with the paragraph;
constexpr std::uint32_t kOfThePlace = kHyphen | kLast;
// and of the node itself:
constexpr std::uint32_t kOverfull = 0x04;     // the line that ends at it is too wide;
constexpr std::uint32_t kStartsPlace = 0x08;  // it is the first of its place's nodes.

constexpr std::uint32_t kCostCeiling = std::numeric_limits<std::uint32_t>::max();

// Widths are counted from the first node's place, and lines are settled to
// move that place on before the count passes this. Only a stretch with
// nowhere at all to end a line can take it further, and then it stops at
// the most 32 bits hold: such a stretch is too wide for any line, which is
// all that needs to be known of it.
constexpr std::uint32_t kFarEnough = 1U << 31U;
constexpr std::uint32_t kWidthCeiling = std::numeric_limits<std::uint32_t>::max();

// The flags are kept in a byte; these do the sums on them as plain unsigned
// numbers.
constexpr bool Has(std::uint8_t flags, std::uint32_t flag) noexcept { return (std::uint32_t{flags} & flag) != 0; }
constexpr std::uint8_t With(std::uint8_t flags, std::uint32_t flag) noexcept {
  return static_cast<std::uint8_t>(std::uint32_t{flags} | flag);
}
constexpr std::uint8_t Only(std::uint8_t flags, std::uint32_t mask) noexcept {
  return static_cast<std::uint8_t>(std::uint32_t{flags} & mask);
}

// A cost that has outgrown its 32 bits stays at the most they hold.
constexpr std::uint32_t Capped(std::uint64_t cost) noexcept {
  return cost > kCostCeiling ? kCostCeiling : static_cast<std::uint32_t>(cost);
}

// The cheapest line found so far that ends at a place, for one count of
// lines before it.
struct Best {
  bool found = false;
  std::uint32_t cost = 0;
  std::uint16_t parent = kNone;
};

void Consider(Best& best, std::uint64_t cost, std::size_t parent) noexcept {
  const std::uint32_t capped = Capped(cost);
  if (!best.found || capped < best.cost) {
    best.found = true;
    best.cost = capped;
    best.parent = static_cast<std::uint16_t>(parent);
  }
}

}  // namespace

// What the header says of its size.
static_assert(sizeof(LineBreaker::Node) == 32);
// A place makes a node for each count of lines there could be before it,
// and there has to be room for two places at the least.
static_assert(LineBreaker::kMinNodes >= 2 * (LineBreaker::kMaxFirstLines + 1));

const char* BreakStatusName(BreakStatus status) noexcept {
  switch (status) {
    case BreakStatus::kOk:
      return "ok";
    case BreakStatus::kNotStarted:
      return "not started";
    case BreakStatus::kNoMemory:
      return "no memory";
    case BreakStatus::kBadWidths:
      return "bad widths";
  }
  return "unknown";
}

// A node is named by a 16-bit number, with one value kept for "none".
LineBreaker::LineBreaker(std::span<Node> nodes) noexcept
    : nodes_(nodes.first(std::min<std::size_t>(nodes.size(), kNone))) {}

BreakStatus LineBreaker::Begin(const LineWidths& widths, LineSink& sink) noexcept {
  if (nodes_.size() < kMinNodes) {
    status_ = BreakStatus::kNoMemory;
    return status_;
  }
  status_ = BreakStatus::kBadWidths;
  if (widths.first.size() > kMaxFirstLines || widths.rest == 0 || widths.rest > kMaxWidth) {
    return status_;
  }
  std::uint16_t widest = widths.rest;
  for (std::size_t i = 0; i < widths.first.size(); ++i) {
    const std::uint16_t width = widths.first[i];
    if (width == 0 || width > kMaxWidth) {
      return status_;
    }
    first_[i] = width;
    widest = std::max(widest, width);
  }
  firstCount_ = static_cast<std::uint8_t>(widths.first.size());
  rest_ = widths.rest;
  widest_ = widest;
  sink_ = &sink;

  // The first node is the paragraph's start.
  nodes_[0] = Node{};
  nodes_[0].parent = kNone;
  nodes_[0].flags = With(std::uint8_t{0}, kStartsPlace);
  count_ = 1;
  barrier_ = 0;
  lastPlace_ = 0;
  mostNodes_ = 1;
  total_ = 0;
  items_ = 0;
  boxSincePlace_ = false;
  waiting_ = Waiting::kNothing;
  lines_ = 0;
  overfull_ = 0;
  settledEarly_ = 0;
  status_ = BreakStatus::kOk;
  return status_;
}

std::uint32_t LineBreaker::Pending() const noexcept {
  return status_ == BreakStatus::kOk ? nodes_[0].nextItem : items_;
}

std::uint16_t LineBreaker::WidthOf(std::uint8_t line) const noexcept {
  return line < firstCount_ ? first_[line] : rest_;
}

// ---- The items ----
//
// Whether a line can end at a space or a break depends on what follows it:
// only a box makes it a place worth ending a line. So each waits until the
// next item says, and nothing is made of one that a box does not follow.

void LineBreaker::AddBox(std::uint16_t width) noexcept {
  if (status_ != BreakStatus::kOk) {
    return;
  }
  if (waiting_ != Waiting::kNothing) {
    PlaceWaiting();
  }
  total_ += std::min<std::uint32_t>(width, kWidthCeiling - total_);
  boxSincePlace_ = true;
  ++items_;
}

void LineBreaker::AddSpace(std::uint16_t width) noexcept {
  if (status_ != BreakStatus::kOk) {
    return;
  }
  if (!boxSincePlace_) {
    // Nothing has come since the paragraph began, or since a line was made
    // to end: the space is dropped, and the line begins after it.
    for (std::size_t n = lastPlace_; n < count_; ++n) {
      nodes_[n].nextItem = items_ + 1;
    }
    ++items_;
    return;
  }
  if (waiting_ != Waiting::kSpace) {
    // A break that was waiting gives way to it: a line would not end at a
    // hyphen with a space to follow.
    waiting_ = Waiting::kSpace;
    waitingEnd_ = total_;
    waitingItem_ = items_;
    waitingExtra_ = 0;
    waitingPenalty_ = 0;
  }
  total_ += std::min<std::uint32_t>(width, kWidthCeiling - total_);
  ++items_;
}

void LineBreaker::AddBreak(std::uint16_t width, std::uint32_t penalty) noexcept {
  if (status_ != BreakStatus::kOk) {
    return;
  }
  if (boxSincePlace_ && waiting_ == Waiting::kNothing) {
    waiting_ = Waiting::kBreak;
    waitingEnd_ = total_;
    waitingItem_ = items_;
    waitingExtra_ = width;
    waitingPenalty_ = std::min(penalty, kMaxPenalty);
  }
  ++items_;
}

void LineBreaker::AddForcedBreak() noexcept {
  if (status_ != BreakStatus::kOk) {
    return;
  }
  MakeRoom();
  // A space straight before it is not on the line; a break straight before
  // it comes to nothing.
  const bool afterSpace = waiting_ == Waiting::kSpace;
  Place place;
  place.end = afterSpace ? waitingEnd_ : total_;
  place.start = total_;
  place.endItem = afterSpace ? waitingItem_ : items_;
  place.nextItem = items_ + 1;
  place.flags = With(std::uint8_t{0}, kLast);
  waiting_ = Waiting::kNothing;
  AddPlace(place, nodes_.size() - count_);
  // No line reaches across it.
  barrier_ = lastPlace_;
  boxSincePlace_ = false;
  ++items_;
}

void LineBreaker::PlaceWaiting() noexcept {
  MakeRoom();
  Place place;
  place.end = waitingEnd_;
  place.start = total_;
  place.endItem = waitingItem_;
  place.nextItem = items_;
  if (waiting_ == Waiting::kBreak) {
    place.extra = waitingExtra_;
    place.penalty = waitingPenalty_;
    place.flags = With(std::uint8_t{0}, kHyphen);
  }
  waiting_ = Waiting::kNothing;
  AddPlace(place, nodes_.size() - count_);
}

BreakStatus LineBreaker::Finish() noexcept {
  if (status_ != BreakStatus::kOk) {
    return BreakStatus::kNotStarted;
  }
  if (boxSincePlace_) {
    // The last line: what has come since the latest place a line could end.
    MakeRoom();
    const bool afterSpace = waiting_ == Waiting::kSpace;
    Place place;
    place.end = afterSpace ? waitingEnd_ : total_;
    place.start = total_;
    place.endItem = afterSpace ? waitingItem_ : items_;
    place.nextItem = items_;
    place.flags = With(std::uint8_t{0}, kLast);
    AddPlace(place, nodes_.size() - count_);
  }
  waiting_ = Waiting::kNothing;

  // The lines of the cheapest way to have got to the end.
  const std::size_t leaf = CheapestAtLatestPlace();
  TurnChainRound(leaf);
  for (std::size_t at = 0; at != leaf;) {
    const std::size_t next = nodes_[at].parent;
    GiveOut(nodes_[at], nodes_[next]);
    at = next;
  }
  status_ = BreakStatus::kNotStarted;
  return BreakStatus::kOk;
}

// ---- Places where a line could end ----
//
// The nodes are in the order of their places. Each has for a parent the
// node where, the cheapest way, the line that ends at it began; the first
// node, which has none, is where the lines not yet given out begin.

void LineBreaker::AddPlace(const Place& place, std::size_t room) noexcept {
  // One line could end here for each node it could begin at. Of the ones
  // with the same number of lines before them, only the cheapest is worth
  // keeping: whatever comes after would cost the same after any of them.
  std::array<Best, kMaxFirstLines + 1> best{};
  const bool last = Has(place.flags, kLast);
  bool any = false;
  for (std::size_t n = count_; n-- > barrier_;) {
    const Node& from = nodes_[n];
    const std::uint32_t width = place.end - from.start + place.extra;
    if (width > widest_) {
      break;  // Too much for any line, and every node before this one is further back still.
    }
    const std::uint16_t allowed = WidthOf(from.line);
    if (width > allowed) {
      continue;
    }
    const std::uint64_t slack = allowed - width;
    const std::uint64_t cost = from.cost + (last ? 0 : slack * slack) + place.penalty;
    Consider(best[std::min<std::size_t>(from.line + 1U, firstCount_)], cost, n);
    any = true;
  }

  std::uint8_t flags = place.flags;
  if (!any) {
    // Nothing fits, so the line is what has come since the latest place one
    // could end, however wide that is.
    flags = With(flags, kOverfull);
    for (std::size_t n = lastPlace_; n < count_; ++n) {
      const Node& from = nodes_[n];
      const std::uint32_t width = place.end - from.start + place.extra;
      const std::uint16_t allowed = WidthOf(from.line);
      const std::uint64_t over = width > allowed ? width - allowed : 0;
      const std::uint64_t cost =
          from.cost + kOverfullCost + std::min<std::uint64_t>(over * over, kOverfullCost - 1) + place.penalty;
      Consider(best[std::min<std::size_t>(from.line + 1U, firstCount_)], cost, n);
    }
  }

  // No more nodes than there is room for: the dearest are left out. That is
  // only ever needed when the places are being gone through again from a
  // later start, where a place can be got to with a count of lines it could
  // not be before.
  std::size_t found = 0;
  for (const Best& one : best) {
    found += one.found ? 1U : 0U;
  }
  for (; found > std::max<std::size_t>(room, 1); --found) {
    Best* dearest = nullptr;
    for (Best& one : best) {
      if (one.found && (dearest == nullptr || one.cost >= dearest->cost)) {
        dearest = &one;
      }
    }
    dearest->found = false;
  }

  const std::size_t firstNew = count_;
  for (std::size_t line = 0; line <= firstCount_; ++line) {
    if (!best[line].found) {
      continue;
    }
    Node& node = nodes_[count_];
    node.start = place.start;
    node.end = place.end;
    node.endItem = place.endItem;
    node.nextItem = place.nextItem;
    node.cost = best[line].cost;
    node.penalty = place.penalty;
    node.extra = place.extra;
    node.parent = best[line].parent;
    node.line = static_cast<std::uint8_t>(line);
    node.flags = count_ == firstNew ? With(flags, kStartsPlace) : flags;
    ++count_;
  }
  lastPlace_ = firstNew;
  mostNodes_ = std::max(mostNodes_, count_);
}

std::size_t LineBreaker::CheapestAtLatestPlace() const noexcept {
  // Of equals, the one with the fewest lines before it, which is the first.
  std::size_t cheapest = lastPlace_;
  for (std::size_t n = lastPlace_ + 1; n < count_; ++n) {
    if (nodes_[n].cost < nodes_[cheapest].cost) {
      cheapest = n;
    }
  }
  return cheapest;
}

// ---- Giving lines out ----

void LineBreaker::GiveOut(const Node& from, const Node& to) noexcept {
  Line line;
  line.firstItem = from.nextItem;
  line.endItem = to.endItem;
  line.width = to.end - from.start + to.extra;
  line.number = lines_;
  line.hyphenated = Has(to.flags, kHyphen);
  line.last = Has(to.flags, kLast);
  line.overfull = Has(to.flags, kOverfull);
  ++lines_;
  linesBroken.Add();
  if (line.overfull) {
    ++overfull_;
    linesOverfull.Add();
  }
  sink_->OnLine(line);
}

void LineBreaker::TurnChainRound(std::size_t leaf) noexcept {
  std::uint16_t towards = kNone;
  for (std::size_t at = leaf;;) {
    const std::uint16_t up = nodes_[at].parent;
    nodes_[at].parent = towards;
    if (up == kNone) {
      break;
    }
    towards = static_cast<std::uint16_t>(at);
    at = up;
  }
}

std::size_t LineBreaker::CheapestOpenEnd() const noexcept {
  // As if the paragraph ended with what has been added: its last line
  // would run from some node to there, and costs nothing if it fits. The
  // first node is left out, since no line ends at it to settle.
  const std::uint32_t end = waiting_ == Waiting::kNothing ? total_ : waitingEnd_;
  std::size_t cheapest = 0;
  for (std::size_t n = count_; n-- > std::max<std::size_t>(barrier_, 1);) {
    const Node& from = nodes_[n];
    const std::uint32_t width = end - from.start;
    if (width > widest_) {
      break;
    }
    if (width <= WidthOf(from.line) && (cheapest == 0 || from.cost < nodes_[cheapest].cost)) {
      cheapest = n;
    }
  }
  // Where what has come since the latest place fits on no line, that place
  // is where the last line would have to begin.
  return cheapest != 0 ? cheapest : CheapestAtLatestPlace();
}

void LineBreaker::MakeRoom() noexcept {
  // When the nodes run out, the lines of the earlier half of them are
  // settled, and one line at the least. The same is done if the widths have
  // been counted a very long way. If the only node is the first there is no
  // line to settle, and room enough.
  const std::size_t need = firstCount_ + 1U;
  while (count_ + need > nodes_.size() || total_ > kFarEnough) {
    const std::size_t leaf = CheapestOpenEnd();
    if (leaf == 0) {
      break;
    }
    TurnChainRound(leaf);
    std::size_t root = nodes_[0].parent;
    while (root != leaf && nodes_[root].parent <= count_ / 2) {
      root = nodes_[root].parent;
    }
    SettleTo(root);
  }
}

void LineBreaker::Settle(std::uint32_t item) noexcept {
  if (status_ != BreakStatus::kOk || nodes_[0].nextItem >= item) {
    return;
  }
  // While everything not yet given out would go on one line, the cheapest
  // thing is to leave it there. (The first node's place is where widths are
  // counted from.)
  const std::uint32_t end = waiting_ == Waiting::kNothing ? total_ : waitingEnd_;
  if (barrier_ == 0 && end <= WidthOf(nodes_[0].line)) {
    return;
  }
  const std::size_t leaf = CheapestOpenEnd();
  if (leaf == 0) {
    return;
  }
  TurnChainRound(leaf);
  std::size_t root = nodes_[0].parent;
  while (root != leaf && nodes_[root].nextItem < item) {
    root = nodes_[root].parent;
  }
  SettleTo(root);
}

void LineBreaker::SettleTo(std::size_t root) noexcept {
  for (std::size_t at = 0; at != root;) {
    const std::size_t next = nodes_[at].parent;
    GiveOut(nodes_[at], nodes_[next]);
    ++settledEarly_;
    linesSettledEarly.Add();
    at = next;
  }

  // `root` becomes the first node, and widths are counted from its place,
  // which keeps them small however long the paragraph goes on. What its
  // place's other nodes stood for, a different count of lines before it, is
  // no longer so.
  const Node base = nodes_[root];
  const std::size_t had = count_;
  std::size_t read = root + 1;
  while (read < had && !Has(nodes_[read].flags, kStartsPlace)) {
    ++read;
  }
  Node& first = nodes_[0];
  first = base;
  first.start = 0;
  first.end = 0;
  first.cost = 0;
  first.parent = kNone;
  first.flags = With(std::uint8_t{0}, kStartsPlace);
  count_ = 1;
  barrier_ = 0;
  lastPlace_ = 0;

  // Every place after it is gone through again, in order. The cheapest way
  // to each had been found among all the ways from the old first node, and
  // now has to be one that comes by way of the new. A place's new nodes go
  // where nodes already gone through were, so they are never more than
  // there is room for before the next place's.
  while (read < had) {
    const Node old = nodes_[read];
    ++read;
    while (read < had && !Has(nodes_[read].flags, kStartsPlace)) {
      ++read;
    }
    Place place;
    place.end = old.end - base.start;
    place.start = old.start - base.start;
    place.endItem = old.endItem;
    place.nextItem = old.nextItem;
    place.penalty = old.penalty;
    place.extra = old.extra;
    place.flags = Only(old.flags, kOfThePlace);
    AddPlace(place, read - count_);
    if (Has(place.flags, kLast)) {
      barrier_ = lastPlace_;
    }
  }
  total_ -= base.start;
  if (waiting_ != Waiting::kNothing) {
    waitingEnd_ -= base.start;
  }
}

}  // namespace bn::layout
