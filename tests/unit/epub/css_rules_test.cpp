#include "epub/css_rules.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "css_reference.h"
#include "epub/css.h"
#include "epub/xml.h"

namespace {

using bn::epub::CssCascade;
using bn::epub::CssProperty;
using bn::epub::CssReader;
using bn::epub::CssRules;
using bn::epub::CssStatus;
using bn::epub::CssStyle;
using bn::epub::CssValue;

std::span<const std::uint8_t> Bytes(std::string_view text) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string Number(std::int32_t thousandths) {
  const std::int64_t size = thousandths < 0 ? -std::int64_t{thousandths} : thousandths;
  std::string out = (thousandths < 0 ? "-" : "") + std::to_string(size / 1000);
  if (size % 1000 != 0) {
    std::string fraction = std::to_string(1000 + (size % 1000)).substr(1);
    while (fraction.back() == '0') {
      fraction.pop_back();
    }
    out += "." + fraction;
  }
  return out;
}

// What a style sets, as "property: value; property: value", in the order
// of the properties.
std::string StyleText(const CssStyle& style) {
  std::string out;
  for (std::size_t i = 0; i < bn::epub::kCssProperties; ++i) {
    const auto property = static_cast<CssProperty>(i);
    if (!style.Has(property)) {
      continue;
    }
    const CssValue& value = style[property];
    std::string text;
    if (value.kind == CssValue::Kind::kKeyword) {
      text = bn::epub::CssKeywordName(value.keyword);
    } else if (value.kind == CssValue::Kind::kLength) {
      text = Number(value.number) + bn::epub::CssUnitName(value.unit);
    } else {
      // The kind of face; and names, which a cascade never gives, would
      // show beside it.
      text = "{" + std::string(bn::epub::CssFamilyClassName(value.familyClass)) + "}" + std::string(value.families);
    }
    out += (out.empty() ? "" : "; ") + std::string(bn::epub::CssPropertyName(property)) + ": " + text;
  }
  return out;
}

// A table of rules, and beside it the plain working-out of the same
// rules, both told of the same sheets.
class Book {
 public:
  explicit Book(std::size_t bytes = 65536) : memory_(bytes), rules(memory_) {}

  // Adds a sheet to both, and returns its number.
  std::uint8_t Sheet(std::string_view text) {
    std::uint8_t sheet = 0xFF;
    EXPECT_TRUE(rules.BeginSheet(sheet));
    bn::core::MemoryByteSource source(Bytes(text));
    CssReader reader;
    EXPECT_EQ(reader.Read(source, rules), CssStatus::kOk);
    rules.EndSheet();

    reference.BeginSheet();
    bn::core::MemoryByteSource again(Bytes(text));
    EXPECT_EQ(reader.Read(again, reference), CssStatus::kOk);
    reference.EndSheet();
    return sheet;
  }

  // Whether the table has every rule it was told of, so that the two can
  // be set side by side.
  bool Whole() const { return rules.LeftOutForRoom() == 0 && rules.LeftOutForNodes() == 0; }

 private:
  std::vector<std::uint8_t> memory_;

 public:
  CssRules rules;
  bn::test::CssReference reference;
};

// What going through a document came to.
struct Walked {
  // A line for each element: a space for each element it is inside, its
  // name, then what was set for it in braces, then what was set for its
  // first letter after "first-letter", each only if anything was.
  std::string lines;
  std::uint32_t tooDeep = 0;
  std::uint32_t tooManyClasses = 0;
  std::uint64_t mostWork = 0;
  std::uint64_t mostAllowed = 0;  // The most that MostWorkFor() allowed any element.
  std::size_t levelBytes = 0;
  bool agreed = true;  // The plain working-out said the same of every element.
};

// Goes through `document` with a cascade over the book's rules, given
// `sheets`, and with the plain working-out beside it.
Walked Walk(Book& book, const std::vector<std::uint8_t>& sheets, std::string_view document,
            std::size_t cascadeBytes = 4096) {
  class Walker final : public bn::epub::XmlHandler {
   public:
    Walker(Book& book, CssCascade& cascade, Walked& walked) : book_(book), cascade_(cascade), walked_(walked) {}

    void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
      const std::size_t depth = cascade_.Depth();
      CssStyle style;
      CssStyle firstLetter;
      cascade_.Open(name, attributes, style, firstLetter);
      walked_.mostAllowed =
          std::max(walked_.mostAllowed, CssCascade::MostWorkFor(book_.rules.Names(), book_.rules.Nodes(),
                                                                book_.rules.Selectors(), attributes.Count(), depth));
      EXPECT_LE(cascade_.Work(), CssCascade::MostWorkFor(book_.rules.Names(), book_.rules.Nodes(),
                                                         book_.rules.Selectors(), attributes.Count(), depth));

      std::vector<std::pair<std::string, std::string>> pairs;
      pairs.reserve(attributes.Count());
      for (std::size_t i = 0; i < attributes.Count(); ++i) {
        pairs.emplace_back(attributes.Name(i), attributes.Value(i));
      }
      CssStyle plain;
      CssStyle plainFirstLetter;
      book_.reference.Open(name, pairs, plain, plainFirstLetter);
      // The two can only be set side by side if the table has every rule.
      if (book_.Whole() && (!bn::test::CssReference::Same(style, plain) ||
                            !bn::test::CssReference::Same(firstLetter, plainFirstLetter))) {
        walked_.agreed = false;
        ADD_FAILURE() << "for <" << name << "> at depth " << depth << " the cascade has {" << StyleText(style)
                      << "} first-letter {" << StyleText(firstLetter) << "}, and the plain working-out {"
                      << StyleText(plain) << "} first-letter {" << StyleText(plainFirstLetter) << "}";
      }

      walked_.lines += std::string(depth, ' ') + std::string(name);
      if (style.set != 0) {
        walked_.lines += " {" + StyleText(style) + "}";
      }
      if (firstLetter.set != 0) {
        walked_.lines += " first-letter {" + StyleText(firstLetter) + "}";
      }
      walked_.lines += '\n';
    }
    void OnEndElement(std::string_view /*name*/) noexcept override {
      cascade_.Close();
      book_.reference.Close();
    }
    void OnText(std::string_view /*text*/) noexcept override {}
    void OnEntity(std::string_view /*name*/) noexcept override {}

   private:
    Book& book_;
    CssCascade& cascade_;
    Walked& walked_;
  };

