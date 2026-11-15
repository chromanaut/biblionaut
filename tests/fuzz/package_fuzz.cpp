// The fuzz target for the package reader (epub/package.h).
//
// What it is given:
//
//   byte 0   bit 0:        0, the target makes the book; 1, the rest is the book
//            bits 1 and 2: how much memory the package has
//            bit 3:        how much memory the XML parser has
//            bit 4:        in a book the target makes, the files are compressed
//            bit 5:        and are in the ZIP file last first
//            bit 6:        and the ZIP file ends with a comment
//
//   then, for a book the target makes:
//
//   2 bytes  the length of what follows, low byte first
//   so many  META-INF/container.xml
//   the rest OEBPS/content.opf, the package file
//
//   The book is a sound ZIP file of those two and of a few files that a
//   package may name (kOtherFiles below): a ZIP file made up of the bytes
//   themselves is stopped at an entry's checksum by nearly every change to
//   them. For a book that is the bytes themselves, the rest is the ZIP file.
//
// The package is read a step at a time, twice. What has to hold, whatever
// the bytes are:
//
//   - the reading ends, with a status it can give; and a sound ZIP file is
//     never called unreadable;
//   - no step reads more from storage than a step may (five blocks);
//   - nothing is said of the book until its reading is over;
//   - a book that was read has documents; they, and its manifest, fit in
//     the memory the package was given; each document that is in the ZIP
//     file can be found again by the name its entry has, as the same file;
//   - the title, author and language are whole characters, with no line
//     end, tab or two spaces together in them;
//   - the second reading says what the first said.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "epub/package.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "fuzz_book.h"
#include "fuzz_support.h"

namespace {

using bn::epub::Package;
using bn::epub::PackageStatus;
using bn::epub::SpineItem;
using bn::epub::ZipStatus;

constexpr std::array<std::size_t, 4> kPackageMemoryBytes = {24576, 4096, 512, 64};
constexpr std::array<std::size_t, 2> kParserMemoryBytes = {32768, 12288};

// The files a made book has besides its container and its package, for a
// package to name. The kept inputs name them, and what the engine makes of
// those mostly still does.
constexpr std::array<std::string_view, 9> kOtherFiles = {
    "OEBPS/toc.ncx",    "OEBPS/nav.xhtml",   "OEBPS/cover.jpg", "OEBPS/ch1.xhtml", "OEBPS/text/ch2.xhtml",
    "OEBPS/ch 3.xhtml", "OEBPS/notes.xhtml", "OEBPS/style.css", "ch4.xhtml"};

// A reading can be made as long as its book's maker likes, a step at a
// time, but not endless: a book of so many bytes cannot ask for more steps
// than this.
constexpr std::size_t MostSteps(std::size_t bookBytes) noexcept { return 2048 * (bookBytes + 64); }

std::vector<std::uint8_t> MadeBook(bn::fuzz::Input& input, unsigned choices) {
  const std::span<const std::uint8_t> container = input.Take(input.Number16());
  const std::span<const std::uint8_t> package = input.Rest();
  std::vector<bn::fuzz::BookFile> files;
  files.emplace_back("mimetype", bn::fuzz::BytesOfText("application/epub+zip"));
  files.emplace_back("META-INF/container.xml", std::vector<std::uint8_t>(container.begin(), container.end()));
  files.emplace_back("OEBPS/content.opf", std::vector<std::uint8_t>(package.begin(), package.end()));
  for (const std::string_view name : kOtherFiles) {
    files.emplace_back(std::string(name), bn::fuzz::BytesOfText("what this file holds is of no interest here"));
  }
  return bn::fuzz::MakeBook(files, (choices & 0x10U) != 0, (choices & 0x20U) != 0, (choices & 0x40U) != 0);
}

void CheckText(std::string_view text, std::size_t most) {
  BN_FUZZ_CHECK(text.size() <= most, "a piece of what the book says of itself is longer than there is room for");
  BN_FUZZ_CHECK(bn::fuzz::IsWholeCharacters(text), "a piece of what the book says of itself is cut inside a character");
  BN_FUZZ_CHECK(text.find_first_of("\t\n\r") == std::string_view::npos && text.find("  ") == std::string_view::npos &&
                    !text.starts_with(' '),
                "a piece of what the book says of itself has a run of spaces or line ends left in it");
}

// What a reading said, to set beside what another says.
struct Said {
  PackageStatus status = PackageStatus::kOk;
  std::size_t documents = 0;
  std::uint32_t manifestItems = 0;
  std::uint32_t unknownSpineItems = 0;
  std::string title;

  bool operator==(const Said&) const = default;
};

struct Reading {
  Reading(std::span<const std::uint8_t> file, std::size_t packageBytes, std::size_t parserBytes)
      : book(file), parserMemory(parserBytes, 0xEE), packageMemory(packageBytes, 0xEE) {}

