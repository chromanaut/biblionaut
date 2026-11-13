// bn-zip: lists, checks and reads out the entries of a ZIP file, such as an
// EPUB, with the code the firmware reads one with: the same reader, through
// the same kind of file object with the same 4 KB buffer.
//
// It has the file open twice, once for the directory and once for the
// entries. With one file object for both, every step from an entry back to
// the directory would read a block of the file over again.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "core/metrics.h"
#include "core/storage.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::ZipArchive;
using bn::epub::ZipEntry;
using bn::epub::ZipEntryReader;
using bn::epub::ZipStatus;
using bn::tools::OneFile;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s list FILE\n"
               "       %s check FILE\n"
               "       %s cat FILE NAME\n"
               "\n"
               "Reads a ZIP file, such as an EPUB, the way the firmware does.\n"
               "\n"
               "  list   prints a line for each entry: its checksum, its size, the size it\n"
               "         takes up in the file, how it is kept, its flags and its name\n"
               "  check  reads every entry to its end, checking its size and checksum, and\n"
               "         looks each one up by name; says what it cost in reads of the file\n"
               "  cat    writes the entry called NAME to standard output\n"
               "\n"
               "In a name, a backslash and any byte below a space are printed as \\xNN.\n"
               "Ends with 0 if all was well, 1 if the file or an entry could not be read.\n",
               program, program, program);
}

// A name as it is printed: every byte that could confuse a reader of lines
// written out as its number.
std::string Printable(std::string_view name) {
  std::string out;
  for (const char ch : name) {
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

std::string MethodName(const ZipEntry& entry) {
  if (entry.method == bn::epub::kZipStored) {
    return "stored";
  }
  if (entry.method == bn::epub::kZipDeflated) {
    return "deflated";
  }
  return "method-" + std::to_string(entry.method);
}

// The ZIP file, open as the firmware would have it.
struct OpenZip {
  explicit OpenZip(const std::string& path) : storage(path) {}

  // Says what is wrong and returns false if it cannot be opened.
  bool Open(const std::string& path) {
    const StorageStatus started = storage.Start();
    if (started != StorageStatus::kOk ||
        directory.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk ||
        entries.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
      std::fprintf(stderr, "error: cannot read %s%s\n", path.c_str(),
                   started == StorageStatus::kTooBig ? ": it is 4 GB or more" : "");
      return false;
    }
    const ZipStatus status = archive.Open(directory);
    if (status != ZipStatus::kOk) {
      std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::ZipStatusName(status));
      return false;
    }
    return true;
  }

  OneFile storage;
  std::array<std::uint8_t, 4096> directoryBuffer{};
  std::array<std::uint8_t, 4096> entryBuffer{};
  bn::core::BlockFile directory{directoryBuffer};
  bn::core::BlockFile entries{entryBuffer};
  ZipArchive archive;
  std::vector<std::uint8_t> window = std::vector<std::uint8_t>(bn::core::Inflater::kWindowBytes);
  ZipEntryReader reader{window};
  std::vector<char> name = std::vector<char>(65536);  // Room for the longest name there can be.
};

int List(OpenZip& zip) {
  ZipArchive::Cursor cursor;
  bool allRead = true;
  for (;;) {
    ZipEntry entry;
    const ZipStatus status = zip.archive.Next(cursor, entry, zip.name);
    if (status == ZipStatus::kEnd) {
      break;
    }
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      std::fprintf(stderr, "error: the directory: %s\n", bn::epub::ZipStatusName(status));
      return 1;
    }
    const std::string name = Printable(std::string_view(zip.name.data(), entry.nameLength));
    if (status == ZipStatus::kUnsupported) {
      std::printf("-------- %10s %10s %-9s %04x %s\n", "?", "?", "zip64", static_cast<unsigned>(entry.flags),
                  name.c_str());
      allRead = false;
      continue;
    }
    std::printf("%08x %10u %10u %-9s %04x %s\n", static_cast<unsigned>(entry.crc32), static_cast<unsigned>(entry.size),
                static_cast<unsigned>(entry.compressedSize), MethodName(entry).c_str(),
                static_cast<unsigned>(entry.flags), name.c_str());
  }
  return allRead ? 0 : 1;
}

