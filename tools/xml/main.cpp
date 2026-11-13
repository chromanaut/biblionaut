// bn-xml: reads XML with the parser the firmware reads it with, the same
// one in the same bounded memory, and says what it found: for one file,
// every event; for an EPUB, a line for each of its XML files.
//
// Events are written one to a line, in a form made to be compared with
// what another parser makes of the same document:
//
//   (name          an element begins
//   Aname value    one of its attributes, in the order they were written
//   )name          an element ends
//   -text          text, all that comes between two other events as one line
//   &name          a reference to an entity that was not expanded
//
// with a backslash, a line end, a tab and a carriage return in text and
// values written as \\, \n, \t and \r.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::XmlParser;
using bn::epub::XmlStatus;
using bn::epub::ZipStatus;

// Far more than a device would give it, so that what a document uses is
// what it wants, not what it was held to. --memory holds it to less.
constexpr std::size_t kDefaultMemoryBytes = 1024UL * 1024UL;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s events FILE [--memory BYTES]\n"
               "       %s book FILE [--memory BYTES]\n"
               "\n"
               "Reads XML the way the firmware does.\n"
               "\n"
               "  events  prints what the parser finds in the XML file FILE, an event a line:\n"
               "            (name   an element begins        )name   it ends\n"
               "            Aname value   an attribute       -text   text\n"
               "            &name   an entity that was not expanded\n"
               "  book    parses every XML file in the EPUB (or other ZIP file) FILE, those\n"
               "          whose names end in .xhtml .html .htm .xml .opf or .ncx, and prints\n"
               "          for each: how it ended, the checksum (CRC-32) of its events as\n"
               "          `events` would print them, how many there were, the most memory\n"
               "          the parser used, the line it stopped at, and its name\n"
               "\n"
               "  --memory BYTES  the memory the parser is given (default: %zu)\n"
               "\n"
               "Ends with 0 if every document was XML, 1 if not or if something could not be read.\n",
               program, program, kDefaultMemoryBytes);
}

// Takes the events and writes them out as lines, or only adds them up.
class EventWriter final : public bn::epub::XmlHandler {
 public:
  explicit EventWriter(bool print) : print_(print) {}

  void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
    FlushText();
    line_ = "(";
    line_ += name;
    Emit();
    for (std::size_t i = 0; i < attributes.Count(); ++i) {
      line_ = "A";
      line_ += attributes.Name(i);
      line_ += ' ';
      AppendEscaped(attributes.Value(i));
      Emit();
    }
  }
  void OnEndElement(std::string_view name) noexcept override {
    FlushText();
    line_ = ")";
    line_ += name;
    Emit();
  }
  void OnText(std::string_view text) noexcept override { text_ += text; }
  void OnEntity(std::string_view name) noexcept override {
    FlushText();
    line_ = "&";
    line_ += name;
    Emit();
  }

  // Call when the document is over: text may still be waiting.
  void Finish() noexcept { FlushText(); }

  std::uint32_t Checksum() const noexcept { return checksum_; }
  std::uint32_t Events() const noexcept { return events_; }

 private:
  void FlushText() noexcept {
    if (text_.empty()) {
      return;
    }
    line_ = "-";
    AppendEscaped(text_);
    text_.clear();
    Emit();
  }
  void AppendEscaped(std::string_view raw) {
    for (const char ch : raw) {
      switch (ch) {
        case '\\':
          line_ += "\\\\";
          break;
        case '\n':
          line_ += "\\n";
          break;
        case '\t':
          line_ += "\\t";
          break;
        case '\r':
          line_ += "\\r";
          break;
        default:
          line_ += ch;
      }
    }
  }
  void Emit() noexcept {
    line_ += '\n';
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
    checksum_ = bn::core::Crc32({reinterpret_cast<const std::uint8_t*>(line_.data()), line_.size()}, checksum_);
    ++events_;
    if (print_) {
      std::fwrite(line_.data(), 1, line_.size(), stdout);
    }
  }

  bool print_;
  std::string line_;
  std::string text_;
  std::uint32_t checksum_ = 0;
  std::uint32_t events_ = 0;
};

// A whole file of the computer's, as a source of bytes.
class FileSource final : public bn::core::ByteSource {
 public:
  explicit FileSource(std::FILE* file) : file_(file) {}
  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    const std::size_t read = std::fread(dst.data(), 1, dst.size(), file_);
    return read == 0 && std::ferror(file_) != 0 ? kError : static_cast<std::ptrdiff_t>(read);
  }

 private:
  std::FILE* file_;
};

