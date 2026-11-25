// bn-css: reads CSS with the reader the firmware reads it with, and says
// what it found: for one sheet, every rule; for an EPUB, a line for each
// of its style sheets and a count of what was passed over; for a document
// and the sheets it is given, what the rules come to for each of its
// elements; and for an EPUB's documents, the sheets each asks for, found
// as the firmware finds them, and what was left out in finding them.
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
//
// What the rules come to for a document's elements is written the same
// way:
//
//   < name               an element opens
//   P property: value    something that is set for it, the value that wins
//   F property: value    something that is set for its first letter
//   >                    the element closes
//
// And the sheets a document asks for:
//
//   D name               a document of the book
//   S path               a sheet it is given, in the order their rules
//                        apply; `S <style>` for its own `<style>` rules
//   X kind: text         something that was left out in finding them

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
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
#include "epub/book_files.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/xml.h"
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

// What `cascade` gives the parser of a document, which is far more than a
// document needs, and the cascade for the elements that are open.
constexpr std::size_t kParserBytes = 1024UL * 1024UL;
constexpr std::size_t kCascadeBytes = 4096;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s rules FILE\n"
               "       %s style TEXT\n"
               "       %s book FILE [--dropped]\n"
               "       %s cascade [--in BOOK] [--sum] DOCUMENT SHEET...\n"
               "       %s sheets BOOK [--dropped]\n"
               "       %s document [--sum] BOOK DOCUMENT\n"
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
               "         in them, how many things were passed over, and its name; at the end what\n"
               "         all of them came to; and then what keeping all their rules in one table\n"
               "         takes: its bytes, and the names, nodes, rules and selectors in it, and how\n"
               "         many rules it had to leave out\n"
               "  cascade  keeps the rules of each SHEET in a table, goes through the XML file\n"
               "         DOCUMENT with them, the first sheet's rules applying first, and prints for\n"
               "         each element what they come to:\n"
               "           < name   an element opens      >   it closes\n"
               "           P property: value   something set for it, the value that wins\n"
               "           F property: value   something set for its first letter\n"
               "  sheets   finds, for every document of the EPUB (or other ZIP file) BOOK, those\n"
               "         whose names end in .xhtml, .html or .htm, the style sheets it asks for:\n"
               "         those its head links, each after the sheets it imports, and its own\n"
               "         `<style>` rules. Prints for each:\n"
               "           D name   the document          S path   a sheet it is given, in order\n"
               "         with `S <style>` for its own rules; and at the end how many documents\n"
               "         and sheets there were, what was left out by its kind, and what the table\n"
               "         of the book's rules takes\n"
               "  document  finds the sheets of the one DOCUMENT of BOOK in the same way, prints\n"
               "         them and what was left out, and then goes through the document with them\n"
               "         and prints what `cascade` prints\n"
               "\n"
               "  --dropped  with book: after that, a line for each different thing that was\n"
               "             passed over: `dropped`, how many times, its kind, and what it was\n"
               "  --in BOOK  with cascade: DOCUMENT and each SHEET are the names of entries in the\n"
               "             EPUB (or other ZIP file) BOOK\n"
               "             with sheets: under each document, a line for each thing left out:\n"
               "               X kind: text\n"
               "  --sum      with cascade or document: print one line in place of the elements,\n"
               "             with the checksum (CRC-32) of their lines, how many elements and lines\n"
               "             there were, and what the table and the cascade took\n"
               "\n"
               "Ends with 0, or 1 if something could not be read.\n",
               program, program, program, program, program, program);
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

// Tells two handlers of the same sheet.
class Both final : public bn::epub::CssHandler {
 public:
  Both(bn::epub::CssHandler& first, bn::epub::CssHandler& second) : first_(first), second_(second) {}

