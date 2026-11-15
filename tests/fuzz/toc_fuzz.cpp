// The fuzz target for the table of contents reader (epub/toc.h).
//
// What it is given:
//
//   byte 0   bits 0 and 1: which tables of contents the book has: a nav,
//                          an NCX, or (2 and 3) both
//            bits 2 and 3: which is asked for: the best, the nav, the NCX
//            bit 4:        the book's files are compressed
//            bits 5 to 7:  the handler asks for no more after its first,
//                          second, third, tenth or hundredth entry; 0 and
//                          the rest, never
//   byte 1   bit 0:        how much memory the XML parser has
//
//   then, for a book with one table: the file, to the end. For a book with
//   both: 2 bytes, the length of the nav, low byte first; the nav; and the
//   rest is the NCX.
//
// The book is a sound ZIP file the target makes around those bytes, with a
// package that names them and four documents for their links to point at.
// The table is read a step at a time, twice. What has to hold, whatever
// the bytes are:
//
//   - the reading ends, with a status it can give: never "unreadable",
//     the ZIP file being sound;
//   - no step reads more from storage than a step may (five blocks);
//   - each entry's label is whole characters, at most kMaxLabel bytes, with
//     no line end, tab or two spaces together in it and no space at either
//     end; its fragment is at most kMaxFragment bytes; it points at one of
//     the book's documents or at none, and at no place within none;
//   - the reader has counted the entries it gave; a handler that asked for
//     no more is given no more, and the reading then says it was stopped;
//   - a table is only called cut short if it gave an entry;
//   - a second reading, never stopped, gives at least the entries of the
//     first, and the same ones if the first was not stopped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "epub/package.h"
#include "epub/toc.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "fuzz_book.h"
#include "fuzz_support.h"

namespace {

using bn::epub::Package;
using bn::epub::PackageStatus;
using bn::epub::TocEntry;
using bn::epub::TocKind;
using bn::epub::TocReader;
using bn::epub::TocStatus;

constexpr std::array<std::size_t, 2> kParserMemoryBytes = {32768, 12288};
constexpr std::array<TocKind, 4> kKinds = {TocKind::kBest, TocKind::kNav, TocKind::kNcx, TocKind::kBest};
// The count of entries after which the handler asks for no more. 0 for
// never.
constexpr std::array<std::uint32_t, 8> kStopAfter = {0, 1, 2, 3, 10, 100, 0, 0};

constexpr std::string_view kContainer =
    R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">)"
    R"(<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>)"
    R"(</container>)";

// The package of the made book: four documents, the last of them set
// aside, and whichever tables of contents it has.
std::string PackageFile(bool hasNav, bool hasNcx) {
  std::string text = R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0">)"
                     R"(<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>A made book</dc:title>)"
                     R"(</metadata><manifest>)";
  if (hasNav) {
    text += R"(<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>)";
  }
  if (hasNcx) {
    text += R"(<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>)";
  }
  text += R"(<item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>)"
          R"(<item id="c2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>)"
          R"(<item id="c3" href="ch%203.xhtml" media-type="application/xhtml+xml"/>)"
          R"(<item id="n" href="notes.xhtml" media-type="application/xhtml+xml"/>)"
          R"(</manifest><spine toc="ncx"><itemref idref="c1"/><itemref idref="c2"/><itemref idref="c3"/>)"
          R"(<itemref idref="n" linear="no"/></spine></package>)";
  return text;
}

// Checks each entry it is given, and asks for no more after so many.
class Listener final : public bn::epub::TocHandler {
 public:
  Listener(std::size_t documents, std::uint32_t stopAfter) noexcept : documents_(documents), stopAfter_(stopAfter) {}

  [[nodiscard]] bool OnEntry(const TocEntry& entry) noexcept override {
    BN_FUZZ_CHECK(!stopped, "a handler was given an entry after it had asked for no more");
    const std::string_view label = entry.label;
    BN_FUZZ_CHECK(label.size() <= TocReader::kMaxLabel, "a label is longer than the longest that is kept");
    BN_FUZZ_CHECK(bn::fuzz::IsWholeCharacters(label), "a label is cut inside a character");
    BN_FUZZ_CHECK(label.find_first_of("\t\n\r") == std::string_view::npos &&
                      label.find("  ") == std::string_view::npos && !label.starts_with(' ') && !label.ends_with(' '),
                  "a label has a run of spaces or line ends left in it, or a space at one end");
    BN_FUZZ_CHECK(entry.fragment.size() <= TocReader::kMaxFragment, "a fragment is longer than the longest kept");
    (void)bn::fuzz::Touch(entry.fragment);
    BN_FUZZ_CHECK(entry.document == Package::kNotInSpine || entry.document < documents_,
                  "an entry points at a document past the last");
    BN_FUZZ_CHECK(entry.document != Package::kNotInSpine || entry.fragment.empty(),
                  "an entry points at a place within no document");
    ++entries;
    // What the entries were, for setting one reading beside another.
    digest = (digest * 31) + bn::fuzz::Touch(label) + (entry.depth * 7U) + static_cast<std::uint32_t>(entry.document);
    if (entries == stopAfter_) {
      stopped = true;
      return false;
    }
    return true;
  }

  std::uint32_t entries = 0;
  std::uint32_t digest = 0;
  bool stopped = false;

 private:
  std::size_t documents_;
  std::uint32_t stopAfter_;
};

struct Reading {
  Reading(std::span<const std::uint8_t> file, std::size_t parserBytes) : book(file), parserMemory(parserBytes, 0xEE) {}

