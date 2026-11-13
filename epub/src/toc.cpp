#include "epub/toc.h"

#include <algorithm>
#include <array>

#include "names.h"

namespace bn::epub {

namespace {

// How many levels of a nav's lists are told apart when deciding whether a
// span is an entry. Lists that deep are not found in books.
constexpr int kTrackedLevels = 32;
// The deepest an entry is said to be.
constexpr int kMaxDepth = 0xFFFF;

bool IsList(std::string_view local) noexcept { return local == "ol" || local == "ul"; }

// Whether a nav element says it is the table of contents: an attribute
// called type, with whatever prefix the document gave EPUB's names (it is
// always "epub:"), has "toc" among its words.
bool IsTocNav(const XmlAttributes& attributes) noexcept {
  for (std::size_t i = 0; i < attributes.Count(); ++i) {
    if (XmlLocalName(attributes.Name(i)) == "type" && HasWord(attributes.Value(i), "toc")) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* TocStatusName(TocStatus status) noexcept {
  switch (status) {
    case TocStatus::kOk:
      return "ok";
    case TocStatus::kNotStarted:
      return "not started";
    case TocStatus::kNoToc:
      return "no table of contents";
    case TocStatus::kNotToc:
      return "not a table of contents";
    case TocStatus::kNoMemory:
      return "no memory";
    case TocStatus::kUnreadable:
      return "unreadable";
    case TocStatus::kStopped:
      return "stopped";
  }
  return "unknown";
}

const char* TocKindName(TocKind kind) noexcept {
  switch (kind) {
    case TocKind::kBest:
      return "best";
    case TocKind::kNav:
      return "nav";
    case TocKind::kNcx:
      return "ncx";
  }
  return "unknown";
}

TocStatus TocReader::End(TocStatus status) noexcept {
  running_ = false;
  status_ = status;
  return status;
}

TocStatus TocReader::Begin(const Package& package, core::BlockFile& entries, ZipEntryReader& reader, XmlParser& xml,
                           TocHandler& handler, TocKind kind) noexcept {
  package_ = &package;
  xml_ = &xml;
  handler_ = &handler;
  kind_ = TocKind::kBest;
  cutShort_ = false;
  stopped_ = false;
  entries_ = 0;
  ownPath_.Clear();
  depth_ = 0;
  tableDepth_ = 0;
  foundTable_ = false;
  level_ = 0;
  captureDepth_ = 0;
  itemHasEntry_ = 0;
  pending_ = false;

  const ZipEntry* entry = nullptr;
  if (kind != TocKind::kNcx && package.Nav() != nullptr) {
    entry = package.Nav();
    kind_ = TocKind::kNav;
  } else if (kind != TocKind::kNav && package.Ncx() != nullptr) {
    entry = package.Ncx();
    kind_ = TocKind::kNcx;
  }
  if (entry == nullptr) {
    return End(TocStatus::kNoToc);
  }

  // The file's own path, which the package did not keep: its links are
  // worked out from the directory it is in.
  std::array<char, kMaxBookPath> name{};
  std::size_t length = 0;
  ZipStatus opened = ReadZipEntryName(entries, *entry, name, length);
  if (opened == ZipStatus::kOk && length > name.size()) {
    opened = ZipStatus::kDamaged;  // The package found it by a name that fits.
  }
  if (opened == ZipStatus::kOk) {
    opened = reader.Open(entries, *entry);
  }
  if (opened != ZipStatus::kOk) {
    return End(TocStatus::kUnreadable);
  }
  ownPath_.Append(std::string_view(name.data(), length));

  if (xml.Begin(reader, *this) != XmlStatus::kOk) {
    return End(TocStatus::kNoMemory);  // All that can stop a parser beginning.
  }
  running_ = true;
  status_ = TocStatus::kOk;
  return status_;
}

bool TocReader::Step() noexcept {
  if (!running_) {
    return false;
  }
  if (xml_->Step()) {
    return true;
  }
  switch (xml_->Status()) {
    case XmlStatus::kOk:
      (void)End(foundTable_ ? TocStatus::kOk : TocStatus::kNotToc);
      break;
    case XmlStatus::kStopped:
      (void)End(TocStatus::kStopped);
      break;
    case XmlStatus::kNoMemory:
      (void)End(TocStatus::kNoMemory);
      break;
    case XmlStatus::kSourceFailed:
      (void)End(TocStatus::kUnreadable);
      break;
    default:
      // Not XML from here on. What it gave before that is kept.
      cutShort_ = entries_ > 0;
      (void)End(cutShort_ ? TocStatus::kOk : TocStatus::kNotToc);
      break;
  }
  return false;
}

TocStatus TocReader::Read(const Package& package, core::BlockFile& entries, ZipEntryReader& reader, XmlParser& xml,
                          TocHandler& handler, TocKind kind) noexcept {
  if (Begin(package, entries, reader, xml, handler, kind) == TocStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

void TocReader::BeginEntry(int depth) noexcept {
  pending_ = true;
  hasLabel_ = false;
  hasTarget_ = false;
  pendingDepth_ = static_cast<std::uint16_t>(std::clamp(depth, 0, kMaxDepth));
  document_ = Package::kNotInSpine;
  label_.Clear();
  fragment_.Clear();
}

void TocReader::SetTarget(std::string_view reference) noexcept {
  hasTarget_ = true;
  std::array<char, kMaxBookPath> path{};
  const std::size_t length = ResolveBookPath(BookDirectory(ownPath_.View()), reference, path);
  std::string_view file(path.data(), length);
  if (length == 0 && reference.starts_with('#')) {
    file = ownPath_.View();  // A place in the table of contents' own file.
  }
  document_ = file.empty() ? Package::kNotInSpine : package_->SpineIndexOf(file);
  if (document_ == Package::kNotInSpine) {
    return;  // A place within nothing is nowhere.
  }
  std::array<char, kMaxFragment> fragment{};
  fragment_.Append(std::string_view(fragment.data(), ReferenceFragment(reference, fragment)));
}

void TocReader::Emit() noexcept {
  pending_ = false;
  label_.RemoveTrailingSpace();
  ++entries_;
  const TocEntry entry{.label = label_.View(),
                       .labelCut = label_.Truncated(),
                       .depth = pendingDepth_,
                       .document = document_,
                       .fragment = fragment_.View()};
  if (!handler_->OnEntry(entry)) {
    stopped_ = true;
    xml_->Stop();
  }
}

void TocReader::OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept {
  ++depth_;
  if (stopped_) {
    return;  // The parser may still tell of the end of the element it was in.
  }
  const std::string_view local = XmlLocalName(name);
  if (kind_ == TocKind::kNcx) {
    StartInNcx(local, attributes);
  } else {
    StartInNav(local, attributes);
  }
}

void TocReader::StartInNcx(std::string_view local, const XmlAttributes& attributes) noexcept {
  if (tableDepth_ == 0) {
    if (local == "navMap" && !foundTable_) {
      tableDepth_ = depth_;
      foundTable_ = true;
      level_ = 0;
    }
    return;
  }
  if (local == "navPoint") {
    // An entry is given before the entries under it: if the one this is
    // under has not been, now is the time.
    if (pending_) {
      Emit();
    }
    BeginEntry(level_);
    ++level_;
    return;
  }
  // The label is the first text of the entry, and the target its first
  // content. (What the map has outside every point, a note about itself
  // say, is taken too, and dropped when the next entry begins.)
  if (local == "text" && !hasLabel_) {
    hasLabel_ = true;
    captureDepth_ = depth_;
  } else if (local == "content" && !hasTarget_) {
    std::string_view source;
    if (attributes.Find("src", source)) {
      SetTarget(source);
    }
  }
}

void TocReader::StartInNav(std::string_view local, const XmlAttributes& attributes) noexcept {
  if (tableDepth_ == 0) {
    // The first nav that says it is the table of contents.
    if (local == "nav" && !foundTable_ && IsTocNav(attributes)) {
      tableDepth_ = depth_;
      foundTable_ = true;
      level_ = 0;
    }
    return;
  }
  if (captureDepth_ != 0) {
    // Inside a label: whatever this is, its text is the label's. A link
    // inside a heading that had none makes the heading point somewhere.
    std::string_view reference;
    if (local == "a" && pending_ && !hasTarget_ && attributes.Find("href", reference)) {
      SetTarget(reference);
    }
    return;
  }
  if (IsList(local)) {
    ++level_;
    return;
  }
  if (level_ == 0) {
    return;  // Not in the list yet: the table's heading, say.
  }
  const std::uint32_t bit = level_ <= kTrackedLevels ? std::uint32_t{1} << static_cast<unsigned>(level_ - 1) : 0;
  if (local == "li") {
    itemHasEntry_ &= ~bit;
    return;
  }
  if (local == "a") {
    BeginEntry(level_ - 1);
    std::string_view reference;
    if (attributes.Find("href", reference)) {
      SetTarget(reference);
    }
    captureDepth_ = depth_;
  } else if (local == "span" && (itemHasEntry_ & bit) == 0) {
    // Where a link would be, a heading with no link.
    BeginEntry(level_ - 1);
    captureDepth_ = depth_;
  }
}

void TocReader::OnEndElement(std::string_view name) noexcept {
  const std::string_view local = XmlLocalName(name);
  if (!stopped_ && tableDepth_ != 0) {
    if (captureDepth_ == depth_) {
      // The end of a label. In a nav, that is the end of the entry.
      captureDepth_ = 0;
      if (kind_ == TocKind::kNav && pending_) {
        if (level_ >= 1 && level_ <= kTrackedLevels) {
          itemHasEntry_ |= std::uint32_t{1} << static_cast<unsigned>(level_ - 1);
        }
        Emit();
      }
    } else if (captureDepth_ == 0) {
      if (kind_ == TocKind::kNcx && local == "navPoint") {
        if (pending_) {
          Emit();  // It had no entries under it to bring this on sooner.
        }
        --level_;
      } else if (kind_ == TocKind::kNav && IsList(local)) {
        --level_;
      }
    }
    if (depth_ == tableDepth_) {
      tableDepth_ = 0;
    }
  }
  --depth_;
}

void TocReader::OnText(std::string_view text) noexcept {
  if (captureDepth_ != 0) {
    label_.AppendCollapsed(text);
  }
}

// An entity the parser left alone. The one that turns up is the space
// that does not break; anything else is passed over.
void TocReader::OnEntity(std::string_view name) noexcept {
  if (name == "nbsp") {
    OnText(" ");
  }
}

}  // namespace bn::epub
