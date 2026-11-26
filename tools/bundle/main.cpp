// bn-bundle: makes the bundle of a book, the file the firmware imports a
// book into, with the code the firmware makes it with; says what a bundle
// holds; and prints a chapter of one as text, writes one from that text,
// and looks for a place in one (chapter.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "chapter.h"
#include "core/block_file.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "epub/book_bundle.h"
#include "epub/bundle.h"
#include "epub/package.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::BookBundleStatus;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::PackageStatus;
using bn::epub::ZipStatus;
using bn::tools::Printable;

// More than any book needs for its manifest and its spine; and what the
// XML parser is given, which is what a device gives it.
constexpr std::size_t kPackageMemoryBytes = 1024UL * 1024UL;
constexpr std::size_t kXmlMemoryBytes = 24UL * 1024UL;

// The versions of the importer's rules that a bundle made here says it was
// made under. Nothing is imported yet, so there are no rules.
constexpr std::uint16_t kTextRules = 0;
constexpr std::uint16_t kImporter = 0;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s make BOOK BUNDLE [--written SECONDS]\n"
               "       %s info BUNDLE\n"
               "       %s chapter BUNDLE NUMBER\n"
               "       %s write-chapter BUNDLE NUMBER TEXT\n"
               "       %s find BUNDLE NUMBER block|characters|source|id VALUE\n"
               "\n"
               "make reads the EPUB BOOK the way the firmware does and writes its bundle to\n"
               "BUNDLE: the book's strings, its spine, its pictures and its table of contents,\n"
               "with every chapter not imported. It prints how many of each it wrote, and how\n"
               "many times it went through the EPUB's directory for the pictures.\n"
               "\n"
               "  --written SECONDS   when the EPUB was last written, to put in the bundle,\n"
               "                      where the file's own time is not the one wanted\n"
               "\n"
               "info reads a bundle and prints what it holds:\n"
               "\n"
               "  format, text rules, importer, made by\n"
               "                      the versions it was made under, and what made it\n"
               "  epub                the EPUB it is of: its size, when it was last written,\n"
               "                      and the checksum of its ZIP directory\n"
               "  package             the checksum of what its package said (bn-epub info)\n"
               "  amiss               what was wrong with the book when it was made, or -\n"
               "  title, author, language, identifier\n"
               "                      as the book gives them, or - for one it does not give\n"
               "  spine               how many documents, and a line for each: + for one in\n"
               "                      the run of the book and - for one set aside, its kind,\n"
               "                      and where the file is in the EPUB as three numbers (where\n"
               "                      its header is, how many bytes it holds, and their\n"
               "                      checksum), or ? for a file the EPUB does not have\n"
               "  pictures            how many, and a line for each: its kind and where it\n"
               "                      is, with \"cover\" after the cover; or ?\n"
               "  contents            how many entries, and a line for each as bn-epub toc\n"
               "                      prints one\n"
               "  slot                the number of the slot in force, how long a slot is,\n"
               "                      and where the segments begin and end\n"
               "  chapters            how many are in each state\n"
               "  chapter N           for each that was imported: its state, its blocks and\n"
               "                      characters, how many things were left out of it, how\n"
               "                      long its segment is and where, and its directory's\n"
               "                      length, place in the segment and checksum\n"
               "  tables              whether each of the four is what its checksum says\n"
               "\n"
               "chapter prints the chapter NUMBER of a bundle, from 0, as text: a line for each\n"
               "block, each thing in a block, and each id, in the document's order, and lines\n"
               "beginning with # for what the chapter's record and directory say of it.\n"
               "\n"
               "write-chapter writes the chapter that the file TEXT describes, in the text that\n"
               "chapter prints, as the chapter NUMBER of the bundle, and commits it. It prints\n"
               "how many blocks and characters it wrote, how long the segment and its\n"
               "directory are, how many things were left out, and how many pictures.\n"
               "\n"
               "find looks in a chapter for the block of a number, the block a count of\n"
               "characters falls in, the last block that began at or before a place in the\n"
               "source document, or an id, and prints where that is: the block, where it\n"
               "begins in the segment, the characters before it, where it began in the\n"
               "source, its kind and its bits for breaking as the index has them, and how far\n"
               "into the block the place is; \"end\" for the chapter's end, \"none\" for a\n"
               "place the chapter does not have.\n"
               "\n"
               "In a name, a backslash and any byte below a space are printed as \\xNN. Ends\n"
               "with 0 if what was asked for was done, 1 if not.\n",
               program, program, program, program, program);
}