int Events(const std::string& path, std::size_t memoryBytes) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  std::vector<std::uint8_t> memory(memoryBytes);
  XmlParser parser(memory);
  EventWriter writer(true);
  FileSource source(file);
  const XmlStatus status = parser.Parse(source, writer);
  writer.Finish();
  std::fclose(file);
  std::fflush(stdout);
  if (status != XmlStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s at line %u, byte %u (expat's error %d)\n", path.c_str(),
                 bn::epub::XmlStatusName(status), static_cast<unsigned>(parser.Line()),
                 static_cast<unsigned>(parser.ByteOffset()), parser.ErrorCode());
    return 1;
  }
  std::fprintf(stderr, "%u events, checksum %08x, %zu bytes of memory used\n", static_cast<unsigned>(writer.Events()),
               static_cast<unsigned>(writer.Checksum()), parser.Memory().HighWaterSinceReset());
  return 0;
}

bool IsXmlName(std::string_view name) {
  for (const std::string_view ending : {".xhtml", ".html", ".htm", ".xml", ".opf", ".ncx"}) {
    if (name.size() < ending.size()) {
      continue;
    }
    bool same = true;
    for (std::size_t i = 0; i < ending.size() && same; ++i) {
      char ch = name[name.size() - ending.size() + i];
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
      same = ch == ending[i];
    }
    if (same) {
      return true;
    }
  }
  return false;
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

int Book(const std::string& path, std::size_t memoryBytes) {
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
  std::vector<std::uint8_t> memory(memoryBytes);
  XmlParser parser(memory);
  std::vector<char> name(65536);

  const auto start = std::chrono::steady_clock::now();
  bn::epub::ZipArchive::Cursor cursor;
  std::uint32_t documents = 0;
  std::uint32_t failed = 0;
  std::uint64_t bytes = 0;
  std::size_t mostMemory = 0;
  for (;;) {
    bn::epub::ZipEntry entry;
    const ZipStatus next = archive.Next(cursor, entry, name);
    if (next == ZipStatus::kEnd) {
      break;
    }
    if (next != ZipStatus::kOk && next != ZipStatus::kUnsupported) {
      std::fprintf(stderr, "error: %s: the directory: %s\n", path.c_str(), bn::epub::ZipStatusName(next));
      return 1;
    }
    const std::string_view entryName(name.data(), entry.nameLength);
    if (!IsXmlName(entryName)) {
      continue;
    }
    ++documents;
    if (next != ZipStatus::kOk || reader.Open(entries, entry) != ZipStatus::kOk) {
      std::printf("unreadable -------- %8u %8u %6u %s\n", 0U, 0U, 0U, Printable(entryName).c_str());
      ++failed;
      continue;
    }
    EventWriter writer(false);
    XmlStatus status = parser.Parse(reader, writer);
    writer.Finish();
    // A document the parser liked may still be one the ZIP file got wrong.
    const bool readWell = status != XmlStatus::kOk || reader.Status() == ZipStatus::kOk;
    bytes += entry.size;
    // Each document begins with all of the memory given back.
    const std::size_t used = parser.Memory().HighWaterSinceReset();
    mostMemory = std::max(mostMemory, used);
    std::string how = bn::epub::XmlStatusName(status);
    if (!readWell) {
      how = "unreadable";
    }
    for (char& ch : how) {
      ch = ch == ' ' ? '-' : ch;
    }
    std::printf("%s %08x %8u %8zu %6u %s\n", how.c_str(), static_cast<unsigned>(writer.Checksum()),
                static_cast<unsigned>(writer.Events()), used, static_cast<unsigned>(parser.Line()),
                Printable(entryName).c_str());
    if (status != XmlStatus::kOk || !readWell) {
      ++failed;
    }
  }
  const double milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("%u documents, %u failed; %llu bytes of XML in %.1f ms; most memory used by one: %zu\n",
              static_cast<unsigned>(documents), static_cast<unsigned>(failed), static_cast<unsigned long long>(bytes),
              milliseconds, mostMemory);
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  std::size_t memoryBytes = kDefaultMemoryBytes;
  if (argc == 5 && std::strcmp(argv[3], "--memory") == 0) {
    char* end = nullptr;
    const unsigned long long asked = std::strtoull(argv[4], &end, 10);
    if (end == nullptr || *end != '\0' || asked == 0 || asked > (1ULL << 30U)) {
      PrintUsage(argv[0]);
      return 2;
    }
    memoryBytes = static_cast<std::size_t>(asked);
  } else if (argc != 3) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (command == "events") {
    return Events(argv[2], memoryBytes);
  }
  if (command == "book") {
    return Book(argv[2], memoryBytes);
  }
  PrintUsage(argv[0]);
  return 2;
}
