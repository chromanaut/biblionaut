#include "epub/style_stack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/xml.h"

// What each element of a document comes to (epub/style_stack.h).

namespace {

using bn::epub::ElementStyle;
using bn::epub::StyleDisplay;
using bn::epub::StyleLeftOut;
using bn::epub::StyleLength;
using bn::epub::StyleStack;
using bn::epub::TextStyle;

std::span<const std::uint8_t> Bytes(std::string_view text) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// So many of `over` to the unit, as a number: 96 of 64 is "1.5".
std::string Part(std::int32_t amount, std::int32_t over) {
  const std::int64_t size = amount < 0 ? -std::int64_t{amount} : amount;
  std::string out = (amount < 0 ? "-" : "") + std::to_string(size / over);
  if (size % over != 0) {
    std::string fraction = std::to_string(1'000'000 + ((size % over) * 1'000'000 / over)).substr(1);
    while (fraction.back() == '0') {
      fraction.pop_back();
    }
    out += "." + fraction;
  }
  return out;
}

// A length as its three amounts, those that are not nothing with a `+`
// between them: "1.5em+40px"; "0" for none.
std::string Length(const StyleLength& length) {
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

std::string Bits(std::uint16_t bits) {
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

// How text is set, as the parts of it that are not as text inside nothing
// has them, each under a name.
using Parts = std::vector<std::pair<std::string, std::string>>;

void TextParts(const TextStyle& text, const StyleStack& stack, Parts& parts) {
  if (text.size != 1000 || text.sizeFromBook) {
    parts.emplace_back("size", std::to_string(text.size) + Book(text.sizeFromBook));
  }
  if (text.bits != 0) {
    parts.emplace_back("text", Bits(text.bits));
  }
  if (text.fromBook != 0) {
    parts.emplace_back("book", Bits(text.fromBook));
  }
  if (text.family != bn::epub::CssFamilyClass::kUnset || text.familyFromBook) {
    parts.emplace_back("family", std::string(bn::epub::CssFamilyClassName(text.family)) + Book(text.familyFromBook));
  }
  if (text.language != 0) {
    parts.emplace_back("language", std::string(stack.Language(text.language)));
  }
}

// The four sides of a margin or a padding, top, right, bottom and left,
// with a `*` after one the book said; nothing if no side has anything.
std::string Sides(const std::array<StyleLength, 4>& sides, std::uint8_t fromBook, bool leftAuto, bool rightAuto) {
  std::string out;
  bool any = false;
  for (std::size_t side = 0; side < sides.size(); ++side) {
    const bool book = ((std::uint32_t{fromBook} >> side) & 1U) != 0;
    const bool isAuto = (side == bn::epub::kStyleLeft && leftAuto) || (side == bn::epub::kStyleRight && rightAuto);
    any = any || book || !sides.at(side).IsZero();
    out += (side == 0 ? "" : " ") + (isAuto ? std::string("auto") : Length(sides.at(side))) + (book ? "*" : "");
  }
  return any ? out : "";
}

// What an element comes to, as the parts of it that are not as an element
// that says nothing and is inside nothing has them, each under a name.
Parts ComesTo(const ElementStyle& said, const StyleStack& stack) {
  Parts parts;
  if (said.display != StyleDisplay::kInline) {
    parts.emplace_back("display", bn::epub::StyleDisplayName(said.display));
  }
  if (said.display == StyleDisplay::kNone) {
    return parts;
  }
  TextParts(said.text, stack, parts);
  if (said.align != bn::epub::StyleAlign::kUnset || said.alignFromBook) {
    parts.emplace_back("align", std::string(bn::epub::StyleAlignName(said.align)) + Book(said.alignFromBook));
  }
  if (said.hasIndent) {
    parts.emplace_back("indent", Length(said.indent) + Book(said.indentFromBook));
  }
  if (said.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kMultiple) {
    parts.emplace_back("lines", "x" + std::to_string(said.lineHeight.multiple) + Book(said.lineHeightFromBook));
  } else if (said.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kLength) {
    parts.emplace_back("lines", Length(said.lineHeight.length) + Book(said.lineHeightFromBook));
  } else if (said.lineHeightFromBook) {
    parts.emplace_back("lines", "normal book");
  }
  if (said.display == StyleDisplay::kListItem) {
    parts.emplace_back("marker", std::string(bn::epub::StyleMarkerName(said.marker)) + Book(said.markerFromBook));
  }
  if (said.rightToLeft) {
    parts.emplace_back("direction", "right-to-left");
  }
  if (said.spacesAsWritten) {
    parts.emplace_back("spaces", "as written");
  }
  if (const std::string margin = Sides(said.margin, said.marginFromBook, said.marginLeftAuto, said.marginRightAuto);
      !margin.empty()) {
    parts.emplace_back("margin", margin);
  }
  if (const std::string padding = Sides(said.padding, said.paddingFromBook, false, false); !padding.empty()) {
    parts.emplace_back("padding", padding);
  }
  if (said.hasWidth) {
    parts.emplace_back("width", Length(said.width));
  }
  if (said.hasHeight) {
    parts.emplace_back("height", Length(said.height));
  }
  if (said.floated != bn::epub::StyleFloat::kNone) {
    parts.emplace_back("float", said.floated == bn::epub::StyleFloat::kLeft ? "left" : "right");
  }
  std::string breaks;
  if (said.breakBefore) {
    breaks += "before ";
  }
  if (said.breakAfter) {
    breaks += "after ";
  }
  if (said.keepWithNext || said.keepWithNextFromBook) {
    breaks +=
        std::string(said.keepWithNext ? "keep-with-next" : "not-with-next") + (said.keepWithNextFromBook ? "* " : " ");
  }
  if (said.keepTogether) {
    breaks += "keep-together ";
  }
  if (!breaks.empty()) {
    breaks.pop_back();
    parts.emplace_back("breaks", breaks);
  }
  if (said.hasFirstLetter) {
    Parts letter;
    TextParts(said.firstLetter, stack, letter);
    std::string all = said.firstLetterFloated ? "floated" : "set";
    for (const auto& [name, what] : letter) {
      all += ", ";
      all += name;
      all += ' ';
      all += what;
    }
    parts.emplace_back("letter", all);
  }
  return parts;
}

// What going through a document came to.
struct Walked {
  // A line for each element: a space for each element it is inside, its
  // name, and after ": " the parts asked for of what it comes to, each as
  // its name and what it is, with "; " between.
  std::string lines;
  std::array<std::uint32_t, bn::epub::kStyleLeftOutKinds> leftOut{};
  std::vector<std::string> languages;
};

// Goes through `document` with a cascade over the rules of `sheet` and a
// stack of styles that keeps `levels` open elements. `only` is the names
// of the parts to tell of, with spaces between; all of them if it is
// empty.
Walked Walk(std::string_view only, std::string_view sheet, std::string_view document, std::size_t levels = 32) {
  class Walker final : public bn::epub::XmlHandler {
   public:
    Walker(bn::epub::CssCascade& cascade, StyleStack& stack, Walked& walked, std::string_view only)
        : cascade_(cascade), stack_(stack), walked_(walked), only_(" " + std::string(only) + " ") {}

    void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
      const std::size_t depth = stack_.Depth();
      bn::epub::CssStyle style;
      bn::epub::CssStyle firstLetter;
      cascade_.Open(name, attributes, style, firstLetter);
      ElementStyle said;
      stack_.Open(name, attributes, style, firstLetter, said);
      EXPECT_EQ(stack_.Depth(), depth + 1);
      std::string told;
      for (const auto& [part, what] : ComesTo(said, stack_)) {
        if (only_ == "  " || only_.find(" " + part + " ") != std::string::npos) {
          told += told.empty() ? "" : "; ";
          told += part;
          told += ' ';
          told += what;
        }
      }
      walked_.lines += std::string(depth, ' ') + std::string(name) + (told.empty() ? "" : ": " + told) + "\n";
    }
    void OnEndElement(std::string_view /*name*/) noexcept override {
      cascade_.Close();
      stack_.Close();
    }
    void OnText(std::string_view /*text*/) noexcept override {}
    void OnEntity(std::string_view /*name*/) noexcept override {}

   private:
    bn::epub::CssCascade& cascade_;
    StyleStack& stack_;
    Walked& walked_;
    std::string only_;
  };

  std::vector<std::uint8_t> tableMemory(65536);
  bn::epub::CssRules rules(tableMemory);
  std::uint8_t number = 0;
  EXPECT_TRUE(rules.BeginSheet(number));
  bn::core::MemoryByteSource sheetSource(Bytes(sheet));
  bn::epub::CssReader reader;
  EXPECT_EQ(reader.Read(sheetSource, rules), bn::epub::CssStatus::kOk);
  rules.EndSheet();
  EXPECT_EQ(rules.LeftOutForRoom() + rules.LeftOutForNodes(), 0U);

  std::vector<std::uint8_t> cascadeMemory(8192);
  bn::epub::CssCascade cascade(rules, cascadeMemory);
  const std::array<std::uint8_t, 1> sheets = {number};
  cascade.Begin(sheets);
  std::vector<std::uint8_t> stackMemory(StyleStack::kLanguagesBytes + (levels * StyleStack::kLevelBytes));
  StyleStack stack(stackMemory);
  EXPECT_EQ(stack.Levels(), levels);
  stack.Begin();

  Walked walked;
  std::vector<std::uint8_t> parserMemory(65536);
  bn::epub::XmlParser parser(parserMemory);
  bn::core::MemoryByteSource source(Bytes(document));
  Walker walker(cascade, stack, walked, only);
  EXPECT_EQ(parser.Parse(source, walker), bn::epub::XmlStatus::kOk) << document;
  EXPECT_EQ(stack.Depth(), 0U);
  for (std::size_t kind = 0; kind < walked.leftOut.size(); ++kind) {
    walked.leftOut.at(kind) = stack.LeftOut(static_cast<StyleLeftOut>(kind));
  }
  for (std::size_t language = 1; language <= stack.Languages(); ++language) {
    walked.languages.emplace_back(stack.Language(static_cast<std::uint8_t>(language)));
  }
  return walked;
}

std::string Styles(std::string_view only, std::string_view sheet, std::string_view document) {
  return Walk(only, sheet, document).lines;
}

// What a cascade would say of an element that one thing is set for: a
// length, or a word.
bn::epub::CssStyle Setting(bn::epub::CssProperty property, std::int32_t number, bn::epub::CssUnit unit) {
  bn::epub::CssStyle style;
  bn::epub::CssValue& value = style.values.at(static_cast<std::size_t>(property));
  value.kind = bn::epub::CssValue::Kind::kLength;
  value.number = number;
  value.unit = unit;
  style.set = 1U << static_cast<unsigned>(property);
  return style;
}

bn::epub::CssStyle Setting(bn::epub::CssProperty property, bn::epub::CssKeyword keyword) {
  bn::epub::CssStyle style;
  bn::epub::CssValue& value = style.values.at(static_cast<std::size_t>(property));
  value.kind = bn::epub::CssValue::Kind::kKeyword;
  value.keyword = keyword;
  style.set = 1U << static_cast<unsigned>(property);
  return style;
}

std::uint32_t LeftOut(const Walked& walked, StyleLeftOut kind) {
  return walked.leftOut.at(static_cast<std::size_t>(kind));
}

TEST(StyleStack, AnElementIsABlockPartOfALineOrNothingByWhatItIs) {
  EXPECT_EQ(Styles("display", "",
                   "<html><head><title/></head><body><div><span/><li/><unknown/><DIV/></div><table><tr><td/></tr>"
                   "<col/></table><math><mi/></math><ruby><rt/></ruby></body></html>"),
            "html: display block\n"
            " head: display none\n"
            "  title: display none\n"
            " body: display block\n"
            "  div: display block\n"
            "   span\n"
            "   li: display list-item\n"
            "   unknown\n"
            "   DIV: display block\n"
            "  table: display block\n"
            "   tr: display block\n"
            "    td: display block\n"
            "   col: display none\n"
            "  math: display none\n"
            "   mi: display none\n"
            "  ruby\n"
            "   rt: display none\n");
}

TEST(StyleStack, EveryElementOfHtmlIsToAPageWhatHtmlHasIt) {
  for (const char* name : {"address", "article", "aside", "blockquote", "body",   "caption",  "center",     "dd",
                           "details", "dir",     "div",   "dl",         "dt",     "fieldset", "figcaption", "figure",
                           "footer",  "form",    "h1",    "h2",         "h3",     "h4",       "h5",         "h6",
                           "header",  "hgroup",  "hr",    "html",       "legend", "listing",  "main",       "menu",
                           "nav",     "ol",      "p",     "plaintext",  "pre",    "section",  "summary",    "table",
                           "tbody",   "td",      "tfoot", "th",         "thead",  "tr",       "ul",         "xmp"}) {
    EXPECT_EQ(Styles("display", "", "<" + std::string(name) + "/>"), std::string(name) + ": display block\n");
  }
  for (const char* name :
       {"area",     "base",  "basefont", "col", "colgroup", "datalist", "head",  "link",     "math",  "meta", "noembed",
        "noframes", "param", "rp",       "rt",  "script",   "source",   "style", "template", "title", "track"}) {
    EXPECT_EQ(Styles("display", "", "<" + std::string(name) + "/>"), std::string(name) + ": display none\n");
  }
  for (const char* name : {"a",    "abbr",   "b",      "big", "br",   "cite", "code", "del", "dfn",  "em",
                           "i",    "img",    "ins",    "kbd", "mark", "q",    "ruby", "s",   "samp", "small",
                           "span", "strike", "strong", "sub", "sup",  "svg",  "tt",   "u",   "var",  "noscript"}) {
    EXPECT_EQ(Styles("display", "", "<" + std::string(name) + "/>"), std::string(name) + "\n");
  }
  EXPECT_EQ(Styles("display", "", "<li/>"), "li: display list-item\n");
  // A name longer than any in the list, and one after the last of them, are parts of a line.
  EXPECT_EQ(Styles("", "", "<blockquotes><zz/></blockquotes>"), "blockquotes\n zz\n");
}

TEST(StyleStack, TheBooksDisplayDecidesWhereItSaysOneOfFour) {
  const Walked walked = Walk("display",
                             ".b { display: block } .i { display: inline } .l { display: list-item } "
                             ".n { display: none } .ib { display: inline-block } .init { display: initial } "
                             ".same { display: inherit }",
                             "<d><span class='b'/><p class='i'/><i class='l'/><p class='n'/><div class='ib'/>"
                             "<span class='ib'/><p class='init'/><p class='same'/><li class='b'/></d>");
  EXPECT_EQ(walked.lines,
            "d\n"
            " span: display block\n"
            " p\n"
            " i: display list-item\n"
            " p: display none\n"
            // `inline-block` changes nothing: each is what its element is.
            " div: display block\n"
            " span\n"
            // `initial` is the property's own start, which is part of a line.
            " p\n"
            // And `inherit` is as nothing said.
            " p: display block\n"
            " li: display block\n");
  // What an element is to a page is its own, and not taken from outside.
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kInheritOfOwn), 1U);
}