  void OnRuleBegin() noexcept override {
    first_.OnRuleBegin();
    second_.OnRuleBegin();
  }
  void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
    first_.OnSelector(selector);
    second_.OnSelector(selector);
  }
  void OnDeclaration(const bn::epub::CssDeclaration& declaration) noexcept override {
    first_.OnDeclaration(declaration);
    second_.OnDeclaration(declaration);
  }
  void OnRuleEnd() noexcept override {
    first_.OnRuleEnd();
    second_.OnRuleEnd();
  }
  void OnImport(std::string_view url) noexcept override {
    first_.OnImport(url);
    second_.OnImport(url);
  }
  void OnDropped(CssDropped dropped, std::string_view text) noexcept override {
    first_.OnDropped(dropped, text);
    second_.OnDropped(dropped, text);
  }

 private:
  bn::epub::CssHandler& first_;
  bn::epub::CssHandler& second_;
};

// The lines of what the rules come to for a document's elements, written
// out or only added up.
class Lines {
 public:
  explicit Lines(bool print) : print_(print) {}

  void Add(const std::string& line) {
    if (print_) {
      std::fwrite(line.data(), 1, line.size(), stdout);
      std::fputc('\n', stdout);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(line.data());
    checksum_ = bn::core::Crc32({bytes, line.size()}, checksum_);
    const std::array<std::uint8_t, 1> end = {'\n'};
    checksum_ = bn::core::Crc32(end, checksum_);
    ++count_;
  }

  std::uint32_t Checksum() const { return checksum_; }
  std::uint32_t Count() const { return count_; }

 private:
  bool print_;
  std::uint32_t checksum_ = 0;
  std::uint32_t count_ = 0;
};

// Goes through a document beside its parser, asking the cascade about
// each element and writing down what it says.
class Styler final : public bn::epub::XmlHandler {
 public:
  Styler(bn::epub::CssCascade& cascade, Lines& lines) : cascade_(cascade), lines_(lines) {}

  void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
    bn::epub::CssStyle style;
    bn::epub::CssStyle firstLetter;
    cascade_.Open(name, attributes, style, firstLetter);
    ++elements;
    lines_.Add("< " + Escaped(name));
    Set('P', style);
    Set('F', firstLetter);
  }
  void OnEndElement(std::string_view /*name*/) noexcept override {
    cascade_.Close();
    lines_.Add(">");
  }
  void OnText(std::string_view /*text*/) noexcept override {}
  void OnEntity(std::string_view /*name*/) noexcept override {}

  std::uint32_t elements = 0;

 private:
  void Set(char mark, const bn::epub::CssStyle& style) {
    for (std::size_t i = 0; i < bn::epub::kCssProperties; ++i) {
      const auto property = static_cast<bn::epub::CssProperty>(i);
      if (style.Has(property)) {
        lines_.Add(std::string(1, mark) + " " + bn::epub::CssPropertyName(property) + ": " +
                   Escaped(ValueText(style[property])));
      }
    }
  }

  bn::epub::CssCascade& cascade_;
  Lines& lines_;
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
  // Every sheet's rules also go into one table, as large as a table can
  // be, to say what keeping them takes.
  std::vector<std::uint8_t> tableMemory(bn::epub::CssRules::kMaxBytes);
  bn::epub::CssRules table(tableMemory);
  Both both(writer, table);
  std::uint32_t unnumbered = 0;  // Sheets after the most a table numbers.
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
    std::uint8_t number = 0;
    const bool numbered = table.BeginSheet(number);
    unnumbered += numbered ? 0 : 1;
    const bool read =
        opened && reader.Read(entryReader, both) == CssStatus::kOk && entryReader.Status() == ZipStatus::kOk;
    table.EndSheet();
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
  std::printf(
      "css_table_bytes=%zu css_table_names=%zu css_table_nodes=%zu css_table_rules=%u css_table_selectors=%u "
      "css_table_left_out=%u css_table_sheets_left_out=%u\n",
      table.Bytes(), table.Names(), table.Nodes(), static_cast<unsigned>(table.Rules()),
      static_cast<unsigned>(table.Selectors()), static_cast<unsigned>(table.LeftOutForRoom() + table.LeftOutForNodes()),
      static_cast<unsigned>(unnumbered));
  if (printDropped) {
    for (const auto& [what, count] : writer.tally) {
      std::printf("dropped %u %s: ", static_cast<unsigned>(count), what.first.c_str());
      std::fwrite(what.second.data(), 1, what.second.size(), stdout);
      std::fputc('\n', stdout);
    }
  }
  return failed == 0 ? 0 : 1;
}

