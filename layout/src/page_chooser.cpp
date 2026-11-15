#include "layout/page_chooser.h"

#include <algorithm>

namespace bn::layout {

namespace {

// What an entry's flags say of its line:
constexpr std::uint32_t kMayBegin = 0x01;  // a page may begin with it;
constexpr std::uint32_t kStarts = 0x02;    // it starts a page whatever is above it.

// A height far more than any page's, where PageHolding stops adding up: a
// step is two 16-bit numbers at the most, so a sum held to this never
// passes what 32 bits hold.
constexpr std::uint32_t kFarTooHigh = 1U << 30U;

constexpr bool Has(std::uint8_t flags, std::uint32_t flag) noexcept { return (std::uint32_t{flags} & flag) != 0; }

}  // namespace

// What the header says of its size.
static_assert(sizeof(PageFromBottom::Entry) == 12);

const char* PageStatusName(PageStatus status) noexcept {
  switch (status) {
    case PageStatus::kOk:
      return "ok";
    case PageStatus::kNotStarted:
      return "not started";
    case PageStatus::kNoMemory:
      return "no memory";
    case PageStatus::kBadHeight:
      return "bad height";
  }
  return "unknown";
}

// ---- Down from the first line ----

PageStatus PageFromTop::Begin(std::uint16_t height) noexcept {
  // Whatever page there was is gone, whether or not this one can be begun.
  over_ = false;
  tooTall_ = false;
  pageHeight_ = height;
  lines_ = 0;
  height_ = 0;
  top_ = 0;
  endLines_ = 0;
  endHeight_ = 0;
  status_ = height == 0 ? PageStatus::kBadHeight : PageStatus::kOk;
  return status_;
}

bool PageFromTop::Add(const PageLine& line) noexcept {
  if (status_ != PageStatus::kOk || over_) {
    return false;
  }
  if (lines_ == 0) {
    // The page's first line, which it holds whatever its height. The gap
    // above it is only there if it is a line that starts a page.
    top_ = line.startsPage ? line.gapBefore : 0U;
    height_ = top_ + line.height;
    lines_ = 1;
    tooTall_ = height_ > pageHeight_;
    return true;
  }
  if (line.startsPage) {
    over_ = true;
    return false;
  }
  const std::uint32_t top = height_ + line.gapBefore;
  const std::uint32_t bottom = top + line.height;
  if (bottom <= pageHeight_) {
    if (!line.keepWithPrevious) {
      // The page could end above this line.
      endLines_ = lines_;
      endHeight_ = height_;
    }
    top_ = top;
    height_ = bottom;
    ++lines_;
    return true;
  }
  // It does not fit. If it is kept with the line above, the page ends at
  // the latest place it could, and the lines after that go with this one.
  // If there is no such place, the lines kept together are more than a
  // page, and it ends here.
  over_ = true;
  if (line.keepWithPrevious && endLines_ != 0) {
    lines_ = endLines_;
    height_ = endHeight_;
  }
  return false;
}

// ---- Up from the last line ----

PageFromBottom::PageFromBottom(std::span<Entry> entries) noexcept : entries_(entries) {}

PageStatus PageFromBottom::Begin(std::uint16_t height) noexcept {
  // Whatever page there was is gone, whether or not this one can be begun.
  done_ = false;
  tooTall_ = false;
  cutShort_ = false;
  pageHeight_ = height;
  highest_ = Start{};
  mayBegin_ = Start{};
  topGap_ = 0;
  inBlock_ = false;
  cut_ = false;
  head_ = 0;
  size_ = 0;
  blockLines_ = 0;
  bottom_ = 0;
  room_ = 0;
  last_ = PageLine{};
  status_ = PageStatus::kOk;
  if (entries_.size() < kMinEntries) {
    status_ = PageStatus::kNoMemory;
  } else if (height == 0) {
    status_ = PageStatus::kBadHeight;
  }
  return status_;
}

void PageFromBottom::BeginBlock() noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return;
  }
  inBlock_ = true;
  cut_ = false;
  head_ = 0;
  size_ = 0;
  blockLines_ = 0;
  bottom_ = 0;
  // What this block's lines may take: the page, less the lines below and
  // the gap between those and this block's last line.
  room_ = static_cast<std::int32_t>(pageHeight_);
  if (highest_.lines != 0) {
    room_ -= static_cast<std::int32_t>(highest_.height) + static_cast<std::int32_t>(topGap_);
  }
}