TEST(StyleStack, WhatIsNothingHidesAllInsideIt) {
  EXPECT_EQ(Styles("", ".n { display: none } b { display: block; font-size: 2em }",
                   "<d><p class='n'><b>x<i/></b></p><p hidden=''><b/></p><p hidden='hidden'/><i><b/></i></d>"),
            "d\n"
            // And nothing else is said of it.
            " p: display none\n"
            "  b: display none\n"
            "   i: display none\n"
            " p: display none\n"
            "  b: display none\n"
            " p: display none\n"
            " i: text italic\n"
            "  b: display block; size 2000 book; text bold italic\n");
}

TEST(StyleStack, CountsTheMathematicsAndTheNotesOfARubyThatItLeavesOut) {
  const Walked walked = Walk("display", ".b { display: block } .n { display: none }",
                             "<d><math><mi/><math/></math><ruby><b/><rt/><rp/></ruby><rt class='b'/>"
                             "<p class='n'><math/><rt/></p><p hidden=''><rt/></p><MATH/></d>");
  EXPECT_EQ(walked.lines,
            "d\n"
            " math: display none\n"
            "  mi: display none\n"
            "  math: display none\n"
            // A ruby's own text is kept.
            " ruby\n"
            "  b\n"
            "  rt: display none\n"
            "  rp: display none\n"
            // What the book says is to be shown is.
            " rt: display block\n"
            " p: display none\n"
            "  math: display none\n"
            "  rt: display none\n"
            " p: display none\n"
            "  rt: display none\n"
            " MATH: display none\n");
  // The first and the last, and the note of the ruby: not what is inside
  // something that is nothing already, and not the brackets for a reader
  // that shows no notes, which nobody shows.
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kNotShown), 3U);
  EXPECT_EQ(LeftOut(Walk("", "", "<p><i/></p>"), StyleLeftOut::kNotShown), 0U);
}

