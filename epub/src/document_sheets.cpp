#include "epub/document_sheets.h"

#include <algorithm>
#include <cstring>

#include "core/trace.h"
#include "epub/trace_ids.h"

namespace bn::epub {

namespace {

// The spaces that part the words of an attribute: HTML's.
constexpr bool IsSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr char Lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether `text` is `word`, whatever its letters' case. `word` is in small
// letters.
bool Is(std::string_view text, std::string_view word) noexcept {
  return text.size() == word.size() &&
         std::equal(text.begin(), text.end(), word.begin(), [](char a, char b) { return Lower(a) == b; });
}

std::string_view Trimmed(std::string_view text) noexcept {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// Whether one of the words of `list`, which spaces part, is `word`.
bool HasWord(std::string_view list, std::string_view word) noexcept {
  while (true) {
    list = Trimmed(list);
    if (list.empty()) {
      return false;
    }
    std::size_t length = 0;
    while (length < list.size() && !IsSpace(list[length])) {
      ++length;
    }
    std::string_view first = list;
    first.remove_suffix(list.size() - length);
    if (Is(first, word)) {
      return true;
    }
    list.remove_prefix(length);
  }
}

// Whether a `type` says CSS, or says nothing.
bool IsCss(std::string_view type) noexcept {
  type = Trimmed(type);
  return type.empty() || Is(type, "text/css");
}

}  // namespace

const char* SheetLeftOutName(SheetLeftOut what) noexcept {
  switch (what) {
    case SheetLeftOut::kLinkMedia:
      return "link for another medium";
    case SheetLeftOut::kLinkNotInBook:
      return "link to no file of the book";
    case SheetLeftOut::kLinkTooMany:
      return "link after the most followed";
    case SheetLeftOut::kImportNotInBook:
      return "import of no file of the book";
    case SheetLeftOut::kImportTooDeep:
      return "import too deep";
    case SheetLeftOut::kImportLoop:
      return "import of a sheet on the way to it";
    case SheetLeftOut::kImportTooMany:
      return "import after the most remembered";
    case SheetLeftOut::kSheetTooMany:
      return "sheet with no room";
    case SheetLeftOut::kSheetNotRead:
      return "sheet not read to its end";
    case SheetLeftOut::kStyleMedia:
      return "style for another medium";
    case SheetLeftOut::kStyleTooLong:
      return "style too long";
    case SheetLeftOut::kStyleImport:
      return "import in a style";
    case SheetLeftOut::kHeadNotRead:
      return "head not read through";
  }
  return "?";
}

const char* SheetsStatusName(SheetsStatus status) noexcept {
  switch (status) {
    case SheetsStatus::kOk:
      return "ok";
    case SheetsStatus::kNotStarted:
      return "not started";
    case SheetsStatus::kNoDocument:
      return "no document";
  }
  return "?";
}

DocumentSheets::DocumentSheets(BookFiles& files, XmlParser& parser, CssReader& reader, CssRules& rules,
                               std::span<char> names, std::span<std::uint8_t> style) noexcept
    : files_(files),
      parser_(parser),
      reader_(reader),
      rules_(rules),
      // A name's place is kept in 16 bits.
      names_(names.first(std::min<std::size_t>(names.size(), UINT16_MAX))),
      style_(style) {}

void DocumentSheets::BeginBook() noexcept {
  if (phase_ == Phase::kSheet || ownStyleBegun_) {
    rules_.EndSheet();
  }
  rules_.Clear();
  status_ = SheetsStatus::kNotStarted;
  phase_ = Phase::kIdle;
  known_ = 0;
  sheetsRead_ = 0;
  namesUsed_ = 0;
  imported_ = 0;
  leftOut_ = {};
  linked_ = 0;
  linkAt_ = 0;
  listed_ = 0;
  depth_ = 0;
  pending_ = kNone;
  reading_ = kNone;
  styleUsed_ = 0;
  styleDepth_ = 0;
  ownStyle_ = false;
  ownStyleKept_ = false;
  ownStyleBegun_ = false;
  styleReading_ = false;
}

void DocumentSheets::Begin(std::string_view path) noexcept {
  // Whatever was in hand is dropped, and the last document's own rules go.
  if (phase_ == Phase::kSheet) {
    DropSheetInHand();
  }
  if (ownStyleBegun_) {
    rules_.EndSheet();
  }
  if (ownStyleKept_) {
    rules_.BackTo(ownStylePlace_);
  }
  ownStyle_ = false;
  ownStyleKept_ = false;
  ownStyleBegun_ = false;
  styleReading_ = false;
  linked_ = 0;
  linkAt_ = 0;
  listed_ = 0;
  depth_ = 0;
  pending_ = kNone;
  reading_ = kNone;
  styleUsed_ = 0;
  styleDepth_ = 0;
  styleAt_ = 0;
  rootSeen_ = false;
  began_ = BN_TRACE_NOW();
  if (path.empty() || path.size() > document_.size()) {
    documentLength_ = 0;
    status_ = SheetsStatus::kNoDocument;
    phase_ = Phase::kDone;
    return;
  }
  std::memcpy(document_.data(), path.data(), path.size());
  documentLength_ = path.size();
  status_ = SheetsStatus::kOk;
  phase_ = Phase::kOpening;
}

bool DocumentSheets::Step() noexcept {
  switch (phase_) {
    case Phase::kIdle:
    case Phase::kDone:
      return false;
    case Phase::kOpening: {
      core::ByteSource* const source = files_.Open({document_.data(), documentLength_});
      if (source == nullptr) {
        status_ = SheetsStatus::kNoDocument;
        phase_ = Phase::kDone;
        BN_TRACE_SPAN_SINCE(kSpanDocumentSheets, began_, 0);
        return false;
      }
      const XmlStatus begun = parser_.Begin(*source, *this);
      if (begun != XmlStatus::kOk) {
        Leave(SheetLeftOut::kHeadNotRead, XmlStatusName(begun));
        phase_ = Phase::kWalking;
      } else {
        phase_ = Phase::kHead;
      }
      break;
    }
    case Phase::kHead:
      StepHead();
      break;
    case Phase::kWalking:
      StepWalk();
      break;
    case Phase::kSheet:
      StepSheet();
      break;
    case Phase::kOwnStyle:
      StepOwnStyle();
      break;
  }
  return phase_ != Phase::kDone;
}

SheetsStatus DocumentSheets::Find(std::string_view path) noexcept {
  Begin(path);
  while (Step()) {
  }
  return status_;
}

std::string_view DocumentSheets::PathOf(std::uint8_t sheet) const noexcept {
  for (std::size_t i = 0; i < known_; ++i) {
    if (sheets_[i].known == Known::kRead && sheets_[i].number == sheet) {
      return NameOf(sheets_[i]);
    }
  }
  return {};
}

// ---- The head ----

void DocumentSheets::OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept {
  if (styleDepth_ > 0) {
    // An element inside a `<style>`: what text it has goes with the rest.
    ++styleDepth_;
    return;
  }
  const std::string_view local = XmlLocalName(name);
  if (!rootSeen_) {
    rootSeen_ = true;
    // A document that is not HTML has no head to read.
    if (!Is(local, "html")) {
      parser_.Stop();
    }
    return;
  }
  if (Is(local, "body")) {
    parser_.Stop();
    return;
  }
  if (Is(local, "link")) {
    Link(attributes);
    return;
  }
  if (!Is(local, "style")) {
    return;
  }
  styleDepth_ = 1;
  styleBegan_ = styleUsed_;
  styleFits_ = true;
  std::string_view type;
  std::string_view media;
  static_cast<void>(attributes.Find("type", type));
  static_cast<void>(attributes.Find("media", media));
  styleTaken_ = IsCss(type);
  if (styleTaken_ && !CssMediaApplies(media)) {
    Leave(SheetLeftOut::kStyleMedia, media);
    styleTaken_ = false;
  }
}

void DocumentSheets::OnEndElement(std::string_view /*name*/) noexcept {
  if (styleDepth_ == 0) {
    return;
  }
  --styleDepth_;
  if (styleDepth_ > 0 || !styleTaken_) {
    return;
  }
  if (!styleFits_) {
    styleUsed_ = styleBegan_;
    Leave(SheetLeftOut::kStyleTooLong, {});
  } else if (styleUsed_ > styleBegan_) {
    // The byte that ends it, which OnText() left room for. XML has no
    // character that is nought, so none is in the text.
    style_[styleUsed_++] = 0;
  }
}

void DocumentSheets::OnText(std::string_view text) noexcept {
  if (styleDepth_ == 0 || !styleTaken_ || !styleFits_) {
    return;
  }
  if (text.size() >= style_.size() - styleUsed_) {
    styleFits_ = false;
    return;
  }
  std::memcpy(style_.data() + styleUsed_, text.data(), text.size());
  styleUsed_ += text.size();
}

void DocumentSheets::OnEntity(std::string_view /*name*/) noexcept {}

void DocumentSheets::Link(const XmlAttributes& attributes) noexcept {
  std::string_view href;
  std::string_view rel;
  std::string_view type;
  std::string_view media;
  if (!attributes.Find("href", href)) {
    return;
  }
  static_cast<void>(attributes.Find("rel", rel));
  static_cast<void>(attributes.Find("type", type));
  static_cast<void>(attributes.Find("media", media));
  if (!HasWord(rel, "stylesheet") || HasWord(rel, "alternate") || !IsCss(type)) {
    return;
  }
  if (!CssMediaApplies(media)) {
    Leave(SheetLeftOut::kLinkMedia, href);
    return;
  }
  const std::size_t length = ResolveBookPath(BookDirectory({document_.data(), documentLength_}), href, path_);
  if (length == 0) {
    Leave(SheetLeftOut::kLinkNotInBook, href);
    return;
  }
  const std::string_view path(path_.data(), length);
  if (linked_ == kMaxLinks) {
    Leave(SheetLeftOut::kLinkTooMany, path);
    return;
  }
  const std::uint8_t sheet = Know(path, Known::kUnread);
  if (sheet == kNone) {
    Leave(SheetLeftOut::kSheetTooMany, path);
    return;
  }
  links_[linked_++] = sheet;
}

void DocumentSheets::StepHead() noexcept {
  if (parser_.Step()) {
    return;
  }
  if (styleDepth_ > 0) {
    // The head ended inside a `<style>`: it is not kept.
    styleUsed_ = styleBegan_;
    styleDepth_ = 0;
  }
  const XmlStatus ended = parser_.Status();
  if (ended != XmlStatus::kOk && ended != XmlStatus::kStopped) {
    Leave(SheetLeftOut::kHeadNotRead, XmlStatusName(ended));
  }
  phase_ = Phase::kWalking;
}

// ---- The list ----

void DocumentSheets::StepWalk() noexcept {
  if (pending_ != kNone) {
    // A sheet to go into: read first, if it has not been.
    const Sheet& sheet = sheets_[pending_];
    if (sheet.known == Known::kUnread) {
      OpenSheet(pending_);
    } else if (sheet.known == Known::kRead) {
      way_[depth_++] = {.sheet = pending_, .next = 0};
      pending_ = kNone;
    } else {
      // Missing, or no file of the book at all.
      Leave(pendingIsImport_ ? SheetLeftOut::kImportNotInBook : SheetLeftOut::kLinkNotInBook, NameOf(sheet));
      pending_ = kNone;
    }
    return;
  }
  if (depth_ == 0) {
    // The next link; or, after the last, the document's own rules.
    if (linkAt_ == linked_) {
      styleAt_ = 0;
      phase_ = Phase::kOwnStyle;
      return;
    }
    pending_ = links_[linkAt_++];
    pendingIsImport_ = false;
    return;
  }
  Level& level = way_[depth_ - 1];
  const Sheet& sheet = sheets_[level.sheet];
  if (level.next == sheet.imports) {
    // Its imports are before it: now the sheet itself.
    List(sheet.number);
    --depth_;
    return;
  }
  const std::uint8_t target = imports_[sheet.firstImport + level.next];
  ++level.next;
  if (depth_ > kMaxDepth) {
    Leave(SheetLeftOut::kImportTooDeep, NameOf(sheets_[target]));
    return;
  }
  for (std::size_t i = 0; i < depth_; ++i) {
    if (way_[i].sheet == target) {
      Leave(SheetLeftOut::kImportLoop, NameOf(sheets_[target]));
      return;
    }
  }
  pending_ = target;
  pendingIsImport_ = true;
}

void DocumentSheets::OpenSheet(std::uint8_t index) noexcept {
  Sheet& sheet = sheets_[index];
  // Its place in the table first: with none to be had, the file is not
  // opened.
  sheetPlace_ = rules_.Here();
  std::uint8_t number = 0;
  if (!rules_.BeginSheet(number)) {
    Leave(SheetLeftOut::kSheetTooMany, NameOf(sheet));
    pending_ = kNone;
    return;
  }
  core::ByteSource* const source = files_.Open(NameOf(sheet));
  if (source == nullptr) {
    // Said, and counted, by the next step, and by every later one that
    // comes to it.
    rules_.EndSheet();
    rules_.BackTo(sheetPlace_);
    sheet.known = Known::kMissing;
    return;
  }
  sheet.number = number;
  sheet.firstImport = static_cast<std::uint8_t>(imported_);
  sheet.imports = 0;
  reading_ = index;
  reader_.Begin(*source, *this);
  phase_ = Phase::kSheet;
}

void DocumentSheets::StepSheet() noexcept {
  if (reader_.Step()) {
    return;
  }
  rules_.EndSheet();
  Sheet& sheet = sheets_[reading_];
  sheet.known = Known::kRead;
  ++sheetsRead_;
  if (listener_ != nullptr) {
    listener_->OnSheetRead(NameOf(sheet), sheet.number, reader_);
  }
  if (reader_.Status() != CssStatus::kOk) {
    Leave(SheetLeftOut::kSheetNotRead, NameOf(sheet));
  }
  reading_ = kNone;
  // It is still the sheet to go into, which the next step does.
  phase_ = Phase::kWalking;
}

void DocumentSheets::DropSheetInHand() noexcept {
  rules_.EndSheet();
  rules_.BackTo(sheetPlace_);
  Sheet& sheet = sheets_[reading_];
  sheet.known = Known::kUnread;
  // Its imports were the last remembered.
  imported_ = sheet.firstImport;
  sheet.imports = 0;
  reading_ = kNone;
}

void DocumentSheets::StepOwnStyle() noexcept {
  if (styleReading_) {
    styleReading_ = reader_.Step();
    return;
  }
  if (NextOwnStyle()) {
    return;
  }
  if (ownStyleBegun_) {
    rules_.EndSheet();
    ownStyleBegun_ = false;
    List(ownStyleNumber_);
    ownStyle_ = true;
  }
  Finish();
}

bool DocumentSheets::NextOwnStyle() noexcept {
  if (styleAt_ >= styleUsed_) {
    return false;
  }
  if (!ownStyleBegun_) {
    // The document's own sheet, which the next Begin() takes out again.
    ownStylePlace_ = rules_.Here();
    if (!rules_.BeginSheet(ownStyleNumber_)) {
      Leave(SheetLeftOut::kSheetTooMany, "style");
      styleAt_ = styleUsed_;
      return false;
    }
    ownStyleBegun_ = true;
    ownStyleKept_ = true;
  }
  std::size_t end = styleAt_;
  while (end < styleUsed_ && style_[end] != 0) {
    ++end;
  }
  styleSource_ = core::MemoryByteSource(style_.subspan(styleAt_, end - styleAt_));
  styleAt_ = end + 1;
  reader_.Begin(styleSource_, *this);
  styleReading_ = true;
  return true;
}

void DocumentSheets::Finish() noexcept {
  phase_ = Phase::kDone;
  BN_TRACE_SPAN_SINCE(kSpanDocumentSheets, began_, static_cast<std::uint32_t>(listed_));
}

// ---- A sheet ----

void DocumentSheets::OnRuleBegin() noexcept { rules_.OnRuleBegin(); }
void DocumentSheets::OnSelector(const CssSelector& selector) noexcept { rules_.OnSelector(selector); }
void DocumentSheets::OnDeclaration(const CssDeclaration& declaration) noexcept { rules_.OnDeclaration(declaration); }
void DocumentSheets::OnRuleEnd() noexcept { rules_.OnRuleEnd(); }
void DocumentSheets::OnDropped(CssDropped dropped, std::string_view text) noexcept { rules_.OnDropped(dropped, text); }

void DocumentSheets::OnImport(std::string_view url) noexcept {
  if (phase_ == Phase::kOwnStyle) {
    Leave(SheetLeftOut::kStyleImport, url);
    return;
  }
  Sheet& sheet = sheets_[reading_];
  // From the folder of the sheet that names it. What names no file of the
  // book is remembered as it was written, to be said for each document
  // that comes to it.
  const std::size_t length = ResolveBookPath(BookDirectory(NameOf(sheet)), url, path_);
  const std::string_view name = length == 0 ? url : std::string_view(path_.data(), length);
  if (imported_ == kMaxImports) {
    Leave(SheetLeftOut::kImportTooMany, name);
    return;
  }
  const std::uint8_t target = Know(name, length == 0 ? Known::kNowhere : Known::kUnread);
  if (target == kNone) {
    Leave(SheetLeftOut::kSheetTooMany, name);
    return;
  }
  imports_[imported_++] = target;
  ++sheet.imports;
}

// ---- What is kept ----

std::uint8_t DocumentSheets::Know(std::string_view name, Known as) noexcept {
  const bool nowhere = as == Known::kNowhere;
  for (std::size_t i = 0; i < known_; ++i) {
    if ((sheets_[i].known == Known::kNowhere) == nowhere && NameOf(sheets_[i]) == name) {
      return static_cast<std::uint8_t>(i);
    }
  }
  if (known_ == kMaxSheets || name.size() > names_.size() - namesUsed_) {
    return kNone;
  }
  if (!name.empty()) {
    std::memcpy(names_.data() + namesUsed_, name.data(), name.size());
  }
  sheets_[known_] = {.nameAt = static_cast<std::uint16_t>(namesUsed_),
                     .nameLength = static_cast<std::uint16_t>(name.size()),
                     .known = as,
                     .number = 0,
                     .firstImport = 0,
                     .imports = 0};
  namesUsed_ += name.size();
  return static_cast<std::uint8_t>(known_++);
}

std::string_view DocumentSheets::NameOf(const Sheet& sheet) const noexcept {
  return {names_.data() + sheet.nameAt, sheet.nameLength};
}

void DocumentSheets::List(std::uint8_t number) noexcept {
  // Named before: it counts where it is named last.
  const std::span<std::uint8_t> listed(list_.data(), listed_);
  const auto was = std::find(listed.begin(), listed.end(), number);
  if (was != listed.end()) {
    std::copy(was + 1, listed.end(), was);
    --listed_;
  }
  if (listed_ < list_.size()) {
    list_[listed_++] = number;
  }
}

void DocumentSheets::Leave(SheetLeftOut what, std::string_view text) noexcept {
  ++leftOut_[static_cast<std::size_t>(what)];
  if (listener_ != nullptr) {
    listener_->OnLeftOut(what, text);
  }
}

}  // namespace bn::epub
