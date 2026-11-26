// The fuzz target for the making of a book's bundle (epub/book_bundle.h).
//
// What it is given:
//
//   byte 0   bit 0:        0, the target makes the book; 1, the rest is the book
//            bit 1:        how much memory the XML parser has
//            bit 2:        in a book the target makes, the files are compressed
//            bit 3:        and are in the ZIP file last first
//            bit 4:        and the ZIP file ends with a comment
//
//   then, for a book the target makes:
//
//   2 bytes  the length of what follows, low byte first
//   so many  OEBPS/content.opf, the package file
//   2 bytes  the length of what follows
//   so many  OEBPS/toc.ncx
//   the rest OEBPS/nav.xhtml
//
//   The book is a sound ZIP file of those three, a container that points
//   at the package, and a few documents and pictures that a package may
//   name (kOtherFiles below): a ZIP file made up of the bytes themselves
//   is stopped at an entry's checksum by nearly every change to them. For
//   a book that is the bytes themselves, the rest is the ZIP file.
//
// The book's package is read, and its bundle made a step at a time. What
// has to hold, whatever the bytes are:
//
//   - a book whose package could not be read is no book to make a bundle
//     of, and nothing is written for it;
//   - the making ends, with a status it can give; a sound ZIP file whose
//     package was read is never refused;
//   - no step reads more of the book than a step may (five blocks), writes
//     more than two pages of the bundle, each a whole page at a page's
//     place, or syncs more than once;
//   - a bundle that was made opens, and says what the making said of it:
//     as many documents and pictures as the package has, the EPUB's size,
//     the package's checksum, and the checksum of the ZIP file's directory
//     as it is worked out here from the file's bytes;
//   - each of its four tables is what its checksum says;
//   - its strings are the package's title, author, language and
//     identifier;
//   - a document's record says where the package says its file is, of the
//     kind it is, in the run of the book or not;
//   - a picture's record that says its file is there is of the first file
//     in the ZIP file's directory that the package takes for that picture,
//     and every picture with such a file has such a record; the cover is
//     marked only on the picture the package says is the cover; and the
//     directory was gone through once for every 170 pictures;
//   - its contents are the entries a reading of the table of contents
//     gives here, in their order, and the header says they are not the
//     whole of the book's if that reading did not end well;
//   - every chapter is not imported;
//   - made again, the bundle is the same byte for byte.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/book_bundle.h"
#include "epub/bundle.h"
#include "epub/package.h"
#include "epub/package_digest.h"
#include "epub/toc.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "fuzz_book.h"
#include "fuzz_support.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::BookBundler;
using bn::epub::BookBundleStatus;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::Package;
using bn::epub::PackageStatus;
using bn::epub::SpineItem;
using bn::epub::ZipEntry;
using bn::epub::ZipStatus;
using bn::test::FakeStorage;
using Bytes = std::vector<std::uint8_t>;

constexpr std::array<std::size_t, 2> kParserMemoryBytes = {32768, 12288};
constexpr std::size_t kPackageMemoryBytes = 65536;

// The files a made book has besides its package and its tables of
// contents, for a package to name. The kept inputs name them, and what the
// engine makes of those mostly still does.
constexpr std::array<std::string_view, 10> kOtherFiles = {
    "OEBPS/ch1.xhtml",   "OEBPS/img/cover.jpg", "OEBPS/text/ch2.xhtml", "OEBPS/img/plate.png", "OEBPS/ch 3.xhtml",
    "OEBPS/img/a b.gif", "OEBPS/notes.xhtml",   "OEBPS/img/d.svg",      "OEBPS/style.css",     "ch4.xhtml"};

constexpr std::string_view kContainer =
    "<container><rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

// A making can be as long as its book's maker likes, a step at a time,
// but not endless: a book of so many bytes cannot ask for more steps than
// this.
constexpr std::size_t MostSteps(std::size_t bookBytes) noexcept { return 4096 * (bookBytes + 64); }