  bn::fuzz::Book book;
  bn::epub::ZipArchive archive;
  std::vector<std::uint8_t> parserMemory;
  bn::epub::XmlParser xml{parserMemory};
  std::vector<std::uint8_t> packageMemory;
  Package package{packageMemory};
};

// Reads the package of the book in `reading`, a step at a time.
Said ReadPackage(Reading& reading, std::size_t bookBytes) {
  Package& package = reading.package;
  bool more =
      package.Begin(reading.archive, reading.book.entries, reading.book.reader, reading.xml) == PackageStatus::kOk;
  std::size_t steps = 0;
  while (more) {
    const std::size_t mark = reading.book.Mark();
    more = package.Step();
    ++steps;
    BN_FUZZ_CHECK(reading.book.BytesSince(mark) <= bn::fuzz::kMostStorageForAStep,
                  "one step of reading a package read more from storage than a step may");
    BN_FUZZ_CHECK(steps <= MostSteps(bookBytes), "the reading of a package does not end");
    if (more) {
      BN_FUZZ_CHECK(package.Status() == PackageStatus::kOk && package.Spine().empty() && package.Cover() == nullptr &&
                        package.Nav() == nullptr && package.Ncx() == nullptr &&
                        package.SpineIndexOf("OEBPS/ch1.xhtml") == Package::kNotInSpine,
                    "something was said of a book before its reading was over");
    }
  }
  BN_FUZZ_CHECK(!package.Step(), "a reading that was over went on");
  return {.status = package.Status(),
          .documents = package.Spine().size(),
          .manifestItems = package.ManifestItems(),
          .unknownSpineItems = package.UnknownSpineItems(),
          .title = std::string(package.Title())};
}

// Looks at everything a package that was read says.
void CheckWhatWasRead(Reading& reading, bool made) {
  const Package& package = reading.package;
  const std::span<const SpineItem> spine = package.Spine();
  BN_FUZZ_CHECK(!spine.empty(), "a book was read that has no documents");
  BN_FUZZ_CHECK(!package.PackagePath().empty(), "a book was read whose package file has no path");
  BN_FUZZ_CHECK(package.MemoryUsed() <= reading.packageMemory.size() &&
                    (spine.size() * 28) + (std::size_t{package.ManifestItems()} * 16) <= reading.packageMemory.size(),
                "a package holds more than the memory it was given");

  std::array<char, bn::epub::kMaxBookPath> name{};
  for (std::size_t i = 0; i < spine.size(); ++i) {
    const SpineItem& item = spine[i];
    BN_FUZZ_CHECK(item.type <= bn::epub::MediaType::kFont, "a document has a kind there is not");
    if (!item.present) {
      continue;
    }
    std::size_t length = 0;
    const ZipStatus named = bn::epub::ReadZipEntryName(reading.book.entries, item.entry, name, length);
    if (!made) {
      continue;  // In a ZIP file of the bytes themselves, the entry's own header may say anything.
    }
    BN_FUZZ_CHECK(named == ZipStatus::kOk && length <= name.size(), "a document's entry has no name to read back");
    // By that name the document is found again: at this place, or at an
    // earlier one that is the same file.
    const std::size_t place = package.SpineIndexOf(std::string_view(name.data(), length));
    BN_FUZZ_CHECK(place <= i && spine[place].present && spine[place].entry.headerOffset == item.entry.headerOffset,
                  "a document is not found, at its first place, by the name of its own entry");
  }
  for (const bn::epub::ZipEntry* entry : {package.Cover(), package.Nav(), package.Ncx()}) {
    if (entry != nullptr && made) {
      std::size_t length = 0;
      BN_FUZZ_CHECK(bn::epub::ReadZipEntryName(reading.book.entries, *entry, name, length) == ZipStatus::kOk,
                    "the cover or a table of contents has an entry with no name to read back");
    }
  }
  // A path that is not the book's, and one that is no path.
  BN_FUZZ_CHECK(package.SpineIndexOf("") == Package::kNotInSpine || package.SpineIndexOf("") < spine.size(),
                "a path was found at a place past the last document");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const bool made = (choices & 1U) == 0;
  std::vector<std::uint8_t> file;
  if (made) {
    file = MadeBook(input, choices);
  } else {
    const std::span<const std::uint8_t> rest = input.Rest();
    file.assign(rest.begin(), rest.end());
  }

  Reading reading(file, kPackageMemoryBytes[(choices >> 1U) & 3U], kParserMemoryBytes[(choices >> 3U) & 1U]);
  const ZipStatus opened = reading.archive.Open(reading.book.directory);
  BN_FUZZ_CHECK(!made || opened == ZipStatus::kOk, "a ZIP file the target made could not be opened");

  const Said first = ReadPackage(reading, file.size());
  if (made) {
    BN_FUZZ_CHECK(first.status != PackageStatus::kUnreadable, "a sound ZIP file was called unreadable");
  }
  CheckText(reading.package.Title(), 256);
  CheckText(reading.package.Author(), 128);
  CheckText(reading.package.Language(), 16);
  if (first.status == PackageStatus::kOk) {
    CheckWhatWasRead(reading, made);
  } else {
    BN_FUZZ_CHECK(first.status <= PackageStatus::kUnreadable, "a reading ended with a status there is not");
    BN_FUZZ_CHECK(first.documents == 0 || first.status != PackageStatus::kNoContainer,
                  "a book with no container has documents");
  }

  // And again, with the same things, which have all been used once.
  const Said second = ReadPackage(reading, file.size());
  BN_FUZZ_CHECK(second == first, "a second reading of the same book did not say what the first said");
  return 0;
}