TEST(StyleStack, ASizeIsSoManyThousandthsOfTheReadersOwn) {
  EXPECT_EQ(Styles("size",
                   ".em { font-size: 1.5em } .pc { font-size: 80% } .px { font-size: 12px } .pt { font-size: 12pt } "
                   ".small { font-size: small } .xxs { font-size: xx-small } .xs { font-size: x-small } "
                   ".m { font-size: medium } .l { font-size: large } .xl { font-size: x-large } "
                   ".xxl { font-size: xx-large } .init { font-size: initial } .zero { font-size: 0 }",
                   "<d><a class='em'/><a class='pc'/><a class='px'/><a class='pt'/><a class='small'/><a class='xxs'/>"
                   "<a class='xs'/><a class='m'/><a class='l'/><a class='xl'/><a class='xxl'/><a class='init'/>"
                   "<a class='zero'/><a/></d>"),
            "d\n"
            " a: size 1500 book\n"
            " a: size 800 book\n"
            // A pixel is a sixteenth of the reader's size, and a point four thirds of a pixel.
            " a: size 750 book\n"
            " a: size 1000 book\n"
            " a: size 813 book\n"
            " a: size 563 book\n"
            " a: size 625 book\n"
            " a: size 1000 book\n"
            " a: size 1125 book\n"
            " a: size 1500 book\n"
            " a: size 2000 book\n"
            " a: size 1000 book\n"
            " a: size 0 book\n"
            " a\n");
}

TEST(StyleStack, AnEmAndAPercentageAreOfTheSizeOutsideAndARemOfTheDocumentsOwn) {
  EXPECT_EQ(Styles("size",
                   "html { font-size: 120% } .em { font-size: 1.5em } .pc { font-size: 50% } .rem { font-size: 2rem } "
                   ".same { font-size: inherit } .smaller { font-size: smaller } .larger { font-size: larger } "
                   ".px { font-size: 16px }",
                   "<html><p class='em'><i class='em'><b class='pc'/><b class='rem'/><b class='same'/>"
                   "<b class='smaller'/><b class='larger'/><b class='px'/><u/></i></p></html>"),
            "html: size 1200 book\n"
            " p: size 1800 book\n"
            "  i: size 2700 book\n"
            "   b: size 1350 book\n"
            "   b: size 2400 book\n"
            "   b: size 2700 book\n"
            // A step is of 1.2: five sixths, and six fifths.
            "   b: size 2250 book\n"
            "   b: size 3240 book\n"
            "   b: size 1000 book\n"
            // What is inside has the size it is inside, as the book's still.
            "   u: size 2700 book\n");
  // The document's own element has its rem of the reader's size.
  EXPECT_EQ(Styles("size", "html { font-size: 2rem }", "<html><i/></html>"),
            "html: size 2000 book\n i: size 2000 book\n");
  // A size is held to the most that is kept.
  EXPECT_EQ(Styles("size", "p { font-size: 20em }", "<p><p><p><p/></p></p></p>"),
            "p: size 20000 book\n p: size 65535 book\n  p: size 65535 book\n   p: size 65535 book\n");
  // To the nearest thousandth, and a half goes up: one and a half times
  // 1001 is 1501.5, and five sixths of 1001 and of 1003 are 834.17 and
  // 835.83.
  EXPECT_EQ(Styles("size", ".a { font-size: 1.001em } .b { font-size: 1.003em } .h { font-size: 1.5em }",
                   "<d><a class='a'><a class='h'/><small/></a><a class='b'><small/></a></d>"),
            "d\n a: size 1001 book\n  a: size 1502 book\n  small: size 834 book\n a: size 1003 book\n"
            "  small: size 836 book\n");
}

TEST(StyleStack, AHeadingAndSmallAndBigHaveTheirSizesFromWhatTheyAre) {
  EXPECT_EQ(Styles("size", "",
                   "<d><h1/><h2/><h3/><h4/><h5/><h6/><small/><big/><sup/><sub/><h1><small><small/></small></h1></d>"),
            "d\n"
            " h1: size 2000\n"
            " h2: size 1500\n"
            " h3: size 1170\n"
            " h4\n"
            " h5: size 830\n"
            " h6: size 670\n"
            " small: size 833\n"
            " big: size 1200\n"
            " sup: size 833\n"
            " sub: size 833\n"
            " h1: size 2000\n"
            "  small: size 1667\n"
            "   small: size 1389\n");
  // What the book says of a heading's size is of the size outside it, not
  // of what the heading would have had; and inside a size the book set,
  // an element's own step is of the book's size.
  EXPECT_EQ(Styles("size", "h1 { font-size: 1.5em } div { font-size: 80% }", "<d><h1/><div><h2><big/></h2></div></d>"),
            "d\n h1: size 1500 book\n div: size 800 book\n  h2: size 1200 book\n   big: size 1440 book\n");
}

TEST(StyleStack, TextIsBoldAtAWeightOfSixHundredOrOver) {
  EXPECT_EQ(Styles("text book",
                   ".b { font-weight: bold } .n { font-weight: normal } .er { font-weight: bolder } "
                   ".l { font-weight: lighter } .w6 { font-weight: 600 } .w5 { font-weight: 599 } "
                   ".w9 { font-weight: 900 } .same { font-weight: inherit } .init { font-weight: initial }",
                   "<d><a class='b'><a/><a class='n'/><a class='same'/><a class='init'/><a class='l'/></a>"
                   "<a class='er'/><a class='w6'/><a class='w5'/><a class='w9'/><b/><strong/><th/>"
                   "<b class='n'/><b class='b'/><a class='same'/></d>"),
            "d\n"
            " a: text bold; book bold\n"
            "  a: text bold; book bold\n"
            "  a: book bold\n"
            "  a: text bold; book bold\n"
            "  a: book bold\n"
            "  a: book bold\n"
            " a: text bold; book bold\n"
            " a: text bold; book bold\n"
            " a: book bold\n"
            " a: text bold; book bold\n"
            // By being what it is, which is not the book's saying so.
            " b: text bold\n"
            " strong: text bold\n"
            " th: text bold\n"
            " b: book bold\n"
            " b: text bold; book bold\n"
            " a: book bold\n");
}

TEST(StyleStack, TextIsSlantedAndInSmallCapitalsAsTheBookOrTheElementSays) {
  EXPECT_EQ(Styles("text book",
                   ".i { font-style: italic } .o { font-style: oblique } .n { font-style: normal } "
                   ".same { font-style: inherit; font-variant: inherit } .sc { font-variant: small-caps } "
                   ".nc { font-variant: normal }",
                   "<d><a class='i sc'><a/><a class='n nc'/><a class='same'/></a><a class='o'/><i/><em/><cite/><var/>"
                   "<dfn/><address/><em class='n'><i/></em></d>"),
            "d\n"
            " a: text italic small-caps; book italic small-caps\n"
            "  a: text italic small-caps; book italic small-caps\n"
            "  a: book italic small-caps\n"
            "  a: text italic small-caps; book italic small-caps\n"
            " a: text italic; book italic\n"
            " i: text italic\n"
            " em: text italic\n"
            " cite: text italic\n"
            " var: text italic\n"
            " dfn: text italic\n"
            " address: text italic\n"
            " em: book italic\n"
            // An element's own slant is the element's, whoever said what outside it.
            "  i: text italic\n");
}

