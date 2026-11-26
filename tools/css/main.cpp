// bn-css: reads CSS with the reader the firmware reads it with, and says
// what it found: for one sheet, every rule; for an EPUB, a line for each
// of its style sheets and a count of what was passed over; for a document
// and the sheets it is given, what the rules come to for each of its
// elements; and for an EPUB's documents, the sheets each asks for, found
// as the firmware finds them, and what was left out in finding them: the
// documents by their names as the ZIP file has them, or those of the
// book's spine in reading order, as a reader goes through a book.
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
#include "epub/document_blocks.h"
#include "epub/document_sheets.h"
#include "epub/package.h"
#include "epub/sheets_digest.h"
#include "epub/style_stack.h"
#include "epub/toc.h"
#include "epub/toc_digest.h"
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
// What `sheets --spine` gives the reader of a book's package: more than
// any book needs.
constexpr std::size_t kPackageBytes = 256UL * 1024UL;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s rules FILE\n"
               "       %s style TEXT\n"
               "       %s family TEXT | --names\n"
               "       %s book FILE [--dropped]\n"
               "       %s cascade [--in BOOK] [--sum] [--styles | --blocks] DOCUMENT SHEET...\n"
               "       %s sheets BOOK [--spine [--by-name]] [--dropped]\n"
               "       %s document [--sum] [--styles | --blocks] BOOK DOCUMENT\n"
               "\n"
               "Reads CSS the way the firmware does.\n"
               "\n"
               "  rules  prints what the reader finds in the style sheet FILE, a thing a line:\n"
               "           {   a rule begins              }   it ends\n"
               "           S a,b,c selector   a selector, after its counts of ids, classes and elements\n"
               "           D property: value  something the rule sets\n"
               "           I url   an @import             X kind: text   something passed over\n"
               "  style  prints the same for TEXT read as a `style` attribute: its declarations\n"
               "  family prints, for TEXT read as a font-family value, each of its names with the\n"
               "         kind of face it is and what said so (`keyword`, `name`), or `-` for a name\n"
               "         that is not known; and then the kind of face the whole value comes to,\n"
               "         which is that of the first name that says. With --names in place of TEXT,\n"
               "         every name that is known, after its kind of face: `nothing` for a word\n"
               "         that says none, and `word` after one that counts only as a whole word\n"
               "  book   reads every style sheet in the EPUB (or other ZIP file) FILE, those whose\n"
               "         names end in .css, and prints for each: how it ended, the checksum\n"
               "         (CRC-32) of what `rules` would print for it, its rules, the declarations\n"
               "         in them, how many things were passed over, and its name; at the end how\n"
               "         many font-family values they have, how many of those a keyword classed, a\n"
               "         known name, or nothing, and how many came to each kind of face; what all\n"
               "         the sheets came to; and then what keeping all their rules in one table\n"
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
               "         and sheets there were, a checksum of all of that, the reads of the file\n"
               "         it took, what was left out by its kind, and what the table of the\n"
               "         book's rules takes\n"
               "  document  finds the sheets of the one DOCUMENT of BOOK in the same way, prints\n"
               "         them and what was left out, and then goes through the document with them\n"
               "         and prints what `cascade` prints\n"
               "\n"
               "  --dropped  with book: after that, a line for each different thing that was\n"
               "             passed over: `dropped`, how many times, its kind, and what it was;\n"
               "             and one for each different font-family value that nothing classed:\n"
               "             `unclassed`, how many times, and its names\n"
               "  --in BOOK  with cascade: DOCUMENT and each SHEET are the names of entries in the\n"
               "             EPUB (or other ZIP file) BOOK\n"
               "             with sheets: under each document, a line for each thing left out:\n"
               "               X kind: text\n"
               "  --spine    with sheets: the documents are those of the book's spine, in reading\n"
               "             order, each opened from where the book's package says it is, as a\n"
               "             reader goes through a book; not every file with a document's name\n"
               "  --by-name  with --spine: each document is found in the ZIP file by its name\n"
               "             all the same, to see what that costs in reads beside going to it\n"
               "  --styles   with cascade or document: print for each element not what the rules\n"
               "             set for it but what it comes to, with what it takes from the element\n"
               "             it is inside and what its element gives it, as amounts:\n"
               "               C display: block     what it is to a page's lines, if not part of one\n"
               "               C size: 1500         its text's size, in thousandths of the reader's\n"
               "               C text: bold italic  how its text is set\n"
               "               C margin-top: 1.5em  a length, as ems of the reader's size, pixels\n"
               "                                    and percentages of the page's width, added\n"
               "             and the rest by their names; `book` after what the book said, and\n"
               "             nothing after what the element is given by being what it is; F for\n"
               "             what its first letter comes to\n"
               "  --blocks   with cascade or document: print not the elements but the blocks of\n"
               "             text the document is made into, as a bundle keeps them:\n"
               "               B heading 2          a block begins: its kind, a heading's level\n"
               "               R space-before: 1em  something of its record, a line for each thing\n"
               "                                    that is not as a block that says nothing has it\n"
               "               S text bold          how the text from here on is set\n"
               "               L   a line ends (br)       P   a picture (img)\n"
               "               F   the floated text the block began with ends\n"
               "               E   the block ends\n"
               "  --sum      with cascade or document: print one line in place of the elements,\n"
               "             with the checksum (CRC-32) of their lines, how many elements and lines\n"
               "             there were, and what the table and the cascade took; with --styles,\n"
               "             also how many languages the document names and how many times each\n"
               "             kind of thing was not taken as written; with --blocks, the blocks\n"
               "             of each kind as well, and what else was counted of them\n"
               "\n"
               "Ends with 0, or 1 if something could not be read.\n",
               program, program, program, program, program, program, program);
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
      if (value.families.empty()) {
        // From a cascade: the kind of face, the names not kept.
        return bn::epub::CssFamilyClassName(value.familyClass);
      }
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
    if (declaration.value.kind == bn::epub::CssValue::Kind::kFamilies) {
      bn::epub::CssFamilyHow how = bn::epub::CssFamilyHow::kNothing;
      const bn::epub::CssFamilyClass familyClass = bn::epub::CssClassOfFamilies(declaration.value.families, how);
      ++families;
      ++familiesOf.at(static_cast<std::size_t>(familyClass));
      ++familiesBy.at(static_cast<std::size_t>(how));
      if (how == bn::epub::CssFamilyHow::kNothing) {
        ++unclassed[Escaped(ValueText(declaration.value))];
      }
    }
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

  // The font-family values told of: how many, how many came to each kind
  // of face, how many were classed in each way (by CssFamilyHow), and the
  // ones that nothing classed, by their names.
  std::uint32_t families = 0;
  std::array<std::uint32_t, bn::epub::kCssFamilyClasses> familiesOf{};
  std::array<std::uint32_t, 3> familiesBy{};
  std::map<std::string, std::uint32_t> unclassed;

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