  Walked walked;
  std::vector<std::uint8_t> cascadeMemory(cascadeBytes);
  CssCascade cascade(book.rules, cascadeMemory);
  cascade.Begin(sheets);
  book.reference.Begin(sheets, cascade.Levels());
  walked.levelBytes = cascade.LevelBytes();

  std::vector<std::uint8_t> parserMemory(65536);
  bn::epub::XmlParser parser(parserMemory);
  bn::core::MemoryByteSource source(Bytes(document));
  Walker walker(book, cascade, walked);
  EXPECT_EQ(parser.Parse(source, walker), bn::epub::XmlStatus::kOk) << document;
  EXPECT_EQ(cascade.Depth(), 0U);
  walked.tooDeep = cascade.TooDeep();
  walked.tooManyClasses = cascade.TooManyClasses();
  walked.mostWork = cascade.MostWork();
  return walked;
}

// What one sheet comes to for a document.
std::string Styles(std::string_view sheet, std::string_view document) {
  Book book;
  const std::uint8_t number = book.Sheet(sheet);
  EXPECT_TRUE(book.Whole());
  return Walk(book, {number}, document).lines;
}

TEST(CssCascade, GivesAnElementWhatTheRulesThatAreTrueOfItSet) {
  EXPECT_EQ(Styles("p { margin-top: 1em } .note { text-indent: 0 } #x { float: left } div { display: block }",
                   "<div><p class='note' id='x'/><p/><span class='note'>text</span></div>"),
            "div {display: block}\n"
            " p {text-indent: 0px; margin-top: 1em; float: left}\n"
            " p {margin-top: 1em}\n"
            " span {text-indent: 0px}\n");
  // Every part of a selector's last part has to be true.
  EXPECT_EQ(Styles("p.a.b#x { float: left }",
                   "<d><p class='a b' id='x'/><p class='b a c' id='x'/><p class='a' id='x'/><p class='a b'/>"
                   "<q class='a b' id='x'/><p class='a b' id='y'/></d>"),
            "d\n p {float: left}\n p {float: left}\n p\n p\n q\n p\n");
  // With no rules, and with none that are true of anything, nothing is set.
  EXPECT_EQ(Styles("", "<a><b/></a>"), "a\n b\n");
  EXPECT_EQ(Styles("p { margin: 0 }", "<a><b/></a>"), "a\n b\n");
}

TEST(CssCascade, TakesAnElementsNameInAnyCaseAndItsClassesAndIdAsWritten) {
  EXPECT_EQ(Styles("P { float: left } .Note { float: right } #Top { width: 1px }",
                   "<d><P/><p/><p class='note'/><p class='Note'/><i id='top'/><i id='Top'/></d>"),
            "d\n P {float: left}\n p {float: left}\n p {float: left}\n p {float: right}\n i\n i {width: 1px}\n");
}

TEST(CssCascade, LetsTheSelectorThatRanksHigherWin) {
  // An id over a class over an element's name, wherever each is written.
  EXPECT_EQ(Styles("#x { float: left } .a { float: right } p { float: none }",
                   "<d><p class='a' id='x'/><p class='a'/><p/></d>"),
            "d\n p {float: left}\n p {float: right}\n p {float: none}\n");
  // More of the same kind over fewer; and an attribute counts as a class.
  EXPECT_EQ(Styles("p.a.b { width: 2px } p.a { width: 1px } div p { height: 2px } p { height: 1px } "
                   "p[lang] { text-indent: 2px } p.a { text-indent: 1px } p { text-indent: 0 }",
                   "<div><p class='a b' lang='en'/></div>"),
            "div\n p {text-indent: 1px; width: 2px; height: 2px}\n");
  // One of a higher kind over any number of a lower: a class over eight
  // element names, and an id over as many classes as a selector can have,
  // with `:link`, which counts as one, written as often as there is room
  // to write it.
  std::string links;
  for (int i = 0; i < 190; ++i) {
    links += ":link";
  }
  EXPECT_EQ(Styles(".a { float: left } a a a a a a a p { float: right } #x { width: 1px } "
                   "p.a.a.a.a.a.a.a.a.a.a.a.a[b][b][b][b][b][b][b][b]" +
                       links + " { width: 2px; height: 2px }",
                   "<a><a><a><a><a><a><a><p class='a' id='x' b=''/></a></a></a></a></a></a></a>"),
            "a\n a\n  a\n   a\n    a\n     a\n      a\n       p {float: left; width: 1px; height: 2px}\n");
}

TEST(CssCascade, LetsWhatIsWrittenLaterWinBetweenSelectorsOfOneRank) {
  EXPECT_EQ(Styles(".a { float: left } .b { float: right }", "<d><p class='a b'/><p class='b a'/></d>"),
            "d\n p {float: right}\n p {float: right}\n");
  EXPECT_EQ(
      Styles(".b { float: left } .a { float: right } .b { width: 1px } p { width: 2px }", "<d><p class='a b'/></d>"),
      "d\n p {float: right; width: 1px}\n");
  // Within one rule the same: the last, unless an earlier one is important.
  EXPECT_EQ(Styles("p { float: left; float: right; width: 1px !important; width: 2px; margin: 1px; margin-top: 2px }",
                   "<p/>"),
            "p {margin-top: 2px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px; float: right; width: 1px}\n");
}

TEST(CssCascade, LetsWhatIsImportantWinOverRankAndOrder) {
  EXPECT_EQ(Styles("p { float: left !important; width: 1px !important } #x { float: right; width: 2px !important } "
                   ".a { float: none }",
                   "<p class='a' id='x'/>"),
            "p {float: left; width: 2px}\n");
}

TEST(CssCascade, PutsAnElementsOwnStyleOverTheRulesButNotOverWhatIsImportant) {
  EXPECT_EQ(Styles("#x { float: left; width: 1px !important; height: 1px !important }",
                   "<d><p id='x' style='float: right; width: 2px; height: 2px !important; text-indent: 1em; "
                   "text-indent: 2em; color: red'/><p style='margin: 0 1px'/><p style=''/></d>"),
            "d\n"
            " p {text-indent: 2em; float: right; width: 1px; height: 2px}\n"
            " p {margin-top: 0px; margin-right: 1px; margin-bottom: 0px; margin-left: 1px}\n"
            " p\n");
}

