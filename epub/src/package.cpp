#include "epub/package.h"

#include <algorithm>
#include <array>
#include <new>

#include "core/metrics.h"
#include "core/trace.h"
#include "epub/trace_ids.h"
#include "names.h"

namespace bn::epub {

namespace {

// Packages read, and those that could not be.
core::Counter packagesRead{"packages_read"};
core::Counter packagesFailed{"packages_failed"};

constexpr std::string_view kContainerName = "META-INF/container.xml";
constexpr std::string_view kPackageMediaType = "application/oebps-package+xml";

// What is noted of an item of the manifest besides its numbers.
constexpr std::uint32_t kIsNav = 0x01;         // The package says it is the EPUB 3 table of contents,
constexpr std::uint32_t kIsCoverImage = 0x02;  // or the cover picture.
constexpr std::uint32_t kWanted = 0x04;        // It is a document of the spine, the cover or a table of contents.

// The notes are kept in a byte; these do the sums on them as plain
// unsigned numbers.
constexpr bool Has(std::uint8_t flags, std::uint32_t flag) noexcept { return (std::uint32_t{flags} & flag) != 0; }
constexpr std::uint8_t With(std::uint8_t flags, std::uint32_t flag) noexcept {
  return static_cast<std::uint8_t>(std::uint32_t{flags} | flag);
}
constexpr std::uint8_t Without(std::uint8_t flags, std::uint32_t flag) noexcept {
  return static_cast<std::uint8_t>(std::uint32_t{flags} & ~flag);
}

bool IsPicture(MediaType type) noexcept {
  return type == MediaType::kJpeg || type == MediaType::kPng || type == MediaType::kGif || type == MediaType::kSvg;
}

}  // namespace

const char* PackageStatusName(PackageStatus status) noexcept {
  switch (status) {
    case PackageStatus::kOk:
      return "ok";
    case PackageStatus::kNoContainer:
      return "no container";
    case PackageStatus::kNoPackage:
      return "no package";
    case PackageStatus::kNotPackage:
      return "not a package";
    case PackageStatus::kNoMemory:
      return "no memory";
    case PackageStatus::kUnreadable:
      return "unreadable";
  }
  return "unknown";
}

MediaType MediaTypeOf(std::string_view name) noexcept {
  struct Known {
    std::string_view name;
    MediaType type;
  };
  constexpr std::array<Known, 9> kKnown = {{
      {"application/xhtml+xml", MediaType::kXhtml},
      {"text/html", MediaType::kXhtml},
      {"text/css", MediaType::kCss},
      {"application/x-dtbncx+xml", MediaType::kNcx},
      {"image/jpeg", MediaType::kJpeg},
      {"image/jpg", MediaType::kJpeg},  // Not a media type, but books say it.
      {"image/png", MediaType::kPng},
      {"image/gif", MediaType::kGif},
      {"image/svg+xml", MediaType::kSvg},
  }};
  for (const Known& known : kKnown) {
    if (SameButForCase(name, known.name)) {
      return known.type;
    }
  }
  // Fonts go by a dozen names: "font/otf", "application/x-font-ttf",
  // "application/vnd.ms-opentype", "application/font-sfnt", ...
  for (const std::string_view part : {"font", "opentype", "truetype"}) {
    for (std::size_t at = 0; at + part.size() <= name.size(); ++at) {
      if (SameButForCase(std::string_view(name.data() + at, part.size()), part)) {
        return MediaType::kFont;
      }
    }
  }
  return MediaType::kOther;
}

const char* MediaTypeName(MediaType type) noexcept {
  switch (type) {
    case MediaType::kOther:
      return "other";
    case MediaType::kXhtml:
      return "xhtml";
    case MediaType::kCss:
      return "css";
    case MediaType::kNcx:
      return "ncx";
    case MediaType::kJpeg:
      return "jpeg";
    case MediaType::kPng:
      return "png";
    case MediaType::kGif:
      return "gif";
    case MediaType::kSvg:
      return "svg";
    case MediaType::kFont:
      return "font";
  }
  return "unknown";
}

// ---- What is listened for in container.xml and in the package file ----

void Package::OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept {
  const std::string_view local = XmlLocalName(name);
  if (stage_ == Stage::kReadContainer) {
    StartInContainer(local, attributes);
    return;
  }
  ++depth_;
  if (depth_ == 2) {
    // The parts of a package.
    section_ = local == "metadata"   ? Section::kMetadata
               : local == "manifest" ? Section::kManifest
               : local == "spine"    ? Section::kSpine
                                     : Section::kOther;
    std::string_view toc;
    if (section_ == Section::kSpine && attributes.Find("toc", toc) && !toc.empty()) {
      ncxId_ = NameHash(toc);
      hasNcxId_ = true;
    }
    return;
  }
  if (depth_ < 3) {
    return;
  }
  switch (section_) {
    case Section::kMetadata:
      StartInMetadata(local, attributes);
      break;
    case Section::kManifest:
      if (depth_ == 3 && local == "item") {
        ManifestItem(attributes);
      }
      break;
    case Section::kSpine:
      if (depth_ == 3 && local == "itemref") {
        SpineReference(attributes);
      }
      break;
    case Section::kOther:
      break;
  }
}

void Package::OnEndElement(std::string_view /*name*/) noexcept {
  if (stage_ != Stage::kReadPackage) {
    return;
  }
  if (capture_ != Capture::kNone && depth_ == captureDepth_) {
    title_.RemoveTrailingSpace();
    author_.RemoveTrailingSpace();
    language_.RemoveTrailingSpace();
    capture_ = Capture::kNone;
  }
  --depth_;
}

void Package::OnText(std::string_view text) noexcept {
  switch (capture_) {
    case Capture::kNone:
      break;
    case Capture::kTitle:
      title_.AppendCollapsed(text);
      break;
    case Capture::kAuthor:
      author_.AppendCollapsed(text);
      break;
    case Capture::kLanguage:
      language_.AppendCollapsed(text);
      break;
  }
}

// An entity the parser left alone. The one that turns up in titles is the
// space that does not break; anything else is passed over.
void Package::OnEntity(std::string_view name) noexcept {
  if (name == "nbsp") {
    OnText(" ");
  }
}

void Package::StartInContainer(std::string_view local, const XmlAttributes& attributes) noexcept {
  if (local != "rootfile") {
    return;
  }
  std::string_view fullPath;
  if (!attributes.Find("full-path", fullPath) || fullPath.empty()) {
    return;
  }
  // The first one that says it is a package; failing that, the first.
  std::string_view type;
  const bool isPackage = attributes.Find("media-type", type) && SameButForCase(type, kPackageMediaType);
  if (packagePath_.Empty() || (isPackage && !foundPackage_)) {
    packagePath_.Clear();
    packagePath_.Append(fullPath);
    foundPackage_ = isPackage;
  }
}

void Package::StartInMetadata(std::string_view local, const XmlAttributes& attributes) noexcept {
  if (capture_ != Capture::kNone) {
    return;  // Inside a title: whatever this is, its text is the title's.
  }
  // The first title, the first creator and the first language that have
  // anything in them. Old books write these names with a capital.
  if (SameButForCase(local, "title") && title_.Empty()) {
    capture_ = Capture::kTitle;
  } else if (SameButForCase(local, "creator") && author_.Empty()) {
    capture_ = Capture::kAuthor;
  } else if (SameButForCase(local, "language") && language_.Empty()) {
    capture_ = Capture::kLanguage;
  } else if (local == "meta") {
    // EPUB 2's way of naming the cover: <meta name="cover" content="the-id"/>.
    std::string_view metaName;
    std::string_view content;
    if (!hasCoverId_ && attributes.Find("name", metaName) && metaName == "cover" &&
        attributes.Find("content", content) && !content.empty()) {
      coverId_ = NameHash(content);
      hasCoverId_ = true;
    }
  }
  captureDepth_ = depth_;
}

void Package::ManifestItem(const XmlAttributes& attributes) noexcept {
  std::string_view href;
  (void)attributes.Find("href", href);
  std::array<char, kMaxBookPath> path{};
  const std::size_t length = ResolveBookPath(base_, href, path);
  if (length == 0) {
    return;  // Not a file of the book: somewhere else, or nowhere at all.
  }
  std::string_view id;
  std::string_view type;
  std::string_view properties;
  std::uint8_t flags = 0;
  // An item with no id gets the number of an empty one, which nothing is
  // ever looked up by.
  (void)attributes.Find("id", id);
  (void)attributes.Find("media-type", type);
  if (attributes.Find("properties", properties)) {
    if (HasWord(properties, "nav")) {
      flags = With(flags, kIsNav);
    }
    if (HasWord(properties, "cover-image")) {
      flags = With(flags, kIsCoverImage);
    }
  }
  if (!AddItem(NameHash(id), NameHash(std::string_view(path.data(), length)), MediaTypeOf(type), flags)) {
    OutOfMemory();
  }
}

void Package::SpineReference(const XmlAttributes& attributes) noexcept {
  std::string_view idref;
  if (!attributes.Find("idref", idref) || idref.empty()) {
    return;
  }
  std::string_view linear;
  const bool isLinear = !attributes.Find("linear", linear) || linear != "no";
  if (!AddSpine(NameHash(idref), isLinear)) {
    OutOfMemory();
  }
}

void Package::OutOfMemory() noexcept {
  outOfMemory_ = true;
  xml_->Stop();
}

// ---- The package ----

Package::Package(std::span<std::uint8_t> memory) noexcept {
  // Both ends on a boundary that either kind of record can sit at.
  constexpr std::size_t kAlignment = std::max(alignof(SpineItem), alignof(Item));
  const auto address = reinterpret_cast<std::uintptr_t>(memory.data());  // NOLINT(*-reinterpret-cast): to align it.
  const std::size_t skipped = static_cast<std::size_t>((kAlignment - (address % kAlignment)) % kAlignment);
  if (memory.data() == nullptr || memory.size() <= skipped) {
    return;
  }
  const std::size_t usable = (memory.size() - skipped) / kAlignment * kAlignment;
  std::uint8_t* start = memory.data() + skipped;
  // The block is bytes; the records are made in it as they are added.
  spine_ = reinterpret_cast<SpineItem*>(start);         // NOLINT(*-reinterpret-cast)
  itemsEnd_ = reinterpret_cast<Item*>(start + usable);  // NOLINT(*-reinterpret-cast)
  capacityBytes_ = usable;
}

void Package::Clear() noexcept {
  spineCount_ = 0;
  itemCount_ = 0;
  memoryUsed_ = 0;
  cutShort_ = false;
  outOfMemory_ = false;
  unknownSpineItems_ = 0;
  title_.Clear();
  author_.Clear();
  language_.Clear();
  packagePath_.Clear();
  cover_ = {};
  nav_ = {};
  ncx_ = {};
  coverId_ = 0;
  ncxId_ = 0;
  hasCoverId_ = false;
  hasNcxId_ = false;
  cursor_ = {};
  looked_ = 0;
  triedDecoded_ = false;
  resolved_ = 0;
  kept_ = 0;
  wanted_ = 0;
  depth_ = 0;
  section_ = Section::kOther;
  capture_ = Capture::kNone;
  captureDepth_ = 0;
  base_ = {};
  foundPackage_ = false;
}

void Package::EndPart() noexcept {
  // The part of the reading that has just ended goes into the trace.
  // Finding a file and parsing it are one part; so are the two halves of
  // the matching.
  switch (stage_) {
    case Stage::kIdle:
      break;
    case Stage::kFindContainer:
    case Stage::kReadContainer:
      BN_TRACE_SPAN_SINCE(kSpanPackageContainer, partBegan_, 0);
      break;
    case Stage::kFindPackage:
    case Stage::kReadPackage:
      BN_TRACE_SPAN_SINCE(kSpanPackageFile, partBegan_, 0);
      break;
    case Stage::kResolveSpine:
    case Stage::kResolveRest:
      // How many documents the spine named: those kept and those dropped.
      BN_TRACE_SPAN_SINCE(kSpanPackageResolve, partBegan_,
                          static_cast<std::uint32_t>(spineCount_) + unknownSpineItems_);
      break;
    case Stage::kFindFiles:
      BN_TRACE_SPAN_SINCE(kSpanPackageFindFiles, partBegan_, itemCount_);
      break;
  }
  partBegan_ = BN_TRACE_NOW();
}

PackageStatus Package::End(PackageStatus status) noexcept {
  EndPart();
  stage_ = Stage::kIdle;
  if (status != PackageStatus::kOk) {
    // Nothing half found is left to be mistaken for an answer.
    spineCount_ = 0;
    cover_ = {};
    nav_ = {};
    ncx_ = {};
    packagesFailed.Add();
  }
  status_ = status;
  BN_TRACE_TOP_SPAN_SINCE(kSpanPackageRead, began_, static_cast<std::uint32_t>(status));
  return status;
}

void Package::MoveTo(Stage stage) noexcept {
  const bool samePart = (stage_ == Stage::kFindContainer && stage == Stage::kReadContainer) ||
                        (stage_ == Stage::kFindPackage && stage == Stage::kReadPackage) ||
                        (stage_ == Stage::kResolveSpine && stage == Stage::kResolveRest);
  if (!samePart) {
    EndPart();
  }
  stage_ = stage;
}

bool Package::AddItem(std::uint64_t idHash, std::uint64_t pathHash, MediaType type, std::uint8_t flags) noexcept {
  const std::size_t used = (spineCount_ * sizeof(SpineItem)) + (itemCount_ * sizeof(Item));
  if (capacityBytes_ - used < sizeof(Item)) {
    return false;
  }
  ::new (static_cast<void*>(&ItemAt(itemCount_))) Item{.idLow = static_cast<std::uint32_t>(idHash),
                                                       .pathLow = static_cast<std::uint32_t>(pathHash),
                                                       .idHigh = static_cast<std::uint16_t>(idHash >> 32U),
                                                       .pathHigh = static_cast<std::uint16_t>(pathHash >> 32U),
                                                       .type = type,
                                                       .flags = flags,
                                                       .spineIndex = kNoSpineIndex};
  ++itemCount_;
  memoryUsed_ = std::max(memoryUsed_, used + sizeof(Item));
  return true;
}

bool Package::AddSpine(std::uint64_t idHash, bool linear) noexcept {
  const std::size_t used = (spineCount_ * sizeof(SpineItem)) + (itemCount_ * sizeof(Item));
  if (capacityBytes_ - used < sizeof(SpineItem)) {
    return false;
  }
  // Until the ids are matched, the entry's first two numbers hold the id
  // the spine gave: Resolve() puts the item's place there, and
  // FindFiles() the entry itself.
  SpineItem* item = ::new (static_cast<void*>(spine_ + spineCount_)) SpineItem{};
  item->entry.headerOffset = static_cast<std::uint32_t>(idHash);
  item->entry.compressedSize = static_cast<std::uint32_t>(idHash >> 32U);
  item->linear = linear;
  ++spineCount_;
  memoryUsed_ = std::max(memoryUsed_, used + sizeof(SpineItem));
  return true;
}

std::uint32_t Package::ItemWithId(std::uint64_t idHash) const noexcept {
  const auto low = static_cast<std::uint32_t>(idHash);
  const auto high = static_cast<std::uint16_t>(idHash >> 32U);
  for (std::uint32_t index = 0; index < itemCount_; ++index) {
    const Item& item = ItemAt(index);
    if (item.idLow == low && item.idHigh == high) {
      return index;
    }
  }
  return kNoItem;
}

std::size_t Package::SpineIndexOf(std::string_view path) const noexcept {
  if (spineCount_ == 0 || Reading()) {
    return kNotInSpine;  // No book, or one that could not be read, or one not yet read.
  }
  const std::uint64_t hash = NameHash(path);
  const auto low = static_cast<std::uint32_t>(hash);
  const auto high = static_cast<std::uint16_t>(hash >> 32U);
  // Two items can have one path, and the spine may use either or both of
  // them: the place is the first that any of them has.
  std::size_t first = kNotInSpine;
  for (std::uint32_t index = 0; index < itemCount_; ++index) {
    const Item& item = ItemAt(index);
    if (item.pathLow == low && item.pathHigh == high && item.spineIndex != kNoSpineIndex) {
      first = std::min<std::size_t>(first, item.spineIndex);
    }
  }
  return first;
}

// ---- The reading, a piece at a time ----

PackageStatus Package::Begin(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                             XmlParser& xml) noexcept {
  Clear();
  packagesRead.Add();
  archive_ = &archive;
  entries_ = &entries;
  reader_ = &reader;
  xml_ = &xml;
  began_ = BN_TRACE_NOW();
  partBegan_ = began_;
  if (spine_ == nullptr) {
    return End(PackageStatus::kNoMemory);
  }
  status_ = PackageStatus::kOk;
  stage_ = Stage::kFindContainer;
  return status_;
}

bool Package::Step() noexcept {
  switch (stage_) {
    case Stage::kIdle:
      break;
    case Stage::kFindContainer:
      FindContainer();
      break;
    case Stage::kReadContainer:
      ReadContainer();
      break;
    case Stage::kFindPackage:
      FindPackage();
      break;
    case Stage::kReadPackage:
      ReadPackageFile();
      break;
    case Stage::kResolveSpine:
      ResolveSpine();
      break;
    case Stage::kResolveRest:
      ResolveRest();
      break;
    case Stage::kFindFiles:
      FindFiles();
      break;
  }
  return Reading();
}

PackageStatus Package::Read(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                            XmlParser& xml) noexcept {
  if (Begin(archive, entries, reader, xml) == PackageStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

ZipStatus Package::NextEntry(ZipEntry& entry, std::span<char> name, std::uint32_t& gone) noexcept {
  const bool first = cursor_.index == 0;
  const std::uint32_t before = cursor_.offset;
  const ZipStatus status = archive_->Next(cursor_, entry, name);
  if (status == ZipStatus::kOk || status == ZipStatus::kUnsupported) {
    // Where the first entry is, only the archive knows: what is counted
    // for it is the least a record of its name's length takes.
    gone += first ? 46U + entry.nameLength : cursor_.offset - before;
  }
  return status;
}

Package::Looked Package::LookFor(std::string_view name, ZipEntry& entry) noexcept {
  std::array<char, kMaxBookPath> found{};
  std::uint32_t gone = 0;
  for (std::uint32_t entries = 0; entries < kEntriesPerStep && gone < kDirectoryBytesPerStep; ++entries) {
    if (looked_ >= archive_->EntryCount()) {
      // Every entry has been looked at. (An archive that is not open has
      // none, and says what it is when it is asked for one.)
      return archive_->IsOpen() ? Looked::kMissing : Looked::kUnreadable;
    }
    ZipEntry candidate;
    const ZipStatus status = NextEntry(candidate, found, gone);
    if (status == ZipStatus::kEnd) {
      // Round to the start, for the entries that come before where the
      // search began. That is this step's work done: what is read next is
      // somewhere else in the file.
      cursor_ = {};
      return Looked::kMore;
    }
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      return Looked::kUnreadable;
    }
    ++looked_;
    if (candidate.nameLength == name.size() && std::string_view(found.data(), name.size()) == name) {
      if (status == ZipStatus::kUnsupported) {
        return Looked::kUnreadable;  // It is there, in a form that cannot be read.
      }
      entry = candidate;
      return Looked::kFound;
    }
  }
  return Looked::kMore;
}

bool Package::BeginParsing(const ZipEntry& entry) noexcept {
  if (reader_->Open(*entries_, entry) != ZipStatus::kOk) {
    (void)End(PackageStatus::kUnreadable);
    return false;
  }
  if (xml_->Begin(*reader_, *this) != XmlStatus::kOk) {
    (void)End(PackageStatus::kNoMemory);  // All that can stop a parser beginning.
    return false;
  }
  return true;
}

// Where the package file is: container.xml says.
void Package::FindContainer() noexcept {
  ZipEntry entry;
  switch (LookFor(kContainerName, entry)) {
    case Looked::kMore:
      break;
    case Looked::kMissing:
      (void)End(PackageStatus::kNoContainer);
      break;
    case Looked::kUnreadable:
      (void)End(PackageStatus::kUnreadable);
      break;
    case Looked::kFound:
      if (BeginParsing(entry)) {
        MoveTo(Stage::kReadContainer);
      }
      break;
  }
}

void Package::ReadContainer() noexcept {
  if (xml_->Step()) {
    return;
  }
  switch (xml_->Status()) {
    case XmlStatus::kSourceFailed:
      (void)End(PackageStatus::kUnreadable);
      return;
    case XmlStatus::kNoMemory:
      (void)End(PackageStatus::kNoMemory);
      return;
    default:
      break;  // What matters is whether it named a package before it ended, however it ended.
  }
  if (packagePath_.Empty() || packagePath_.Truncated()) {
    (void)End(PackageStatus::kNoContainer);
    return;
  }
  // The search carries on from where container.xml was, which is usually
  // just before the package file.
  looked_ = 0;
  MoveTo(Stage::kFindPackage);
}

// The package file. Its path is taken as written; if no entry has that
// name, as a reference would be read, with its %XX turned into bytes.
void Package::FindPackage() noexcept {
  ZipEntry entry;
  switch (LookFor(PackagePath(), entry)) {
    case Looked::kMore:
      break;
    case Looked::kUnreadable:
      (void)End(PackageStatus::kUnreadable);
      break;
    case Looked::kMissing: {
      std::array<char, kMaxBookPath> decoded{};
      const std::size_t length = triedDecoded_ ? 0 : ResolveBookPath("", PackagePath(), decoded);
      const std::string_view other(decoded.data(), length);
      if (length == 0 || other == PackagePath()) {
        (void)End(PackageStatus::kNoPackage);
        break;
      }
      packagePath_.Clear();
      packagePath_.Append(other);
      triedDecoded_ = true;
      looked_ = 0;
      break;
    }
    case Looked::kFound:
      if (BeginParsing(entry)) {
        base_ = BookDirectory(PackagePath());
        depth_ = 0;
        section_ = Section::kOther;
        capture_ = Capture::kNone;
        MoveTo(Stage::kReadPackage);
      }
      break;
  }
}

void Package::ReadPackageFile() noexcept {
  if (xml_->Step()) {
    return;
  }
  const XmlStatus ended = xml_->Status();
  if (outOfMemory_ || ended == XmlStatus::kNoMemory) {
    (void)End(PackageStatus::kNoMemory);
    return;
  }
  if (ended == XmlStatus::kSourceFailed) {
    (void)End(PackageStatus::kUnreadable);
    return;
  }
  // A package that goes wrong after its spine has said all that is needed
  // of it.
  cutShort_ = ended != XmlStatus::kOk && ended != XmlStatus::kStopped;
  capture_ = Capture::kNone;
  resolved_ = 0;
  kept_ = 0;
  MoveTo(Stage::kResolveSpine);
}

void Package::ResolveSpine() noexcept {
  // Each document's id becomes the item it names. One that names no item
  // is dropped. Looking an id up goes through the manifest, so only so
  // many are looked up in one step.
  std::uint32_t compares = 0;
  for (; resolved_ < spineCount_ && compares < kComparesPerStep; ++resolved_) {
    compares += itemCount_ + 1;
    const SpineItem& in = spine_[resolved_];
    const std::uint64_t id = in.entry.headerOffset | (std::uint64_t{in.entry.compressedSize} << 32U);
    const std::uint32_t index = ItemWithId(id);
    if (index == kNoItem) {
      ++unknownSpineItems_;
      continue;
    }
    Item& item = ItemAt(index);
    item.flags = With(item.flags, kWanted);
    if (item.spineIndex == kNoSpineIndex && kept_ < kNoSpineIndex) {
      item.spineIndex = static_cast<std::uint16_t>(kept_);
    }
    const bool linear = in.linear;
    SpineItem& out = spine_[kept_++];
    out.linear = linear;
    out.entry = {};
    out.entry.headerOffset = index;
    out.present = false;
    out.type = item.type;
  }
  if (resolved_ < spineCount_) {
    return;
  }
  spineCount_ = kept_;
  MoveTo(Stage::kResolveRest);
}

void Package::ResolveRest() noexcept {
  // The cover: the item the package marks as the cover picture; failing
  // that, the one EPUB 2's <meta name="cover"> names, if it is a picture
  // (some books name the cover's page there, not its picture).
  // The tables of contents: the item marked as the nav; and the NCX the
  // spine names, or failing that the first item that is one.
  for (std::uint32_t index = 0; index < itemCount_; ++index) {
    const Item& item = ItemAt(index);
    if (Has(item.flags, kIsCoverImage) && cover_.item == kNoItem) {
      cover_.item = index;
    }
    if (Has(item.flags, kIsNav) && nav_.item == kNoItem) {
      nav_.item = index;
    }
  }
  if (cover_.item == kNoItem && hasCoverId_) {
    const std::uint32_t index = ItemWithId(coverId_);
    if (index != kNoItem && IsPicture(ItemAt(index).type)) {
      cover_.item = index;
    }
  }
  if (hasNcxId_) {
    ncx_.item = ItemWithId(ncxId_);
  }
  for (std::uint32_t index = 0; index < itemCount_ && ncx_.item == kNoItem; ++index) {
    if (ItemAt(index).type == MediaType::kNcx) {
      ncx_.item = index;
    }
  }
  for (Found* found : {&cover_, &nav_, &ncx_}) {
    if (found->item != kNoItem) {
      ItemAt(found->item).flags = With(ItemAt(found->item).flags, kWanted);
      found->type = ItemAt(found->item).type;
    }
  }
  // How many files the directory is to be gone through for.
  wanted_ = 0;
  for (std::uint32_t index = 0; index < itemCount_; ++index) {
    wanted_ += Has(ItemAt(index).flags, kWanted) ? 1 : 0;
  }
  if (spineCount_ == 0) {
    (void)End(PackageStatus::kNotPackage);
    return;
  }
  cursor_ = {};
  MoveTo(Stage::kFindFiles);
}

void Package::FindFiles() noexcept {
  // Once through the directory, in its own order, some entries at a step.
  // Each entry's name is turned into its number and looked for among the
  // manifest's.
  std::array<char, kMaxBookPath> name{};
  std::uint32_t gone = 0;
  std::uint32_t compares = 0;
  bool ended = false;
  for (std::uint32_t entries = 0;
       wanted_ > 0 && entries < kEntriesPerStep && gone < kDirectoryBytesPerStep && compares < kComparesPerStep;
       ++entries) {
    ZipEntry entry;
    const ZipStatus status = NextEntry(entry, name, gone);
    if (status == ZipStatus::kEnd) {
      ended = true;
      break;
    }
    if (status == ZipStatus::kUnsupported || (status == ZipStatus::kOk && entry.nameLength > name.size())) {
      continue;  // An entry that cannot be read, or with a name longer than any the manifest's were kept by.
    }
    if (status != ZipStatus::kOk) {
      (void)End(PackageStatus::kUnreadable);
      return;
    }
    compares += itemCount_;
    const std::uint64_t hash = NameHash(std::string_view(name.data(), entry.nameLength));
    const auto low = static_cast<std::uint32_t>(hash);
    const auto high = static_cast<std::uint16_t>(hash >> 32U);
    for (std::uint32_t index = 0; index < itemCount_; ++index) {
      Item& item = ItemAt(index);
      if (item.pathLow != low || item.pathHigh != high || !Has(item.flags, kWanted)) {
        continue;
      }
      // Wanted, and not found before: if the ZIP file has the name twice,
      // the first is the one.
      item.flags = Without(item.flags, kWanted);
      --wanted_;
      for (std::size_t i = 0; i < spineCount_; ++i) {
        if (!spine_[i].present && spine_[i].entry.headerOffset == index) {
          spine_[i].entry = entry;
          spine_[i].present = true;
        }
      }
      for (Found* found : {&cover_, &nav_, &ncx_}) {
        if (found->item == index) {
          found->entry = entry;
          found->present = true;
        }
      }
    }
  }
  if (wanted_ > 0 && !ended) {
    return;
  }
  // What was not found has no entry.
  for (std::size_t i = 0; i < spineCount_; ++i) {
    if (!spine_[i].present) {
      spine_[i].entry = {};
    }
  }
  (void)End(PackageStatus::kOk);
}

}  // namespace bn::epub