// So many of `over` to the unit, as a number: 96 of 64 is "1.5".
std::string Part(std::int32_t amount, std::int32_t over) {
  const std::int64_t size = amount < 0 ? -std::int64_t{amount} : amount;
  std::string out = (amount < 0 ? "-" : "") + std::to_string(size / over);
  if (size % over != 0) {
    std::array<char, 12> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%06lld",
                  static_cast<long long>((size % over) * 1'000'000 / over));
    out += fraction.data();
    while (out.back() == '0') {
      out.pop_back();
    }
  }
  return out;
}

// A length as its three amounts, those that are not nothing with a `+`
// between them: "1.5em+40px".
std::string LengthText(const bn::epub::StyleLength& length) {
  std::string out;
  const auto add = [&out](std::int32_t amount, std::int32_t over, const char* unit) {
    if (amount != 0) {
      out += (out.empty() ? "" : "+") + Part(amount, over) + unit;
    }
  };
  add(length.em, 64, "em");
  add(length.px, 8, "px");
  add(length.percent, 64, "%");
  return out.empty() ? "0" : out;
}

// The names of the bits of how text is set, with spaces between.
std::string BitsText(std::uint16_t bits) {
  constexpr std::array<const char*, 7> kNames = {"bold",       "italic", "underline", "line-through",
                                                 "small-caps", "raised", "lowered"};
  std::string out;
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    if (((std::uint32_t{bits} >> i) & 1U) != 0) {
      out += (out.empty() ? "" : " ") + std::string(kNames.at(i));
    }
  }
  return out;
}

const char* Book(bool fromBook) { return fromBook ? " book" : ""; }

// Goes through a document beside its parser, asking the cascade about
// each element and writing down what it says; or, with a stack of
// styles, what the element comes to.
class Styler final : public bn::epub::XmlHandler {
 public:
  Styler(bn::epub::CssCascade& cascade, Lines& lines, bn::epub::StyleStack* stack = nullptr)
      : cascade_(cascade), lines_(lines), stack_(stack) {}