TEST(CssCascade, GivesBackEveryKindOfValueAsItWasWritten) {
  EXPECT_EQ(Styles("p { font-family: Georgia, \"Times New Roman\" , serif; font-weight: 700; line-height: 1.25; "
                   "margin: -0.5em 12pt 150% 0; text-indent: 40000px; width: 32.768px; height: 32.767px; "
                   "font-size: larger; padding-top: 0.001rem }",
                   "<p/>"),
            "p {font-weight: 700; font-family: {serif}; font-size: larger; "
            "line-height: 1.25; text-indent: 40000px; margin-top: -0.5em; margin-right: 16px; margin-bottom: 150%; "
            "margin-left: 0px; padding-top: 0.001rem; width: 32.768px; height: 32.767px}\n");
}

TEST(CssCascade, GivesAListOfFamiliesAsTheKindOfFaceItComesTo) {
  EXPECT_EQ(
      Styles("p { font-family: \"No Such Face\", 'Courier New', serif } i { font-family: \"Trade Gothic LT Std\" } "
             "b { font-family: Zapfino } u { font-family: \"A Face Nothing Knows\" } "
             "s { font: italic 1em/1 Georgia } q { font-family: inherit } "
             "p::first-letter { font-family: Wingdings }",
             "<d><p/><i/><b/><u/><s/><q/><em/></d>"),
      "d\n"
      " p {font-family: {mono}} first-letter {font-family: {other}}\n"
      " i {font-family: {sans}}\n"
      " b {font-family: {script}}\n"
      " u {font-family: {other}}\n"
      " s {font-style: italic; font-weight: normal; font-variant: normal; font-family: {serif}; font-size: 1em; "
      "line-height: 1}\n"
      " q {font-family: inherit}\n"
      " em\n");
  // An element's own `style` is given the same way, and wins.
  EXPECT_EQ(Styles("p { font-family: Georgia }",
                   "<d><p style='font-family: \"Letter Gothic\", serif'/><p style=\"font: 1em 'Gill Sans'\"/></d>"),
            "d\n"
            " p {font-family: {mono}}\n"
            " p {font-style: normal; font-weight: normal; font-variant: normal; font-family: {sans}; font-size: 1em; "
            "line-height: normal}\n");
}

TEST(CssRules, KeepsOfAListOfFamiliesAByteAndNoneOfItsNames) {
  Book shortName;
  (void)shortName.Sheet("p { font-family: x }");
  Book longNames;
  (void)longNames.Sheet(
      "p { font-family: \"A Face With A Very Long Name Indeed\", \"And Another After It\", Georgia, "
      "'Times New Roman', serif }");
  EXPECT_EQ(longNames.rules.Bytes(), shortName.rules.Bytes());
  EXPECT_EQ(longNames.rules.Names(), 1U) << "the element's name, and no family's";
  // It takes what a word does: the property, the form and a byte.
  Book word;
  (void)word.Sheet("p { font-style: italic }");
  EXPECT_EQ(shortName.rules.Bytes(), word.rules.Bytes());
}

TEST(CssCascade, FollowsASelectorOutThroughTheElementsAnElementIsInside) {
  // Inside, at any depth; and straight inside.
  EXPECT_EQ(Styles("div p { float: left } div > p { width: 1px } section div i { height: 1px }",
                   "<section><p/><div><p><i/></p><q><p/></q></div></section>"),
            "section\n p\n div\n  p {float: left; width: 1px}\n   i {height: 1px}\n  q\n   p {float: left}\n");
  // The nearest element that will do is not always the one: here the
  // nearest `b` is not straight inside an `a`, and one further out is.
  EXPECT_EQ(Styles("a > b c { float: left } a > b > c d { width: 1px }",
                   "<a><b><x><b><c><y><c><d/></c></y></c></b></x></b></a>"),
            "a\n b\n  x\n   b\n    c {float: left}\n     y\n      c {float: left}\n       d\n");
  EXPECT_EQ(Styles("a > b > c d { width: 1px }", "<a><b><c><b><c><d/></c></b></c></b></a>"),
            "a\n b\n  c\n   b\n    c\n     d {width: 1px}\n");
  // Nothing is outside the outermost element.
  EXPECT_EQ(Styles("a a { float: left } a > a { width: 1px } * > a { height: 1px }", "<a><a/></a>"),
            "a\n a {float: left; width: 1px; height: 1px}\n");
}

TEST(CssCascade, FollowsASelectorBackThroughTheElementsBeforeAnElement) {
  // The element straight before, text between them or not, inside the
  // same element; never an element's parent, nor one inside another.
  EXPECT_EQ(Styles("h2 + p { text-indent: 0 }",
                   "<d><h2/><p/><p/><h2/>text<p/><h2><p/></h2><q><h2/></q><p/><h2/><i/><p/></d>"),
            "d\n h2\n p {text-indent: 0px}\n p\n h2\n p {text-indent: 0px}\n h2\n  p\n q\n  h2\n p\n h2\n i\n p\n");
  // As many before it as the selector names.
  EXPECT_EQ(Styles("hgroup > h2 + p + p + p + p { float: left } h2 + p + p { width: 1px } p + p { height: 1px }",
                   "<hgroup><h2/><p/><p/><p/><p/><p/></hgroup>"),
            "hgroup\n h2\n p\n p {width: 1px; height: 1px}\n p {height: 1px}\n p {float: left; height: 1px}\n"
            " p {height: 1px}\n");
  // The element before one that the element is inside; and before the
  // first of all there is nothing.
  EXPECT_EQ(Styles("tr + tr td { float: left } i + tr > td + td { width: 1px } * + table { height: 1px }",
                   "<table><tr><td/></tr><tr><td/><td/></tr><i/><tr><td/><td/></tr></table>"),
            "table\n tr\n  td\n tr\n  td {float: left}\n  td {float: left}\n i\n tr\n  td\n  td {width: 1px}\n");
}