int Check(OpenZip& zip) {
  const auto start = std::chrono::steady_clock::now();
  ZipArchive::Cursor cursor;
  std::array<std::uint8_t, 4096> part{};
  std::uint32_t count = 0;
  std::uint32_t failed = 0;
  std::uint64_t bytesOut = 0;
  std::uint64_t bytesIn = 0;
  for (;;) {
    ZipEntry entry;
    ZipStatus status = zip.archive.Next(cursor, entry, zip.name);
    if (status == ZipStatus::kEnd) {
      break;
    }
    if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
      std::printf("FAILED  the directory: %s\n", bn::epub::ZipStatusName(status));
      ++failed;
      break;
    }
    ++count;
    const std::string_view name(zip.name.data(), entry.nameLength);
    if (status == ZipStatus::kOk) {
      status = zip.reader.Open(zip.entries, entry);
    }
    if (status == ZipStatus::kOk) {
      for (;;) {
        const std::ptrdiff_t read = zip.reader.Read(part);
        if (read <= 0) {
          break;
        }
        bytesOut += static_cast<std::uint64_t>(read);
      }
      status = zip.reader.Status();
      bytesIn += entry.compressedSize;
    }
    // The other way to an entry: by its name. It must lead to the same
    // place, unless two entries share the name.
    if (status == ZipStatus::kOk) {
      ZipEntry found;
      const ZipStatus byName = zip.archive.Find(name, found);
      if (byName != ZipStatus::kOk) {
        status = byName;
      } else if (found.headerOffset != entry.headerOffset) {
        std::printf("note    another entry has the same name: %s\n", Printable(name).c_str());
      }
    }
    if (status != ZipStatus::kOk) {
      std::printf("FAILED  %s: %s\n", bn::epub::ZipStatusName(status), Printable(name).c_str());
      ++failed;
    }
  }
  const double milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("%u entries, %u failed; %llu bytes read from %llu in the file, in %.1f ms\n",
              static_cast<unsigned>(count), static_cast<unsigned>(failed), static_cast<unsigned long long>(bytesOut),
              static_cast<unsigned long long>(bytesIn), milliseconds);
  std::array<char, 1024> metrics{};
  bn::core::DefaultMetrics().FormatLine(metrics);
  std::printf("%s\n", metrics.data());
  return failed == 0 ? 0 : 1;
}

int Cat(OpenZip& zip, const std::string& name) {
  ZipEntry entry;
  ZipStatus status = zip.archive.Find(name, entry);
  if (status == ZipStatus::kOk) {
    status = zip.reader.Open(zip.entries, entry);
  }
  if (status == ZipStatus::kOk) {
    std::array<std::uint8_t, 4096> part{};
    for (;;) {
      const std::ptrdiff_t read = zip.reader.Read(part);
      if (read <= 0) {
        break;
      }
      if (std::fwrite(part.data(), 1, static_cast<std::size_t>(read), stdout) != static_cast<std::size_t>(read)) {
        std::fprintf(stderr, "error: cannot write the output\n");
        return 1;
      }
    }
    status = zip.reader.Status();
  }
  if (status != ZipStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", name.c_str(), bn::epub::ZipStatusName(status));
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  const bool wantsName = command == "cat";
  if ((command != "list" && command != "check" && command != "cat") || argc != (wantsName ? 4 : 3)) {
    PrintUsage(argv[0]);
    return 2;
  }
  const std::string path = argv[2];
  OpenZip zip(path);
  if (!zip.Open(path)) {
    return 1;
  }
  if (command == "list") {
    return List(zip);
  }
  if (command == "check") {
    return Check(zip);
  }
  return Cat(zip, argv[3]);
}