TEST(StyleStack, LinesUnderAndThroughAddUpThroughTheElementsATextIsInside) {
  EXPECT_EQ(Styles("text book",
                   ".u { text-decoration: underline } .s { text-decoration: line-through } "
                   ".o { text-decoration: overline } .n { text-decoration: none } .same { text-decoration: inherit }",
                   "<d><a class='u'><a class='s'><a class='n'/><a class='o'/><a class='same'/></a></a><u/><ins/><s/>"
                   "<strike/><del/><u class='n'/><u class='s'/><u><s class='n'/></u><p class='u'><b/></p></d>"),
            "d\n"
            " a: text underline; book underline\n"
            "  a: text underline line-through; book underline line-through\n"
            // Nothing takes away a line drawn by an element outside.
            "   a: text underline line-through; book underline line-through\n"
            "   a: text underline line-through; book underline line-through\n"
            "   a: text underline line-through; book underline line-through\n"
            " u: text underline\n"
            " ins: text underline\n"
            " s: text line-through\n"
            " strike: text line-through\n"
            " del: text line-through\n"
            // What the book says of the element itself stands in for the element's own line.
            " u\n"
            " u: text line-through; book line-through\n"
            " u: text underline\n"
            "  s: text underline\n"
            // And a block's line goes on into what is inside it.
            " p: text underline; book underline\n"
            "  b: text bold underline; book underline\n");
}

TEST(StyleStack, TextIsRaisedOrLoweredWithinALineAndNoFurther) {
  EXPECT_EQ(
      Styles("display text book",
             ".up { vertical-align: super } .top { vertical-align: top } .tt { vertical-align: text-top } "
             ".len { vertical-align: 0.3em } .pc { vertical-align: 50% } .down { vertical-align: sub } "
             ".bot { vertical-align: bottom } .tb { vertical-align: text-bottom } .neg { vertical-align: -2px } "
             ".base { vertical-align: baseline } .mid { vertical-align: middle } .zero { vertical-align: 0 } "
             ".same { vertical-align: inherit } .init { vertical-align: initial } .b { display: block }",
             "<d><a class='up'/><a class='top'/><a class='tt'/><a class='len'/><a class='pc'/><a class='down'/>"
             "<a class='bot'/><a class='tb'/><a class='neg'/><a class='base'/><a class='mid'/><a class='zero'/>"
             "<a class='init'/><sup><i><a class='same'/><a class='base'/><a class='down'/><a class='b'/></i></sup>"
             "<sub/><sup class='base'/><sub class='up'/><td class='top'/><p class='up'><i/></p>"
             "<sup class='b'/></d>"),
      "d\n"
      " a: text raised; book raised lowered\n"
      " a: text raised; book raised lowered\n"
      " a: text raised; book raised lowered\n"
      " a: text raised; book raised lowered\n"
      " a: text raised; book raised lowered\n"
      " a: text lowered; book raised lowered\n"
      " a: text lowered; book raised lowered\n"
      " a: text lowered; book raised lowered\n"
      " a: text lowered; book raised lowered\n"
      " a: book raised lowered\n"
      " a: book raised lowered\n"
      " a: book raised lowered\n"
      " a: book raised lowered\n"
      " sup: text raised\n"
      // It stays so through what is inside, as part of the same line.
      "  i: text italic raised\n"
      "   a: text italic raised; book raised lowered\n"
      "   a: text italic; book raised lowered\n"
      "   a: text italic lowered; book raised lowered\n"
      // And ends at a block.
      "   a: display block; text italic\n"
      " sub: text lowered\n"
      " sup: book raised lowered\n"
      " sub: text raised; book raised lowered\n"
      // On what is not part of a line it says nothing of text.
      " td: display block\n"
      " p: display block\n"
      "  i: text italic\n"
      " sup: display block\n");
}

TEST(StyleStack, AFamilyIsTheKindOfFaceFromTheBookOrFromTheElement) {
  EXPECT_EQ(Styles("family",
                   ".sans { font-family: Arial } .serif { font-family: Georgia, sans-serif } "
                   ".other { font-family: \"No Such Face\" } .same { font-family: inherit } "
                   ".init { font-family: initial }",
                   "<d><a class='sans'><a/><a class='serif'/><a class='init'/><code/><code class='same'/></a>"
                   "<a class='other'/><code/><kbd/><samp/><tt/><pre><b/></pre><listing/><xmp/><plaintext/>"
                   "<code class='serif'/><a class='same'/></d>"),
            "d\n"
            " a: family sans book\n"
            "  a: family sans book\n"
            "  a: family serif book\n"
            "  a: family unset book\n"
            // The element's own kind of face, over what it is inside.
            "  code: family mono\n"
            "  code: family sans book\n"
            " a: family other book\n"
            " code: family mono\n"
            " kbd: family mono\n"
            " samp: family mono\n"
            " tt: family mono\n"
            " pre: family mono\n"
            "  b: family mono\n"
            " listing: family mono\n"
            " xmp: family mono\n"
            " plaintext: family mono\n"
            " code: family serif book\n"
            " a: family unset book\n");
}

TEST(StyleStack, ALanguageIsTheNearestSaidWithXmlLangBeforeLang) {
  const Walked walked = Walk("language", "",
                             "<html xml:lang='en-GB' lang='fr'><p lang='de'><i/><b xml:lang='de-CH'/><b lang='DE'/>"
                             "<b lang=''><i/></b></p><p/><p lang='EN-gb'/><p xml:lang='' lang='it'/></html>");
  EXPECT_EQ(walked.lines,
            "html: language en-GB\n"
            " p: language de\n"
            "  i: language de\n"
            "  b: language de-CH\n"
            // A tag in letters of another case is the same language, as it was first written.
            "  b: language de\n"
            // And an empty one is none.
            "  b\n"
            "   i\n"
            " p: language en-GB\n"
            " p: language en-GB\n"
            " p\n");
  EXPECT_EQ(walked.languages, (std::vector<std::string>{"en-GB", "de", "de-CH"}));
}

TEST(StyleStack, KeepsSoManyLanguagesOfSoManyBytesAndCountsTheRest) {
  std::string document = "<d>";
  for (std::size_t i = 0; i < StyleStack::kMaxLanguages + 2; ++i) {
    document += "<p lang='l" + std::to_string(i) + "'/>";
  }
  const std::string longest(StyleStack::kMaxLanguageBytes, 'x');
  document += "<p lang='l0'><i lang='" + longest + "y'/></p></d>";
  Walked walked = Walk("language", "", document);
  EXPECT_EQ(walked.languages.size(), StyleStack::kMaxLanguages);
  EXPECT_EQ(walked.languages.back(), "l14");
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kLanguage), 3U);
  // One that is not kept leaves the element with the language outside it.
  EXPECT_NE(walked.lines.find(" p: language l14\n p\n p\n p: language l0\n  i: language l0\n"), std::string::npos)
      << walked.lines;

  // A tag of just the most bytes is kept.
  walked = Walk("language", "", "<p lang='" + longest + "'/>");
  EXPECT_EQ(walked.languages, std::vector<std::string>{longest});
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kLanguage), 0U);
  // And one a byte longer is not, though there is room for a language.
  walked = Walk("language", "", "<p lang='en'><i lang='" + longest + "y'/></p>");
  EXPECT_EQ(walked.languages, std::vector<std::string>{"en"});
  EXPECT_EQ(walked.lines, "p: language en\n i: language en\n");
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kLanguage), 1U);
}