TEST(CssCascade, AsksOfAnAttributeWhatTheSelectorAsks) {
  const std::string document =
      "<d><p/><p a=''/><p a='x'/><p a='x y'/><p a='y x-z'/><p a='x-z'/><p a='xz'/><p a='zx'/><p a='X'/></d>";
  const auto which = [&document](std::string_view selector) {
    // Which of the nine it is true of, as a row of marks.
    const std::string lines = Styles(std::string(selector) + " { float: left }", document);
    std::string marks;
    for (std::size_t at = lines.find("\n p"); at != std::string::npos; at = lines.find("\n p", at + 1)) {
      marks += lines.compare(at + 3, 2, " {") == 0 ? 'x' : '.';
    }
    return marks;
  };
  EXPECT_EQ(which("[a]"), ".xxxxxxxx");
  EXPECT_EQ(which("[a=x]"), "..x......");
  EXPECT_EQ(which("[a='']"), ".x.......");
  EXPECT_EQ(which("[a~=x]"), "..xx.....");
  EXPECT_EQ(which("[a~='x y']"), ".........");
  EXPECT_EQ(which("[a~='']"), ".........");
  EXPECT_EQ(which("[a|=x]"), "..x..x...");
  EXPECT_EQ(which("[a^=x]"), "..xx.xx..");
  EXPECT_EQ(which("[a$=x]"), "..x....x.");
  EXPECT_EQ(which("[a*=x]"), "..xxxxxx.");
  EXPECT_EQ(which("[a^='']"), ".........");
  EXPECT_EQ(which("[a$='']"), ".........");
  EXPECT_EQ(which("[a*='']"), ".........");
  // Any case, of the value's letters, if the selector says so.
  EXPECT_EQ(which("[a=x i]"), "..x.....x");
  EXPECT_EQ(which("[a~=X i]"), "..xx....x");
  EXPECT_EQ(which("[a*=Z i]"), "....xxxx.");
  // More than one thing asked: all of them.
  EXPECT_EQ(which("[a^=x][a$=z]"), ".....xx..");
  EXPECT_EQ(which("[a][b]"), ".........");
}

TEST(CssCascade, TakesAnAttributesNameWithItsPrefixAsADocumentWritesIt) {
  EXPECT_EQ(Styles("[epub|type~=chapter] { float: left } [type] { width: 1px } [*|type] { height: 1px } "
                   "[LANG] { text-indent: 0 }",
                   "<d xmlns:epub='e' xmlns:o='o'><p epub:type='a chapter'/><p type='chapter'/>"
                   "<p o:type='chapter' Lang='en'/><p EPUB:TYPE='chapter'/></d>"),
            "d\n p {float: left; height: 1px}\n p {width: 1px; height: 1px}\n p {text-indent: 0px; height: 1px}\n"
            " p {float: left; height: 1px}\n");
}

TEST(CssCascade, TriesEveryElementAgainstTheSelectorsThatNameNeitherElementClassNorId) {
  EXPECT_EQ(
      Styles("* { float: left } [hidden] { width: 1px } *[hidden] > * { height: 1px }", "<d><p hidden=''><i/></p></d>"),
      "d {float: left}\n p {float: left; width: 1px}\n  i {float: left; height: 1px}\n");
}

TEST(CssCascade, KeepsWhatIsSetForAFirstLetterApart) {
  EXPECT_EQ(Styles("p.first::first-letter { float: left; font-size: 3em } p { font-size: 1em } "
                   "div > p::first-letter { line-height: 1 } p:first-letter { font-size: 2em }",
                   "<div><p class='first' style='float: right'/><p/></div>"),
            "div\n"
            " p {font-size: 1em; float: right} first-letter {font-size: 3em; line-height: 1; float: left}\n"
            " p {font-size: 1em} first-letter {font-size: 2em; line-height: 1}\n");
}

TEST(CssCascade, GivesADocumentTheSheetsItAsksForAndNoOthers) {
  Book book;
  const std::uint8_t first = book.Sheet("p { float: left; width: 1px }");
  const std::uint8_t second = book.Sheet("p { float: right } div p { height: 1px }");
  const std::uint8_t third = book.Sheet("p { float: none; text-indent: 0 }");
  EXPECT_EQ(first, 0);
  EXPECT_EQ(second, 1);
  EXPECT_EQ(third, 2);
  const std::string_view document = "<div><p/></div>";
  EXPECT_EQ(Walk(book, {first}, document).lines, "div\n p {float: left; width: 1px}\n");
  EXPECT_EQ(Walk(book, {second}, document).lines, "div\n p {float: right; height: 1px}\n");
  EXPECT_EQ(Walk(book, {}, document).lines, "div\n p\n");
  // A sheet given later wins over one given earlier, whichever was added
  // to the table first; unless a selector of the earlier ranks higher.
  EXPECT_EQ(Walk(book, {first, second, third}, document).lines,
            "div\n p {text-indent: 0px; float: none; width: 1px; height: 1px}\n");
  EXPECT_EQ(Walk(book, {third, second, first}, document).lines,
            "div\n p {text-indent: 0px; float: left; width: 1px; height: 1px}\n");
  EXPECT_EQ(Walk(book, {third, first}, document).lines, "div\n p {text-indent: 0px; float: left; width: 1px}\n");
  // A sheet given twice is as if it were only where it was given last; a
  // number that is no sheet's is passed over.
  EXPECT_EQ(Walk(book, {first, third, first}, document).lines, "div\n p {text-indent: 0px; float: left; width: 1px}\n");
  EXPECT_EQ(Walk(book, {9, first, 200}, document).lines, "div\n p {float: left; width: 1px}\n");
}

TEST(CssCascade, LooksAtOnlySoManyOfAnElementsClasses) {
  std::string classes;
  for (std::size_t i = 0; i < CssCascade::kMaxClasses; ++i) {
    classes += "c" + std::to_string(i) + (i % 2 == 0 ? " " : "\t\n");
  }
  Book book;
  const std::uint8_t sheet = book.Sheet(".c0 { float: left } .c15 { width: 1px } .late { height: 1px }");
  const Walked within = Walk(book, {sheet}, "<p class='" + classes + "'/>");
  EXPECT_EQ(within.lines, "p {float: left; width: 1px}\n");
  EXPECT_EQ(within.tooManyClasses, 0U);
  const Walked over = Walk(book, {sheet}, "<d><p class='" + classes + " late'/><p class='late'/></d>");
  EXPECT_EQ(over.lines, "d\n p {float: left; width: 1px}\n p {height: 1px}\n");
  EXPECT_EQ(over.tooManyClasses, 1U);
  // A class written twice is looked at twice, and counts for one: the
  // selectors kept under it are tried once, however often it is written.
  EXPECT_EQ(Walk(book, {sheet}, "<p class='c0 c0  c0 c15'/>").lines, "p {float: left; width: 1px}\n");
  std::string many;
  for (int i = 0; i < 100; ++i) {
    many += ".again.x" + std::to_string(i) + " { margin: 0 }\n";
  }
  Book repeated;
  const std::uint8_t number = repeated.Sheet(many);
  const std::uint64_t once = Walk(repeated, {number}, "<p class='again'/>").mostWork;
  const std::uint64_t often = Walk(repeated, {number}, "<p class='again again again again again again'/>").mostWork;
  EXPECT_GE(once, 100U);
  EXPECT_LT(often, once + 100);
}

