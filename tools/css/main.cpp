// bn-css: reads CSS with the reader the firmware reads it with, and says
// what it found: for one sheet, every rule; for an EPUB, a line for each
// of its style sheets and a count of what was passed over.
//
// What is found is written one thing to a line, in a form made to be
// compared with what another reader makes of the same sheet:
//
//   {                    a rule begins
//   S a,b,c selector     one of its selectors, after how CSS ranks it: the
//                        counts of ids, of classes and of element names
//   D property: value    one thing it sets (with " !important" if it was)
//   }                    the rule ends
//   I url                an @import
//   X kind: text         something that was passed over
//
// A selector is written with one space for descent, " > " for a child and
// " + " for the element straight after; each part as its element's name,
// then its id, then its classes, then what it asks of attributes, and `*`
// for a part with none of them; and " ::first-letter" at the end if it has
// it. What is asked of an attribute is written as [name], or as
// [name="value"] with the sign the sheet had before the `=`, the name as a
// document writes it ("epub:type"), `*|` before it if any prefix will do,
// and " i" after the value if any case will.
// A length is written as a number and its unit, in pixels where the sheet
// had points or inches. A backslash, a line end, a tab and a carriage
// return in any text are written as \\, \n, \t and \r.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "core/checksum.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "epub/css.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::CssDropped;
using bn::epub::CssReader;
using bn::epub::CssStatus;
using bn::epub::ZipStatus;

// How much of what was passed over is written out, at the most.
constexpr std::size_t kMostDroppedText = 80;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s rules FILE\n"
               "       %s style TEXT\n"
               "       %s book FILE [--dropped]\n"
               "\n"
               "Reads CSS the way the firmware does.\n"
               "\n"
               "  rules  prints what the reader finds in the style sheet FILE, a thing a line:\n"
               "           {   a rule begins              }   it ends\n"
               "           S a,b,c selector   a selector, after its counts of ids, classes and elements\n"
               "           D property: value  something the rule sets\n"
               "           I url   an @import             X kind: text   something passed over\n"
               "  style  prints the same for TEXT read as a `style` attribute: its declarations\n"
               "  book   reads every style sheet in the EPUB (or other ZIP file) FILE, those whose\n"
               "         names end in .css, and prints for each: how it ended, the checksum\n"
               "         (CRC-32) of what `rules` would print for it, its rules, the declarations\n"
               "         in them, how many things were passed over, and its name; and at the end\n"
               "         what all of them came to\n"
               "\n"
               "  --dropped  with book: after that, a line for each different thing that was\n"
               "             passed over: `dropped`, how many times, its kind, and what it was\n"
               "\n"
               "Ends with 0, or 1 if something could not be read.\n",
               program, program, program);
}

std::string Escaped(std::string_view raw) {
  std::string out;
  for (const char ch : raw) {
    if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\\') {
      out += "\\\\";
    } else {
      out += ch;
    }
  }
  return out;
}

// So many thousandths, as a number: 1500 is "1.5".
std::string Number(std::int32_t thousandths) {
  const std::int64_t value = thousandths;
  const std::int64_t size = value < 0 ? -value : value;
  std::string out = (value < 0 ? "-" : "") + std::to_string(size / 1000);
  if (size % 1000 != 0) {
    std::array<char, 8> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%03d", static_cast<int>(size % 1000));
    out += fraction.data();
    while (out.back() == '0') {
      out.pop_back();
    }
  }
  return out;
}

std::string ValueText(const bn::epub::CssValue& value) {
  switch (value.kind) {
    case bn::epub::CssValue::Kind::kKeyword:
      return bn::epub::CssKeywordName(value.keyword);
    case bn::epub::CssValue::Kind::kLength:
      return Number(value.number) + bn::epub::CssUnitName(value.unit);
    case bn::epub::CssValue::Kind::kFamilies: {
      std::string out;
      std::array<char, 256> name{};
      std::string_view rest = value.families;
      for (std::string_view one = bn::epub::CssNextFamily(rest, name); !one.empty();
           one = bn::epub::CssNextFamily(rest, name)) {
        out += (out.empty() ? "" : ", ") + std::string(one);
      }
      return out;
    }
  }
  return "?";
}