  void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
    bn::epub::CssStyle style;
    bn::epub::CssStyle firstLetter;
    cascade_.Open(name, attributes, style, firstLetter);
    ++elements;
    lines_.Add("< " + Escaped(name));
    if (stack_ == nullptr) {
      Set('P', style);
      Set('F', firstLetter);
      return;
    }
    bn::epub::ElementStyle said;
    stack_->Open(name, attributes, style, firstLetter, said);
    ComesTo(said);
  }
  void OnEndElement(std::string_view /*name*/) noexcept override {
    cascade_.Close();
    if (stack_ != nullptr) {
      stack_->Close();
    }
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

  // How text is set, as lines that begin with `mark`.
  void Text(char mark, const bn::epub::TextStyle& text) {
    const std::string start(1, mark);
    if (text.size != 1000 || text.sizeFromBook) {
      lines_.Add(start + " size: " + std::to_string(text.size) + Book(text.sizeFromBook));
    }
    if (text.bits != 0) {
      lines_.Add(start + " text: " + BitsText(text.bits));
    }
    if (text.fromBook != 0) {
      lines_.Add(start + " text-book: " + BitsText(text.fromBook));
    }
    if (text.family != bn::epub::CssFamilyClass::kUnset || text.familyFromBook) {
      lines_.Add(start + " family: " + bn::epub::CssFamilyClassName(text.family) + Book(text.familyFromBook));
    }
    if (text.language != 0) {
      lines_.Add(start + " language: " + Escaped(stack_->Language(text.language)));
    }
  }

  // What an element comes to, a line for each thing that is not as an
  // element that says nothing and is inside nothing would have it.
  void ComesTo(const bn::epub::ElementStyle& said) {
    using bn::epub::StyleDisplay;
    using bn::epub::StyleLineHeight;
    if (said.display != StyleDisplay::kInline) {
      lines_.Add(std::string("C display: ") + bn::epub::StyleDisplayName(said.display));
    }
    if (said.display == StyleDisplay::kNone) {
      return;
    }
    Text('C', said.text);
    if (said.align != bn::epub::StyleAlign::kUnset || said.alignFromBook) {
      lines_.Add(std::string("C align: ") + bn::epub::StyleAlignName(said.align) + Book(said.alignFromBook));
    }
    if (said.hasIndent) {
      lines_.Add("C indent: " + LengthText(said.indent) + Book(said.indentFromBook));
    }
    if (said.lineHeight.kind == StyleLineHeight::Kind::kMultiple) {
      lines_.Add("C line-height: x" + std::to_string(said.lineHeight.multiple) + Book(said.lineHeightFromBook));
    } else if (said.lineHeight.kind == StyleLineHeight::Kind::kLength) {
      lines_.Add("C line-height: " + LengthText(said.lineHeight.length) + Book(said.lineHeightFromBook));
    } else if (said.lineHeightFromBook) {
      lines_.Add("C line-height: normal book");
    }
    if (said.display == StyleDisplay::kListItem) {
      lines_.Add(std::string("C marker: ") + bn::epub::StyleMarkerName(said.marker) + Book(said.markerFromBook));
    }
    if (said.rightToLeft) {
      lines_.Add("C right-to-left");
    }
    if (said.spacesAsWritten) {
      lines_.Add("C spaces-as-written");
    }
    constexpr std::array<const char*, 4> kSides = {"top", "right", "bottom", "left"};
    for (const bool margin : {true, false}) {
      for (std::size_t side = 0; side < kSides.size(); ++side) {
        const bn::epub::StyleLength& length = margin ? said.margin.at(side) : said.padding.at(side);
        const std::uint32_t fromBook = margin ? said.marginFromBook : said.paddingFromBook;
        const bool book = ((fromBook >> side) & 1U) != 0;
        const bool isAuto = margin && ((side == bn::epub::kStyleLeft && said.marginLeftAuto) ||
                                       (side == bn::epub::kStyleRight && said.marginRightAuto));
        if (!length.IsZero() || book) {
          lines_.Add(std::string("C ") + (margin ? "margin-" : "padding-") + kSides.at(side) + ": " +
                     (isAuto ? std::string("auto") : LengthText(length)) + Book(book));
        }
      }
    }
    if (said.hasWidth) {
      lines_.Add("C width: " + LengthText(said.width));
    }
    if (said.hasHeight) {
      lines_.Add("C height: " + LengthText(said.height));
    }
    if (said.floated != bn::epub::StyleFloat::kNone) {
      lines_.Add(std::string("C float: ") + (said.floated == bn::epub::StyleFloat::kLeft ? "left" : "right"));
    }
    if (said.breakBefore) {
      lines_.Add("C break-before");
    }
    if (said.breakAfter) {
      lines_.Add("C break-after");
    }
    if (said.keepWithNext || said.keepWithNextFromBook) {
      lines_.Add(std::string(said.keepWithNext ? "C keep-with-next" : "C keep-with-next: no") +
                 Book(said.keepWithNextFromBook));
    }
    if (said.keepTogether) {
      lines_.Add("C keep-together");
    }
    if (said.hasFirstLetter) {
      lines_.Add(said.firstLetterFloated ? "F first-letter: floated" : "F first-letter");
      Text('F', said.firstLetter);
    }
  }

  bn::epub::CssCascade& cascade_;
  Lines& lines_;
  bn::epub::StyleStack* stack_;
};

// The memory a stack of styles is given: the languages' tags, and 32
// elements open at once.
constexpr std::size_t kStyleBytes = bn::epub::StyleStack::kLanguagesBytes + (32 * bn::epub::StyleStack::kLevelBytes);

// What a stack of styles did not take as written, as words for the line
// that sums a document up.
std::string StylesLeftOut(const bn::epub::StyleStack& stack) {
  constexpr std::array<const char*, bn::epub::kStyleLeftOutKinds> kNames = {
      "styles_too_deep", "page_margins", "inherits_of_own", "languages_left_out", "breaks_avoided_before", "not_shown"};
  std::string out = " languages=" + std::to_string(stack.Languages());
  for (std::size_t kind = 0; kind < kNames.size(); ++kind) {
    out += std::string(" ") + kNames.at(kind) + "=" +
           std::to_string(stack.LeftOut(static_cast<bn::epub::StyleLeftOut>(kind)));
  }
  return out;
}