TEST(StyleStack, ABlocksLinesStandAsTheBookAlignOrTheElementSays) {
  EXPECT_EQ(Styles("align",
                   ".l { text-align: left } .r { text-align: right } .c { text-align: center } "
                   ".j { text-align: justify } .s { text-align: start } .e { text-align: end } "
                   ".same { text-align: inherit } .init { text-align: initial } .b { display: block }",
                   "<d><div class='j'><i/><div class='l'/><div class='same'/><div class='init'/><center/></div>"
                   "<div class='r'/><div class='c'/><div class='s'/><div class='e'/>"
                   "<center><p/><th/><p align='left'/></center><th/><caption/>"
                   "<div align='RIGHT'><p/><p align='justify'/><p align='middle'/><p align='center' class='l'/></div>"
                   "<table align='center'/><caption align='left'/><hr align='right'/><i align='center'/>"
                   "<i align='center' class='b'/></d>"),
            "d\n"
            " div: align justify book\n"
            "  i: align justify book\n"
            "  div: align left book\n"
            "  div: align justify book\n"
            "  div: align unset book\n"
            // The element's, over what it is inside.
            "  center: align center\n"
            " div: align right book\n"
            " div: align center book\n"
            " div: align start book\n"
            " div: align end book\n"
            " center: align center\n"
            "  p: align center\n"
            "  th: align center\n"
            "  p: align left book\n"
            " th: align center\n"
            " caption: align center\n"
            // `align`, in any case, counts as the book's; a word that is none of the four says nothing.
            " div: align right book\n"
            "  p: align right book\n"
            "  p: align justify book\n"
            "  p: align right book\n"
            // And a rule's word is over the attribute's.
            "  p: align left book\n"
            // On a table, a caption and a rule it is not of lines; nor on what is part of a line.
            " table\n"
            " caption: align center\n"
            " hr\n"
            " i\n"
            " i: align center book\n");
}

TEST(StyleStack, AFirstLinesIndentIsAnAmountThatPassesToWhatIsInside) {
  EXPECT_EQ(Styles("size indent",
                   ".i { text-indent: 1.5em; font-size: 2em } .big { font-size: 3em } "
                   ".neg { text-indent: -2em } .pc { text-indent: 5% } .px { text-indent: 12px } "
                   ".rem { text-indent: 1rem } .zero { text-indent: 0 } .same { text-indent: inherit } "
                   ".init { text-indent: initial } .mm { text-indent: 25.4mm }",
                   "<d><a class='i'><a class='big'/><a class='same'/><a class='zero'/><a class='init'/></a>"
                   "<a class='neg'/><a class='pc'/><a class='px'/><a class='rem'/><a class='same'/><a class='mm'/>"
                   "<a/></d>"),
            "d\n"
            // An em of its own size: 1.5 of twice the reader's.
            " a: size 2000 book; indent 3em book\n"
            // Which what is inside it has, as that amount, whatever its own size.
            "  a: size 6000 book; indent 3em book\n"
            "  a: size 2000 book; indent 3em book\n"
            "  a: size 2000 book; indent 0 book\n"
            "  a: size 2000 book; indent 0 book\n"
            " a: indent -2em book\n"
            " a: indent 5% book\n"
            " a: indent 12px book\n"
            " a: indent 1em book\n"
            " a: indent 0 book\n"
            " a: indent 96px book\n"
            " a\n");
}

TEST(StyleStack, TheDistanceBetweenLinesIsAMultipleOfTheSizeOrALength) {
  EXPECT_EQ(Styles("lines",
                   ".n { line-height: 1.5 } .em { line-height: 1.2em } .pc { line-height: 120% } "
                   ".px { line-height: 20px } .rem { line-height: 1.5rem } .normal { line-height: normal } "
                   ".same { line-height: inherit } .init { line-height: initial } .zero { line-height: 0 } "
                   ".huge { line-height: 99 }",
                   "<html><a class='n'><a/><a class='normal'/><a class='same'/><a class='init'/></a><a class='em'/>"
                   "<a class='pc'/><a class='px'><a/></a><a class='rem'/><a class='zero'/><a class='huge'/>"
                   "<a class='same'/></html>"),
            "html\n"
            " a: lines x1500 book\n"
            "  a: lines x1500 book\n"
            "  a: lines normal book\n"
            "  a: lines x1500 book\n"
            "  a: lines normal book\n"
            " a: lines x1200 book\n"
            " a: lines x1200 book\n"
            " a: lines 20px book\n"
            "  a: lines 20px book\n"
            " a: lines 1.5em book\n"
            " a: lines x0 book\n"
            " a: lines x65535 book\n"
            " a: lines normal book\n");
}

TEST(StyleStack, WhatStandsBeforeAnItemIsByItsListThenByTypeThenByTheBook) {
  EXPECT_EQ(
      Styles("marker",
             ".none { list-style-type: none } .roman { list-style: upper-roman inside } "
             ".same { list-style-type: inherit } .init { list-style-type: initial } .item { display: list-item }",
             "<d><ul><li/><ul><li/><ul><li/><ul><li/></ul></ul></ul><ol><li/></ol></ul><ol><li/><ul><li/></ul></ol>"
             "<menu><li/></menu><dir><li/></dir><li/>"
             "<ol type='a'><li/><li type='I'/><li type='x'/><li type='DISC'/></ol><ol type='A'><li/></ol>"
             "<ol type='i'><li/></ol><ol type='1'><li/></ol><ul type='Square'><li/></ul><ul type='a'><li/></ul>"
             "<ol class='none'><li/><li class='roman'/><li class='init'/><ol><li class='same'/></ol></ol>"
             "<ol type='a' class='roman'><li/></ol><p type='a' class='item'/><p type='a'/></d>"),
      "d\n"
      // A list of bullets by how deep among lists it is.
      " ul\n"
      "  li: marker disc\n"
      "  ul\n"
      "   li: marker circle\n"
      "   ul\n"
      "    li: marker square\n"
      "    ul\n"
      "     li: marker square\n"
      "  ol\n"
      "   li: marker decimal\n"
      " ol\n"
      "  li: marker decimal\n"
      "  ul\n"
      "   li: marker circle\n"
      " menu\n"
      "  li: marker disc\n"
      " dir\n"
      "  li: marker disc\n"
      // An item in no list has a disc.
      " li: marker disc\n"
      // `type`, on the list or the item, as the book's: a numbered list's letters by their case, a bullet in any.
      " ol\n"
      "  li: marker lower-alpha book\n"
      "  li: marker upper-roman book\n"
      "  li: marker lower-alpha book\n"
      "  li: marker disc book\n"
      " ol\n"
      "  li: marker upper-alpha book\n"
      " ol\n"
      "  li: marker lower-roman book\n"
      " ol\n"
      "  li: marker decimal book\n"
      " ul\n"
      "  li: marker square book\n"
      " ul\n"
      "  li: marker lower-alpha book\n"
      // What a rule says passes to the items, until a list inside begins again.
      " ol\n"
      "  li: marker none book\n"
      "  li: marker upper-roman book\n"
      "  li: marker disc book\n"
      "  ol\n"
      "   li: marker decimal book\n"
      // And is over what `type` says.
      " ol\n"
      "  li: marker upper-roman book\n"
      " p: marker lower-alpha book\n"
      " p\n");
}

TEST(StyleStack, EveryWordTheBookHasForWhatStandsBeforeAnItemIsTold) {
  EXPECT_EQ(Styles("marker",
                   ".a { list-style-type: disc } .b { list-style-type: circle } .c { list-style-type: square } "
                   ".d { list-style-type: decimal } .e { list-style-type: decimal-leading-zero } "
                   ".f { list-style-type: lower-alpha } .g { list-style-type: upper-alpha } "
                   ".h { list-style-type: lower-roman } .i { list-style-type: upper-roman } "
                   ".j { list-style-type: lower-latin } .k { list-style-type: upper-latin } "
                   ".l { list-style-type: none }",
                   "<ol><li class='a'/><li class='b'/><li class='c'/><li class='d'/><li class='e'/><li class='f'/>"
                   "<li class='g'/><li class='h'/><li class='i'/><li class='j'/><li class='k'/><li class='l'/></ol>"),
            "ol\n"
            " li: marker disc book\n"
            " li: marker circle book\n"
            " li: marker square book\n"
            " li: marker decimal book\n"
            " li: marker decimal-leading-zero book\n"
            " li: marker lower-alpha book\n"
            " li: marker upper-alpha book\n"
            " li: marker lower-roman book\n"
            " li: marker upper-roman book\n"
            " li: marker lower-alpha book\n"
            " li: marker upper-alpha book\n"
            " li: marker none book\n");
}