std::string SelectorText(const bn::epub::CssSelector& selector) {
  std::string out;
  for (const bn::epub::CssCompound& part : selector.compounds) {
    if (part.combinator == bn::epub::CssCombinator::kDescendant) {
      out += ' ';
    } else if (part.combinator == bn::epub::CssCombinator::kChild) {
      out += " > ";
    } else if (part.combinator == bn::epub::CssCombinator::kAdjacent) {
      out += " + ";
    }
    if (part.element.empty() && part.id.empty() && part.classes.empty() && part.attributes.empty()) {
      out += '*';
    }
    out += part.element;
    if (!part.id.empty()) {
      out += "#" + std::string(part.id);
    }
    for (const std::string_view name : part.classes) {
      out += "." + std::string(name);
    }
    for (const bn::epub::CssAttribute& attribute : part.attributes) {
      out += std::string("[") + (attribute.anyPrefix ? "*|" : "") + std::string(attribute.name);
      if (attribute.test != bn::epub::CssAttributeTest::kPresent) {
        out += std::string(bn::epub::CssAttributeTestName(attribute.test)) + "\"" + std::string(attribute.value) +
               "\"" + (attribute.anyCase ? " i" : "");
      }
      out += ']';
    }
  }
  if (selector.firstLetter) {
    out += " ::first-letter";
  }
  return out;
}

// Takes what a reader tells of and writes it out as lines, or only adds
// the lines up; and keeps count of what was passed over, by what it was.
class Writer final : public bn::epub::CssHandler {
 public:
  explicit Writer(bool print) : print_(print) {}

  void OnRuleBegin() noexcept override { Line("{"); }
  void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
    const std::uint32_t rank = selector.specificity;
    Line("S " + std::to_string(rank >> 16U) + "," + std::to_string((rank >> 8U) & 0xFFU) + "," +
         std::to_string(rank & 0xFFU) + " " + Escaped(SelectorText(selector)));
  }
  void OnDeclaration(const bn::epub::CssDeclaration& declaration) noexcept override {
    Line(std::string("D ") + bn::epub::CssPropertyName(declaration.property) + ": " +
         Escaped(ValueText(declaration.value)) + (declaration.important ? " !important" : ""));
  }
  void OnRuleEnd() noexcept override { Line("}"); }
  void OnImport(std::string_view url) noexcept override { Line("I " + Escaped(url)); }
  void OnDropped(CssDropped dropped, std::string_view text) noexcept override {
    const std::string what = Escaped(text.substr(0, std::min(text.size(), kMostDroppedText)));
    Line(std::string("X ") + bn::epub::CssDroppedName(dropped) + ": " + what);
    ++tally[{bn::epub::CssDroppedName(dropped), what}];
  }

  // The checksum of the lines so far, and how many there were.
  std::uint32_t Checksum() const { return checksum_; }
  std::uint32_t Lines() const { return lines_; }
  void Restart() {
    checksum_ = 0;
    lines_ = 0;
  }

  // What was passed over, by kind and text, and how many times.
  std::map<std::pair<std::string, std::string>, std::uint32_t> tally;

 private:
  void Line(const std::string& line) {
    if (print_) {
      // Written by its length: a sheet may have a byte of zero in it.
      std::fwrite(line.data(), 1, line.size(), stdout);
      std::fputc('\n', stdout);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(line.data());
    checksum_ = bn::core::Crc32({bytes, line.size()}, checksum_);
    const std::array<std::uint8_t, 1> end = {'\n'};
    checksum_ = bn::core::Crc32(end, checksum_);
    ++lines_;
  }

  bool print_;
  std::uint32_t checksum_ = 0;
  std::uint32_t lines_ = 0;
};