void PageFromBottom::DropOldest() noexcept {
  head_ = (head_ + 1) % entries_.size();
  --size_;
  cut_ = true;
}

void PageFromBottom::Add(const PageLine& line) noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return;
  }
  if (!inBlock_) {
    BeginBlock();
  }
  // Places are counted down the block from its first line, in numbers that
  // go round when they pass 32 bits. Only the difference of two is ever
  // used, and no two that are compared are a page apart.
  const bool first = blockLines_ == 0;
  const std::uint32_t top = first ? 0U : bottom_ + line.gapBefore;
  bottom_ = top + line.height;
  ++blockLines_;
  last_ = line;

  if (line.startsPage && !first) {
    // Nothing above it in the block can be on its page.
    size_ = 0;
    cut_ = true;
  }
  if (size_ == entries_.size()) {
    // No entry to remember it by: the oldest is let go, and the page will
    // begin lower than it might have.
    DropOldest();
    cutShort_ = true;
  }
  Entry& entry = At(size_);
  ++size_;
  // If the page began with a line that starts a page, the gap above that
  // line would be on it: so that line is counted as beginning that much
  // higher.
  entry.top = line.startsPage ? top - line.gapBefore : top;
  entry.tag = line.tag;
  entry.gap = line.gapBefore;
  entry.flags = static_cast<std::uint8_t>((line.startsPage || !line.keepWithPrevious ? kMayBegin : 0U) |
                                          (line.startsPage ? kStarts : 0U));

  // A line stays only while everything from it to here fits. What is below
  // it only grows, so one that has stopped fitting never fits again.
  while (size_ != 0 && (room_ < 0 || bottom_ - At(0).top > static_cast<std::uint32_t>(room_))) {
    DropOldest();
  }
}

bool PageFromBottom::EndBlock() noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return true;
  }
  inBlock_ = false;
  if (blockLines_ == 0) {
    return false;  // Nothing in it: the block above is wanted as before.
  }
  blockLines_ = 0;

  if (size_ == 0) {
    if (highest_.lines == 0) {
      // Not even the page's last line fits. It has the page to itself.
      highest_.tag = last_.tag;
      highest_.lines = 1;
      highest_.height = std::uint32_t{last_.height} + (last_.startsPage ? last_.gapBefore : 0U);
      highest_.starts = last_.startsPage;
      mayBegin_ = highest_;
      tooTall_ = highest_.height > pageHeight_;
    }
    // Otherwise nothing of this block fits above what was taken already.
    done_ = true;
    return true;
  }

  // The lines that fit are the entries, and the highest of them is the
  // oldest. Below them are the lines taken before, after a gap.
  const std::uint32_t below = highest_.lines == 0 ? 0U : highest_.height + topGap_;
  const std::uint32_t linesBelow = highest_.lines;
  const Entry& oldest = At(0);
  highest_.tag = oldest.tag;
  highest_.lines = linesBelow + static_cast<std::uint32_t>(size_);
  highest_.height = (bottom_ - oldest.top) + below;
  highest_.starts = Has(oldest.flags, kStarts);
  topGap_ = oldest.gap;

  // The highest of them a page may begin with. If there is none, the one
  // found among the lines below stands.
  for (std::size_t n = 0; n < size_; ++n) {
    const Entry& entry = At(n);
    if (Has(entry.flags, kMayBegin)) {
      mayBegin_.tag = entry.tag;
      mayBegin_.lines = linesBelow + static_cast<std::uint32_t>(size_ - n);
      mayBegin_.height = (bottom_ - entry.top) + below;
      mayBegin_.starts = Has(entry.flags, kStarts);
      break;
    }
  }

  // The block above is wanted only if every line of this one is on the
  // page, and the first of them is not one that starts a page.
  done_ = cut_ || highest_.starts;
  return done_;
}