TEST(StyleStack, TextRunsRightToLeftAndItsSpacesAreAsWrittenWhereTheDocumentSays) {
  EXPECT_EQ(Styles("direction spaces", "",
                   "<d><p dir='rtl'><i/><b dir='LTR'><i/></b><b dir='auto'/></p><p dir='ltr'/><pre><i/></pre>"
                   "<listing/><xmp/><plaintext/><code/></d>"),
            "d\n"
            " p: direction right-to-left\n"
            "  i: direction right-to-left\n"
            "  b\n"
            "   i\n"
            // A word that is neither leaves it as it was.
            "  b: direction right-to-left\n"
            " p\n"
            " pre: spaces as written\n"
            "  i: spaces as written\n"
            " listing: spaces as written\n"
            " xmp: spaces as written\n"
            " plaintext: spaces as written\n"
            " code\n");
}

TEST(StyleStack, AnElementHasTheSpaceAroundItThatItsElementGivesIt) {
  EXPECT_EQ(Styles("margin padding", "",
                   "<d><p/><blockquote/><figure/><dl><dt/><dd><ul/><dl/></dd></dl><pre/><hr/>"
                   "<ul><li/><ul/><ol/><dl/></ul><ol/><menu/>"
                   "<dir/><h1/><h2/><h3/><h4/><h5/><h6/><div/><table/><td/><address/></d>"),
            "d\n"
            " p: margin 1em 0 1em 0\n"
            " blockquote: margin 1em 40px 1em 40px\n"
            " figure: margin 1em 40px 1em 40px\n"
            " dl: margin 1em 0 1em 0\n"
            "  dt\n"
            "  dd: margin 0 0 0 40px\n"
            "   ul: padding 0 0 0 40px\n"
            "   dl\n"
            " pre: margin 1em 0 1em 0\n"
            " hr: margin 0.5em 0 0.5em 0\n"
            " ul: margin 1em 0 1em 0; padding 0 0 0 40px\n"
            "  li\n"
            // A list inside a list has no space above or below, a list of terms among them.
            "  ul: padding 0 0 0 40px\n"
            "  ol: padding 0 0 0 40px\n"
            "  dl\n"
            " ol: margin 1em 0 1em 0; padding 0 0 0 40px\n"
            " menu: margin 1em 0 1em 0; padding 0 0 0 40px\n"
            " dir: margin 1em 0 1em 0; padding 0 0 0 40px\n"
            // A heading's ems are of its own size: 0.67 of twice the reader's, and so on.
            " h1: margin 1.34375em 0 1.34375em 0\n"
            " h2: margin 1.25em 0 1.25em 0\n"
            " h3: margin 1.171875em 0 1.171875em 0\n"
            " h4: margin 1.328125em 0 1.328125em 0\n"
            " h5: margin 1.390625em 0 1.390625em 0\n"
            " h6: margin 1.5625em 0 1.5625em 0\n"
            " div\n"
            " table\n"
            " td\n"
            " address\n");
}

TEST(StyleStack, TheBooksMarginsAndPaddingAreAmountsOfItsOwnSize) {
  const Walked walked =
      Walk("margin padding",
           ".a { margin: 1em 2px 3% 0 } .b { font-size: 2em; margin: 1em; padding: 0.5em 1rem } "
           ".neg { margin: -1.5em 0 0 -10px } .auto { margin: 0 auto } .left { margin-left: auto } "
           ".init { margin: initial; padding: initial } .same { margin: inherit; padding-top: inherit } "
           ".in { margin-left: 1in; padding-left: 2.54cm } .most { margin-top: 99999px; margin-left: -99999px } "
           ".half { margin-top: 0.008em; padding-top: 0.063px; margin-left: 0.5% } "
           ".under { margin-top: 0.007em; padding-top: 0.062px }",
           "<html><p class='a'/><p class='b'/><p class='neg'/><p class='auto'/><p class='left'/><p class='init'/>"
           "<blockquote class='same'/><i class='a'/><p class='in'/><p class='most'/><i class='half'/><i class='under'/>"
           "</html>");
  EXPECT_EQ(walked.lines,
            "html\n"
            " p: margin 1em* 2px* 3%* 0*\n"
            // An em is of the element's own size, and a rem of the document's.
            " p: margin 2em* 2em* 2em* 2em*; padding 1em* 1em* 1em* 1em*\n"
            " p: margin -1.5em* 0* 0* -10px*\n"
            " p: margin 0* auto* 0* auto*\n"
            " p: margin 1em 0 1em auto*\n"
            " p: margin 0* 0* 0* 0*; padding 0* 0* 0* 0*\n"
            // `inherit` for what is an element's own is as nothing said: the element's.
            " blockquote: margin 1em 40px 1em 40px\n"
            // What is part of a line has them as said, for whoever lays it out to use or not.
            " i: margin 1em* 2px* 3%* 0*\n"
            " p: margin 1em 0 1em 96px*; padding 0 0 0 96px*\n"
            // Each amount is held to what sixteen bits take.
            " p: margin 4095.875px* 0 1em -4096px*\n"
            // And is to the nearest of what is kept, a 64th of an em or of a percent and an 8th of a
            // pixel: just over half of one is one, and just under is none.
            " i: margin 0.015625em* 0 0 0.5%*; padding 0.125px* 0 0 0\n"
            " i: margin 0* 0 0 0; padding 0* 0 0 0\n");
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kInheritOfOwn), 5U);
}

TEST(StyleStack, TheMarginsAndPaddingOfThePageAreLeftOutAndCounted) {
  const Walked walked = Walk("margin padding",
                             "html { margin: 1em; padding: 0 } body { margin: 0 5% 2em; padding-left: 10px } "
                             "div { margin: 5% }",
                             "<html><body><div/></body></html>");
  EXPECT_EQ(walked.lines, "html\n body\n  div: margin 5%* 5%* 5%* 5%*\n");
  // Four of `html`'s margin, three of `body`'s that are not nothing, and one padding.
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kPageMargin), 8U);
  // A page that sets them all to nothing leaves nothing out.
  EXPECT_EQ(
      LeftOut(Walk("", "html, body { margin: 0; padding: 0 }", "<html><body/></html>"), StyleLeftOut::kPageMargin), 0U);
}

TEST(StyleStack, TellsOfAWidthAHeightAndTheSideAnElementIsFloatedTo) {
  const Walked walked =
      Walk("width height float",
           ".w { width: 75%; height: 2em; font-size: 2em } .a { width: auto; height: auto } .l { float: left } "
           ".r { float: right } .n { float: none } .same { width: inherit; height: inherit; float: inherit } "
           ".zero { width: 0 }",
           "<d><p class='w'/><p class='a'/><i class='l'/><p class='r'/><i class='n'/><p class='same'/><i "
           "class='zero'/></d>");
  EXPECT_EQ(walked.lines,
            "d\n"
            " p: width 75%; height 4em\n"
            " p\n"
            " i: float left\n"
            " p: float right\n"
            " i\n"
            " p\n"
            " i: width 0\n");
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kInheritOfOwn), 3U);
}

TEST(StyleStack, TellsWhereAPageMayEndAndKeepsAHeadingWithWhatFollows) {
  const Walked walked =
      Walk("breaks",
           ".bb { page-break-before: always } .ba { page-break-after: always } .ka { break-after: avoid } "
           ".ki { break-inside: avoid } .ab { break-before: avoid } .auto { break-before: auto; break-after: auto; "
           "break-inside: auto } .same { break-before: inherit; break-after: inherit; break-inside: inherit } "
           ".left { page-break-before: left }",
           "<d><p class='bb'/><p class='ba'/><p class='ka'/><p class='ki'/><p class='ab'/><p class='auto'/><h1/>"
           "<h2 class='auto'/><h3 class='ba'/><h4 class='ka'/><h5 class='same'/><p class='same'/><p class='left'/>"
           "<p class='bb ba ki'/></d>");
  EXPECT_EQ(walked.lines,
            "d\n"
            " p: breaks before\n"
            " p: breaks after not-with-next*\n"
            " p: breaks keep-with-next*\n"
            " p: breaks keep-together\n"
            " p\n"
            " p: breaks not-with-next*\n"
            // A heading is kept with what follows it, by being one.
            " h1: breaks keep-with-next\n"
            // Unless the book says otherwise.
            " h2: breaks not-with-next*\n"
            " h3: breaks after not-with-next*\n"
            " h4: breaks keep-with-next*\n"
            " h5: breaks keep-with-next\n"
            " p\n"
            " p: breaks before\n"
            " p: breaks before after not-with-next* keep-together\n");
  // "No page may end before this" is kept by nothing.
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kBreakAvoidBefore), 1U);
  EXPECT_EQ(LeftOut(walked, StyleLeftOut::kInheritOfOwn), 6U);
}

