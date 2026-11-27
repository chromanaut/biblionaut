#include "reader/layout_check.h"

#include <algorithm>
#include <span>

#include "core/metrics.h"
#include "core/trace.h"
#include "log_line.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

using core::StorageStatus;
using layout::LineBreaker;
using layout::ParagraphSetter;
using layout::ParagraphStatus;

// 1 if the last layout check passed, 0 if it failed.
core::Gauge layoutCheck{"layout_check"};
// How fast the last check set the text on the card, the first way, in KB
// (of 1024 bytes) of text a second.
core::Gauge layoutKbPerSecond{"layout_kb_per_s"};
// The longest call into the setter when every call was timed, in
// microseconds.
core::Gauge layoutLongestCallUs{"layout_longest_call_us"};

// The node counts the breaker is timed with.
constexpr std::array<std::size_t, 3> kBreakerNodes = {64, 256, LayoutCheck::kMostNodes};
// Items of the breaker's dearest paragraph given at one step.
constexpr std::size_t kItemsPerStep = 256;

// The text made to be awkward for the setter, each set at two widths: one
// run of bytes with no space in it; soft hyphens end to end; line feeds by
// the hundred; hyphens by the thousand; and spaces by the thousand.
constexpr std::size_t kAwkwardTexts = 5;
constexpr std::array<std::uint16_t, 2> kAwkwardWidths = {40, LayoutCheck::kWidth};

std::uint32_t AwkwardLength(std::size_t which) noexcept {
  constexpr std::array<std::uint32_t, kAwkwardTexts> kLengths = {5000, 1500, 700, 3001, 3002};
  return kLengths[which];
}

char AwkwardByte(std::size_t which, std::uint32_t at) noexcept {
  switch (which) {
    case 0:
      return 'a';
    case 1: {
      constexpr std::array<char, 3> kSoftHyphened = {'a', '\xC2', '\xAD'};
      return kSoftHyphened[at % 3];
    }
    case 2:
      return '\n';
    case 3:
      return at < 3000 ? '-' : 'a';
    default:
      return at == 0 ? 'a' : (at == 3001 ? 'b' : ' ');
  }
}

// Long words for the hyphenator, each found this many times at a step.
constexpr std::size_t kWordRepeats = 64;
constexpr std::array<std::string_view, 8> kLongWords = {
    "pneumonoultramicroscopicsilicovolcanoconiosis",
    "antidisestablishmentarianism",
    "supercalifragilisticexpialidocious",
    "incomprehensibilities",
    "Honorificabilitudinitatibus",
    "hyphenationhyphenationhyphenationhyphenationhyphenationhyphenat",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "\xC3\xA9lectroenc\xC3\xA9phalographiquement",
};

// Made-up text the face is timed over: words of letters of every width,
// with a space after each.
constexpr std::string_view kFaceText =
    "The lamp was lit before anyone had asked for it, and the harbour went on with its evening. ";

}  // namespace

void LayoutCheck::Lines::OnLine(const layout::TextLine& line) noexcept {
  digest.Add(line);
  hyphens += line.hyphen ? 1U : 0U;
  tooWide += line.overfull ? 1U : 0U;
}

LayoutCheck::LayoutCheck(core::Storage& storage, core::Clock& clock, core::TraceStream& stream,
                         std::span<const std::uint8_t> builtIn) noexcept
    : storage_(storage),
      clock_(clock),
      stream_(stream),
      builtIn_(builtIn),
      setter_(std::span(nodes_).first(kNodes), records_),
      file_(fileBuffer_) {}

void LayoutCheck::Begin() noexcept {
  if (state_ == State::kRunning) {
    return;
  }
  state_ = State::kRunning;
  failed_ = false;
  longestStepUs_ = 0;
  longestStep_ = Step::kNone;
  faceUs_ = 0;
  breakerRuns_ = {};
  breaker_.reset();
  tableOpen_ = false;
  tableBuiltIn_ = false;
  tableSize_ = 0;
  tableUs_ = 0;
  setterRun_ = {};
  wordRun_ = {};
  awkwardAt_ = 0;
  passes_ = {};
  passCount_ = 0;
  passIndex_ = 0;
  timing_ = nullptr;
  timeEveryCall_ = false;
  file_.Close();
  beganTrace_ = BN_TRACE_NOW();
  MoveTo(Step::kTimeFace);
}