Bytes MadeBook(bn::fuzz::Input& input, unsigned choices) {
  const std::span<const std::uint8_t> package = input.Take(input.Number16());
  const std::span<const std::uint8_t> ncx = input.Take(input.Number16());
  const std::span<const std::uint8_t> nav = input.Rest();
  std::vector<bn::fuzz::BookFile> files;
  files.emplace_back("mimetype", bn::fuzz::BytesOfText("application/epub+zip"));
  files.emplace_back("META-INF/container.xml", bn::fuzz::BytesOfText(kContainer));
  for (std::size_t i = 0; i < kOtherFiles.size(); ++i) {
    files.emplace_back(std::string(kOtherFiles[i]), bn::fuzz::BytesOfText("file " + std::to_string(i)));
    if (i == 3) {
      files.emplace_back("OEBPS/toc.ncx", Bytes(ncx.begin(), ncx.end()));
    }
    if (i == 6) {
      files.emplace_back("OEBPS/content.opf", Bytes(package.begin(), package.end()));
    }
  }
  files.emplace_back("OEBPS/nav.xhtml", Bytes(nav.begin(), nav.end()));
  return bn::fuzz::MakeBook(files, (choices & 4U) != 0, (choices & 8U) != 0, (choices & 16U) != 0);
}

// An entry of a table of contents, as a bundle keeps one.
struct Kept {
  unsigned depth = 0;
  unsigned chapter = 0;
  std::string fragment;
  std::string label;

  bool operator==(const Kept&) const = default;
};

// Takes the entries of a table of contents as a bundle would keep them.
class Entries final : public bn::epub::TocHandler {
 public:
  bool OnEntry(const bn::epub::TocEntry& entry) noexcept override {
    entries.push_back({.depth = std::min<unsigned>(entry.depth, 255),
                       .chapter = entry.document < bn::epub::kBundleNoChapter ? static_cast<unsigned>(entry.document)
                                                                              : bn::epub::kBundleNoChapter,
                       .fragment = std::string(entry.fragment),
                       .label = std::string(entry.label)});
    return true;
  }
  std::vector<Kept> entries;
};

// A book, read as the device reads one, and a second piece of storage for
// its bundle.
struct Making {
  Making(std::span<const std::uint8_t> file, std::size_t parserBytes)
      : book(file), parserMemory(parserBytes, 0xEE), packageMemory(kPackageMemoryBytes, 0xEE) {
    BN_FUZZ_CHECK(card.Start() == StorageStatus::kOk, "the storage did not start");
  }

  bn::fuzz::Book book;
  bn::epub::ZipArchive archive;
  Bytes parserMemory;
  bn::epub::XmlParser xml{parserMemory};
  Bytes packageMemory;
  Package package{packageMemory};
  FakeStorage card;
  alignas(4) std::array<std::uint8_t, 4096> bundleBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  alignas(4) std::array<std::uint8_t, 4096> records{};
  BlockFile bundleFile{bundleBuffer};
  BookBundler bundler{page, records};
};

BundleBook About() {
  BundleBook about;
  about.textRules = 5;
  about.importer = 9;
  about.epubModified = 0x12345678;
  about.madeBy[0] = 'f';
  return about;
}

// Makes the bundle at `path`, holding every step to what a step may do.
BookBundleStatus Make(Making& making, const char* path, std::size_t bookBytes) {
  FakeStorage& card = making.card;
  BN_FUZZ_CHECK(making.bundleFile.Open(card, path, OpenMode::kCreate) == StorageStatus::kOk, "no file for the bundle");
  bool more = making.bundler.Begin(making.package, making.archive, making.book.entries, making.book.reader, making.xml,
                                   making.bundleFile, About()) == BookBundleStatus::kOk;
  std::size_t steps = 0;
  while (more) {
    const std::size_t mark = making.book.Mark();
    const std::size_t writes = card.written.size();
    const int syncs = card.syncs;
    more = making.bundler.Step();
    ++steps;
    BN_FUZZ_CHECK(making.book.BytesSince(mark) <= bn::fuzz::kMostStorageForAStep,
                  "one step of making a bundle read more of the book than a step may");
    BN_FUZZ_CHECK(card.written.size() <= writes + 2, "one step of making a bundle wrote more than two pages");
    for (std::size_t i = writes; i < card.written.size(); ++i) {
      BN_FUZZ_CHECK(card.written[i].size == 4096 && card.written[i].offset % 4096 == 0,
                    "a write of a bundle being made is not a whole page at a page's place");
    }
    BN_FUZZ_CHECK(card.syncs <= syncs + 1, "one step of making a bundle synced more than once");
    BN_FUZZ_CHECK(steps <= MostSteps(bookBytes), "the making of a bundle does not end");
  }
  BN_FUZZ_CHECK(!making.bundler.Step(), "a making that was over went on");
  making.bundleFile.Close();
  return making.bundler.Status();
}