TEST(CssCascade, KeepsOfAnOpenElementOnlyABitForEachNode) {
  Book book;
  // No selector asks anything of the elements around the one it styles:
  // nothing is kept, and no depth is too deep.
  const std::uint8_t plain = book.Sheet("p { float: left } .a { width: 1px }");
  EXPECT_EQ(book.rules.Nodes(), 0U);
  std::string deep;
  for (int i = 0; i < 200; ++i) {
    deep += "<p>";
  }
  for (int i = 0; i < 200; ++i) {
    deep += "</p>";
  }
  const Walked nothingKept = Walk(book, {plain}, deep, 0);
  EXPECT_EQ(nothingKept.levelBytes, 0U);
  EXPECT_EQ(nothingKept.tooDeep, 0U);

  // "div" and "div p" are two nodes, however many selectors ask for them.
  const std::uint8_t around =
      book.Sheet("div p { float: right } div > i { height: 1px } div p + q, div p + i { width: 2px }");
  EXPECT_EQ(book.rules.Nodes(), 2U);
  EXPECT_EQ(Walk(book, {plain, around}, "<div><p/></div>").levelBytes, 2U);
}

TEST(CssCascade, GivesAnElementTooDeepToKeepTheRulesThatAskNothingOfTheElementsAroundIt) {
  Book book;
  const std::uint8_t sheet = book.Sheet(
      "p { float: left } d p { width: 1px } d > p { height: 1px } "
      "p + p { text-indent: 0 } p > i { line-height: 1 } p i { font-size: 1em }");
  // Room for two open elements: the third deep is not kept, though it is
  // still asked about the two outside it; what is inside it is asked only
  // what can be told from itself.
  const std::string_view document = "<d><p><i/><i/><p><i/></p></p><p/><p/></d>";
  const Walked roomy = Walk(book, {sheet}, document);
  EXPECT_EQ(roomy.tooDeep, 0U);
  EXPECT_EQ(roomy.lines,
            "d\n"
            " p {float: left; width: 1px; height: 1px}\n"
            "  i {font-size: 1em; line-height: 1}\n"
            "  i {font-size: 1em; line-height: 1}\n"
            "  p {float: left; width: 1px}\n"
            "   i {font-size: 1em; line-height: 1}\n"
            " p {text-indent: 0px; float: left; width: 1px; height: 1px}\n"
            " p {text-indent: 0px; float: left; width: 1px; height: 1px}\n");
  const Walked tight = Walk(book, {sheet}, document, 2 * roomy.levelBytes);
  EXPECT_EQ(tight.tooDeep, 4U);
  EXPECT_EQ(tight.lines,
            "d\n"
            " p {float: left; width: 1px; height: 1px}\n"
            "  i {font-size: 1em; line-height: 1}\n"
            "  i {font-size: 1em; line-height: 1}\n"
            "  p {float: left; width: 1px}\n"
            "   i\n"
            " p {text-indent: 0px; float: left; width: 1px; height: 1px}\n"
            " p {text-indent: 0px; float: left; width: 1px; height: 1px}\n");
  // With room for one, an element is not known by the one after it.
  const Walked one = Walk(book, {sheet}, document, roomy.levelBytes);
  EXPECT_EQ(one.lines,
            "d\n"
            " p {float: left; width: 1px; height: 1px}\n"
            "  i\n"
            "  i\n"
            "  p {float: left}\n"
            "   i\n"
            " p {float: left; width: 1px; height: 1px}\n"
            " p {float: left; width: 1px; height: 1px}\n");
  // With room for none, every element is too deep.
  const Walked none = Walk(book, {sheet}, document, 0);
  EXPECT_EQ(none.tooDeep, 8U);
  EXPECT_EQ(none.lines, "d\n p {float: left}\n  i\n  i\n  p {float: left}\n   i\n p {float: left}\n p {float: left}\n");
}

TEST(CssCascade, DoesNotSeeRulesAddedSinceTheDocumentBegan) {
  Book book;
  const std::uint8_t first = book.Sheet("p { float: left }");
  std::vector<std::uint8_t> memory(1024);
  CssCascade cascade(book.rules, memory);
  const std::array<std::uint8_t, 2> sheets = {0, 1};
  cascade.Begin(sheets);
  const std::uint8_t second = book.Sheet("p { width: 1px } d p { height: 1px } * { text-indent: 0 }");
  EXPECT_EQ(first, 0);
  EXPECT_EQ(second, 1);
  const bn::epub::XmlAttributes none(nullptr);
  CssStyle style;
  CssStyle firstLetter;
  cascade.Open("d", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "");
  cascade.Open("p", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "float: left");
  cascade.Close();
  cascade.Close();
  cascade.Begin(sheets);
  cascade.Open("d", none, style, firstLetter);
  cascade.Open("p", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "text-indent: 0px; float: left; width: 1px; height: 1px");
  // Closing more than was opened does nothing.
  cascade.Close();
  cascade.Close();
  cascade.Close();
  EXPECT_EQ(cascade.Depth(), 0U);
}

