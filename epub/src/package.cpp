#include "epub/package.h"

#include <algorithm>
#include <array>
#include <new>

#include "core/metrics.h"
#include "core/trace.h"
#include "epub/trace_ids.h"

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

constexpr char Lower(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

// Whether two names are the same but for capitals among the letters of
// English.
bool SameButForCase(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

// Whether `word` is one of the words of `list`, which has spaces between
// them.
bool HasWord(std::string_view list, std::string_view word) noexcept {
  std::size_t start = 0;
  for (std::size_t i = 0; i <= list.size(); ++i) {
    if (i == list.size() || list[i] == ' ' || list[i] == '\t' || list[i] == '\n' || list[i] == '\r') {
      if (std::string_view(list.data() + start, i - start) == word) {
        return true;
      }
      start = i + 1;
    }
  }
  return false;
}

bool IsPicture(MediaType type) noexcept {
  return type == MediaType::kJpeg || type == MediaType::kPng || type == MediaType::kGif || type == MediaType::kSvg;
}

// How parsing one entry of the book went.
enum class Parsed : std::uint8_t {
  kWhole,       // To its end, and it is XML.
  kMissing,     // There is no entry of that name.
  kNotXml,      // The parser stopped in it.
  kStopped,     // The handler stopped the parser.
  kNoMemory,    // The parser ran out of memory.
  kUnreadable,  // The ZIP file is damaged there, or storage failed.
};

// Parses the entry called `name`, telling `handler` what is in it.
Parsed ParseEntry(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader, XmlParser& xml,
                  std::string_view name, XmlHandler& handler) noexcept {
  ZipEntry entry;
  ZipStatus status = archive.Find(name, entry);
  if (status == ZipStatus::kNotFound) {
    return Parsed::kMissing;
  }
  if (status == ZipStatus::kOk) {
    status = reader.Open(entries, entry);
  }
  if (status != ZipStatus::kOk) {
    return Parsed::kUnreadable;
  }
  switch (xml.Parse(reader, handler)) {
    case XmlStatus::kOk:
      return Parsed::kWhole;
    case XmlStatus::kNoMemory:
      return Parsed::kNoMemory;
    case XmlStatus::kStopped:
      return Parsed::kStopped;
    case XmlStatus::kSourceFailed:
      return Parsed::kUnreadable;
    default:
      return Parsed::kNotXml;
  }
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

// ---- What is listened for in container.xml ----

namespace {

class ContainerHandler final : public XmlHandler {
 public:
  explicit ContainerHandler(core::FixedText<kMaxBookPath>& path) noexcept : path_(path) {}

  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override {
    if (XmlLocalName(name) != "rootfile") {
      return;
    }
    std::string_view fullPath;
    if (!attributes.Find("full-path", fullPath) || fullPath.empty()) {
      return;
    }
    // The first one that says it is a package; failing that, the first.
    std::string_view type;
    const bool isPackage = attributes.Find("media-type", type) && SameButForCase(type, kPackageMediaType);
    if (path_.Empty() || (isPackage && !foundPackage_)) {
      path_.Clear();
      path_.Append(fullPath);
      foundPackage_ = isPackage;
    }
  }
  void OnEndElement(std::string_view /*name*/) noexcept override {}
  void OnText(std::string_view /*text*/) noexcept override {}
  void OnEntity(std::string_view /*name*/) noexcept override {}

 private:
  core::FixedText<kMaxBookPath>& path_;
  bool foundPackage_ = false;
};

}  // namespace

// ---- What is listened for in the package file ----

class Package::PackageHandler final : public XmlHandler {
 public:
  PackageHandler(Package& package, XmlParser& xml) noexcept
      : package_(package), xml_(xml), base_(BookDirectory(package.PackagePath())) {}

  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override {
    ++depth_;
    const std::string_view local = XmlLocalName(name);
    if (depth_ == 2) {
      // The parts of a package.
      section_ = local == "metadata"   ? Section::kMetadata
                 : local == "manifest" ? Section::kManifest
                 : local == "spine"    ? Section::kSpine
                                       : Section::kOther;
      std::string_view toc;
      if (section_ == Section::kSpine && attributes.Find("toc", toc) && !toc.empty()) {
        package_.ncxId_ = NameHash(toc);
        package_.hasNcxId_ = true;
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
          Item(attributes);
        }
        break;
      case Section::kSpine:
        if (depth_ == 3 && local == "itemref") {
          ItemRef(attributes);
        }
        break;
      case Section::kOther:
        break;
    }
  }

  void OnEndElement(std::string_view /*name*/) noexcept override {
    if (capture_ != Capture::kNone && depth_ == captureDepth_) {
      package_.title_.RemoveTrailingSpace();
      package_.author_.RemoveTrailingSpace();
      package_.language_.RemoveTrailingSpace();
      capture_ = Capture::kNone;
    }
    --depth_;
  }

  void OnText(std::string_view text) noexcept override {
    switch (capture_) {
      case Capture::kNone:
        break;
      case Capture::kTitle:
        package_.title_.AppendCollapsed(text);
        break;
      case Capture::kAuthor:
        package_.author_.AppendCollapsed(text);
        break;
      case Capture::kLanguage:
        package_.language_.AppendCollapsed(text);
        break;
    }
  }

  // An entity the parser left alone. The one that turns up in titles is
  // the space that does not break; anything else is passed over.
  void OnEntity(std::string_view name) noexcept override {
    if (name == "nbsp") {
      OnText(" ");
    }
  }

 private:
  enum class Section : std::uint8_t { kOther, kMetadata, kManifest, kSpine };
  enum class Capture : std::uint8_t { kNone, kTitle, kAuthor, kLanguage };

  void StartInMetadata(std::string_view local, const XmlAttributes& attributes) noexcept {
    if (capture_ != Capture::kNone) {
      return;  // Inside a title: whatever this is, its text is the title's.
    }
    // The first title, the first creator and the first language that have
    // anything in them. Old books write these names with a capital.
    if (SameButForCase(local, "title") && package_.title_.Empty()) {
      capture_ = Capture::kTitle;
    } else if (SameButForCase(local, "creator") && package_.author_.Empty()) {
      capture_ = Capture::kAuthor;
    } else if (SameButForCase(local, "language") && package_.language_.Empty()) {
      capture_ = Capture::kLanguage;
    } else if (local == "meta") {
      // EPUB 2's way of naming the cover: <meta name="cover" content="the-id"/>.
      std::string_view metaName;
      std::string_view content;
      if (!package_.hasCoverId_ && attributes.Find("name", metaName) && metaName == "cover" &&
          attributes.Find("content", content) && !content.empty()) {
        package_.coverId_ = NameHash(content);
        package_.hasCoverId_ = true;
      }
    }
    captureDepth_ = depth_;
  }

  void Item(const XmlAttributes& attributes) noexcept {
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
    // An item with no id gets the number of an empty one, which nothing
    // is ever looked up by.
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
    if (!package_.AddItem(NameHash(id), NameHash(std::string_view(path.data(), length)), MediaTypeOf(type), flags)) {
      OutOfMemory();
    }
  }

  void ItemRef(const XmlAttributes& attributes) noexcept {
    std::string_view idref;
    if (!attributes.Find("idref", idref) || idref.empty()) {
      return;
    }
    std::string_view linear;
    const bool isLinear = !attributes.Find("linear", linear) || linear != "no";
    if (!package_.AddSpine(NameHash(idref), isLinear)) {
      OutOfMemory();
    }
  }

  void OutOfMemory() noexcept {
    package_.outOfMemory_ = true;
    xml_.Stop();
  }

  Package& package_;
  XmlParser& xml_;
  std::string_view base_;
  int depth_ = 0;
  Section section_ = Section::kOther;
  Capture capture_ = Capture::kNone;
  int captureDepth_ = 0;
};

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
}

PackageStatus Package::Fail(PackageStatus status) noexcept {
  // Nothing half found is left to be mistaken for an answer.
  spineCount_ = 0;
  cover_ = {};
  nav_ = {};
  ncx_ = {};
  status_ = status;
  packagesFailed.Add();
  return status;
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
                                                       .spare = 0};
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

void Package::Resolve() noexcept {
  // The spine: each document's id becomes the item it names. One that
  // names no item is dropped.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < spineCount_; ++i) {
    const std::uint64_t id = spine_[i].entry.headerOffset | (std::uint64_t{spine_[i].entry.compressedSize} << 32U);
    const std::uint32_t index = ItemWithId(id);
    if (index == kNoItem) {
      ++unknownSpineItems_;
      continue;
    }
    ItemAt(index).flags = With(ItemAt(index).flags, kWanted);
    SpineItem& out = spine_[kept++];
    out.linear = spine_[i].linear;
    out.entry = {};
    out.entry.headerOffset = index;
    out.present = false;
    out.type = ItemAt(index).type;
  }
  spineCount_ = kept;

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
}

PackageStatus Package::FindFiles(ZipArchive& archive) noexcept {
  std::uint32_t wanted = 0;
  for (std::uint32_t index = 0; index < itemCount_; ++index) {
    wanted += Has(ItemAt(index).flags, kWanted) ? 1 : 0;
  }
  // Once through the directory, in its own order. Each entry's name is
  // turned into its number and looked for among the manifest's.
  std::array<char, kMaxBookPath> name{};
  ZipArchive::Cursor cursor;
  while (wanted > 0) {
    ZipEntry entry;
    const ZipStatus status = archive.Next(cursor, entry, name);
    if (status == ZipStatus::kEnd) {
      break;
    }
    if (status == ZipStatus::kUnsupported || (status == ZipStatus::kOk && entry.nameLength > name.size())) {
      continue;  // An entry that cannot be read, or with a name longer than any the manifest's were kept by.
    }
    if (status != ZipStatus::kOk) {
      return PackageStatus::kUnreadable;
    }
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
      --wanted;
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
  // What was not found has no entry.
  for (std::size_t i = 0; i < spineCount_; ++i) {
    if (!spine_[i].present) {
      spine_[i].entry = {};
    }
  }
  return PackageStatus::kOk;
}

PackageStatus Package::Read(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                            XmlParser& xml) noexcept {
  const std::uint32_t began = BN_TRACE_NOW();
  const PackageStatus status = ReadParts(archive, entries, reader, xml);
  BN_TRACE_TOP_SPAN_SINCE(kSpanPackageRead, began, static_cast<std::uint32_t>(status));
  return status;
}

PackageStatus Package::ReadParts(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader,
                                 XmlParser& xml) noexcept {
  Clear();
  packagesRead.Add();
  if (spine_ == nullptr) {
    return Fail(PackageStatus::kNoMemory);
  }

  // Where the package file is.
  Parsed parsed = Parsed::kMissing;
  {
    BN_TRACE_SCOPE(kSpanPackageContainer);
    ContainerHandler container(packagePath_);
    parsed = ParseEntry(archive, entries, reader, xml, kContainerName, container);
  }
  switch (parsed) {
    case Parsed::kUnreadable:
      return Fail(PackageStatus::kUnreadable);
    case Parsed::kNoMemory:
      return Fail(PackageStatus::kNoMemory);
    default:
      break;  // What matters is whether it named a package before it ended, however it ended.
  }
  if (packagePath_.Empty() || packagePath_.Truncated()) {
    return Fail(PackageStatus::kNoContainer);
  }

  // The package file. Its path is taken as written; if no entry has that
  // name, as a reference would be read, with its %XX turned into bytes.
  {
    BN_TRACE_SCOPE(kSpanPackageFile);
    PackageHandler handler(*this, xml);
    parsed = ParseEntry(archive, entries, reader, xml, PackagePath(), handler);
    if (parsed == Parsed::kMissing) {
      std::array<char, kMaxBookPath> decoded{};
      const std::size_t length = ResolveBookPath("", PackagePath(), decoded);
      const std::string_view other(decoded.data(), length);
      if (length == 0 || other == PackagePath()) {
        return Fail(PackageStatus::kNoPackage);
      }
      packagePath_.Clear();
      packagePath_.Append(other);
      PackageHandler again(*this, xml);
      parsed = ParseEntry(archive, entries, reader, xml, PackagePath(), again);
    }
  }
  if (outOfMemory_ || parsed == Parsed::kNoMemory) {
    return Fail(PackageStatus::kNoMemory);
  }
  switch (parsed) {
    case Parsed::kMissing:
      return Fail(PackageStatus::kNoPackage);
    case Parsed::kUnreadable:
      return Fail(PackageStatus::kUnreadable);
    case Parsed::kNotXml:
      // A package that goes wrong after its spine has said all that is
      // needed of it.
      cutShort_ = true;
      break;
    default:
      break;
  }

  {
    BN_TRACE_SCOPE_ARG(kSpanPackageResolve, static_cast<std::uint32_t>(spineCount_));
    Resolve();
  }
  if (spineCount_ == 0) {
    return Fail(PackageStatus::kNotPackage);
  }
  PackageStatus found = PackageStatus::kOk;
  {
    BN_TRACE_SCOPE_ARG(kSpanPackageFindFiles, itemCount_);
    found = FindFiles(archive);
  }
  if (found != PackageStatus::kOk) {
    return Fail(found);
  }
  status_ = PackageStatus::kOk;
  return status_;
}

}  // namespace bn::epub
