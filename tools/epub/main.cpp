// bn-epub: says what a book says of itself, read with the code the
// firmware reads it with: its title and author, the documents it is made
// of in reading order, and where its cover and its tables of contents are.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "core/metrics.h"
#include "core/storage.h"
#include "epub/package.h"
#include "epub/package_digest.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::PackageStatus;
using bn::epub::ZipStatus;

// More than any book needs, so that what a book uses is what it wants.
// --memory holds it to less.
constexpr std::size_t kDefaultMemoryBytes = 256UL * 1024UL;
// What the XML parser is given: what a device would give it.
constexpr std::size_t kXmlMemoryBytes = 16UL * 1024UL;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s info FILE [--memory BYTES]\n"
               "\n"
               "Reads the package of the EPUB FILE the way the firmware does, and prints:\n"
               "\n"
               "  title, author, language   as the book gives them\n"
               "  package                   the path of its package file\n"
               "  manifest                  how many files it lists\n"
               "  cover, nav, ncx           its cover picture and its tables of contents:\n"
               "                            the kind of file and its path, or - for none\n"
               "  spine                     how many documents it is read as, and then a line\n"
               "                            for each in reading order: + for one in the main\n"
               "                            run of the book and - for one set aside, the kind\n"
               "                            of file, and its path (? if it is not in the book)\n"
               "  checksum                  of all of the above (epub/package_digest.h): what\n"
               "                            a device's book check reports for the same book\n"
               "  memory                    the most of its memory the reader used, in bytes\n"
               "  cost                      what it asked of the file\n"
               "\n"
               "  --memory BYTES  the memory the reader is given for the manifest and the\n"
               "                  spine (default: %zu)\n"
               "\n"
               "In a path, a backslash and any byte below a space are printed as \\xNN.\n"
               "Ends with 0 if the package was read, 1 if not.\n",
               program, kDefaultMemoryBytes);
}

// Text as it is printed: every byte that could confuse a reader of lines
// written out as its number.
std::string Printable(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == '\\') {
      std::array<char, 8> escaped{};
      std::snprintf(escaped.data(), escaped.size(), "\\x%02X", static_cast<unsigned>(byte));
      out += escaped.data();
    } else {
      out += ch;
    }
  }
  return out;
}

// The path of an entry, read back from the file.
std::string PathOf(bn::core::BlockFile& file, const bn::epub::ZipEntry& entry) {
  std::vector<char> name(65536);
  std::size_t length = 0;
  if (bn::epub::ReadZipEntryName(file, entry, name, length) != ZipStatus::kOk || length > name.size()) {
    return "(unreadable)";
  }
  return Printable(std::string_view(name.data(), length));
}

int Info(const std::string& path, std::size_t memoryBytes) {
  bn::tools::OneFile storage(path);
  std::array<std::uint8_t, 4096> directoryBuffer{};
  std::array<std::uint8_t, 4096> entryBuffer{};
  bn::core::BlockFile directory{directoryBuffer};
  bn::core::BlockFile entries{entryBuffer};
  bn::epub::ZipArchive archive;
  if (storage.Start() != StorageStatus::kOk ||
      directory.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk ||
      entries.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  if (const ZipStatus status = archive.Open(directory); status != ZipStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::ZipStatusName(status));
    return 1;
  }
  std::vector<std::uint8_t> window(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader{window};
  std::vector<std::uint8_t> xmlMemory(kXmlMemoryBytes);
  bn::epub::XmlParser xml(xmlMemory);
  std::vector<std::uint8_t> memory(memoryBytes);
  bn::epub::Package package(memory);

  const PackageStatus status = package.Read(archive, entries, reader, xml);
  // What the reading cost, before the printing below reads each path back
  // from the file, which costs a read apiece and is no part of it.
  std::array<char, 1024> cost{};
  bn::core::DefaultMetrics().FormatLine(cost);
  if (status != PackageStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::PackageStatusName(status));
    return 1;
  }
  // What a book does not say is shown as "-", as a file it has not got is.
  const auto said = [](std::string_view text) { return text.empty() ? std::string("-") : Printable(text); };
  std::printf("title: %s\n", said(package.Title()).c_str());
  std::printf("author: %s\n", said(package.Author()).c_str());
  std::printf("language: %s\n", said(package.Language()).c_str());
  std::printf("package: %s\n", Printable(package.PackagePath()).c_str());
  std::printf("manifest: %u\n", static_cast<unsigned>(package.ManifestItems()));
  if (package.Cover() != nullptr) {
    std::printf("cover: %s %s\n", bn::epub::MediaTypeName(package.CoverType()),
                PathOf(entries, *package.Cover()).c_str());
  } else {
    std::printf("cover: -\n");
  }
  std::printf("nav: %s\n", package.Nav() != nullptr ? PathOf(entries, *package.Nav()).c_str() : "-");
  std::printf("ncx: %s\n", package.Ncx() != nullptr ? PathOf(entries, *package.Ncx()).c_str() : "-");
  std::printf("spine: %zu\n", package.Spine().size());
  for (const bn::epub::SpineItem& item : package.Spine()) {
    std::printf("%c %s %s\n", item.linear ? '+' : '-', bn::epub::MediaTypeName(item.type),
                item.present ? PathOf(entries, item.entry).c_str() : "?");
  }
  if (package.UnknownSpineItems() != 0) {
    std::printf("unknown: %u\n", static_cast<unsigned>(package.UnknownSpineItems()));
  }
  if (package.CutShort()) {
    std::printf("cut short: the package file stopped being XML after its spine\n");
  }
  std::printf("checksum: %08x\n", static_cast<unsigned>(bn::epub::PackageChecksum(package)));
  std::printf("memory: %zu\n", package.MemoryUsed());
  std::printf("cost: %s\n", cost.data());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  std::size_t memoryBytes = kDefaultMemoryBytes;
  if (argc == 5 && std::strcmp(argv[3], "--memory") == 0) {
    char* end = nullptr;
    const unsigned long long asked = std::strtoull(argv[4], &end, 10);
    if (end == nullptr || *end != '\0' || asked > (1ULL << 30U)) {
      PrintUsage(argv[0]);
      return 2;
    }
    memoryBytes = static_cast<std::size_t>(asked);
  } else if (argc != 3) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (command == "info") {
    return Info(argv[2], memoryBytes);
  }
  PrintUsage(argv[0]);
  return 2;
}