std::uint32_t DroppedInAll(const CssReader& reader) {
  std::uint32_t all = 0;
  for (std::size_t kind = 0; kind < bn::epub::kCssDroppedKinds; ++kind) {
    all += reader.Dropped(static_cast<CssDropped>(kind));
  }
  return all;
}

int Rules(const std::string& path) {
  bn::tools::OneFile storage(path);
  std::array<std::uint8_t, 4096> buffer{};
  bn::core::BlockFile file{buffer};
  if (storage.Start() != StorageStatus::kOk ||
      file.Open(storage, "/css", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Writer writer(true);
  CssReader reader;
  if (reader.Read(file, writer) != CssStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::CssStatusName(reader.Status()));
    return 1;
  }
  return 0;
}

int Style(std::string_view text) {
  Writer writer(true);
  bn::epub::ReadCssDeclarations(text, writer);
  return 0;
}

bool HasCssEnding(std::string_view name) {
  constexpr std::string_view kEnding = ".css";
  return name.size() > kEnding.size() &&
         std::equal(kEnding.begin(), kEnding.end(), name.end() - static_cast<std::ptrdiff_t>(kEnding.size()),
                    [](char a, char b) { return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b); });
}

int Book(const std::string& path, bool printDropped) {
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
  bn::epub::ZipEntryReader entryReader{window};
  std::vector<char> name(65536);

  Writer writer(false);
  CssReader reader;
  bn::epub::ZipArchive::Cursor cursor;
  std::uint32_t sheets = 0;
  std::uint32_t failed = 0;
  std::uint64_t bytes = 0;
  std::uint32_t rules = 0;
  std::uint32_t declarations = 0;
  std::uint32_t dropped = 0;
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
    const std::string_view entryName(name.data(), std::min<std::size_t>(entry.nameLength, name.size()));
    if (!HasCssEnding(entryName)) {
      continue;
    }
    ++sheets;
    writer.Restart();
    const bool opened = next == ZipStatus::kOk && entryReader.Open(entries, entry) == ZipStatus::kOk;
    const bool read =
        opened && reader.Read(entryReader, writer) == CssStatus::kOk && entryReader.Status() == ZipStatus::kOk;
    if (!read) {
      std::printf("unreadable -------- %6u %6u %6u %s\n", 0U, 0U, 0U, Escaped(entryName).c_str());
      ++failed;
      continue;
    }
    bytes += entry.size;
    rules += reader.Rules();
    declarations += reader.Declarations();
    dropped += DroppedInAll(reader);
    std::printf("ok %08x %6u %6u %6u %s\n", static_cast<unsigned>(writer.Checksum()),
                static_cast<unsigned>(reader.Rules()), static_cast<unsigned>(reader.Declarations()),
                static_cast<unsigned>(DroppedInAll(reader)), Escaped(entryName).c_str());
  }
  std::printf("css_sheets=%u css_failed=%u css_bytes=%llu css_rules=%u css_declarations=%u css_dropped=%u\n",
              static_cast<unsigned>(sheets), static_cast<unsigned>(failed), static_cast<unsigned long long>(bytes),
              static_cast<unsigned>(rules), static_cast<unsigned>(declarations), static_cast<unsigned>(dropped));
  if (printDropped) {
    for (const auto& [what, count] : writer.tally) {
      std::printf("dropped %u %s: ", static_cast<unsigned>(count), what.first.c_str());
      std::fwrite(what.second.data(), 1, what.second.size(), stdout);
      std::fputc('\n', stdout);
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  if (command == "rules" && argc == 3) {
    return Rules(argv[2]);
  }
  if (command == "style" && argc == 3) {
    return Style(argv[2]);
  }
  if (command == "book" && (argc == 3 || (argc == 4 && std::strcmp(argv[3], "--dropped") == 0))) {
    return Book(argv[2], argc == 4);
  }
  PrintUsage(argv[0]);
  return 2;
}