// The memory a document's blocks are found in: the languages' tags, and
// 32 elements open at once.
constexpr std::size_t kBlockBytes =
    bn::epub::StyleStack::kLanguagesBytes + (32 * bn::epub::DocumentBlocks::kLevelBytes);

// How text is set, as its parts with "; " between them; "plain" for text
// that nothing has said anything of.
std::string StyleText(const bn::epub::TextStyle& text, const bn::epub::StyleStack& stack) {
  std::string out;
  const auto add = [&out](const std::string& part) { out += (out.empty() ? "" : "; ") + part; };
  if (text.size != 1000 || text.sizeFromBook) {
    add("size " + std::to_string(text.size) + Book(text.sizeFromBook));
  }
  if (text.bits != 0) {
    add("text " + BitsText(text.bits));
  }
  if (text.fromBook != 0) {
    add("text-book " + BitsText(text.fromBook));
  }
  if (text.family != bn::epub::CssFamilyClass::kUnset || text.familyFromBook) {
    add(std::string("family ") + bn::epub::CssFamilyClassName(text.family) + Book(text.familyFromBook));
  }
  if (text.language != 0) {
    add("language " + Escaped(stack.Language(text.language)));
  }
  return out.empty() ? "plain" : out;
}

// Goes through a document beside its parser, asking the cascade about
// each element and writing down the blocks the document is made into.
class Blocker final : public bn::epub::XmlHandler {
 public:
  Blocker(bn::epub::CssCascade& cascade, Lines& lines, bn::epub::DocumentBlocks& finder)
      : cascade_(cascade), lines_(lines), blocks_(finder) {}