  bn::fuzz::Book book;
  bn::epub::ZipArchive archive;
  std::vector<std::uint8_t> parserMemory;
  bn::epub::XmlParser xml{parserMemory};
  std::vector<std::uint8_t> packageMemory = std::vector<std::uint8_t>(4096, 0xEE);
  Package package{packageMemory};
  TocReader toc;
};

// Reads the table of contents a step at a time, telling `listener`.
TocStatus ReadToc(Reading& reading, Listener& listener, TocKind kind, std::size_t bookBytes) {
  bool more = reading.toc.Begin(reading.package, reading.book.entries, reading.book.reader, reading.xml, listener,
                                kind) == TocStatus::kOk;
  std::size_t steps = 0;
  while (more) {
    const std::size_t mark = reading.book.Mark();
    more = reading.toc.Step();
    ++steps;
    BN_FUZZ_CHECK(reading.book.BytesSince(mark) <= bn::fuzz::kMostStorageForAStep,
                  "one step of reading a table of contents read more from storage than a step may");
    BN_FUZZ_CHECK(steps <= 2048 * (bookBytes + 64), "the reading of a table of contents does not end");
  }
  BN_FUZZ_CHECK(!reading.toc.Step(), "a reading that was over went on");
  BN_FUZZ_CHECK(reading.toc.Entries() == listener.entries, "the reader's count of entries is not what it gave");
  return reading.toc.Status();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const unsigned moreChoices = input.Byte();
  const unsigned tables = choices & 3U;
  const bool hasNav = tables != 1;
  const bool hasNcx = tables != 0;
  const TocKind kind = kKinds[(choices >> 2U) & 3U];

  std::vector<bn::fuzz::BookFile> files;
  files.emplace_back("mimetype", bn::fuzz::BytesOfText("application/epub+zip"));
  files.emplace_back("META-INF/container.xml", bn::fuzz::BytesOfText(kContainer));
  files.emplace_back("OEBPS/content.opf", bn::fuzz::BytesOfText(PackageFile(hasNav, hasNcx)));
  if (hasNav) {
    const std::span<const std::uint8_t> nav = hasNcx ? input.Take(input.Number16()) : input.Rest();
    files.emplace_back("OEBPS/nav.xhtml", std::vector<std::uint8_t>(nav.begin(), nav.end()));
  }
  if (hasNcx) {
    const std::span<const std::uint8_t> ncx = input.Rest();
    files.emplace_back("OEBPS/toc.ncx", std::vector<std::uint8_t>(ncx.begin(), ncx.end()));
  }
  for (const std::string_view name :
       {"OEBPS/ch1.xhtml", "OEBPS/text/ch2.xhtml", "OEBPS/ch 3.xhtml", "OEBPS/notes.xhtml"}) {
    files.emplace_back(std::string(name), bn::fuzz::BytesOfText("what this file holds is of no interest here"));
  }
  const std::vector<std::uint8_t> file = bn::fuzz::MakeBook(files, (choices & 0x10U) != 0, false, false);

  Reading reading(file, kParserMemoryBytes[moreChoices & 1U]);
  const bool opened = reading.archive.Open(reading.book.directory) == bn::epub::ZipStatus::kOk &&
                      reading.package.Read(reading.archive, reading.book.entries, reading.book.reader, reading.xml) ==
                          PackageStatus::kOk &&
                      reading.package.Spine().size() == 4 && (reading.package.Nav() != nullptr) == hasNav &&
                      (reading.package.Ncx() != nullptr) == hasNcx;
  BN_FUZZ_CHECK(opened, "the book the target made was not read as it was made");

  Listener first(reading.package.Spine().size(), kStopAfter[(choices >> 5U) & 7U]);
  const TocStatus status = ReadToc(reading, first, kind, file.size());
  const bool asked = (kind != TocKind::kNcx && hasNav) || (kind != TocKind::kNav && hasNcx);
  if (!asked) {
    BN_FUZZ_CHECK(status == TocStatus::kNoToc && first.entries == 0 && reading.toc.Kind() == TocKind::kBest,
                  "a table of contents the book does not have was read");
    return 0;
  }
  BN_FUZZ_CHECK(reading.toc.Kind() == (kind != TocKind::kNcx && hasNav ? TocKind::kNav : TocKind::kNcx),
                "the table that was read is not the one that was asked for");
  BN_FUZZ_CHECK(status == TocStatus::kOk || status == TocStatus::kNotToc || status == TocStatus::kNoMemory ||
                    status == TocStatus::kStopped,
                "the reading of a table of contents in a sound ZIP file ended with a status it cannot give");
  BN_FUZZ_CHECK(first.stopped == (status == TocStatus::kStopped),
                "a reading that was stopped does not say so, or one says so that was not");
  BN_FUZZ_CHECK(!reading.toc.CutShort() || (status == TocStatus::kOk && first.entries > 0),
                "a table is called cut short that gave no entry, or that was not read");
  BN_FUZZ_CHECK(status != TocStatus::kNotToc || first.entries == 0, "what is called no table of contents gave entries");

  // And again, with the same things, not stopped this time.
  Listener second(reading.package.Spine().size(), 0);
  const TocStatus again = ReadToc(reading, second, kind, file.size());
  if (first.stopped) {
    BN_FUZZ_CHECK(second.entries >= first.entries, "a second reading gave fewer entries than one that was stopped");
  } else {
    BN_FUZZ_CHECK(again == status && second.entries == first.entries && second.digest == first.digest,
                  "a second reading of the same table did not give what the first gave");
  }
  return 0;
}