std::string Said(std::string_view text) { return text.empty() ? std::string("-") : Printable(text); }

int Make(const std::string& bookPath, const std::string& bundlePath, bool hasWritten, std::uint32_t written) {
  bn::tools::OneFile book(bookPath);
  alignas(4) static std::array<std::uint8_t, 4096> directoryBuffer{};
  alignas(4) static std::array<std::uint8_t, 4096> entryBuffer{};
  bn::core::BlockFile directory(directoryBuffer);
  bn::core::BlockFile entries(entryBuffer);
  if (book.Start() != StorageStatus::kOk ||
      directory.Open(book, "/book", bn::core::OpenMode::kRead) != StorageStatus::kOk ||
      entries.Open(book, "/book", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", bookPath.c_str());
    return 1;
  }
  bn::epub::ZipArchive archive;
  if (const ZipStatus status = archive.Open(directory); status != ZipStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", bookPath.c_str(), bn::epub::ZipStatusName(status));
    return 1;
  }
  std::vector<std::uint8_t> window(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader(window);
  std::vector<std::uint8_t> xmlMemory(kXmlMemoryBytes);
  bn::epub::XmlParser xml(xmlMemory);
  std::vector<std::uint8_t> memory(kPackageMemoryBytes);
  bn::epub::Package package(memory);
  if (const PackageStatus status = package.Read(archive, entries, reader, xml); status != PackageStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", bookPath.c_str(), bn::epub::PackageStatusName(status));
    return 1;
  }

  bn::tools::OneNewFile made(bundlePath);
  alignas(4) static std::array<std::uint8_t, 4096> bundleBuffer{};
  bn::core::BlockFile bundle(bundleBuffer);
  if (bundle.Open(made, "/bundle", bn::core::OpenMode::kCreate) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot write %s\n", bundlePath.c_str());
    return 1;
  }
  BundleBook about;
  about.textRules = kTextRules;
  about.importer = kImporter;
  about.epubModified = hasWritten ? written : book.Modified();
  const std::string_view madeBy = "bn-bundle";
  std::copy(madeBy.begin(), madeBy.end(), about.madeBy.begin());
  alignas(4) static std::array<std::uint8_t, 4096> page{};
  alignas(4) static std::array<std::uint8_t, 4096> records{};
  bn::epub::BookBundler bundler(page, records);
  const BookBundleStatus status = bundler.Make(package, archive, entries, reader, xml, bundle, about);
  if (status != BookBundleStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", bookPath.c_str(), bn::epub::BookBundleStatusName(status));
    return 1;
  }
  const BundleBook& madeBook = bundler.Book();
  std::printf("documents=%u pictures=%u pictures_present=%u contents=%u passes=%u amiss=%u bytes=%u\n",
              static_cast<unsigned>(madeBook.documents), static_cast<unsigned>(madeBook.pictures),
              static_cast<unsigned>(bundler.PicturesPresent()), static_cast<unsigned>(madeBook.contents),
              static_cast<unsigned>(bundler.Passes()), static_cast<unsigned>(madeBook.amiss),
              static_cast<unsigned>(bundle.Size()));
  return 0;
}

const char* KindName(std::uint8_t kind) {
  return kind <= static_cast<std::uint8_t>(bn::epub::MediaType::kFont)
             ? bn::epub::MediaTypeName(static_cast<bn::epub::MediaType>(kind))
             : "unknown";
}

// Where a file is in the EPUB, as bn-epub's checksum has an entry.
std::string Place(const BundleFile& file) {
  if ((file.flags & bn::epub::kBundleFilePresent) == 0) {
    return "?";
  }
  std::array<char, 48> text{};
  std::snprintf(text.data(), text.size(), "%u %u %08x", static_cast<unsigned>(file.entry.headerOffset),
                static_cast<unsigned>(file.entry.size), static_cast<unsigned>(file.entry.crc32));
  return text.data();
}

int Info(const std::string& path) {
  bn::tools::OneFile storage(path);
  alignas(4) static std::array<std::uint8_t, 4096> buffer{};
  alignas(4) static std::array<std::uint8_t, 4096> page{};
  bn::core::BlockFile file(buffer);
  if (storage.Start() != StorageStatus::kOk ||
      file.Open(storage, "/bundle", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Bundle bundle(page);
  if (const BundleStatus status = bundle.Open(file); status != BundleStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::BundleStatusName(status));
    return 1;
  }
  const BundleBook& book = bundle.Book();
  std::printf("format: %u\n", static_cast<unsigned>(bn::epub::kBundleFormat));
  std::printf("text rules: %u\n", static_cast<unsigned>(book.textRules));
  std::printf("importer: %u\n", static_cast<unsigned>(book.importer));
  const std::size_t madeByLength =
      static_cast<std::size_t>(std::find(book.madeBy.begin(), book.madeBy.end(), '\0') - book.madeBy.begin());
  std::printf("made by: %s\n", Said(std::string_view(book.madeBy.data(), madeByLength)).c_str());
  std::printf("epub: %u bytes, written %u, directory %08x\n", static_cast<unsigned>(book.epubBytes),
              static_cast<unsigned>(book.epubModified), static_cast<unsigned>(book.epubDirectoryChecksum));
  std::printf("package: %08x\n", static_cast<unsigned>(book.packageChecksum));
  std::string amiss;
  if ((book.amiss & bn::epub::kBundleContentsNotWhole) != 0) {
    amiss += "contents not whole";
  }
  if ((book.amiss & bn::epub::kBundlePackageCutShort) != 0) {
    amiss += amiss.empty() ? "package cut short" : ", package cut short";
  }
  std::printf("amiss: %s\n", amiss.empty() ? "-" : amiss.c_str());

  // The strings: each a length and its bytes.
  const auto readTable = [&bundle, &path](BundleTable table, std::vector<std::uint8_t>& bytes) {
    bytes.resize(bundle.Table(table).bytes);
    if (bundle.ReadTable(table, 0, bytes) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: a table could not be read\n", path.c_str());
      return false;
    }
    return true;
  };
  std::vector<std::uint8_t> bytes;
  if (!readTable(BundleTable::kStrings, bytes)) {
    return 1;
  }
  std::size_t at = 0;
  for (const char* name : {"title", "author", "language", "identifier"}) {
    std::string text;
    if (at + 2 <= bytes.size()) {
      const std::size_t length = bytes[at] | (static_cast<std::size_t>(bytes[at + 1]) << 8U);
      at += 2;
      if (at + length <= bytes.size()) {
        text.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                    bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
      }
      at += length;
    }
    std::printf("%s: %s\n", name, Said(text).c_str());
  }

  std::printf("spine: %u\n", static_cast<unsigned>(book.documents));
  for (std::size_t i = 0; i < book.documents; ++i) {
    BundleFile entry;
    if (bundle.ReadSpine(i, entry) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: a document's record could not be read\n", path.c_str());
      return 1;
    }
    std::printf("%c %s %s\n", (entry.flags & bn::epub::kBundleFileInRun) != 0 ? '+' : '-', KindName(entry.kind),
                Place(entry).c_str());
  }
  std::printf("pictures: %u\n", static_cast<unsigned>(book.pictures));
  for (std::size_t i = 0; i < book.pictures; ++i) {
    BundleFile entry;
    if (bundle.ReadPicture(i, entry) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: a picture's record could not be read\n", path.c_str());
      return 1;
    }
    if ((entry.flags & bn::epub::kBundleFilePresent) == 0) {
      std::printf("?\n");
    } else {
      std::printf("%s %s%s\n", KindName(entry.kind), Place(entry).c_str(),
                  (entry.flags & bn::epub::kBundleFileCover) != 0 ? " cover" : "");
    }
  }

  // The contents: how far in, the fragment's length, the document, the
  // label's length, the fragment, the label.
  if (!readTable(BundleTable::kContents, bytes)) {
    return 1;
  }
  std::printf("contents: %u\n", static_cast<unsigned>(book.contents));
  at = 0;
  for (std::size_t i = 0; i < book.contents && at + 6 <= bytes.size(); ++i) {
    const std::size_t fragment = bytes[at + 1];
    const unsigned chapter = bytes[at + 2] | (static_cast<unsigned>(bytes[at + 3]) << 8U);
    const std::size_t label = bytes[at + 4] | (static_cast<std::size_t>(bytes[at + 5]) << 8U);
    if (at + 6 + fragment + label > bytes.size()) {
      break;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as the characters they are.
    const char* text = reinterpret_cast<const char*>(bytes.data()) + at + 6;
    std::printf("%u\t%s\t%s\t%s\n", static_cast<unsigned>(bytes[at]),
                chapter == bn::epub::kBundleNoChapter ? "-" : std::to_string(chapter).c_str(),
                fragment == 0 ? "-" : Printable(std::string_view(text, fragment)).c_str(),
                Printable(std::string_view(text + fragment, label)).c_str());
    at += 6 + fragment + label;
  }

  std::printf("slot: sequence %u, %u bytes, segments from %u to %u\n", static_cast<unsigned>(bundle.Sequence()),
              static_cast<unsigned>(bn::epub::BundleSlotBytes(book.documents)),
              static_cast<unsigned>(bundle.DataBegin()), static_cast<unsigned>(bundle.DataEnd()));
  std::array<unsigned, 5> states{};
  for (std::size_t i = 0; i < book.documents; ++i) {
    BundleChapter chapter;
    if (bundle.ReadChapter(i, chapter) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: a chapter's record could not be read\n", path.c_str());
      return 1;
    }
    ++states.at(static_cast<std::size_t>(chapter.state));
  }
  std::printf("chapters: %u not imported, %u whole, %u cut short, %u not documents, %u refused\n", states[0], states[1],
              states[2], states[3], states[4]);
  // And a line for each that has a segment.
  for (std::size_t i = 0; i < book.documents; ++i) {
    BundleChapter chapter;
    if (bundle.ReadChapter(i, chapter) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: a chapter's record could not be read\n", path.c_str());
      return 1;
    }
    if (chapter.segmentBytes != 0) {
      std::printf(
          "chapter %zu: %s, %u blocks, %u characters, %u left out, %u bytes at %u, directory of %u at %u %08x\n", i,
          chapter.state == bn::epub::ChapterState::kWhole ? "whole" : "cut short",
          static_cast<unsigned>(chapter.blocks), static_cast<unsigned>(chapter.characters),
          static_cast<unsigned>(chapter.leftOut), static_cast<unsigned>(chapter.segmentBytes),
          static_cast<unsigned>(chapter.segmentOffset), static_cast<unsigned>(chapter.directoryBytes),
          static_cast<unsigned>(chapter.directoryOffset), static_cast<unsigned>(chapter.directoryChecksum));
    }
  }
  std::printf("tables:");
  bool whole = true;
  for (const BundleTable table :
       {BundleTable::kStrings, BundleTable::kSpine, BundleTable::kPictures, BundleTable::kContents}) {
    const BundleStatus status = bundle.Check(table);
    whole = whole && status == BundleStatus::kOk;
    std::printf(" %s", bn::epub::BundleStatusName(status));
  }
  std::printf("\n");
  return whole ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  if (command == "info" && argc == 3) {
    return Info(argv[2]);
  }
  if ((command == "chapter" && argc == 4) || (command == "write-chapter" && argc == 5) ||
      (command == "find" && argc == 6)) {
    char* end = nullptr;
    const unsigned long long number = std::strtoull(argv[3], &end, 10);
    if (end == nullptr || *end != '\0' || *argv[3] == '\0' || number > 65535) {
      PrintUsage(argv[0]);
      return 2;
    }
    if (command == "chapter") {
      return bn::tools::PrintChapter(argv[2], number);
    }
    if (command == "write-chapter") {
      return bn::tools::WriteChapter(argv[2], number, argv[4]);
    }
    return bn::tools::FindInChapter(argv[2], number, argv[4], argv[5]);
  }
  if (command == "make" && (argc == 4 || argc == 6)) {
    std::uint32_t written = 0;
    if (argc == 6) {
      char* end = nullptr;
      const unsigned long long asked = std::strtoull(argv[5], &end, 10);
      if (std::strcmp(argv[4], "--written") != 0 || end == nullptr || *end != '\0' || asked > 0xFFFFFFFFULL) {
        PrintUsage(argv[0]);
        return 2;
      }
      written = static_cast<std::uint32_t>(asked);
    }
    return Make(argv[2], argv[3], argc == 6, written);
  }
  PrintUsage(argv[0]);
  return 2;
}