  void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
    bn::epub::CssStyle style;
    bn::epub::CssStyle firstLetter;
    cascade_.Open(name, attributes, style, firstLetter);
    ++elements;
    bn::epub::ElementStyle said;
    Tell(blocks_.Open(name, attributes, style, firstLetter, said), 0, attributes.Count(), 0);
    if (name == "img" || name == "IMG") {
      Tell(blocks_.Picture(), 'P', 0, 0);
    }
  }
  void OnEndElement(std::string_view /*name*/) noexcept override {
    cascade_.Close();
    Tell(blocks_.Close(), 0, 0, 0);
  }
  void OnText(std::string_view text) noexcept override { Tell(blocks_.Text(text), 'T', 0, text.size()); }
  // A character, whichever it is.
  void OnEntity(std::string_view /*name*/) noexcept override { Tell(blocks_.Text("&"), 'T', 0, 1); }

  // What was counted, as words for the line that sums a document up.
  std::string Counts() const {
    std::string out = " blocks=" + std::to_string(blocks);
    constexpr std::array<const char*, bn::epub::kBlockKinds> kKinds = {"paragraphs", "headings", "list_items", "rules",
                                                                       "cells"};
    for (std::size_t kind = 0; kind < kKinds.size(); ++kind) {
      out += std::string(" ") + kKinds.at(kind) + "=" + std::to_string(kinds.at(kind));
    }
    out += " line_ends=" + std::to_string(lineEnds) + " pictures=" + std::to_string(pictures) +
           " markers=" + std::to_string(markers) + " first_letters=" + std::to_string(firstLetters) +
           " floated_letters=" + std::to_string(floatedLetters) + " begin_floated=" + std::to_string(beginFloated) +
           " floated=" + std::to_string(floated) + " centred=" + std::to_string(centred) +
           " blocks_most_work=" + std::to_string(mostWork) + " work_over=" + std::to_string(workOver);
    return out;
  }

  std::uint32_t elements = 0;
  std::uint32_t blocks = 0;
  std::array<std::uint32_t, bn::epub::kBlockKinds> kinds{};
  std::uint32_t lineEnds = 0;
  std::uint32_t pictures = 0;
  std::uint32_t markers = 0;
  std::uint32_t firstLetters = 0;
  std::uint32_t floatedLetters = 0;
  std::uint32_t beginFloated = 0;
  std::uint32_t floated = 0;
  std::uint32_t centred = 0;
  std::uint32_t mostWork = 0;
  std::uint32_t workOver = 0;  // Calls that did more work than the most they may.

 private:
  // What a call made of the blocks; `content` is T for text, P for a
  // picture, and nothing for an element.
  void Tell(const bn::epub::BlockChange& change, char content, std::size_t attributes, std::size_t textBytes) {
    mostWork = std::max(mostWork, blocks_.Work());
    workOver += blocks_.Work() > bn::epub::DocumentBlocks::MostWorkFor(attributes, textBytes, blocks_.Levels()) ? 1 : 0;
    if (change.ends) {
      lines_.Add("E");
    }
    if (change.begins) {
      Begins(blocks_.Record());
      styled_ = false;
    }
    if (change.lineEnds) {
      lines_.Add("L");
      ++lineEnds;
    }
    if (change.floatEnds) {
      lines_.Add("F");
    }
    if (content == 'P' && change.shown) {
      lines_.Add("P");
      ++pictures;
    }
    if (content == 'T' && change.shown) {
      const bn::epub::TextStyle style = blocks_.TextNow();
      if (!styled_ || !(style == style_)) {
        lines_.Add("S " + StyleText(style, blocks_.Styles()));
        style_ = style;
        styled_ = true;
      }
    }
  }

  // A block begins: its kind, and a line for each thing of its record
  // that is not as a block that says nothing has it.
  void Begins(const bn::epub::BlockRecord& record) {
    using bn::epub::StyleLineHeight;
    ++blocks;
    ++kinds.at(static_cast<std::size_t>(record.kind));
    lines_.Add(std::string("B ") + bn::epub::BlockKindName(record.kind) +
               (record.headingLevel != 0 ? " " + std::to_string(record.headingLevel) : ""));
    if (record.align != bn::epub::StyleAlign::kUnset || record.alignFromBook) {
      lines_.Add(std::string("R align: ") + bn::epub::StyleAlignName(record.align) + Book(record.alignFromBook));
    }
    if (record.rightToLeft) {
      lines_.Add("R right-to-left");
    }
    if (record.spacesAsWritten) {
      lines_.Add("R spaces-as-written");
    }
    if (record.size != 1000 || record.sizeFromBook) {
      lines_.Add("R size: " + std::to_string(record.size) + Book(record.sizeFromBook));
    }
    if (record.lineHeight.kind == StyleLineHeight::Kind::kMultiple) {
      lines_.Add("R line-height: x" + std::to_string(record.lineHeight.multiple) + Book(record.lineHeightFromBook));
    } else if (record.lineHeight.kind == StyleLineHeight::Kind::kLength) {
      lines_.Add("R line-height: " + LengthText(record.lineHeight.length) + Book(record.lineHeightFromBook));
    } else if (record.lineHeightFromBook) {
      lines_.Add("R line-height: normal book");
    }
    if (record.hasIndent) {
      lines_.Add("R indent: " + LengthText(record.indent) + Book(record.indentFromBook));
    }
    if (!record.marginLeft.IsZero() || record.marginLeftFromBook) {
      lines_.Add("R margin-left: " + LengthText(record.marginLeft) + Book(record.marginLeftFromBook));
    }
    if (!record.marginRight.IsZero() || record.marginRightFromBook) {
      lines_.Add("R margin-right: " + LengthText(record.marginRight) + Book(record.marginRightFromBook));
    }
    if (!record.spaceBefore.IsZero() || record.spaceBeforeFromBook) {
      lines_.Add("R space-before: " + LengthText(record.spaceBefore) + Book(record.spaceBeforeFromBook));
    }
    if (record.breakBefore) {
      lines_.Add("R break-before");
    }
    if (record.keepWithNext || record.keepWithNextFromBook) {
      lines_.Add(std::string(record.keepWithNext ? "R keep-with-next" : "R keep-with-next: no") +
                 Book(record.keepWithNextFromBook));
    }
    if (record.keepTogether) {
      lines_.Add("R keep-together");
    }
    if (record.floated) {
      lines_.Add("R floated");
      ++floated;
    }
    if (record.centred) {
      lines_.Add("R centred: " + LengthText(record.width));
      ++centred;
    }
    if (record.quoteDepth != 0) {
      lines_.Add("R quote-depth: " + std::to_string(record.quoteDepth));
    }
    if (record.listDepth != 0) {
      lines_.Add("R list-depth: " + std::to_string(record.listDepth));
    }
    if (record.hasMarker) {
      lines_.Add(std::string("R marker: ") + bn::epub::StyleMarkerName(record.marker) + " " +
                 std::to_string(record.number) + Book(record.markerFromBook));
      ++markers;
    }
    if (record.hasFirstLetter) {
      lines_.Add(std::string(record.firstLetterFloated ? "R first-letter floated: " : "R first-letter: ") +
                 StyleText(record.firstLetter, blocks_.Styles()));
      ++firstLetters;
      floatedLetters += record.firstLetterFloated ? 1 : 0;
    }
    if (record.beginsFloated) {
      lines_.Add("R begins-floated");
      ++beginFloated;
    }
  }

  bn::epub::CssCascade& cascade_;
  Lines& lines_;
  bn::epub::DocumentBlocks& blocks_;
  bool styled_ = false;  // A style has been told of since the block began.
  bn::epub::TextStyle style_;
};

// What of a document `cascade` and `document` print.
enum class Show : std::uint8_t { kRules, kStyles, kBlocks };

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

const char* HowName(bn::epub::CssFamilyHow how) {
  switch (how) {
    case bn::epub::CssFamilyHow::kNothing:
      return "-";
    case bn::epub::CssFamilyHow::kKeyword:
      return "keyword";
    case bn::epub::CssFamilyHow::kName:
      return "name";
  }
  return "?";
}