TEST(StyleStack, AFirstLetterIsSetAsItsElementsTextThenAsItsOwnRuleSays) {
  EXPECT_EQ(Styles("size text letter",
                   "p { font-size: 2em; font-style: italic } p.drop::first-letter { float: left; font-size: 3em; "
                   "font-weight: bold; font-family: Georgia; line-height: 1 } "
                   "p.plain::first-letter { font-variant: small-caps; font-style: normal } "
                   "p.right::first-letter { float: right; font-size: inherit; vertical-align: super } "
                   "p.none::first-letter { float: none; text-decoration: underline; font-size: 1rem }",
                   "<html><p class='drop'><i/></p><p class='plain'/><p class='right'/><p class='none'/><p/></html>"),
            "html\n"
            // Three times its element's size, and its element's slant with it.
            " p: size 2000 book; text italic; letter floated, size 6000 book, text bold italic, book bold italic, "
            "family serif book\n"
            // Which is the element's alone, and nothing of what is inside it.
            "  i: size 2000 book; text italic\n"
            " p: size 2000 book; text italic; letter set, size 2000 book, text small-caps, book italic small-caps\n"
            " p: size 2000 book; text italic; letter floated, size 2000 book, text italic raised, "
            "book italic raised lowered\n"
            " p: size 2000 book; text italic; letter set, size 1000 book, text italic underline, "
            "book italic underline\n"
            " p: size 2000 book; text italic\n");
}

TEST(StyleStack, TellsWhatTheBookSaidApartFromWhatTheElementGives) {
  // The same things by the element alone, by the book alone, and by the
  // book over the element.
  EXPECT_EQ(Styles("",
                   "h1.x { font-size: 2em; font-weight: bold; margin: 0.67em 0; text-align: center; "
                   "break-after: avoid } p.x { margin: 1em 0; text-indent: 0; line-height: normal } "
                   "code.x { font-family: monospace } li.x { list-style-type: disc }",
                   "<d><h1/><h1 class='x'/><p/><p class='x'/><code/><code class='x'/><li/><li class='x'/></d>"),
            "d\n"
            " h1: display block; size 2000; text bold; margin 1.34375em 0 1.34375em 0; breaks keep-with-next\n"
            " h1: display block; size 2000 book; text bold; book bold; align center book; "
            "margin 1.34375em* 0* 1.34375em* 0*; breaks keep-with-next*\n"
            " p: display block; margin 1em 0 1em 0\n"
            " p: display block; indent 0 book; lines normal book; margin 1em* 0* 1em* 0*\n"
            " code: family mono\n"
            " code: family mono book\n"
            " li: display list-item; marker disc\n"
            " li: display list-item; marker disc book\n");
}

TEST(StyleStack, KeepsSoManyOpenElementsAndGivesADeeperOneWhatTheDeepestKeptGives) {
  const std::string_view sheet = "a { font-size: 2em } q { font-style: italic } c { display: none }";
  const std::string_view document = "<a><q><a><a><i/><c><u><u/></u><b/></c><u/></a><a/></a><a/></q><q/></a>";
  // With room for all of them.
  EXPECT_EQ(Walk("display size text", sheet, document).lines,
            "a: size 2000 book\n"
            " q: size 2000 book; text italic\n"
            "  a: size 4000 book; text italic\n"
            "   a: size 8000 book; text italic\n"
            "    i: size 8000 book; text italic\n"
            "    c: display none\n"
            "     u: display none\n"
            "      u: display none\n"
            "     b: display none\n"
            "    u: size 8000 book; text italic underline\n"
            "   a: size 8000 book; text italic\n"
            "  a: size 4000 book; text italic\n"
            " q: size 2000 book; text italic\n");
  // With room for two: what is deeper is as if it were straight inside
  // the second, and what is nothing still hides all inside it, until it
  // closes.
  const Walked two = Walk("display size text", sheet, document, 2);
  EXPECT_EQ(two.lines,
            "a: size 2000 book\n"
            " q: size 2000 book; text italic\n"
            "  a: size 4000 book; text italic\n"
            "   a: size 4000 book; text italic\n"
            "    i: size 2000 book; text italic\n"
            "    c: display none\n"
            "     u: display none\n"
            "      u: display none\n"
            "     b: display none\n"
            "    u: size 2000 book; text italic underline\n"
            "   a: size 4000 book; text italic\n"
            "  a: size 4000 book; text italic\n"
            " q: size 2000 book; text italic\n");
  // Ten of its thirteen elements are deeper than the second.
  EXPECT_EQ(LeftOut(two, StyleLeftOut::kTooDeep), 10U);
  EXPECT_EQ(LeftOut(Walk("", sheet, document), StyleLeftOut::kTooDeep), 0U);
}

TEST(StyleStack, WithNoMemoryEveryElementIsAsOneInsideNothing) {
  std::array<std::uint8_t, StyleStack::kLanguagesBytes + StyleStack::kLevelBytes - 1> memory{};
  StyleStack stack(memory);
  EXPECT_EQ(stack.Levels(), 0U);
  stack.Begin();
  const bn::epub::CssStyle nothing;
  const std::array<const char*, 3> pairs = {"lang", "en", nullptr};
  ElementStyle said;
  stack.Open("b", bn::epub::XmlAttributes(pairs.data()), nothing, nothing, said);
  EXPECT_EQ(said.text.bits, bn::epub::kTextBold);
  // No language is kept, and it is counted.
  EXPECT_EQ(said.text.language, 0);
  EXPECT_EQ(stack.Languages(), 0U);
  EXPECT_EQ(stack.Language(1), "");
  EXPECT_EQ(stack.Language(0), "");
  stack.Open("i", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  EXPECT_EQ(said.text.bits, bn::epub::kTextItalic) << "not bold: the element outside was not kept";
  EXPECT_EQ(stack.Depth(), 2U);
  EXPECT_EQ(stack.LeftOut(StyleLeftOut::kTooDeep), 2U);
  EXPECT_EQ(stack.LeftOut(StyleLeftOut::kLanguage), 1U);
  stack.Close();
  stack.Close();
  EXPECT_EQ(stack.Depth(), 0U);

  // With memory for just one, one is kept.
  std::array<std::uint8_t, StyleStack::kLanguagesBytes + StyleStack::kLevelBytes> enough{};
  StyleStack one(enough);
  EXPECT_EQ(one.Levels(), 1U);
  StyleStack none(std::span<std::uint8_t>{});
  EXPECT_EQ(none.Levels(), 0U);
}

TEST(StyleStack, ADocumentBegunStartsFromNothing) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (4 * StyleStack::kLevelBytes));
  StyleStack stack(memory);
  stack.Begin();
  const bn::epub::CssStyle sized = Setting(bn::epub::CssProperty::kFontSize, 2000, bn::epub::CssUnit::kEm);
  const bn::epub::CssStyle inherits = Setting(bn::epub::CssProperty::kDisplay, bn::epub::CssKeyword::kInherit);
  const bn::epub::CssStyle nothing;
  const std::array<const char*, 3> pairs = {"lang", "en", nullptr};
  ElementStyle said;
  stack.Open("html", bn::epub::XmlAttributes(pairs.data()), sized, nothing, said);
  stack.Open("b", bn::epub::XmlAttributes(nullptr), inherits, nothing, said);
  EXPECT_EQ(said.text.size, 2000);
  EXPECT_EQ(stack.Languages(), 1U);
  EXPECT_EQ(stack.LeftOut(StyleLeftOut::kInheritOfOwn), 1U);
  // And one that is nothing, deeper than is kept.
  const bn::epub::CssStyle gone = Setting(bn::epub::CssProperty::kDisplay, bn::epub::CssKeyword::kNone);
  stack.Open("i", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  stack.Open("u", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  stack.Open("s", bn::epub::XmlAttributes(nullptr), gone, nothing, said);
  stack.Open("b", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  EXPECT_EQ(said.display, StyleDisplay::kNone);
  EXPECT_EQ(stack.Depth(), 6U);

  // Another document, with those elements still open.
  stack.Begin();
  EXPECT_EQ(stack.Depth(), 0U);
  EXPECT_EQ(stack.Languages(), 0U);
  EXPECT_EQ(stack.LeftOut(StyleLeftOut::kInheritOfOwn), 0U);
  // A rem is of the reader's size again, not of the last document's.
  const bn::epub::CssStyle rem = Setting(bn::epub::CssProperty::kFontSize, 1500, bn::epub::CssUnit::kRem);
  stack.Open("p", bn::epub::XmlAttributes(nullptr), rem, nothing, said);
  EXPECT_EQ(said.display, StyleDisplay::kBlock);
  EXPECT_EQ(said.text.size, 1500);
  EXPECT_EQ(said.text.bits, 0);
  EXPECT_EQ(said.text.language, 0);
}

TEST(StyleStack, ARemOfAnOutermostElementIsOfTheReadersSize) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (4 * StyleStack::kLevelBytes));
  StyleStack stack(memory);
  stack.Begin();
  const bn::epub::CssStyle twice = Setting(bn::epub::CssProperty::kFontSize, 2000, bn::epub::CssUnit::kEm);
  const bn::epub::CssStyle rem = Setting(bn::epub::CssProperty::kFontSize, 1500, bn::epub::CssUnit::kRem);
  const bn::epub::CssStyle nothing;
  ElementStyle said;
  stack.Open("a", bn::epub::XmlAttributes(nullptr), twice, nothing, said);
  stack.Open("a", bn::epub::XmlAttributes(nullptr), rem, nothing, said);
  EXPECT_EQ(said.text.size, 3000);
  stack.Close();
  stack.Close();
  // Whatever an outermost element before it came to.
  stack.Open("a", bn::epub::XmlAttributes(nullptr), rem, nothing, said);
  EXPECT_EQ(said.text.size, 1500);
  stack.Open("a", bn::epub::XmlAttributes(nullptr), rem, nothing, said);
  EXPECT_EQ(said.text.size, 2250);
}