const char* LayoutCheck::LongestStepName() const noexcept {
  switch (longestStep_) {
    case Step::kNone:
      return "none";
    case Step::kTimeFace:
      return "timing the face";
    case Step::kBreak:
      return "breaking the dearest paragraph";
    case Step::kOpenTable:
      return "opening the table";
    case Step::kReadTable:
      return "reading the table";
    case Step::kSetAwkward:
      return "setting awkward text";
    case Step::kFindWords:
      return "hyphenating long words";
    case Step::kOpenText:
      return "opening the text";
    case Step::kReadText:
      return "reading the text";
    case Step::kSetText:
      return "setting the text";
    case Step::kEndPass:
      return "ending a setting";
  }
  return "unknown";
}

void LayoutCheck::Tick() noexcept {
  if (state_ != State::kRunning) {
    return;
  }
  std::uint64_t now = clock_.NowUs();
  const std::uint64_t sliceEnds = now + kSliceUs;
  do {
    const Step step = step_;
    DoStep();
    const std::uint64_t after = clock_.NowUs();
    const std::uint64_t took = after - now;
    if (took > longestStepUs_) {
      longestStepUs_ = took;
      longestStep_ = step;
    }
    if (step == Step::kSetText) {
      Current().setUs += took;
      paragraphUs_ += took;
      if (paragraphDone_) {
        if (paragraphUs_ >= Current().longestParagraphUs) {
          Current().longestParagraphUs = paragraphUs_;
          Current().longestParagraphBytes = lastParagraphBytes_;
          Current().longestParagraphLines = lastParagraphLines_;
        }
        paragraphUs_ = 0;
        paragraphDone_ = false;
      }
    } else if (step == Step::kReadText) {
      Current().cardUs += took;
    } else if (step == Step::kReadTable) {
      tableUs_ += took;
    }
    now = after;
  } while (state_ == State::kRunning && now < sliceEnds);
}

void LayoutCheck::DoStep() noexcept {
  switch (step_) {
    case Step::kNone:
      break;
    case Step::kTimeFace:
      TimeFace();
      break;
    case Step::kBreak:
      Break();
      break;
    case Step::kOpenTable:
      OpenTable();
      break;
    case Step::kReadTable:
      ReadTable();
      break;
    case Step::kSetAwkward:
      SetAwkward();
      break;
    case Step::kFindWords:
      FindWords();
      break;
    case Step::kOpenText:
      OpenText();
      break;
    case Step::kReadText:
      ReadText();
      break;
    case Step::kSetText:
      SetText();
      break;
    case Step::kEndPass:
      EndPass();
      break;
  }
}

void LayoutCheck::MoveTo(Step step) noexcept {
  step_ = step;
  count_ = 0;
}

// ---- The face ----