// What `cascade` reads from: files of the computer's, or the entries of
// one ZIP file.
class Reading {
 public:
  explicit Reading(const std::string& book) : inBook_(!book.empty()), storage_(book) {}

  // Gets the book ready, if there is one.
  bool Start() {
    if (!inBook_) {
      return true;
    }
    return storage_.Start() == StorageStatus::kOk &&
           directory_.Open(storage_, "/zip", bn::core::OpenMode::kRead) == StorageStatus::kOk &&
           entries_.Open(storage_, "/zip", bn::core::OpenMode::kRead) == StorageStatus::kOk &&
           archive_.Open(directory_) == ZipStatus::kOk;
  }

  // The source for what is at `name`, or null if it cannot be read. Good
  // until the next is asked for.
  bn::core::ByteSource* Open(const std::string& name) {
    if (inBook_) {
      bn::epub::ZipEntry entry;
      if (archive_.Find(name, entry) != ZipStatus::kOk || entryReader_.Open(entries_, entry) != ZipStatus::kOk) {
        return nullptr;
      }
      return &entryReader_;
    }
    file_.emplace(name);
    fileBlock_.emplace(fileBuffer_);
    if (file_->Start() != StorageStatus::kOk ||
        fileBlock_->Open(*file_, "/file", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
      return nullptr;
    }
    return &*fileBlock_;
  }

  // Whether what was opened last was read without a fault of the ZIP
  // file's.
  bool Sound() const { return !inBook_ || entryReader_.Status() == ZipStatus::kOk; }

 private:
  bool inBook_;
  bn::tools::OneFile storage_;
  std::array<std::uint8_t, 4096> directoryBuffer_{};
  std::array<std::uint8_t, 4096> entryBuffer_{};
  std::array<std::uint8_t, 4096> fileBuffer_{};
  bn::core::BlockFile directory_{directoryBuffer_};
  bn::core::BlockFile entries_{entryBuffer_};
  bn::epub::ZipArchive archive_;
  std::vector<std::uint8_t> window_ = std::vector<std::uint8_t>(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader entryReader_{window_};
  std::optional<bn::tools::OneFile> file_;
  std::optional<bn::core::BlockFile> fileBlock_;
};

int Cascade(const std::string& book, bool sum, const std::string& document, const std::vector<std::string>& sheets) {
  Reading reading(book);
  if (!reading.Start()) {
    std::fprintf(stderr, "error: cannot read %s\n", book.c_str());
    return 1;
  }

  // The rules of every sheet, in a table as large as a table can be.
  std::vector<std::uint8_t> tableMemory(bn::epub::CssRules::kMaxBytes);
  bn::epub::CssRules table(tableMemory);
  std::vector<std::uint8_t> numbers;
  CssReader reader;
  for (const std::string& sheet : sheets) {
    bn::core::ByteSource* source = reading.Open(sheet);
    std::uint8_t number = 0;
    if (source == nullptr || !table.BeginSheet(number)) {
      std::fprintf(stderr, "error: cannot read %s\n", sheet.c_str());
      return 1;
    }
    const bool read = reader.Read(*source, table) == CssStatus::kOk && reading.Sound();
    table.EndSheet();
    if (!read) {
      std::fprintf(stderr, "error: %s: %s\n", sheet.c_str(), bn::epub::CssStatusName(reader.Status()));
      return 1;
    }
    numbers.push_back(number);
  }

  bn::core::ByteSource* source = reading.Open(document);
  if (source == nullptr) {
    std::fprintf(stderr, "error: cannot read %s\n", document.c_str());
    return 1;
  }
  std::vector<std::uint8_t> cascadeMemory(kCascadeBytes);
  bn::epub::CssCascade cascade(table, cascadeMemory);
  cascade.Begin(numbers);
  Lines lines(!sum);
  Styler styler(cascade, lines);
  std::vector<std::uint8_t> parserMemory(kParserBytes);
  bn::epub::XmlParser parser(parserMemory);
  const bn::epub::XmlStatus status = parser.Parse(*source, styler);
  if (status != bn::epub::XmlStatus::kOk || !reading.Sound()) {
    std::fprintf(stderr, "error: %s: %s at line %u\n", document.c_str(), bn::epub::XmlStatusName(status),
                 static_cast<unsigned>(parser.Line()));
    return 1;
  }
  if (sum) {
    std::printf(
        "cascade %08x elements=%u lines=%u table_bytes=%zu nodes=%zu level_bytes=%zu left_out=%u too_deep=%u "
        "too_many_classes=%u most_work=%llu\n",
        static_cast<unsigned>(lines.Checksum()), static_cast<unsigned>(styler.elements),
        static_cast<unsigned>(lines.Count()), table.Bytes(), table.Nodes(), cascade.LevelBytes(),
        static_cast<unsigned>(table.LeftOutForRoom() + table.LeftOutForNodes()),
        static_cast<unsigned>(cascade.TooDeep()), static_cast<unsigned>(cascade.TooManyClasses()),
        static_cast<unsigned long long>(cascade.MostWork()));
  }
  return 0;
}

// A book, open to have its documents and sheets read: the ZIP file, and
// its entries as a book's files.
class OpenBook {
 public:
  explicit OpenBook(const std::string& path) : storage_(path) {}

  bool Start() {
    return storage_.Start() == StorageStatus::kOk &&
           directory_.Open(storage_, "/zip", bn::core::OpenMode::kRead) == StorageStatus::kOk &&
           entries_.Open(storage_, "/zip", bn::core::OpenMode::kRead) == StorageStatus::kOk &&
           archive_.Open(directory_) == ZipStatus::kOk;
  }

  bn::epub::BookFiles& Files() { return files_; }

  // The names of its documents, in the order the ZIP file has them: false
  // if its directory cannot be read.
  bool Documents(std::vector<std::string>& names) {
    std::vector<char> name(65536);
    bn::epub::ZipArchive::Cursor cursor;
    for (;;) {
      bn::epub::ZipEntry entry;
      const ZipStatus next = archive_.Next(cursor, entry, name);
      if (next == ZipStatus::kEnd) {
        return true;
      }
      if (next != ZipStatus::kOk && next != ZipStatus::kUnsupported) {
        return false;
      }
      const std::string entryName(name.data(), std::min<std::size_t>(entry.nameLength, name.size()));
      std::string lowered = entryName;
      std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                     [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
      if (lowered.ends_with(".xhtml") || lowered.ends_with(".html") || lowered.ends_with(".htm")) {
        names.push_back(entryName);
      }
    }
  }

 private:
  bn::tools::OneFile storage_;
  std::array<std::uint8_t, 4096> directoryBuffer_{};
  std::array<std::uint8_t, 4096> entryBuffer_{};
  bn::core::BlockFile directory_{directoryBuffer_};
  bn::core::BlockFile entries_{entryBuffer_};
  bn::epub::ZipArchive archive_;
  std::vector<std::uint8_t> window_ = std::vector<std::uint8_t>(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader entryReader_{window_};
  bn::epub::ZipBookFiles files_{archive_, entries_, entryReader_};
};

// What finding a document's sheets needs besides the book, and who is
// told of what was left out.
class Finder final : public bn::epub::SheetsListener {
 public:
  explicit Finder(bn::epub::BookFiles& files) : sheets(files, parser, reader, table, names_, style_) {
    sheets.SetListener(this);
    sheets.BeginBook();
  }

  void OnSheetRead(std::string_view /*path*/, std::uint8_t /*sheet*/,
                   const bn::epub::CssReader& /*reader*/) noexcept override {}
  void OnLeftOut(bn::epub::SheetLeftOut what, std::string_view text) noexcept override {
    leftOut.push_back(std::string("X ") + bn::epub::SheetLeftOutName(what) + ": " + Escaped(text));
  }

  // The lines for the document's sheets, in the order of its list.
  std::vector<std::string> SheetLines() const {
    std::vector<std::string> lines;
    const std::span<const std::uint8_t> list = sheets.Sheets();
    for (std::size_t i = 0; i < list.size(); ++i) {
      const bool own = sheets.HasOwnStyle() && i + 1 == list.size();
      lines.push_back("S " + (own ? std::string("<style>") : Escaped(sheets.PathOf(list[i]))));
    }
    return lines;
  }

  std::uint32_t LeftOutInAll() const {
    std::uint32_t all = 0;
    for (std::size_t kind = 0; kind < bn::epub::kSheetLeftOutKinds; ++kind) {
      all += sheets.LeftOut(static_cast<bn::epub::SheetLeftOut>(kind));
    }
    return all;
  }

 private:
  std::vector<std::uint8_t> parserMemory_ = std::vector<std::uint8_t>(kParserBytes);
  std::vector<std::uint8_t> tableMemory_ = std::vector<std::uint8_t>(bn::epub::CssRules::kMaxBytes);
  std::vector<char> names_ = std::vector<char>(bn::epub::DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style_ = std::vector<std::uint8_t>(bn::epub::DocumentSheets::kStyleBytes);

 public:
  bn::epub::XmlParser parser{parserMemory_};
  CssReader reader;
  bn::epub::CssRules table{tableMemory_};
  bn::epub::DocumentSheets sheets;
  // What was left out for the document in hand, a line each.
  std::vector<std::string> leftOut;
};

void PrintLines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
  }
}

int Sheets(const std::string& path, bool printDropped) {
  OpenBook book(path);
  std::vector<std::string> documents;
  if (!book.Start() || !book.Documents(documents)) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Finder finder(book.Files());
  std::uint32_t unopened = 0;
  std::uint64_t given = 0;
  for (const std::string& document : documents) {
    finder.leftOut.clear();
    PrintLines({"D " + Escaped(document)});
    if (finder.sheets.Find(document) != bn::epub::SheetsStatus::kOk) {
      ++unopened;
      continue;
    }
    PrintLines(finder.SheetLines());
    given += finder.sheets.Sheets().size();
    if (printDropped) {
      PrintLines(finder.leftOut);
    }
  }
  // Without the document in hand's own rules: what the book's sheets take.
  finder.sheets.Begin({});
  std::printf(
      "sheets_documents=%zu sheets_unopened=%u sheets_given=%llu sheets_read=%zu sheets_names_bytes=%zu "
      "sheets_left_out=%u\n",
      documents.size(), static_cast<unsigned>(unopened), static_cast<unsigned long long>(given),
      finder.sheets.SheetsRead(), finder.sheets.NamesBytes(), static_cast<unsigned>(finder.LeftOutInAll()));
  // Each kind of thing that was left out, with how many: none, if nothing
  // was.
  std::string kinds = "left_out";
  for (std::size_t kind = 0; kind < bn::epub::kSheetLeftOutKinds; ++kind) {
    const auto what = static_cast<bn::epub::SheetLeftOut>(kind);
    if (finder.sheets.LeftOut(what) == 0) {
      continue;
    }
    std::string name = bn::epub::SheetLeftOutName(what);
    std::replace(name.begin(), name.end(), ' ', '_');
    kinds += " " + name + "=" + std::to_string(finder.sheets.LeftOut(what));
  }
  PrintLines({kinds});
  std::printf(
      "css_table_bytes=%zu css_table_names=%zu css_table_nodes=%zu css_table_rules=%u css_table_selectors=%u "
      "css_table_left_out=%u\n",
      finder.table.Bytes(), finder.table.Names(), finder.table.Nodes(), static_cast<unsigned>(finder.table.Rules()),
      static_cast<unsigned>(finder.table.Selectors()),
      static_cast<unsigned>(finder.table.LeftOutForRoom() + finder.table.LeftOutForNodes()));
  return 0;
}

int Document(const std::string& path, bool sum, const std::string& document) {
  OpenBook book(path);
  if (!book.Start()) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Finder finder(book.Files());
  if (finder.sheets.Find(document) != bn::epub::SheetsStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", document.c_str());
    return 1;
  }
  if (!sum) {
    PrintLines(finder.SheetLines());
    PrintLines(finder.leftOut);
  }

  // The document again, from its start, with the sheets it was given.
  bn::core::ByteSource* source = book.Files().Open(document);
  if (source == nullptr) {
    std::fprintf(stderr, "error: cannot read %s\n", document.c_str());
    return 1;
  }
  std::vector<std::uint8_t> cascadeMemory(kCascadeBytes);
  bn::epub::CssCascade cascade(finder.table, cascadeMemory);
  cascade.Begin(finder.sheets.Sheets());
  Lines lines(!sum);
  Styler styler(cascade, lines);
  const bn::epub::XmlStatus status = finder.parser.Parse(*source, styler);
  if (status != bn::epub::XmlStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s at line %u\n", document.c_str(), bn::epub::XmlStatusName(status),
                 static_cast<unsigned>(finder.parser.Line()));
    return 1;
  }
  if (sum) {
    std::printf(
        "document %08x elements=%u lines=%u sheets=%zu own_style=%u sheets_left_out=%u table_bytes=%zu nodes=%zu "
        "level_bytes=%zu left_out=%u too_deep=%u too_many_classes=%u most_work=%llu\n",
        static_cast<unsigned>(lines.Checksum()), static_cast<unsigned>(styler.elements),
        static_cast<unsigned>(lines.Count()), finder.sheets.Sheets().size(), finder.sheets.HasOwnStyle() ? 1U : 0U,
        static_cast<unsigned>(finder.LeftOutInAll()), finder.table.Bytes(), finder.table.Nodes(), cascade.LevelBytes(),
        static_cast<unsigned>(finder.table.LeftOutForRoom() + finder.table.LeftOutForNodes()),
        static_cast<unsigned>(cascade.TooDeep()), static_cast<unsigned>(cascade.TooManyClasses()),
        static_cast<unsigned long long>(cascade.MostWork()));
  }
  return 0;
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
  if (command == "cascade") {
    std::string book;
    bool sum = false;
    std::vector<std::string> names;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--in" && i + 1 < argc && book.empty()) {
        book = argv[++i];
      } else if (argument == "--sum") {
        sum = true;
      } else {
        names.push_back(argument);
      }
    }
    if (!names.empty()) {
      return Cascade(book, sum, names.front(), {names.begin() + 1, names.end()});
    }
  }
  if (command == "sheets" && (argc == 3 || (argc == 4 && std::strcmp(argv[3], "--dropped") == 0))) {
    return Sheets(argv[2], argc == 4);
  }
  if (command == "document") {
    bool sum = false;
    std::vector<std::string> names;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--sum") {
        sum = true;
      } else {
        names.push_back(argument);
      }
    }
    if (names.size() == 2) {
      return Document(names[0], sum, names[1]);
    }
  }
  PrintUsage(argv[0]);
  return 2;
}