TEST(StyleStack, GoesOnceThroughAnElementsAttributesAndTheLanguagesItKeeps) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (4 * StyleStack::kLevelBytes));
  StyleStack stack(memory);
  stack.Begin();
  const bn::epub::CssStyle nothing;
  ElementStyle said;
  const auto open = [&](std::string_view name, std::vector<const char*> pairs) {
    pairs.push_back(nullptr);
    stack.Open(name, bn::epub::XmlAttributes(pairs.data()), nothing, nothing, said);
    return stack.Work();
  };
  EXPECT_EQ(open("div", {}), 0U);
  // Three attributes, and no language kept yet to hold the tag against.
  EXPECT_EQ(open("p", {"id", "a", "lang", "en", "class", "b"}), 3U);
  // One attribute, and the one language that is kept; then that one found.
  EXPECT_EQ(open("i", {"lang", "de"}), 2U);
  EXPECT_EQ(open("i", {"lang", "EN"}), 2U);
  EXPECT_EQ(open("i", {"lang", "de", "dir", "rtl"}), 4U);
  // An attribute that names no language has none looked at.
  EXPECT_EQ(open("b", {"dir", "ltr"}), 1U);
  for (int i = 0; i < 5; ++i) {
    stack.Close();
  }

  // What is nothing has its attributes looked at, to find that out; what
  // is inside it has not, and names no language.
  EXPECT_EQ(open("p", {"hidden", "", "lang", "fr"}), 2U);
  EXPECT_EQ(said.display, StyleDisplay::kNone);
  EXPECT_EQ(open("b", {"lang", "it", "dir", "rtl", "align", "left"}), 0U);
  EXPECT_EQ(stack.Languages(), 2U);
  stack.Close();
  stack.Close();

  // The most: every language there is room for, and none of them the one
  // an element names.
  for (std::size_t i = stack.Languages(); i < StyleStack::kMaxLanguages; ++i) {
    const std::string tag = "l" + std::to_string(i);
    open("p", {"lang", tag.c_str()});
    stack.Close();
  }
  EXPECT_EQ(stack.Languages(), StyleStack::kMaxLanguages);
  EXPECT_EQ(open("p", {"a", "", "b", "", "c", "", "lang", "one-more"}), StyleStack::MostWorkFor(4));
  EXPECT_EQ(StyleStack::MostWorkFor(4), 4 + StyleStack::kMaxLanguages);
  EXPECT_EQ(StyleStack::MostWorkFor(0), StyleStack::kMaxLanguages);
}

TEST(StyleStack, ReadsAnAttributeByItsWholeNameAsItIsWritten) {
  // Not by how a name begins, and not in letters of another case.
  EXPECT_EQ(Styles("", "",
                   "<d><p langs='de' xml:language='de' dirs='rtl' aligned='right' hiddens='' LANG='de' Dir='rtl'/>"
                   "<ol types='a' TYPE='a'><li/></ol></d>"),
            "d\n"
            " p: display block; margin 1em 0 1em 0\n"
            " ol: display block; margin 1em 0 1em 0; padding 0 0 0 40px\n"
            "  li: display list-item; marker decimal\n");
}

TEST(StyleStack, TakesOfTwoAttributesOfANameTheFirst) {
  // A document cannot have them; whoever hands attributes over might.
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (2 * StyleStack::kLevelBytes));
  StyleStack stack(memory);
  stack.Begin();
  const bn::epub::CssStyle nothing;
  const std::array<const char*, 9> pairs = {"dir", "rtl", "dir", "ltr", "align", "right", "align", "left", nullptr};
  ElementStyle said;
  stack.Open("p", bn::epub::XmlAttributes(pairs.data()), nothing, nothing, said);
  EXPECT_TRUE(said.rightToLeft);
  EXPECT_EQ(said.align, bn::epub::StyleAlign::kRight);
}

TEST(StyleStack, AnElementClosedThatWasNeverOpenedIsNothing) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (2 * StyleStack::kLevelBytes));
  StyleStack stack(memory);
  stack.Begin();
  stack.Close();
  EXPECT_EQ(stack.Depth(), 0U);
  const bn::epub::CssStyle nothing;
  ElementStyle said;
  stack.Open("b", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  stack.Close();
  stack.Close();
  EXPECT_EQ(stack.Depth(), 0U);
  stack.Open("i", bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  EXPECT_EQ(said.text.bits, bn::epub::kTextItalic);
}

TEST(StyleStack, NamesWhatItTellsAndWhatItLeavesOut) {
  EXPECT_STREQ(bn::epub::StyleDisplayName(StyleDisplay::kNone), "none");
  EXPECT_STREQ(bn::epub::StyleDisplayName(StyleDisplay::kInline), "inline");
  EXPECT_STREQ(bn::epub::StyleDisplayName(StyleDisplay::kBlock), "block");
  EXPECT_STREQ(bn::epub::StyleDisplayName(StyleDisplay::kListItem), "list-item");
  EXPECT_STREQ(bn::epub::StyleAlignName(bn::epub::StyleAlign::kUnset), "unset");
  EXPECT_STREQ(bn::epub::StyleAlignName(bn::epub::StyleAlign::kEnd), "end");
  EXPECT_STREQ(bn::epub::StyleMarkerName(bn::epub::StyleMarker::kNone), "none");
  EXPECT_STREQ(bn::epub::StyleMarkerName(bn::epub::StyleMarker::kDecimalLeadingZero), "decimal-leading-zero");
  EXPECT_STREQ(bn::epub::StyleMarkerName(bn::epub::StyleMarker::kUpperRoman), "upper-roman");
  std::vector<std::string> names;
  for (std::size_t kind = 0; kind < bn::epub::kStyleLeftOutKinds; ++kind) {
    names.emplace_back(bn::epub::StyleLeftOutName(static_cast<StyleLeftOut>(kind)));
    EXPECT_NE(names.back(), "?");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end()) << "each kind has a name of its own";
}

}  // namespace
