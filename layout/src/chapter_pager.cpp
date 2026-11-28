#include "layout/chapter_pager.h"

#include <algorithm>

namespace bn::layout {

namespace {

using epub::ItemKind;
using epub::SegmentBlock;
using epub::SegmentItem;
using epub::SegmentStatus;

// All of a block is given to the chooser.
constexpr std::uint32_t kNoLimit = 0xFFFFFFFFU;
// A height no page has room for under a line already on it: what a line is
// offered as when there is no record to keep it in.
constexpr std::uint16_t kTallerThanAnyPage = 65535;

constexpr bool Has(std::uint16_t bits, std::uint16_t bit) noexcept {
  return (std::uint32_t{bits} & std::uint32_t{bit}) != 0;
}

constexpr std::uint16_t AtLeastOne(std::uint16_t height) noexcept { return height == 0 ? std::uint16_t{1} : height; }

}  // namespace

// What the header says of its size.
static_assert(sizeof(ChapterLine) == 28);

const char* PagerStatusName(PagerStatus status) noexcept {
  switch (status) {
    case PagerStatus::kOk:
      return "ok";
    case PagerStatus::kMisused:
      return "misused";
    case PagerStatus::kNoMemory:
      return "no memory";
    case PagerStatus::kBadHeight:
      return "bad height";
    case PagerStatus::kNoSuchBlock:
      return "no such block";
    case PagerStatus::kReadFailed:
      return "read failed";
    case PagerStatus::kSetFailed:
      return "set failed";
  }
  return "unknown";
}

ChapterPager::ChapterPager(epub::SegmentReader& reader, ParagraphSetter& setter, TextMeasure& measure,
                           BlockSetting& setting, std::span<PageFromBottom::Entry> entries,
                           std::span<ChapterLine> lines) noexcept
    : reader_(reader), setter_(setter), measure_(measure), setting_(setting), lines_(lines), bottom_(entries) {}

// ---- Beginning and ending a piece of work ----

PagerStatus ChapterPager::Begin(Job job, std::uint16_t pageHeight) noexcept {
  if (job_ != Job::kNone || !reader_.IsOpen()) {
    return PagerStatus::kMisused;  // Whatever is under way goes on as it was.
  }
  page_ = ChapterPage{};
  step_ = StepWork{};
  feed_ = Feed::kIdle;
  placed_ = false;
  sink_ = nullptr;
  count_ = 0;
  seeking_ = false;
  hasPending_ = false;
  over_ = false;
  hasRefused_ = false;
  forced_ = false;
  given_ = false;
  toPass_ = 0;
  y_ = 0;
  stop_ = false;
  if (lines_.size() < kMinLines) {
    return Fail(PagerStatus::kNoMemory);
  }
  if (pageHeight == 0) {
    return Fail(PagerStatus::kBadHeight);
  }
  pageHeight_ = pageHeight;
  status_ = PagerStatus::kOk;
  job_ = job;
  return status_;
}

PagerStatus ChapterPager::Fail(PagerStatus status) noexcept {
  status_ = status;
  job_ = Job::kNone;
  feed_ = Feed::kIdle;
  placed_ = false;
  return status_;
}

void ChapterPager::FailRead(SegmentStatus status) noexcept {
  readerStatus_ = status;
  Fail(PagerStatus::kReadFailed);
}

void ChapterPager::Done() noexcept {
  status_ = PagerStatus::kOk;
  job_ = Job::kNone;
  feed_ = Feed::kIdle;
}

void ChapterPager::Forget() noexcept {
  remembered_ = {};
  rememberNext_ = 0;
  placed_ = false;
}

PagerStatus ChapterPager::BeginDown(ChapterPlace first, std::uint16_t pageHeight) noexcept {
  if (const PagerStatus status = Begin(Job::kDown, pageHeight); status != PagerStatus::kOk) {
    return status;
  }
  asked_ = first;
  BeginFillingDown(first);
  return status_;
}

PagerStatus ChapterPager::BeginUp(ChapterPlace before, std::uint16_t pageHeight) noexcept {
  if (const PagerStatus status = Begin(Job::kUpFind, pageHeight); status != PagerStatus::kOk) {
    return status;
  }
  if (bottom_.Begin(pageHeight) != PageStatus::kOk) {
    return Fail(PagerStatus::kNoMemory);
  }
  asked_ = before;
  if (before.block >= reader_.Blocks()) {
    // The chapter's end: nothing of a block there, and everything above.
    block_ = reader_.Blocks();
    limit_ = 0;
  } else {
    block_ = before.block;
    limit_ = before.into;
  }
  NextUpBlock();
  return status_;
}

PagerStatus ChapterPager::BeginAbout(ChapterPlace place, std::uint16_t pageHeight) noexcept {
  if (const PagerStatus status = Begin(Job::kAboutFind, pageHeight); status != PagerStatus::kOk) {
    return status;
  }
  if (place.block >= reader_.Blocks()) {
    return Fail(PagerStatus::kNoSuchBlock);
  }
  (void)holding_.Begin(pageHeight);
  asked_ = place;
  firstGiven_ = place.block;
  seeking_ = true;
  holding_.BeginBlock();
  StartBlock(place.block);
  return status_;
}

PagerStatus ChapterPager::BeginLines(std::uint32_t block, ChapterLineSink& sink) noexcept {
  if (const PagerStatus status = Begin(Job::kLines, 1); status != PagerStatus::kOk) {
    return status;
  }
  if (block >= reader_.Blocks()) {
    return Fail(PagerStatus::kNoSuchBlock);
  }
  sink_ = &sink;
  StartBlock(block);
  return status_;
}

bool ChapterPager::Step() noexcept {
  if (job_ == Job::kNone) {
    return false;
  }
  step_ = StepWork{};
  FeedStep();
  return job_ != Job::kNone;
}

// ---- Reading a block and setting it ----

void ChapterPager::Remember(std::uint32_t block, bool keepWithNext, std::uint16_t spaceAfter) noexcept {
  Remembered* slot = nullptr;
  for (Remembered& entry : remembered_) {
    if (entry.known && entry.block == block) {
      slot = &entry;
    }
  }
  if (slot == nullptr) {
    slot = &remembered_[rememberNext_];
    rememberNext_ = (rememberNext_ + 1) % remembered_.size();
  }
  *slot = {.known = true, .keepWithNext = keepWithNext, .spaceAfter = spaceAfter, .block = block};
}

const ChapterPager::Remembered* ChapterPager::Recall(std::uint32_t block) const noexcept {
  for (const Remembered& entry : remembered_) {
    if (entry.known && entry.block == block) {
      return &entry;
    }
  }
  return nullptr;
}

void ChapterPager::StartBlock(std::uint32_t block) noexcept {
  block_ = block;
  stop_ = false;
  blockLines_ = 0;
  somethingSeen_ = false;
  text_ = {};
  picture_ = {};
  keptWithBefore_ = false;
  spaceAbove_ = 0;
  if (block == 0) {
    GoTo(block, Feed::kHead);
    return;
  }
  if (const Remembered* before = Recall(block - 1); before != nullptr) {
    keptWithBefore_ = before->keepWithNext;
    spaceAbove_ = before->spaceAfter;
    GoTo(block, Feed::kHead);
    return;
  }
  GoTo(block - 1, Feed::kBeforeHead);
}

void ChapterPager::GoTo(std::uint32_t block, Feed then) noexcept {
  then_ = then;
  goingTo_ = block;
  if (placed_ && nextBlock_ == block) {
    feed_ = then;
    return;
  }
  placed_ = false;
  // By the index; or, for a block past the last that is indexed, from that
  // one on.
  const std::uint32_t indexed = reader_.IndexedBlocks();
  if (indexed == 0) {
    FailRead(SegmentStatus::kDamaged);
    return;
  }
  passing_ = std::min(block, indexed - 1);
  if (const SegmentStatus status = reader_.BeginFindBlock(passing_); status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  feed_ = Feed::kFinding;
}

void ChapterPager::StepFinding() noexcept {
  ++step_.readerCalls;
  if (reader_.Step()) {
    return;
  }
  if (reader_.Status() != SegmentStatus::kOk) {
    FailRead(reader_.Status());
    return;
  }
  const epub::SegmentPlace& place = reader_.Place();
  if (!place.found || place.atEnd) {
    FailRead(SegmentStatus::kDamaged);
    return;
  }
  if (const SegmentStatus status = reader_.Seek(place); status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  ++blocksFound_;
  if (passing_ == goingTo_) {
    placed_ = true;
    nextBlock_ = goingTo_;
    feed_ = then_;
  } else {
    feed_ = Feed::kPassHead;
  }
}

void ChapterPager::StepPassing() noexcept {
  ++step_.readerCalls;
  if (feed_ == Feed::kPassHead) {
    SegmentBlock head;
    const SegmentStatus status = reader_.ReadBlock(head);
    if (status == SegmentStatus::kOk) {
      feed_ = Feed::kPassItems;
    } else if (status != SegmentStatus::kMore) {
      FailRead(status);
    }
    return;
  }
  SegmentItem item;
  const SegmentStatus status = reader_.NextItem(item);
  if (status == SegmentStatus::kMore) {
    return;
  }
  if (status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  if (item.kind != ItemKind::kEnd) {
    return;
  }
  ++passing_;
  ++blocksPassed_;
  if (passing_ == goingTo_) {
    placed_ = true;
    nextBlock_ = goingTo_;
    feed_ = then_;
  } else {
    feed_ = Feed::kPassHead;
  }
}

void ChapterPager::StepBeforeHead() noexcept {
  ++step_.readerCalls;
  SegmentBlock head;
  const SegmentStatus status = reader_.ReadBlock(head);
  if (status == SegmentStatus::kMore) {
    return;
  }
  if (status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  BlockMeasures measures;
  setting_.MeasureBlock(head, measures);
  keptWithBefore_ = Has(head.style.breaking, epub::kBreakKeepWithNext);
  spaceAbove_ = measures.spaceAfter;
  Remember(block_ - 1, keptWithBefore_, spaceAbove_);
  ++headsRead_;
  placed_ = false;
  if (block_ >= reader_.IndexedBlocks()) {
    // The index has no place for the block: it is come to by reading the
    // one before it through.
    passing_ = block_ - 1;
    goingTo_ = block_;
    then_ = Feed::kHead;
    feed_ = Feed::kPassItems;
    return;
  }
  GoTo(block_, Feed::kHead);
}

void ChapterPager::StepHead() noexcept {
  ++step_.readerCalls;
  const SegmentStatus status = reader_.ReadBlock(head_);
  if (status == SegmentStatus::kMore) {
    return;
  }
  if (status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  placed_ = false;
  measures_ = BlockMeasures{};
  setting_.MeasureBlock(head_, measures_);
  Remember(block_, Has(head_.style.breaking, epub::kBreakKeepWithNext), measures_.spaceAfter);
  lineHeight_ = AtLeastOne(measures_.lineHeight);
  firstWidth_[0] = measures_.firstWidth;
  const bool ownFirst = measures_.firstWidth != measures_.restWidth;
  const ParagraphSettings settings{
      .widths = {.first = ownFirst ? std::span<const std::uint16_t>(firstWidth_) : std::span<const std::uint16_t>(),
                 .rest = measures_.restWidth},
      .hyphenPenalty = measures_.hyphenPenalty};
  if (const ParagraphStatus begun = setter_.Begin(settings, measure_, *this); begun != ParagraphStatus::kOk) {
    setterStatus_ = begun;
    Fail(PagerStatus::kSetFailed);
    return;
  }
  RunMeasures run;
  setting_.MeasureRun(head_, epub::RunStyle{}, run);
  style_ = run.style;
  setter_.UseHyphenator(run.hyphenator);
  ++blocksSet_;
  feed_ = Feed::kItems;
}

void ChapterPager::StepItems() noexcept {
  ++step_.readerCalls;
  SegmentItem item;
  const SegmentStatus status = reader_.NextItem(item);
  if (status == SegmentStatus::kMore) {
    return;
  }
  if (status != SegmentStatus::kOk) {
    FailRead(status);
    return;
  }
  switch (item.kind) {
    case ItemKind::kText:
      text_ = item.text;
      if (!text_.empty()) {
        somethingSeen_ = true;
        feed_ = Feed::kText;
      }
      break;
    case ItemKind::kStyle: {
      RunMeasures run;
      setting_.MeasureRun(head_, item.style, run);
      style_ = run.style;
      setter_.UseHyphenator(run.hyphenator);
      break;
    }
    case ItemKind::kPicture:
      if (head_.kind == static_cast<std::uint8_t>(epub::SegmentBlockKind::kPicture) && !somethingSeen_) {
        pictureNumber_ = item.picture;
        pictureWidth_ = item.width;
        pictureHeight_ = item.height;
        feed_ = Feed::kPictureSize;
      } else {
        picture_ = std::string_view(&kPictureByte, 1);
        feed_ = Feed::kPictureByte;
      }
      somethingSeen_ = true;
      break;
    case ItemKind::kEnd:
      placed_ = true;
      nextBlock_ = block_ + 1;
      feed_ = Feed::kFinish;
      break;
    case ItemKind::kLinkStart:
    case ItemKind::kLinkEnd:
      break;  // Where a link begins and ends is nothing to a page.
  }
}

void ChapterPager::GiveSetter(std::string_view& text) noexcept {
  ++step_.setterCalls;
  text.remove_prefix(setter_.AddText(text, style_));
}

void ChapterPager::FeedStep() noexcept {
  switch (feed_) {
    case Feed::kIdle:
      Fail(PagerStatus::kMisused);
      return;
    case Feed::kFinding:
      StepFinding();
      return;
    case Feed::kPassHead:
    case Feed::kPassItems:
      StepPassing();
      return;
    case Feed::kBeforeHead:
      StepBeforeHead();
      return;
    case Feed::kHead:
      StepHead();
      return;
    case Feed::kItems:
      StepItems();
      return;
    case Feed::kPictureSize: {
      epub::SegmentPicture sized;
      bool found = false;
      if (pictureNumber_ != epub::kSegmentNoPicture) {
        ++step_.readerCalls;
        if (const SegmentStatus status = reader_.ReadPicture(pictureNumber_, sized, found);
            status != SegmentStatus::kOk) {
          FailRead(status);
          return;
        }
      }
      lineHeight_ = AtLeastOne(setting_.PictureHeight(head_, pictureWidth_, pictureHeight_, found ? &sized : nullptr));
      picture_ = std::string_view(&kPictureByte, 1);
      feed_ = Feed::kPictureByte;
      return;
    }
    case Feed::kText:
      GiveSetter(text_);
      if (text_.empty()) {
        feed_ = Feed::kItems;
      }
      break;
    case Feed::kPictureByte:
      GiveSetter(picture_);
      if (picture_.empty()) {
        feed_ = Feed::kItems;
      }
      break;
    case Feed::kFinish:
      ++step_.setterCalls;
      if (setter_.Finish()) {
        if (blockLines_ == 0 && !stop_) {
          // A rule, or a block of nothing but spaces: one line, of nothing.
          OnLine(TextLine{.last = true});
        }
        BlockOver(!stop_);
        return;
      }
      break;
  }
  if (stop_) {
    BlockOver(false);
  }
}

void ChapterPager::OnLine(const TextLine& line) noexcept {
  ++step_.lines;
  if (stop_) {
    return;
  }
  ChapterLine made;
  made.block = block_;
  made.begin = line.begin;
  made.end = line.end;
  made.width = line.width;
  made.height = lineHeight_;
  made.spaces = line.spaces;
  std::uint32_t flags = (line.hyphen ? std::uint32_t{kLineHyphen} : 0U) | (line.last ? std::uint32_t{kLineLast} : 0U) |
                        (line.overfull ? std::uint32_t{kLineOverfull} : 0U);
  const std::uint16_t breaking = head_.style.breaking;
  if (blockLines_ == 0) {
    // The block's first line: the space above the block is above it, it is
    // kept with the line before if the block before asks that, and it
    // starts a page if the block does or the chapter begins with it.
    made.gap = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(std::uint32_t{measures_.spaceBefore} + std::uint32_t{spaceAbove_}, 65535U));
    flags |= keptWithBefore_ ? std::uint32_t{kLineKept} : 0U;
    flags |= block_ == 0 || Has(breaking, epub::kBreakPageBefore) ? std::uint32_t{kLineStarts} : 0U;
  } else {
    flags |= Has(breaking, epub::kBreakKeepTogether) ? std::uint32_t{kLineKept} : 0U;
  }
  made.flags = static_cast<std::uint8_t>(flags);
  ++blockLines_;
  TakeLine(made);
}

// ---- What is made of a block's lines ----

PageLine ChapterPager::PageLineOf(const ChapterLine& line, std::uint32_t tag) noexcept {
  return {.height = line.height,
          .gapBefore = line.gap,
          .keepWithPrevious = line.Is(kLineKept),
          .startsPage = line.Is(kLineStarts),
          .tag = tag};
}

void ChapterPager::TakeLine(const ChapterLine& line) noexcept {
  switch (job_) {
    case Job::kNone:
      return;
    case Job::kLines:
      sink_->OnChapterLine(line);
      return;
    case Job::kDown:
      if (!seeking_) {
        Offer(line);
      } else if (hasPending_ && line.begin > fillFirst_.into) {
        // The line before this one holds the place: the page begins there.
        seeking_ = false;
        Offer(pending_);
        Offer(line);
      } else {
        pending_ = line;
        hasPending_ = true;
      }
      return;
    case Job::kUpFind:
      if (line.begin >= limit_) {
        stop_ = true;  // The page ends above this line.
        return;
      }
      bottom_.Add(PageLineOf(line, line.begin));
      return;
    case Job::kUpFill:
      if (line.block == fillFirst_.block && line.begin < fillFirst_.into) {
        return;  // Above the page.
      }
      if (line.block == asked_.block && line.begin >= asked_.into) {
        stop_ = true;
        return;
      }
      if (toPass_ != 0) {
        --toPass_;
        return;
      }
      if (count_ == lines_.size()) {
        stop_ = true;  // More lines than the chooser counted: none to spare for them.
        return;
      }
      lines_[count_] = line;
      lines_[count_].top = count_ == 0 ? 0U : y_ + line.gap;
      y_ = lines_[count_].top + line.height;
      ++count_;
      return;
    case Job::kAboutFind:
      if (!seeking_) {
        holding_.Add(PageLineOf(line, line.begin));
      } else if (hasPending_ && line.begin > asked_.into) {
        stop_ = true;  // The line before this one holds the place.
      } else {
        if (hasPending_) {
          holding_.Add(PageLineOf(pending_, pending_.begin));
        }
        pending_ = line;
        hasPending_ = true;
      }
      return;
  }
}

void ChapterPager::BlockOver(bool whole) noexcept {
  switch (job_) {
    case Job::kNone:
      return;
    case Job::kLines:
      Done();
      return;
    case Job::kDown:
      if (seeking_) {
        // The block's last line holds the place.
        seeking_ = false;
        if (hasPending_) {
          Offer(pending_);
        }
      }
      if (over_ || block_ + 1 >= reader_.Blocks()) {
        FinishDown();
      } else {
        StartBlock(block_ + 1);
      }
      return;
    case Job::kUpFind:
      if (bottom_.EndBlock()) {
        FinishUpFind();
      } else {
        limit_ = 0;
        NextUpBlock();
      }
      return;
    case Job::kUpFill:
      // The page ends in the block in hand, or with it: where that is the
      // block of the place asked for, where the place is the first
      // character of the next, and where the chapter ends.
      if (!whole || block_ >= asked_.block || block_ + 1 >= reader_.Blocks() ||
          (block_ + 1 == asked_.block && asked_.into == 0)) {
        FinishUpFill();
      } else {
        StartBlock(block_ + 1);
      }
      return;
    case Job::kAboutFind:
      if (seeking_) {
        seeking_ = false;
        if (hasPending_) {
          holding_.Add(PageLineOf(pending_, pending_.begin));
          fillFirst_ = pending_.Place();
        }
      }
      if (holding_.EndBlock() || block_ == 0) {
        FinishAboutFind();
      } else {
        holding_.BeginBlock();
        StartBlock(block_ - 1);
      }
      return;
  }
}

// ---- Down from a line ----

void ChapterPager::BeginFillingDown(ChapterPlace first) noexcept {
  job_ = Job::kDown;
  (void)top_.Begin(pageHeight_);
  fillFirst_ = first;
  count_ = 0;
  over_ = false;
  hasRefused_ = false;
  forced_ = false;
  hasPending_ = false;
  if (first.block >= reader_.Blocks()) {
    FinishDown();
    return;
  }
  seeking_ = true;
  StartBlock(first.block);
}

void ChapterPager::Offer(const ChapterLine& line) noexcept {
  if (over_) {
    return;
  }
  // With no record to keep it in, the line is offered as one that cannot
  // fit: the page then ends as it would have if it had been full, lines
  // that are kept together left for the next page together. A line of
  // that height is refused whatever the page's, there being a line of a
  // pixel or more above it.
  const bool forced = count_ == lines_.size();
  PageLine offered = PageLineOf(line, 0);
  // Whether the page would have taken the line, had there been a record.
  const bool fits =
      !offered.startsPage && top_.Height() + offered.gapBefore + offered.height <= std::uint32_t{pageHeight_};
  if (forced) {
    offered.height = kTallerThanAnyPage;
  }
  if (top_.Add(offered)) {
    lines_[count_] = line;
    lines_[count_].top = top_.Top();
    ++count_;
    return;
  }
  over_ = true;
  forced_ = forced && fits;
  hasRefused_ = true;
  refused_ = line.Place();
  stop_ = true;
}

void ChapterPager::FinishDown() noexcept {
  const std::uint32_t lines = top_.Lines();
  page_.found = lines != 0;
  page_.filledDown = true;
  page_.cutShort = forced_;
  page_.first = lines != 0 ? lines_[0].Place() : fillFirst_;
  page_.next = lines < count_ ? lines_[lines].Place() : hasRefused_ ? refused_ : End();
  page_.height = top_.Height();
  page_.lines = lines;
  Done();
}

// ---- Up to a line ----

void ChapterPager::NextUpBlock() noexcept {
  if (limit_ == 0) {
    // Nothing of the block in hand is above the page's end: the block
    // above it, whole.
    if (block_ == 0) {
      FinishUpFind();
      return;
    }
    --block_;
    limit_ = kNoLimit;
  }
  if (!given_) {
    given_ = true;
    firstGiven_ = block_;
  }
  bottom_.BeginBlock();
  StartBlock(block_);
}

void ChapterPager::FinishUpFind() noexcept {
  if (!given_ || !bottom_.Found()) {
    // Nothing is above the chapter's first line.
    page_.filledDown = false;
    page_.first = asked_;
    page_.next = asked_;
    Done();
    return;
  }
  const ChapterPlace first{.block = firstGiven_ - bottom_.FirstBlock(), .into = bottom_.FirstTag()};
  if (bottom_.AtPageStart()) {
    // It came to a line that starts a page: that line's page, filled down.
    BeginFillingDown(first);
    return;
  }
  job_ = Job::kUpFill;
  fillFirst_ = first;
  count_ = 0;
  y_ = 0;
  toPass_ = bottom_.Lines() > lines_.size() ? bottom_.Lines() - static_cast<std::uint32_t>(lines_.size()) : 0U;
  forced_ = toPass_ != 0;
  StartBlock(first.block);
}

void ChapterPager::FinishUpFill() noexcept {
  page_.found = count_ != 0;
  page_.filledDown = false;
  page_.cutShort = forced_ || bottom_.CutShort();
  page_.first = count_ != 0 ? lines_[0].Place() : fillFirst_;
  page_.next = asked_;
  page_.height = y_;
  page_.lines = count_;
  Done();
}

// ---- About a place ----

void ChapterPager::FinishAboutFind() noexcept {
  const ChapterPlace first =
      holding_.Found() ? ChapterPlace{.block = firstGiven_ - holding_.FirstBlock(), .into = holding_.FirstTag()}
                       : fillFirst_;
  BeginFillingDown(first);
}

}  // namespace bn::layout