void LayoutCheck::TimeFace() noexcept {
  if (count_ == 0) {
    for (std::size_t i = 0; i < chunk_.size(); ++i) {
      chunk_[i] = static_cast<std::uint8_t>(kFaceText[i % kFaceText.size()]);
    }
  }
  // A word at a time, with the space after it, as the setter measures.
  layout::MadeUpFace face;
  const std::string_view text(
      reinterpret_cast<const char*>(chunk_.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      chunk_.size());
  std::array<std::uint16_t, 1> widths{};
  std::array<std::uint16_t, 1> hyphens{};
  const std::uint64_t began = clock_.NowUs();
  std::size_t from = 0;
  while (from < text.size()) {
    std::size_t end = text.find(' ', from);
    end = end == std::string_view::npos ? text.size() : end + 1;
    const std::array<std::uint8_t, 1> ends = {static_cast<std::uint8_t>(end - from)};
    face.Measure(std::string_view(text.data() + from, end - from), 0, ends, widths, hyphens);
    from = end;
  }
  faceUs_ += clock_.NowUs() - began;
  count_ += chunk_.size();
  if (count_ < kFaceBytes) {
    return;
  }
  LogLine line;
  line.Text("layout check: the made-up face alone, ")
      .Number(kFaceBytes)
      .Text(" bytes in ")
      .Number(faceUs_)
      .Text(" us (")
      .Number(KbPerSecond(kFaceBytes, faceUs_))
      .Text(" KB/s).");
  (void)stream_.SendText(line.View());
  MoveTo(Step::kBreak);
}

// ---- The breaker's dearest paragraph ----

void LayoutCheck::Break() noexcept {
  const std::size_t which = count_;
  const std::size_t nodes = kBreakerNodes[which];
  // A box as wide as a line can be, then a space, then more places a line
  // could end than there are nodes, within the width of one line: three
  // for each node, a box and a break each. Then a last box.
  const std::size_t places = 3 * nodes;
  const std::size_t items = 2 + (2 * places) + 1;
  Dearest& run = breakerRuns_[which];
  if (!breaker_.has_value()) {
    breaker_.emplace(std::span(nodes_).first(nodes));
    brokenLines_.count = 0;
    breakerItems_ = 0;
    run.bound = LineBreaker::MostWorkFor(nodes);
    if (breaker_->Begin({.first = {}, .rest = LineBreaker::kMaxWidth}, brokenLines_) != layout::BreakStatus::kOk) {
      Fail("the breaker would not begin its dearest paragraph.");
      return;
    }
  }
  LineBreaker& breaker = *breaker_;
  for (std::size_t given = 0; given < kItemsPerStep && breakerItems_ <= items; ++given) {
    const std::size_t item = breakerItems_++;
    const std::uint64_t before = clock_.NowUs();
    if (item == items) {
      (void)breaker.Finish();
    } else if (item == 0) {
      breaker.AddBox(LineBreaker::kMaxWidth);
    } else if (item == 1) {
      breaker.AddSpace(1);
    } else if (item % 2 == 0) {
      breaker.AddBox(1);
    } else {
      breaker.AddBreak(0, 0);
    }
    const std::uint64_t took = clock_.NowUs() - before;
    ++run.calls;
    run.allUs += took;
    run.longestUs = std::max(run.longestUs, took);
  }
  if (breakerItems_ <= items) {
    return;
  }
  run.mostWork = breaker.MostWork();
  breaker_.reset();
  LogLine line;
  line.Text("layout check: breaker, ")
      .Number(nodes)
      .Text(" nodes: the dearest call ")
      .Number(run.longestUs)
      .Text(" us, ")
      .Number(run.mostWork)
      .Text(" of ")
      .Number(run.bound)
      .Text(" work; ")
      .Number(run.calls)
      .Text(" calls in ")
      .Number(run.allUs)
      .Text(" us.");
  (void)stream_.SendText(line.View());
  if (which + 1 < kBreakerNodes.size()) {
    step_ = Step::kBreak;
    count_ = which + 1;
    return;
  }
  MoveTo(Step::kOpenTable);
}

// ---- The table ----

void LayoutCheck::OpenTable() noexcept {
  const StorageStatus status = file_.Open(storage_, kTablePath, core::OpenMode::kRead);
  LogLine line;
  line.Text("layout check: ");
  if (status != StorageStatus::kOk) {
    line.Text("no table at ").Text(kTablePath).Text(" (").Text(core::StorageStatusName(status)).Text("): ");
    OpenBuiltIn(line);
    return;
  }
  if (file_.Size() > table_.size()) {
    line.Text("the table at ")
        .Text(kTablePath)
        .Text(" is ")
        .Number(file_.Size())
        .Text(" bytes, and there is room for ")
        .Number(table_.size())
        .Text(": ");
    file_.Close();
    OpenBuiltIn(line);
    return;
  }
  tableSize_ = file_.Size();
  MoveTo(Step::kReadTable);
}

void LayoutCheck::ReadTable() noexcept {
  const std::size_t want = std::min<std::size_t>(kChunkBytes, tableSize_ - count_);
  if (want > 0 && !file_.ReadExactly(std::span(table_).subspan(count_, want))) {
    LogLine line;
    line.Text("layout check: the table could not be read (")
        .Text(core::StorageStatusName(file_.LastStatus()))
        .Text("): ");
    file_.Close();
    OpenBuiltIn(line);
    return;
  }
  count_ += want;
  if (count_ < tableSize_) {
    return;
  }
  file_.Close();
  const layout::HyphenStatus status = hyphenator_.Open(std::span(table_).first(tableSize_));
  LogLine line;
  line.Text("layout check: ");
  if (status != layout::HyphenStatus::kOk) {
    line.Text("the file at ")
        .Text(kTablePath)
        .Text(" is not a table (")
        .Text(layout::HyphenStatusName(status))
        .Text("): ");
    OpenBuiltIn(line);
    return;
  }
  tableOpen_ = true;
  line.Text("table: ")
      .Number(tableSize_)
      .Text(" bytes, read in ")
      .Number(tableUs_ / 1000)
      .Text(" ms; no break before ")
      .Count(hyphenator_.LeftMin(), " letter", " letters")
      .Text(" or after ")
      .Count(hyphenator_.RightMin(), " letter.", " letters.");
  (void)stream_.SendText(line.View());
  MoveTo(Step::kSetAwkward);
}

void LayoutCheck::OpenBuiltIn(LogLine& line) noexcept {
  if (builtIn_.empty()) {
    line.Text("the text is set without hyphenation.");
  } else if (const layout::HyphenStatus status = hyphenator_.Open(builtIn_); status != layout::HyphenStatus::kOk) {
    line.Text("the built-in table is not one (")
        .Text(layout::HyphenStatusName(status))
        .Text("): the text is set without hyphenation.");
  } else {
    tableOpen_ = true;
    tableBuiltIn_ = true;
    line.Text("the built-in table, ")
        .Number(builtIn_.size())
        .Text(" bytes; no break before ")
        .Count(hyphenator_.LeftMin(), " letter", " letters")
        .Text(" or after ")
        .Count(hyphenator_.RightMin(), " letter.", " letters.");
  }
  (void)stream_.SendText(line.View());
  MoveTo(Step::kSetAwkward);
}

// ---- The setter's awkward text, and the hyphenator's long words ----

void LayoutCheck::FillAwkward() noexcept {
  const std::size_t which = count_ / kAwkwardWidths.size();
  const std::uint32_t length = AwkwardLength(which);
  const std::size_t room = std::min<std::size_t>(kStepBytes, length - awkwardAt_);
  for (std::size_t i = 0; i < room; ++i) {
    chunk_[i] = static_cast<std::uint8_t>(AwkwardByte(which, awkwardAt_ + static_cast<std::uint32_t>(i)));
  }
  chunkLength_ = room;
}

void LayoutCheck::SetAwkward() noexcept {
  const std::size_t which = count_ / kAwkwardWidths.size();
  if (which >= kAwkwardTexts) {
    LogLine line;
    line.Text("layout check: setter, awkward text: the dearest call ")
        .Number(setterRun_.longestUs)
        .Text(" us, the most work ")
        .Number(setterRun_.mostWork)
        .Text(" of ")
        .Number(setterRun_.bound)
        .Text(", the breaker's ")
        .Number(setterRun_.breakerWork)
        .Text(" of ")
        .Number(LineBreaker::MostWorkFor(kNodes))
        .Text("; ")
        .Number(setterRun_.calls)
        .Text(" calls in ")
        .Number(setterRun_.allUs)
        .Text(" us.");
    (void)stream_.SendText(line.View());
    timing_ = nullptr;
    MoveTo(tableOpen_ ? Step::kFindWords : Step::kOpenText);
    return;
  }
  if (awkwardAt_ == 0) {
    settings_ = {.widths = {.first = {}, .rest = kAwkwardWidths[count_ % kAwkwardWidths.size()]}, .hyphenPenalty = 0};
    setter_.UseHyphenator(tableOpen_ ? &hyphenator_ : nullptr);
    if (setter_.Begin(settings_, face_, lines_) != ParagraphStatus::kOk) {
      Fail("the setter would not begin the awkward text.");
      return;
    }
    style_ = 0;
    timeEveryCall_ = true;
    timing_ = &setterRun_;
    setterRun_.bound = ParagraphSetter::MostWorkFor(kRecords);
  }
  FillAwkward();
  Feed(std::string_view(
      reinterpret_cast<const char*>(chunk_.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      chunkLength_));
  awkwardAt_ += static_cast<std::uint32_t>(chunkLength_);
  if (awkwardAt_ < AwkwardLength(which)) {
    return;
  }
  while (!TimedFinish()) {
  }
  setterRun_.mostWork = std::max(setterRun_.mostWork, setter_.MostWork());
  setterRun_.breakerWork = std::max(setterRun_.breakerWork, setter_.Breaker().MostWork());
  awkwardAt_ = 0;
  ++count_;
}

void LayoutCheck::FindWords() noexcept {
  if (count_ >= kLongWords.size()) {
    LogLine line;
    line.Text("layout check: hyphenator, ")
        .Number(kLongWords.size())
        .Text(" long words: the dearest ")
        .Number(wordRun_.longestUs)
        .Text(" us for ")
        .Number(kWordRepeats)
        .Text(" (")
        .Number(wordRun_.longestUs * 1000 / kWordRepeats)
        .Text(" ns a word), ")
        .Number(wordRun_.mostWork)
        .Text(" of ")
        .Number(wordRun_.bound)
        .Text(" work.");
    (void)stream_.SendText(line.View());
    MoveTo(Step::kOpenText);
    return;
  }
  const std::string_view word = kLongWords[count_];
  std::array<std::uint8_t, layout::Hyphenator::kMaxLetters> places{};
  const std::uint64_t before = clock_.NowUs();
  for (std::size_t round = 0; round < kWordRepeats; ++round) {
    (void)hyphenator_.Find(word, places);
  }
  const std::uint64_t took = clock_.NowUs() - before;
  wordRun_.calls += kWordRepeats;
  wordRun_.allUs += took;
  wordRun_.longestUs = std::max(wordRun_.longestUs, took);
  wordRun_.mostWork = std::max(wordRun_.mostWork, hyphenator_.LastWork());
  wordRun_.bound = hyphenator_.MostWorkFor(layout::Hyphenator::kMaxLetters);
  ++count_;
}

// ---- The text on the card ----

void LayoutCheck::OpenText() noexcept {
  const StorageStatus status = file_.Open(storage_, kTextPath, core::OpenMode::kRead);
  if (status != StorageStatus::kOk) {
    LogLine line;
    line.Text("no text at ")
        .Text(kTextPath)
        .Text(" (")
        .Text(core::StorageStatusName(status))
        .Text("): make it with tools/layout-text.py.");
    Fail(line.View());
    return;
  }
  LogLine line;
  line.Text("layout check: text: ").Number(file_.Size()).Text(" bytes in ").Text(kTextPath).Text(".");
  (void)stream_.SendText(line.View());
  BeginPass();
}

void LayoutCheck::BeginPass() noexcept {
  const std::size_t passes = tableOpen_ ? 3 : 2;
  if (passCount_ >= passes) {
    Finish();
    return;
  }
  passIndex_ = passCount_++;
  Pass& pass = Current();
  pass = {};
  const bool last = passCount_ == passes;
  pass.hyphenated = tableOpen_ && (passCount_ == 1 || last);
  pass.callByCall = last;
  setter_.UseHyphenator(pass.hyphenated ? &hyphenator_ : nullptr);
  settings_ = {.widths = {.first = {}, .rest = kWidth}, .hyphenPenalty = kHyphenPenalty};
  timeEveryCall_ = pass.callByCall;
  timing_ = nullptr;
  measuredBefore_ = face_.BytesMeasured();
  lines_.Clear();
  if (!file_.Seek(0)) {
    Fail("the text could not be gone back to the start of.");
    return;
  }
  chunkLength_ = 0;
  chunkAt_ = 0;
  textEnded_ = false;
  paragraphOpen_ = false;
  lineHasText_ = false;
  feedWaiting_ = false;
  crWaiting_ = false;
  paragraphUs_ = 0;
  paragraphDone_ = false;
  passBeganTrace_ = BN_TRACE_NOW();
  MoveTo(Step::kReadText);
}

void LayoutCheck::ReadText() noexcept {
  const std::ptrdiff_t read = file_.Read(chunk_);
  if (read < 0) {
    LogLine line;
    line.Text("the text could not be read (").Text(core::StorageStatusName(file_.LastStatus())).Text(").");
    Fail(line.View());
    return;
  }
  chunkLength_ = static_cast<std::size_t>(read);
  chunkAt_ = 0;
  textEnded_ = read == 0;
  Current().reads += read == 0 ? 0 : 1;
  MoveTo(Step::kSetText);
}

void LayoutCheck::BeginParagraphIfNone() noexcept {
  if (paragraphOpen_) {
    return;
  }
  if (setter_.Begin(settings_, face_, lines_) != ParagraphStatus::kOk) {
    Fail("the setter would not begin a paragraph.");
    return;
  }
  paragraphOpen_ = true;
  style_ = 0;
  paragraphLinesBefore_ = lines_.digest.Lines();
}

void LayoutCheck::SetText() noexcept {
  if (chunkAt_ >= chunkLength_) {
    if (!textEnded_) {
      MoveTo(Step::kReadText);
      return;
    }
    crWaiting_ = false;  // A carriage return at the very end is left off, as at the end of any line.
    if (paragraphOpen_) {
      EndParagraph();
    }
    MoveTo(Step::kEndPass);
    return;
  }
  // The text is fed to the setter in runs of the chunk's bytes, broken where
  // a byte of the file is not a byte of the text: an asterisk, a line end,
  // and a carriage return that may come before one.
  const auto* bytes =
      reinterpret_cast<const char*>(chunk_.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  std::size_t runStart = chunkAt_;
  const auto feedRun = [&](std::size_t end) {
    if (end > runStart) {
      Feed(std::string_view(bytes + runStart, end - runStart));
    }
  };
  // What a byte of the text does before it is fed: begins the paragraph if
  // none is open, and puts in the line end of the file's line before it.
  const auto textByte = [&]() {
    BeginParagraphIfNone();
    if (feedWaiting_) {
      feedWaiting_ = false;
      Feed("\n");
    }
    lineHasText_ = true;
  };
  const std::size_t stepEnd = std::min(chunkLength_, chunkAt_ + kStepBytes);
  for (std::size_t i = chunkAt_; i < stepEnd && !failed_; ++i) {
    const char ch = bytes[i];
    if (crWaiting_) {
      crWaiting_ = false;
      if (ch != '\n') {
        textByte();
        Feed("\r");
      }
    }
    if (ch == '\r') {
      feedRun(i);
      runStart = i + 1;
      crWaiting_ = true;
    } else if (ch == '\n') {
      feedRun(i);
      runStart = i + 1;
      if (lineHasText_) {
        lineHasText_ = false;
        feedWaiting_ = true;
      } else if (paragraphOpen_) {
        // An empty line: the paragraph is over, and so is this step.
        feedWaiting_ = false;
        EndParagraph();
        chunkAt_ = i + 1;
        return;
      }
    } else {
      if (!paragraphOpen_ || feedWaiting_) {
        feedRun(i);  // Nothing, since a line end or nothing came before.
        runStart = i;
        textByte();
      }
      lineHasText_ = true;
      if (ch == '*') {
        feedRun(i);
        runStart = i + 1;
        style_ = style_ == 0 ? 1 : 0;
      }
    }
  }
  feedRun(stepEnd);
  chunkAt_ = stepEnd;
}

void LayoutCheck::Feed(std::string_view text) noexcept {
  while (!text.empty() && !failed_) {
    text.remove_prefix(TimedAdd(text));
  }
}

std::size_t LayoutCheck::TimedAdd(std::string_view text) noexcept {
  const std::uint32_t askedBefore = setter_.WordsAsked();
  std::size_t taken = 0;
  if (timeEveryCall_) {
    const std::uint64_t before = clock_.NowUs();
    taken = setter_.AddText(text, style_);
    CallTook(clock_.NowUs() - before);
  } else {
    ++Current().calls;
    taken = setter_.AddText(text, style_);
  }
  // The hyphenator keeps the work of the last word it was asked about until
  // it is asked again, so it is only taken when the setter asked. (The
  // awkward text is timed into a Dearest of its own, and its work is not a
  // setting's.)
  if (timing_ == nullptr && setter_.WordsAsked() != askedBefore) {
    Current().mostHyphenatorWork = std::max(Current().mostHyphenatorWork, hyphenator_.LastWork());
  }
  return taken;
}

bool LayoutCheck::TimedFinish() noexcept {
  if (!timeEveryCall_) {
    ++Current().calls;
    return setter_.Finish();
  }
  const std::uint64_t before = clock_.NowUs();
  const bool done = setter_.Finish();
  CallTook(clock_.NowUs() - before);
  return done;
}

void LayoutCheck::CallTook(std::uint64_t us) noexcept {
  if (timing_ != nullptr) {
    ++timing_->calls;
    timing_->allUs += us;
    timing_->longestUs = std::max(timing_->longestUs, us);
    return;
  }
  ++Current().calls;
  Current().longestCallUs = std::max(Current().longestCallUs, us);
}

void LayoutCheck::EndParagraph() noexcept {
  while (!TimedFinish()) {
  }
  Pass& pass = Current();
  ++pass.paragraphs;
  pass.bytes += setter_.Text();
  pass.wordsAsked += setter_.WordsAsked();
  pass.lastResortCuts += setter_.LastResortCuts();
  pass.linesCutShort += setter_.LinesCutShort();
  const LineBreaker& breaker = setter_.Breaker();
  pass.settledEarly += breaker.SettledEarly();
  pass.paragraphsSettledEarly += breaker.SettledEarly() != 0 ? 1U : 0U;
  pass.mostNodes = std::max(pass.mostNodes, breaker.MostNodes());
  pass.mostBreakerWork = std::max(pass.mostBreakerWork, breaker.MostWork());
  pass.mostSetterWork = std::max(pass.mostSetterWork, setter_.MostWork());
  paragraphOpen_ = false;
  paragraphDone_ = true;
  lastParagraphBytes_ = setter_.Text();
  lastParagraphLines_ = lines_.digest.Lines() - paragraphLinesBefore_;
}

void LayoutCheck::EndPass() noexcept {
  Pass& pass = Current();
  pass.lines = lines_.digest.Lines();
  pass.checksum = lines_.digest.Checksum();
  pass.hyphens = lines_.hyphens;
  pass.tooWide = lines_.tooWide;
  pass.bytesMeasured = face_.BytesMeasured() - measuredBefore_;
  SayPass(pass);
  BN_TRACE_TOP_SPAN_SINCE(kSpanLayoutCheckPass, passBeganTrace_, static_cast<std::uint32_t>(passCount_));
  if (passCount_ == 1) {
    layoutKbPerSecond.Set(Clamped(KbPerSecond(pass.bytes, pass.setUs)));
  }
  if (pass.callByCall) {
    layoutLongestCallUs.Set(Clamped(pass.longestCallUs));
  }
  BeginPass();
}

const char* LayoutCheck::PassName(const Pass& pass) const noexcept {
  if (pass.callByCall) {
    return pass.hyphenated ? "hyphenated, every call timed" : "not hyphenated, every call timed";
  }
  return pass.hyphenated ? "hyphenated" : "not hyphenated";
}

void LayoutCheck::SayPass(const Pass& pass) noexcept {
  const char* name = PassName(pass);
  LogLine line;
  line.Text("layout check: ")
      .Text(name)
      .Text(": ")
      .Count(pass.paragraphs, " paragraph, ", " paragraphs, ")
      .Number(pass.bytes)
      .Text(" bytes, ")
      .Count(pass.lines, " line (", " lines (")
      .Number(pass.hyphens)
      .Text(" with a hyphen, ")
      .Number(pass.tooWide)
      .Text(" too wide); checksum ")
      .Hex8(pass.checksum)
      .Text(".");
  (void)stream_.SendText(line.View());

  const std::uint64_t faceShareUs = (pass.bytesMeasured * faceUs_) / kFaceBytes;
  const std::uint64_t pageUs = pass.lines == 0 ? 0 : (pass.setUs * kPageLines) / pass.lines;
  line.Clear();
  line.Text("layout check: ")
      .Text(name)
      .Text(": setting ")
      .Number(pass.setUs / 1000)
      .Text(" ms (")
      .Number(KbPerSecond(pass.bytes, pass.setUs))
      .Text(" KB/s), ")
      .Number(pageUs)
      .Text(" us for ")
      .Number(kPageLines)
      .Text(" lines; the face about ")
      .Number(faceShareUs / 1000)
      .Text(" ms of it; card ")
      .Number(pass.cardUs / 1000)
      .Text(" ms.");
  (void)stream_.SendText(line.View());

  line.Clear();
  line.Text("layout check: ")
      .Text(name)
      .Text(": the longest paragraph ")
      .Number(pass.longestParagraphUs)
      .Text(" us, ")
      .Number(pass.longestParagraphBytes)
      .Text(" bytes in ")
      .Count(pass.longestParagraphLines, " line", " lines")
      .Text("; the most work in a call: setter ")
      .Number(pass.mostSetterWork)
      .Text(", breaker ")
      .Number(pass.mostBreakerWork)
      .Text(", hyphenator ")
      .Number(pass.mostHyphenatorWork)
      .Text(".");
  (void)stream_.SendText(line.View());

  line.Clear();
  line.Text("layout check: ")
      .Text(name)
      .Text(": ")
      .Count(pass.wordsAsked, " word asked about, ", " words asked about, ")
      .Number(pass.lastResortCuts)
      .Text(" last resort, ")
      .Count(pass.settledEarly, " line", " lines")
      .Text(" early in ")
      .Number(pass.paragraphsSettledEarly)
      .Text(", ")
      .Number(pass.linesCutShort)
      .Text(" cut short; ")
      .Number(pass.mostNodes)
      .Text(" nodes; ")
      .Number(pass.bytesMeasured)
      .Text(" bytes measured.");
  (void)stream_.SendText(line.View());

  line.Clear();
  line.Text("layout check: ").Text(name).Text(": ").Count(pass.calls, " call", " calls");
  if (pass.callByCall) {
    line.Text(", the longest ").Number(pass.longestCallUs).Text(" us");
  }
  line.Text(".");
  (void)stream_.SendText(line.View());
}

// ---- The end ----

void LayoutCheck::Fail(std::string_view why) noexcept {
  LogLine line;
  line.Text("layout check: FAILED. ").Text(why);
  (void)stream_.SendText(line.View());
  failed_ = true;
  paragraphOpen_ = false;
  file_.Close();
  Finish();
}

void LayoutCheck::Finish() noexcept {
  file_.Close();
  breaker_.reset();
  bool passed = !failed_;
  if (passed && passCount_ >= 2) {
    // The first setting and the last were set alike but for the timing of
    // every call, and so came to the same lines.
    const Pass& first = passes_[0];
    const Pass& last = passes_[passCount_ - 1];
    if (first.checksum != last.checksum || first.lines != last.lines) {
      LogLine line;
      line.Text("layout check: FAILED. The lines are not the same when every call is timed: checksum ")
          .Hex8(last.checksum)
          .Text(", where it was ")
          .Hex8(first.checksum)
          .Text(".");
      (void)stream_.SendText(line.View());
      passed = false;
    }
  }
  if (passed) {
    (void)stream_.SendText("layout check: passed.");
  }
  LogLine longest;
  longest.Text("layout check: the longest step: ")
      .Number(longestStepUs_)
      .Text(" us, ")
      .Text(LongestStepName())
      .Text(".");
  (void)stream_.SendText(longest.View());
  layoutCheck.Set(passed ? 1 : 0);
  BN_TRACE_TOP_SPAN_SINCE(kSpanLayoutCheck, beganTrace_, passed ? 1 : 0);
  state_ = passed ? State::kPassed : State::kFailed;
  step_ = Step::kNone;
}

}  // namespace bn::reader