TEST(CssCascade, DoesNoMoreWorkForAnElementThanItSays) {
  // Forty selectors that a `p` has to be tried against. Each asks eight
  // things of an element far outside it, which makes forty nodes for
  // every element to be tried against, and each asks for six elements
  // straight inside one another on the way out, which have to be looked
  // for at every depth.
  std::string sheet;
  for (int i = 0; i < 40; ++i) {
    sheet += "[z][z][z][z][z][z][z][w" + std::to_string(i) + "] > a > a > a > a > a > a p { margin: 0 }\n";
  }
  Book book;
  const std::uint8_t number = book.Sheet(sheet);
  ASSERT_TRUE(book.Whole());
  EXPECT_EQ(book.rules.Nodes(), 41U);
  // A `p` thirty deep in `a`, with thirty attributes of which the one
  // asked about is the last, so that each asking goes through them all.
  std::string document = "<r>";
  for (int i = 0; i < 30; ++i) {
    document += "<a>";
  }
  document += "<p";
  for (int i = 0; i < 29; ++i) {
    document += " n" + std::to_string(i) + "=''";
  }
  document += " z=''/>";
  for (int i = 0; i < 30; ++i) {
    document += "</a>";
  }
  document += "</r>";
  const Walked walked = Walk(book, {number}, document);
  EXPECT_TRUE(walked.agreed);
  // The most it says is within twice what such a document takes: it is
  // not so far above that it says nothing.
  EXPECT_LE(walked.mostWork, walked.mostAllowed);
  EXPECT_GT(walked.mostWork * 2, walked.mostAllowed);
}

TEST(CssRules, KeepsEachNameOnceAndCountsWhatItHolds) {
  Book book;
  EXPECT_EQ(book.rules.Bytes(), 2U);
  const std::uint8_t sheet = book.Sheet("p.note { margin-top: 1em }");
  EXPECT_EQ(sheet, 0);
  EXPECT_EQ(book.rules.Sheets(), 1U);
  EXPECT_EQ(book.rules.Rules(), 1U);
  EXPECT_EQ(book.rules.Selectors(), 1U);
  EXPECT_EQ(book.rules.Names(), 2U);
  EXPECT_EQ(book.rules.Nodes(), 0U);
  // Two names of 4 bytes, the letters and 2 for where each is; a rule of
  // 3; a selector of 8 and 6 for its part; a declaration of 4, counted.
  EXPECT_EQ(book.rules.Bytes(), 2U + (4 + 1 + 2) + (4 + 4 + 2) + 3 + (8 + 6) + (1 + 4));

  // The same names again cost nothing: only the rule and its selector.
  const std::size_t before = book.rules.Bytes();
  (void)book.Sheet("p.note { margin-top: 0 }");
  EXPECT_EQ(book.rules.Names(), 2U);
  EXPECT_EQ(book.rules.Bytes(), before + 3 + (8 + 6) + (1 + 2));
  EXPECT_EQ(book.rules.Sheets(), 2U);
  EXPECT_EQ(book.rules.Rules(), 2U);

  // A rule with several selectors keeps what it sets once; a number that
  // needs four bytes takes them.
  const std::size_t middle = book.rules.Bytes();
  (void)book.Sheet("p, .note, p.note { text-indent: 99999px; float: left }");
  EXPECT_EQ(book.rules.Selectors(), 5U);
  EXPECT_EQ(book.rules.Bytes(), middle + 3 + (8 + 4) + (8 + 4) + (8 + 6) + (1 + 6 + 3));
}

TEST(CssRules, DoesNotKeepARuleThatSetsNothingOrHasNoSelector) {
  Book book;
  (void)book.Sheet("p { } q { color: red } a:hover { float: left } i { float: left }");
  EXPECT_EQ(book.rules.Rules(), 1U);
  EXPECT_EQ(book.rules.Selectors(), 1U);
  EXPECT_EQ(book.rules.Names(), 1U);
  EXPECT_EQ(book.rules.LeftOutForRoom(), 0U);
}

TEST(CssRules, LeavesOutWholeARuleThereIsNoRoomForAndKeepsTheRest) {
  // Room for the first rule and not for the second, whose names are long;
  // then for a third that is small.
  Book book(64);
  const std::uint8_t sheet = book.Sheet(
      "p { float: left }\n"
      "p.a-class-with-a-long-name, div.another-of-them > i { width: 1px; float: right }\n"
      "i { width: 2px }");
  EXPECT_EQ(book.rules.Rules(), 2U);
  EXPECT_EQ(book.rules.Selectors(), 2U);
  EXPECT_EQ(book.rules.LeftOutForRoom(), 1U);
  EXPECT_EQ(book.rules.LeftOutForNodes(), 0U);
  EXPECT_EQ(book.rules.Names(), 2U);
  EXPECT_EQ(book.rules.Nodes(), 0U);
  EXPECT_LE(book.rules.Bytes(), 64U);
  EXPECT_FALSE(book.Whole());
  std::vector<std::uint8_t> memory(64);
  CssCascade cascade(book.rules, memory);
  const std::array<std::uint8_t, 1> sheets = {sheet};
  cascade.Begin(sheets);
  const std::array<const char*, 3> classed = {"class", "a-class-with-a-long-name", nullptr};
  CssStyle style;
  CssStyle firstLetter;
  cascade.Open("p", bn::epub::XmlAttributes(classed.data()), style, firstLetter);
  EXPECT_EQ(StyleText(style), "float: left");
  cascade.Open("i", bn::epub::XmlAttributes(nullptr), style, firstLetter);
  EXPECT_EQ(StyleText(style), "width: 2px");

  // With no room at all, or next to none, every rule is left out and
  // nothing else happens.
  for (const std::size_t bytes : {0U, 1U, 2U, 5U, 9U}) {
    Book small(bytes);
    (void)small.Sheet("p { float: left } q { float: right }");
    EXPECT_EQ(small.rules.Rules(), 0U) << bytes;
    EXPECT_EQ(small.rules.LeftOutForRoom(), 2U) << bytes;
    EXPECT_EQ(Walk(small, {0}, "<p/>").lines, "p\n") << bytes;
  }
}

TEST(CssRules, HoldsRulesUpToTheLastByteOfItsBlock) {
  // The same sheet in blocks of every size up to what it needs: each
  // keeps as many of its rules as fit, from the first, and what it keeps
  // is true of the same elements.
  const std::string sheet =
      "p { float: left } div.a > p { width: 1px } h2 + p.b { height: 1px } "
      "#c { text-indent: 0; font-family: \"Courier New\", serif }";
  const std::string_view document = "<div class='a'><h2/><p class='b' id='c'/></div>";
  Book whole;
  const std::uint8_t number = whole.Sheet(sheet);
  const std::size_t needed = whole.rules.Bytes();
  const std::string all = Walk(whole, {number}, document).lines;
  std::uint32_t kept = 0;
  for (std::size_t bytes = 0; bytes <= needed; ++bytes) {
    Book book(bytes);
    (void)book.Sheet(sheet);
    EXPECT_LE(book.rules.Bytes(), std::max<std::size_t>(bytes, 2)) << bytes;
    EXPECT_GE(book.rules.Rules(), kept) << bytes;
    EXPECT_EQ(book.rules.Rules() + book.rules.LeftOutForRoom(), 4U) << bytes;
    kept = book.rules.Rules();
    if (bytes == needed) {
      EXPECT_EQ(kept, 4U);
      EXPECT_EQ(Walk(book, {0}, document).lines, all);
    }
  }
}