// The kind of face each name of a font-family value is, and the kind the
// value comes to.
int Family(std::string_view text) {
  std::array<char, bn::epub::kCssMostFamilyName> name{};
  std::string_view rest = text;
  for (std::string_view one = bn::epub::CssNextFamily(rest, name); !one.empty();
       one = bn::epub::CssNextFamily(rest, name)) {
    bn::epub::CssFamilyHow how = bn::epub::CssFamilyHow::kNothing;
    const bn::epub::CssFamilyClass familyClass = bn::epub::CssClassOfFamily(one, how);
    const std::string shown = Escaped(one);
    std::printf("N %s: %s%s%s\n", shown.c_str(),
                how == bn::epub::CssFamilyHow::kNothing ? "" : bn::epub::CssFamilyClassName(familyClass),
                how == bn::epub::CssFamilyHow::kNothing ? "" : " ", HowName(how));
  }
  std::printf("class %s\n", bn::epub::CssFamilyClassName(bn::epub::CssClassOfFamilies(text)));
  return 0;
}

// Every name that is known, with its kind of face.
int FamilyNames() {
  for (const bn::epub::CssKnownFamily& known : bn::epub::CssKnownFamilies()) {
    const std::string name(known.name);
    std::printf("%s %s%s\n",
                known.familyClass == bn::epub::CssFamilyClass::kUnset ? "nothing"
                                                                      : bn::epub::CssFamilyClassName(known.familyClass),
                name.c_str(), known.wholeWord ? " word" : "");
  }
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
  const auto of = [&writer](bn::epub::CssFamilyClass familyClass) {
    return static_cast<unsigned>(writer.familiesOf.at(static_cast<std::size_t>(familyClass)));
  };
  const auto by = [&writer](bn::epub::CssFamilyHow how) {
    return static_cast<unsigned>(writer.familiesBy.at(static_cast<std::size_t>(how)));
  };
  std::printf(
      "css_families=%u css_families_keyword=%u css_families_name=%u css_families_unclassed=%u "
      "css_families_serif=%u css_families_sans=%u css_families_mono=%u css_families_script=%u css_families_other=%u\n",
      static_cast<unsigned>(writer.families), by(bn::epub::CssFamilyHow::kKeyword), by(bn::epub::CssFamilyHow::kName),
      by(bn::epub::CssFamilyHow::kNothing), of(bn::epub::CssFamilyClass::kSerif), of(bn::epub::CssFamilyClass::kSans),
      of(bn::epub::CssFamilyClass::kMono), of(bn::epub::CssFamilyClass::kScript), of(bn::epub::CssFamilyClass::kOther));
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
    for (const auto& [names, count] : writer.unclassed) {
      std::printf("unclassed %u family: ", static_cast<unsigned>(count));
      std::fwrite(names.data(), 1, names.size(), stdout);
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

int Cascade(const std::string& book, bool sum, Show show, const std::string& document,
            const std::vector<std::string>& sheets) {
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
  std::vector<std::uint8_t> styleMemory(kStyleBytes);
  bn::epub::StyleStack stack(styleMemory);
  stack.Begin();
  Styler styler(cascade, lines, show == Show::kStyles ? &stack : nullptr);
  std::vector<std::uint8_t> blockMemory(kBlockBytes);
  bn::epub::DocumentBlocks blocks(blockMemory);
  blocks.Begin();
  Blocker blocker(cascade, lines, blocks);
  std::vector<std::uint8_t> parserMemory(kParserBytes);
  bn::epub::XmlParser parser(parserMemory);
  const bn::epub::XmlStatus status =
      show == Show::kBlocks ? parser.Parse(*source, blocker) : parser.Parse(*source, styler);
  if (status != bn::epub::XmlStatus::kOk || !reading.Sound()) {
    std::fprintf(stderr, "error: %s: %s at line %u\n", document.c_str(), bn::epub::XmlStatusName(status),
                 static_cast<unsigned>(parser.Line()));
    return 1;
  }
  if (sum) {
    std::printf(
        "%s %08x elements=%u lines=%u table_bytes=%zu nodes=%zu level_bytes=%zu left_out=%u too_deep=%u "
        "too_many_classes=%u most_work=%llu%s\n",
        show == Show::kBlocks   ? "blocks"
        : show == Show::kStyles ? "styles"
                                : "cascade",
        static_cast<unsigned>(lines.Checksum()), static_cast<unsigned>(styler.elements + blocker.elements),
        static_cast<unsigned>(lines.Count()), table.Bytes(), table.Nodes(), cascade.LevelBytes(),
        static_cast<unsigned>(table.LeftOutForRoom() + table.LeftOutForNodes()),
        static_cast<unsigned>(cascade.TooDeep()), static_cast<unsigned>(cascade.TooManyClasses()),
        static_cast<unsigned long long>(cascade.MostWork()),
        show == Show::kBlocks   ? (blocker.Counts() + StylesLeftOut(blocks.Styles())).c_str()
        : show == Show::kStyles ? StylesLeftOut(stack).c_str()
                                : "");
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

  bn::epub::ZipBookFiles& Files() { return files_; }
  // What it is read through, for whoever reads its package.
  bn::epub::ZipArchive& Archive() { return archive_; }
  bn::core::BlockFile& Entries() { return entries_; }
  bn::epub::ZipEntryReader& Reader() { return entryReader_; }
  // The reads of the file so far, and what they brought.
  std::uint32_t Reads() const { return storage_.Reads(); }
  std::uint64_t ReadBytes() const { return storage_.ReadBytes(); }

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

// Which documents `sheets` goes through, and how each is come to.
enum class Through : std::uint8_t {
  kNames,        // Every file with a document's name, in the ZIP file's order, each found by its name.
  kSpine,        // The spine in reading order, each opened from where the package says it is.
  kSpineByName,  // The spine in reading order, each found by its name.
};

int Sheets(const std::string& path, bool printDropped, Through through) {
  OpenBook book(path);
  if (!book.Start()) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Finder finder(book.Files());

  // The documents: their names, and for a spine where each one is.
  std::vector<std::string> documents;
  std::vector<bn::epub::ZipEntry> places;
  std::uint32_t absent = 0;
  if (through == Through::kNames) {
    if (!book.Documents(documents)) {
      std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
      return 1;
    }
  } else {
    std::vector<std::uint8_t> packageMemory(kPackageBytes);
    bn::epub::Package package(packageMemory);
    const bn::epub::PackageStatus read = package.Read(book.Archive(), book.Entries(), book.Reader(), finder.parser);
    if (read != bn::epub::PackageStatus::kOk) {
      std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::PackageStatusName(read));
      return 1;
    }
    for (const bn::epub::SpineItem& item : package.Spine()) {
      if (item.present) {
        places.push_back(item.entry);
      } else {
        ++absent;  // The package names a document the book has not got.
      }
    }
    // Its table of contents next, as a reader that opens a book reads it,
    // and as the book check does before it finds any sheets: what the
    // file's buffers hold when the finding begins is then what they hold
    // there, and the reads that follow are the same reads.
    bn::epub::TocReader toc;
    bn::epub::TocDigest contents;
    static_cast<void>(toc.Read(package, book.Entries(), book.Reader(), finder.parser, contents));
  }

  // What follows is the finding itself: its reads are counted by themselves.
  const std::uint32_t readsBefore = book.Reads();
  const std::uint64_t readBytesBefore = book.ReadBytes();
  bn::epub::SheetsDigest digest;
  // A name longer than a path in a book may be is no document that can be
  // read: as much of it as there is room for stands for it.
  std::vector<char> name(bn::epub::kMaxBookPath);
  const std::size_t count = through == Through::kNames ? documents.size() : places.size();
  for (std::size_t index = 0; index < count; ++index) {
    finder.leftOut.clear();
    std::string document;
    bool opened = true;
    if (through == Through::kNames) {
      document = documents[index];
      static_cast<void>(finder.sheets.Find(document));
    } else {
      std::size_t length = 0;
      if (through == Through::kSpine) {
        // Opened from its place, as the book check opens it, with its name
        // read where its reading begins.
        bn::core::ByteSource* const source = book.Files().OpenAt(places[index], name, length);
        document = std::string(name.data(), length);
        opened = source != nullptr;
        if (opened) {
          static_cast<void>(finder.sheets.Find(document, *source));
        }
      } else {
        // Its name read from its place, and then the document found by it.
        const ZipStatus named = bn::epub::ReadZipEntryName(book.Entries(), places[index], name, length);
        opened = named == ZipStatus::kOk && length <= name.size();
        document = named == ZipStatus::kOk ? std::string(name.data(), std::min(length, name.size())) : std::string();
        if (opened) {
          static_cast<void>(finder.sheets.Find(document));
        }
      }
    }
    PrintLines({"D " + Escaped(document)});
    if (!opened) {
      digest.AddUnopened(document);
      continue;
    }
    digest.Add(document, finder.sheets);
    if (finder.sheets.Status() != bn::epub::SheetsStatus::kOk) {
      continue;
    }
    PrintLines(finder.SheetLines());
    if (printDropped) {
      PrintLines(finder.leftOut);
    }
  }
  // Without the document in hand's own rules: what the book's sheets take.
  finder.sheets.Begin({});
  std::printf(
      "sheets_documents=%u sheets_unopened=%u sheets_given=%llu sheets_read=%zu sheets_names_bytes=%zu "
      "sheets_left_out=%u sheets_own=%u sheets_absent=%u sheets_checksum=%08x\n",
      static_cast<unsigned>(digest.Documents()), static_cast<unsigned>(digest.Unopened()),
      static_cast<unsigned long long>(digest.Given()), finder.sheets.SheetsRead(), finder.sheets.NamesBytes(),
      static_cast<unsigned>(finder.LeftOutInAll()), static_cast<unsigned>(digest.WithOwnStyle()),
      static_cast<unsigned>(absent), static_cast<unsigned>(digest.Checksum()));
  std::printf("sheets_reads=%u sheets_read_bytes=%llu\n", static_cast<unsigned>(book.Reads() - readsBefore),
              static_cast<unsigned long long>(book.ReadBytes() - readBytesBefore));
  // Each kind of thing that was left out, with how many: none, if nothing
  // was.
  std::string kinds = "left_out";
  for (std::size_t kind = 0; kind < bn::epub::kSheetLeftOutKinds; ++kind) {
    const auto what = static_cast<bn::epub::SheetLeftOut>(kind);
    if (finder.sheets.LeftOut(what) == 0) {
      continue;
    }
    std::string kindName = bn::epub::SheetLeftOutName(what);
    std::replace(kindName.begin(), kindName.end(), ' ', '_');
    kinds += " " + kindName + "=" + std::to_string(finder.sheets.LeftOut(what));
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

int Document(const std::string& path, bool sum, Show show, const std::string& document) {
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
  std::vector<std::uint8_t> styleMemory(kStyleBytes);
  bn::epub::StyleStack stack(styleMemory);
  stack.Begin();
  Styler styler(cascade, lines, show == Show::kStyles ? &stack : nullptr);
  std::vector<std::uint8_t> blockMemory(kBlockBytes);
  bn::epub::DocumentBlocks blocks(blockMemory);
  blocks.Begin();
  Blocker blocker(cascade, lines, blocks);
  const bn::epub::XmlStatus status =
      show == Show::kBlocks ? finder.parser.Parse(*source, blocker) : finder.parser.Parse(*source, styler);
  if (status != bn::epub::XmlStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s at line %u\n", document.c_str(), bn::epub::XmlStatusName(status),
                 static_cast<unsigned>(finder.parser.Line()));
    return 1;
  }
  if (sum) {
    std::printf(
        "%s %08x elements=%u lines=%u sheets=%zu own_style=%u sheets_left_out=%u table_bytes=%zu nodes=%zu "
        "level_bytes=%zu left_out=%u too_deep=%u too_many_classes=%u most_work=%llu%s\n",
        show == Show::kBlocks   ? "blocks"
        : show == Show::kStyles ? "styles"
                                : "document",
        static_cast<unsigned>(lines.Checksum()), static_cast<unsigned>(styler.elements + blocker.elements),
        static_cast<unsigned>(lines.Count()), finder.sheets.Sheets().size(), finder.sheets.HasOwnStyle() ? 1U : 0U,
        static_cast<unsigned>(finder.LeftOutInAll()), finder.table.Bytes(), finder.table.Nodes(), cascade.LevelBytes(),
        static_cast<unsigned>(finder.table.LeftOutForRoom() + finder.table.LeftOutForNodes()),
        static_cast<unsigned>(cascade.TooDeep()), static_cast<unsigned>(cascade.TooManyClasses()),
        static_cast<unsigned long long>(cascade.MostWork()),
        show == Show::kBlocks   ? (blocker.Counts() + StylesLeftOut(blocks.Styles())).c_str()
        : show == Show::kStyles ? StylesLeftOut(stack).c_str()
                                : "");
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
  if (command == "family" && argc == 3) {
    return std::strcmp(argv[2], "--names") == 0 ? FamilyNames() : Family(argv[2]);
  }
  if (command == "book" && (argc == 3 || (argc == 4 && std::strcmp(argv[3], "--dropped") == 0))) {
    return Book(argv[2], argc == 4);
  }
  if (command == "cascade") {
    std::string book;
    bool sum = false;
    Show show = Show::kRules;
    std::vector<std::string> names;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--in" && i + 1 < argc && book.empty()) {
        book = argv[++i];
      } else if (argument == "--sum") {
        sum = true;
      } else if (argument == "--styles" && show == Show::kRules) {
        show = Show::kStyles;
      } else if (argument == "--blocks" && show == Show::kRules) {
        show = Show::kBlocks;
      } else {
        names.push_back(argument);
      }
    }
    if (!names.empty()) {
      return Cascade(book, sum, show, names.front(), {names.begin() + 1, names.end()});
    }
  }
  if (command == "sheets" && argc >= 3) {
    bool dropped = false;
    bool spine = false;
    bool byName = false;
    bool known = true;
    for (int i = 3; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--dropped") {
        dropped = true;
      } else if (argument == "--spine") {
        spine = true;
      } else if (argument == "--by-name") {
        byName = true;
      } else {
        known = false;
      }
    }
    if (known && (spine || !byName)) {
      return Sheets(argv[2], dropped, !spine ? Through::kNames : byName ? Through::kSpineByName : Through::kSpine);
    }
  }
  if (command == "document") {
    bool sum = false;
    Show show = Show::kRules;
    std::vector<std::string> names;
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--sum") {
        sum = true;
      } else if (argument == "--styles" && show == Show::kRules) {
        show = Show::kStyles;
      } else if (argument == "--blocks" && show == Show::kRules) {
        show = Show::kBlocks;
      } else {
        names.push_back(argument);
      }
    }
    if (names.size() == 2) {
      return Document(names[0], sum, show, names[1]);
    }
  }
  PrintUsage(argv[0]);
  return 2;
}