// ---- Round a line in the middle ----

PageStatus PageHolding::Begin(std::uint16_t height) noexcept {
  // Whatever page there was is gone, whether or not this one can be begun.
  done_ = false;
  pageHeight_ = height;
  asked_ = Start{};
  highest_ = Start{};
  chosen_ = Start{};
  topGap_ = 0;
  inBlock_ = false;
  blockLines_ = 0;
  blockHeight_ = 0;
  sinceBegin_ = 0;
  mayBegin_ = false;
  last_ = PageLine{};
  status_ = height == 0 ? PageStatus::kBadHeight : PageStatus::kOk;
  return status_;
}

void PageHolding::BeginBlock() noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return;
  }
  inBlock_ = true;
  blockLines_ = 0;
  blockHeight_ = 0;
  sinceBegin_ = 0;
  mayBegin_ = false;
}

void PageHolding::Add(const PageLine& line) noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return;
  }
  if (!inBlock_) {
    BeginBlock();
  }
  const bool first = blockLines_ == 0;
  const std::uint32_t step = std::uint32_t{line.height} + (first ? 0U : line.gapBefore);
  blockHeight_ = std::min(blockHeight_ + step, kFarTooHigh);
  if (first) {
    firstTag_ = line.tag;
    firstGap_ = line.gapBefore;
  }
  if (line.startsPage || !line.keepWithPrevious) {
    // A page may begin here, and this is the lowest such line so far. One
    // that starts a page brings the gap above it.
    mayBegin_ = true;
    beginTag_ = line.tag;
    beginLine_ = blockLines_;
    sinceBegin_ = std::uint32_t{line.height} + (line.startsPage ? line.gapBefore : 0U);
  } else if (mayBegin_) {
    sinceBegin_ = std::min(sinceBegin_ + step, kFarTooHigh);
  }
  ++blockLines_;
  last_ = line;
}

bool PageHolding::EndBlock() noexcept {
  if (status_ != PageStatus::kOk || done_) {
    return true;
  }
  inBlock_ = false;
  if (blockLines_ == 0) {
    return false;  // Nothing in it: the block above is wanted as before.
  }
  const std::uint32_t lines = blockLines_;
  blockLines_ = 0;

  const bool lowest = highest_.lines == 0;
  if (lowest) {
    // The line asked for is the last of the first block given.
    asked_.tag = last_.tag;
    asked_.lines = 1;
    asked_.height = std::uint32_t{last_.height} + (last_.startsPage ? last_.gapBefore : 0U);
  }
  const std::uint32_t below = lowest ? 0U : highest_.height + topGap_;

  if (mayBegin_) {
    // The nearest line above that a page may begin with. If the line asked
    // for is on the page that begins there, that is the page. It is if the
    // two are one line, whatever its height.
    const std::uint32_t height = sinceBegin_ + below;
    chosen_ = asked_;
    if (height <= pageHeight_ || (lowest && beginLine_ + 1 == lines)) {
      chosen_.tag = beginTag_;
      chosen_.lines = highest_.lines + (lines - beginLine_);
      chosen_.height = height;
    }
    done_ = true;
    return true;
  }

  // None of this block's lines may begin a page. If they and the lines
  // below are already more than a page, no line above will do either.
  const std::uint32_t height = blockHeight_ + below;
  if (height > pageHeight_) {
    chosen_ = asked_;
    done_ = true;
    return true;
  }
  highest_.tag = firstTag_;
  highest_.lines += lines;
  highest_.height = height;
  topGap_ = firstGap_;
  return false;
}

}  // namespace bn::layout