TEST(CssRules, LeavesOutARuleThatWouldTakeItPastTheMostNodes) {
  std::string sheet;
  for (std::size_t i = 0; i < CssRules::kMaxNodes; ++i) {
    sheet += "a" + std::to_string(i) + " p { margin-top: " + std::to_string(i) + "px }\n";
  }
  Book book;
  const std::uint8_t number = book.Sheet(sheet + "one-more p, q { float: left }\na7 i, a8 > i { width: 1px }");
  EXPECT_EQ(book.rules.Nodes(), CssRules::kMaxNodes);
  EXPECT_EQ(book.rules.LeftOutForNodes(), 1U);
  EXPECT_EQ(book.rules.LeftOutForRoom(), 0U);
  EXPECT_EQ(book.rules.Rules(), CssRules::kMaxNodes + 1);
  // A rule that asks only for nodes there already are is still taken.
  std::vector<std::uint8_t> memory(4096);
  CssCascade cascade(book.rules, memory);
  const std::array<std::uint8_t, 1> sheets = {number};
  cascade.Begin(sheets);
  EXPECT_EQ(cascade.LevelBytes(), 64U);
  const bn::epub::XmlAttributes none(nullptr);
  CssStyle style;
  CssStyle firstLetter;
  cascade.Open("a254", none, style, firstLetter);
  cascade.Open("a8", none, style, firstLetter);
  cascade.Open("p", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "margin-top: 254px");
  cascade.Close();
  cascade.Open("i", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "width: 1px");
  cascade.Close();
  cascade.Open("q", none, style, firstLetter);
  EXPECT_EQ(StyleText(style), "");
}

TEST(CssRules, GoesBackToWhereItStood) {
  Book book;
  const std::uint8_t kept = book.Sheet("p { float: left } div p.a { width: 1px } [hidden] { display: none }");
  const std::string_view document = "<div><p class='a'/><p class='b' hidden=''/><i/><q/></div>";
  const std::string before = Walk(book, {kept}, document).lines;
  const CssRules::Place place = book.rules.Here();
  const std::size_t bytes = book.rules.Bytes();
  const std::size_t names = book.rules.Names();
  const std::size_t nodes = book.rules.Nodes();

  // A document's own rules: new names before and after the old in order,
  // more selectors under the old names, new nodes, one that is tried
  // against everything.
  std::uint8_t own = 0;
  ASSERT_TRUE(book.rules.BeginSheet(own));
  CssReader reader;
  bn::core::MemoryByteSource source(
      Bytes("p { float: right } a.b > p.a, zz + p.b { width: 2px } * { height: 1px } "
            "i { font-family: A Face } div p + q { float: none }"));
  EXPECT_EQ(reader.Read(source, book.rules), CssStatus::kOk);
  book.rules.EndSheet();
  EXPECT_EQ(own, 1);
  EXPECT_GT(book.rules.Bytes(), bytes);
  EXPECT_GT(book.rules.Names(), names);
  EXPECT_GT(book.rules.Nodes(), nodes);
  {
    std::vector<std::uint8_t> memory(1024);
    CssCascade cascade(book.rules, memory);
    const std::array<std::uint8_t, 2> sheets = {kept, own};
    cascade.Begin(sheets);
    CssStyle style;
    CssStyle firstLetter;
    cascade.Open("i", bn::epub::XmlAttributes(nullptr), style, firstLetter);
    EXPECT_EQ(StyleText(style), "font-family: {other}; height: 1px");
  }

  book.rules.BackTo(place);
  EXPECT_EQ(book.rules.Bytes(), bytes);
  EXPECT_EQ(book.rules.Names(), names);
  EXPECT_EQ(book.rules.Nodes(), nodes);
  EXPECT_EQ(book.rules.Sheets(), 1U);
  EXPECT_EQ(book.rules.Rules(), 3U);
  EXPECT_EQ(book.rules.Selectors(), 3U);
  // What was there before is as it was, to the plain working-out too,
  // which was never told of the sheet that came and went.
  EXPECT_EQ(Walk(book, {kept, own}, document).lines, before);

  // A small sheet in the place of the one that went, which leaves most of
  // what that one wrote where it was: none of it is found again, not the
  // rule for every element nor the ones under names that stayed.
  std::uint8_t small = 0xFF;
  ASSERT_TRUE(book.rules.BeginSheet(small));
  bn::core::MemoryByteSource smallSource(Bytes("u { float: none }"));
  EXPECT_EQ(reader.Read(smallSource, book.rules), CssStatus::kOk);
  book.rules.EndSheet();
  EXPECT_EQ(small, own);
  EXPECT_EQ(Walk(book, {kept, small}, "<div><i/><p class='a'/><a class='b'><p class='a'/></a><q/></div>").lines,
            "div\n i\n p {float: left; width: 1px}\n a\n  p {float: left; width: 1px}\n q\n");
  book.rules.BackTo(place);

  // And it can be added to again, from the same number.
  const std::uint8_t again = book.Sheet("q { float: right } div p + q { width: 3px }");
  EXPECT_EQ(again, 1);
  EXPECT_EQ(Walk(book, {kept, again}, "<div><p/><q/></div>").lines,
            "div\n p {float: left}\n q {float: right; width: 3px}\n");

  // A place that is no longer one is left alone.
  const CssRules::Place later = book.rules.Here();
  book.rules.BackTo(place);
  const std::size_t now = book.rules.Bytes();
  book.rules.BackTo(later);
  EXPECT_EQ(book.rules.Bytes(), now);
  book.rules.Clear();
  EXPECT_EQ(book.rules.Bytes(), 2U);
  EXPECT_EQ(book.rules.Sheets(), 0U);
  EXPECT_EQ(book.rules.Names(), 0U);
}