std::uint32_t Get16(const Bytes& from, std::size_t at) {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}

// The whole of one of a bundle's tables.
Bytes Table(Bundle& bundle, BundleTable table) {
  Bytes bytes(bundle.Table(table).bytes);
  BN_FUZZ_CHECK(bundle.ReadTable(table, 0, bytes) == BundleStatus::kOk, "a table could not be read");
  BN_FUZZ_CHECK(bundle.Check(table) == BundleStatus::kOk, "a table of a bundle just made fails its checksum");
  return bytes;
}

bool SameEntry(const ZipEntry& a, const ZipEntry& b) {
  return a.headerOffset == b.headerOffset && a.compressedSize == b.compressedSize && a.size == b.size &&
         a.crc32 == b.crc32 && a.method == b.method && a.flags == b.flags && a.nameLength == b.nameLength;
}

// Looks at everything a bundle that was made holds, beside the book.
void CheckBundle(Making& making, const Bytes& file, bool made) {
  const Package& package = making.package;
  alignas(4) std::array<std::uint8_t, 4096> buffer{};
  alignas(4) std::array<std::uint8_t, 4096> openPage{};
  BlockFile opened(buffer);
  BN_FUZZ_CHECK(opened.Open(making.card, "/bundle", OpenMode::kRead) == StorageStatus::kOk, "the bundle is not there");
  Bundle bundle(openPage);
  BN_FUZZ_CHECK(bundle.Open(opened) == BundleStatus::kOk, "a bundle that was made does not open");
  const BundleBook& book = bundle.Book();
  BN_FUZZ_CHECK(book == making.bundler.Book(), "the bundle does not say what its making said");
  BN_FUZZ_CHECK(book.textRules == 5 && book.importer == 9 && book.epubModified == 0x12345678 && book.madeBy[0] == 'f',
                "what the bundle was told of its making is not what it says");
  BN_FUZZ_CHECK(book.documents == package.Spine().size() && book.pictures == package.Pictures(),
                "not as many documents or pictures as the package has");
  BN_FUZZ_CHECK(book.epubBytes == file.size() && book.packageChecksum == bn::epub::PackageChecksum(package),
                "not the EPUB's size, or not its package's checksum");
  const std::span<const std::uint8_t> directory =
      std::span(file).subspan(making.archive.DirectoryOffset(), making.archive.DirectoryBytes());
  BN_FUZZ_CHECK(book.epubDirectoryChecksum == bn::core::Crc32(directory),
                "not the checksum of the ZIP file's directory");
  BN_FUZZ_CHECK(bundle.IsOf(static_cast<std::uint32_t>(file.size()), 0x12345678, bn::core::Crc32(directory)),
                "the bundle is not of its own EPUB");
  BN_FUZZ_CHECK(bundle.Totals().notImported == book.documents && bundle.DataEnd() == bundle.DataBegin(),
                "a chapter of a bundle just made is imported");

  // The strings.
  const Bytes strings = Table(bundle, BundleTable::kStrings);
  std::size_t at = 0;
  for (const std::string_view said : {package.Title(), package.Author(), package.Language(), package.Identifier()}) {
    BN_FUZZ_CHECK(
        at + 2 <= strings.size() && Get16(strings, at) == said.size() && at + 2 + said.size() <= strings.size() &&
            std::equal(said.begin(), said.end(), strings.begin() + static_cast<std::ptrdiff_t>(at + 2),
                       [](char letter, std::uint8_t byte) { return static_cast<std::uint8_t>(letter) == byte; }),
        "a string is not what the package says");
    BN_FUZZ_CHECK(bn::fuzz::IsWholeCharacters(said), "a string is cut inside a character");
    at += 2 + said.size();
  }
  BN_FUZZ_CHECK(at == strings.size(), "there is more in the strings than the four of them");

  // The spine.
  (void)Table(bundle, BundleTable::kSpine);
  for (std::size_t i = 0; i < package.Spine().size(); ++i) {
    const SpineItem& item = package.Spine()[i];
    BundleFile record;
    BN_FUZZ_CHECK(bundle.ReadSpine(i, record) == BundleStatus::kOk, "a document's record could not be read");
    const unsigned flags =
        (item.linear ? bn::epub::kBundleFileInRun : 0U) | (item.present ? bn::epub::kBundleFilePresent : 0U);
    BN_FUZZ_CHECK(record.flags == flags && record.kind == static_cast<std::uint8_t>(item.type),
                  "a document's record is not of its kind, or not in the run as it is");
    BN_FUZZ_CHECK(SameEntry(record.entry, item.present ? item.entry : ZipEntry{}),
                  "a document's record does not say where the package says its file is");
  }

  // The pictures: each record of a file that is there is of that picture,
  // and of the first file the directory has for it.
  (void)Table(bundle, BundleTable::kPictures);
  std::vector<BundleFile> pictures(book.pictures);
  std::uint32_t present = 0;
  std::array<char, bn::epub::kMaxBookPath> name{};
  for (std::size_t i = 0; i < pictures.size(); ++i) {
    BN_FUZZ_CHECK(bundle.ReadPicture(i, pictures[i]) == BundleStatus::kOk, "a picture's record could not be read");
    if ((pictures[i].flags & bn::epub::kBundleFilePresent) == 0) {
      BN_FUZZ_CHECK(pictures[i].flags == 0 && pictures[i].kind == 0 && SameEntry(pictures[i].entry, ZipEntry{}),
                    "a picture with no file has something in its record");
    } else {
      ++present;
    }
  }
  BN_FUZZ_CHECK(present == making.bundler.PicturesPresent(), "not as many pictures have files as the making said");
  BN_FUZZ_CHECK(making.bundler.Passes() == (pictures.size() + 169) / 170,
                "the directory was not gone through once for every 170 pictures");
  std::vector<bool> met(pictures.size(), false);
  bn::epub::ZipArchive::Cursor cursor;
  for (;;) {
    ZipEntry entry;
    const ZipStatus status = making.archive.Next(cursor, entry, name);
    if (status == ZipStatus::kEnd) {
      break;
    }
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      // A ZIP file of the bytes themselves, with no pictures: nothing made
      // the bundler go through its directory, which is damaged.
      BN_FUZZ_CHECK(!made && pictures.empty(), "the directory of a book whose bundle was made cannot be gone through");
      break;
    }
    if (status != ZipStatus::kOk || entry.nameLength > name.size()) {
      continue;
    }
    const Package::Picture picture = package.PictureOf(std::string_view(name.data(), entry.nameLength));
    if (picture.index == Package::kNotPicture) {
      continue;
    }
    BN_FUZZ_CHECK(picture.index < pictures.size(), "the package gives a picture a place past the last");
    if (met[picture.index]) {
      continue;  // The ZIP file has the name twice: the first is the one.
    }
    met[picture.index] = true;
    const BundleFile& record = pictures[picture.index];
    const unsigned flags = bn::epub::kBundleFilePresent | (picture.isCover ? bn::epub::kBundleFileCover : 0U);
    BN_FUZZ_CHECK(record.flags == flags && record.kind == static_cast<std::uint8_t>(picture.type) &&
                      SameEntry(record.entry, entry),
                  "a picture's record is not of the first file the directory has for it");
  }
  for (std::size_t i = 0; i < pictures.size(); ++i) {
    BN_FUZZ_CHECK(met[i] == ((pictures[i].flags & bn::epub::kBundleFilePresent) != 0),
                  "a picture has a record of a file the directory does not have for it");
  }

  // The contents, beside a reading of the table of contents here.
  const Bytes contents = Table(bundle, BundleTable::kContents);
  Entries read;
  bn::epub::TocReader toc;
  const bn::epub::TocStatus tocStatus = toc.Read(package, making.book.entries, making.book.reader, making.xml, read);
  const bool whole =
      tocStatus == bn::epub::TocStatus::kNoToc || (tocStatus == bn::epub::TocStatus::kOk && !toc.CutShort());
  BN_FUZZ_CHECK(((book.amiss & bn::epub::kBundleContentsNotWhole) == 0) == whole,
                "the header does not say whether the contents are the whole of the book's");
  BN_FUZZ_CHECK(((book.amiss & bn::epub::kBundlePackageCutShort) != 0) == package.CutShort(),
                "the header does not say whether the package was cut short");
  std::vector<Kept> kept;
  for (at = 0; at < contents.size();) {
    BN_FUZZ_CHECK(at + 6 <= contents.size(), "an entry of the contents is cut off");
    const std::size_t fragment = contents[at + 1];
    const std::size_t label = Get16(contents, at + 4);
    BN_FUZZ_CHECK(at + 6 + fragment + label <= contents.size(), "an entry of the contents runs out of its table");
    const auto text = contents.begin() + static_cast<std::ptrdiff_t>(at + 6);
    kept.push_back({.depth = contents[at],
                    .chapter = Get16(contents, at + 2),
                    .fragment = std::string(text, text + static_cast<std::ptrdiff_t>(fragment)),
                    .label = std::string(text + static_cast<std::ptrdiff_t>(fragment),
                                         text + static_cast<std::ptrdiff_t>(fragment + label))});
    BN_FUZZ_CHECK(kept.back().chapter == bn::epub::kBundleNoChapter || kept.back().chapter < book.documents,
                  "an entry of the contents points into a document the book does not have");
    at += 6 + fragment + label;
  }
  BN_FUZZ_CHECK(kept.size() == book.contents, "not as many entries of the contents as the header says");
  BN_FUZZ_CHECK(kept == read.entries, "the contents are not the entries the table of contents has");

  for (std::size_t i = 0; i < std::min<std::size_t>(book.documents, 64); ++i) {
    BundleChapter chapter;
    BN_FUZZ_CHECK(bundle.ReadChapter(i, chapter) == BundleStatus::kOk && chapter == BundleChapter{},
                  "a chapter of a bundle just made is not as one not imported is");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const bool made = (choices & 1U) == 0;
  Bytes file;
  if (made) {
    file = MadeBook(input, choices);
  } else {
    const std::span<const std::uint8_t> rest = input.Rest();
    file.assign(rest.begin(), rest.end());
  }

  Making making(file, kParserMemoryBytes[(choices >> 1U) & 1U]);
  const bool isZip = making.archive.Open(making.book.directory) == ZipStatus::kOk;
  const PackageStatus read =
      isZip ? making.package.Read(making.archive, making.book.entries, making.book.reader, making.xml)
            : PackageStatus::kUnreadable;
  if (read != PackageStatus::kOk) {
    BN_FUZZ_CHECK(making.bundleFile.Open(making.card, "/bundle", OpenMode::kCreate) == StorageStatus::kOk, "no file");
    const BookBundleStatus status = making.bundler.Make(making.package, making.archive, making.book.entries,
                                                        making.book.reader, making.xml, making.bundleFile, About());
    BN_FUZZ_CHECK(status == BookBundleStatus::kNoBook, "a bundle was made of a book whose package was not read");
    BN_FUZZ_CHECK(making.card.Contents("/bundle").empty(), "something was written for a book that is none");
    return 0;
  }

  const BookBundleStatus status = Make(making, "/bundle", file.size());
  BN_FUZZ_CHECK(status == BookBundleStatus::kOk || status == BookBundleStatus::kUnreadable,
                "the making of a bundle ended in a status it should not");
  BN_FUZZ_CHECK(!made || status == BookBundleStatus::kOk, "a sound ZIP file whose package was read was refused");
  if (status != BookBundleStatus::kOk) {
    return 0;
  }
  CheckBundle(making, file, made);
  // And again, the same.
  BN_FUZZ_CHECK(Make(making, "/again", file.size()) == BookBundleStatus::kOk,
                "the second making did not end as the first");
  BN_FUZZ_CHECK(making.card.Contents("/again") == making.card.Contents("/bundle"),
                "the same book made a different bundle the second time");
  return 0;
}