TEST(CssRules, TakesASheetAtATimeAndOnlySoManySheets) {
  std::vector<std::uint8_t> memory(4096);
  CssRules rules(memory);
  std::uint8_t sheet = 0xFF;
  // Rules told outside a sheet are not kept.
  CssReader reader;
  bn::core::MemoryByteSource outside(Bytes("p { float: left }"));
  EXPECT_EQ(reader.Read(outside, rules), CssStatus::kOk);
  EXPECT_EQ(rules.Rules(), 0U);
  EXPECT_EQ(rules.Bytes(), 2U);

  ASSERT_TRUE(rules.BeginSheet(sheet));
  EXPECT_EQ(sheet, 0);
  std::uint8_t other = 0xFF;
  EXPECT_FALSE(rules.BeginSheet(other));
  EXPECT_EQ(other, 0xFF);
  // A sheet that ends in the middle of a rule leaves that rule out.
  rules.OnRuleBegin();
  bn::core::MemoryByteSource source(Bytes("p { float: left }"));
  EXPECT_EQ(reader.Read(source, rules), CssStatus::kOk);
  rules.OnRuleBegin();
  rules.EndSheet();
  EXPECT_EQ(rules.Rules(), 1U);
  const std::size_t bytes = rules.Bytes();

  for (std::size_t i = 1; i < CssRules::kMaxSheets; ++i) {
    ASSERT_TRUE(rules.BeginSheet(sheet));
    EXPECT_EQ(sheet, i);
    rules.EndSheet();
  }
  EXPECT_FALSE(rules.BeginSheet(sheet));
  EXPECT_EQ(rules.Sheets(), CssRules::kMaxSheets);
  EXPECT_EQ(rules.Bytes(), bytes);
}

TEST(CssCascade, SaysWhatThePlainWorkingOutSaysOfSheetsAndDocumentsMadeAtRandom) {
  std::mt19937 rng(20261116);  // NOLINT(bugprone-random-generator-seed): the same run every time.
  const auto below = [&rng](std::size_t limit) -> std::size_t { return rng() % limit; };
  const auto one = [&below](std::initializer_list<const char*> of) {
    return std::string(*(of.begin() + below(of.size())));
  };

  const auto part = [&]() {
    std::string text;
    if (below(3) != 0) {
      text += one({"div", "p", "span", "h2", "i", "*"});
    }
    for (std::size_t n = below(5) == 0 ? 2 : below(2); n > 0; --n) {
      text += "." + one({"a", "b", "c"});
    }
    if (below(6) == 0) {
      text += "#" + one({"x", "y"});
    }
    if (below(5) == 0) {
      text += one({"[lang]", "[lang|=en]", "[title~=w]", "[k^=a]", "[k$=b i]", "[*|k*=c]", "[title='']"});
    }
    return text.empty() ? std::string("*") : text;
  };
  const auto sheetText = [&]() {
    std::string text;
    for (std::size_t rules = 1 + below(14); rules > 0; --rules) {
      for (std::size_t selectors = 1 + below(3); selectors > 0; --selectors) {
        text += part();
        for (std::size_t more = below(4); more > 0; --more) {
          text += one({" ", " > ", " + ", " ", " + "}) + part();
        }
        if (below(12) == 0) {
          text += "::first-letter";
        }
        text += selectors > 1 ? ", " : "";
      }
      text += " {";
      for (std::size_t declarations = 1 + below(4); declarations > 0; --declarations) {
        text += one({"float: left", "float: right", "width: 1px", "width: 2em", "margin: 0 1px", "text-indent: 0",
                     "font-family: A, b", "font-family: serif", "height: 50%", "height: 70000px"}) +
                (below(7) == 0 ? " !important; " : "; ");
      }
      text += "}\n";
    }
    return text;
  };
  const auto documentText = [&]() {
    std::string text;
    std::vector<std::string> open;
    for (std::size_t elements = 4 + below(40); elements > 0 || !open.empty();) {
      if (elements > 0 && (open.empty() || (open.size() < 7 && below(5) < 3)) && (!open.empty() || text.empty())) {
        const std::string name = one({"div", "p", "span", "h2", "i", "DIV", "q"});
        text += "<" + name;
        if (below(2) == 0) {
          text += " class='" + one({"a", "b", "c", "a b", "c  a", "b c a", ""}) + "'";
        }
        if (below(5) == 0) {
          text += " id='" + one({"x", "y", "z"}) + "'";
        }
        if (below(5) == 0) {
          text += " " + one({"lang='en'", "lang='en-GB'", "title='v w'", "k='ab'", "o:k='xcB'", "title=''"});
        }
        if (below(7) == 0) {
          text += " style='" + one({"float: none", "width: 3px !important", "margin-top: 5px; height: 1px"}) + "'";
        }
        text += '>';
        open.push_back(name);
        --elements;
      } else if (!open.empty()) {
        text += below(4) == 0 ? "words" : "";
        text += "</" + open.back() + ">";
        open.pop_back();
      } else {
        break;
      }
    }
    return text;
  };

  std::size_t elements = 0;
  std::size_t styled = 0;
  for (int round = 0; round < 400; ++round) {
    Book book;
    std::vector<std::uint8_t> sheets;
    for (std::size_t count = 1 + below(3); count > 0; --count) {
      sheets.push_back(book.Sheet(sheetText()));
    }
    ASSERT_TRUE(book.Whole());
    std::shuffle(sheets.begin(), sheets.end(), rng);
    if (below(4) == 0) {
      sheets.pop_back();
    }
    for (int documents = 0; documents < 3; ++documents) {
      // Now and then with room for only a few open elements.
      const std::string document = documentText();
      const Walked walked = Walk(book, sheets, document, below(4) == 0 ? below(6) * 8 : 4096);
      ASSERT_TRUE(walked.agreed) << "round " << round << "\n" << document;
      elements += static_cast<std::size_t>(std::count(walked.lines.begin(), walked.lines.end(), '\n'));
      styled += static_cast<std::size_t>(std::count(walked.lines.begin(), walked.lines.end(), '{'));
    }
  }
  // It was not all about nothing: there were elements, and rules that
  // were true of a good part of them.
  EXPECT_GT(elements, 10000U);
  EXPECT_GT(styled * 3, elements);
}

}  // namespace
