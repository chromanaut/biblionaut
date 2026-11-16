#include "epub/css.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"

namespace {

using bn::epub::CssAttributeTest;
using bn::epub::CssCombinator;
using bn::epub::CssDropped;
using bn::epub::CssReader;
using bn::epub::CssStatus;
using bn::epub::CssValue;

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

// Writes down everything a reader tells of, a line each:
//
//   {                  a rule begins
//   S a,b,c selector   a selector, after its counts of ids, classes and elements
//   D property: value  something the rule sets
//   }                  the rule ends
//   I url              an @import
//   X kind: text       something passed over
class Recorder final : public bn::epub::CssHandler {
 public:
  void OnRuleBegin() noexcept override {
    lines += "{\n";
    outOfOrder = outOfOrder || inRule;
    inRule = true;
    declared = false;
    ++begun;
  }
  void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
    // Selectors come inside a rule, before anything it sets.
    outOfOrder = outOfOrder || !inRule || declared;
    std::string text;
    for (const bn::epub::CssCompound& part : selector.compounds) {
      text += part.combinator == CssCombinator::kDescendant ? " "
              : part.combinator == CssCombinator::kChild    ? " > "
              : part.combinator == CssCombinator::kAdjacent ? " + "
                                                            : "";
      if (part.element.empty() && part.id.empty() && part.classes.empty() && part.attributes.empty()) {
        text += '*';
      }
      text += part.element;
      if (!part.id.empty()) {
        text += "#" + std::string(part.id);
      }
      for (const std::string_view name : part.classes) {
        text += "." + std::string(name);
      }
      // What is asked of an attribute: [name], or [name="value"] with the
      // sign for how the value is held against it.
      for (const bn::epub::CssAttribute& attribute : part.attributes) {
        text += std::string("[") + (attribute.anyPrefix ? "*|" : "") + std::string(attribute.name);
        if (attribute.test != CssAttributeTest::kPresent) {
          text += std::string(bn::epub::CssAttributeTestName(attribute.test)) + "\"" + std::string(attribute.value) +
                  "\"" + (attribute.anyCase ? " i" : "");
        }
        text += ']';
      }
    }
    if (selector.firstLetter) {
      text += " ::first-letter";
    }
    const std::uint32_t rank = selector.specificity;
    lines += "S " + std::to_string(rank >> 16U) + "," + std::to_string((rank >> 8U) & 0xFFU) + "," +
             std::to_string(rank & 0xFFU) + " " + text + "\n";
    ++selectors;
  }
  void OnDeclaration(const bn::epub::CssDeclaration& declaration) noexcept override {
    declared = true;
    std::string value;
    if (declaration.value.kind == CssValue::Kind::kKeyword) {
      value = bn::epub::CssKeywordName(declaration.value.keyword);
    } else if (declaration.value.kind == CssValue::Kind::kLength) {
      value = Number(declaration.value.number) + bn::epub::CssUnitName(declaration.value.unit);
    } else {
      std::array<char, 64> name{};
      std::string_view rest = declaration.value.families;
      for (std::string_view one = bn::epub::CssNextFamily(rest, name); !one.empty();
           one = bn::epub::CssNextFamily(rest, name)) {
        value += (value.empty() ? "" : ", ") + std::string(one);
      }
    }
    lines += std::string("D ") + bn::epub::CssPropertyName(declaration.property) + ": " + value +
             (declaration.important ? " !important" : "") + "\n";
  }
  void OnRuleEnd() noexcept override {
    lines += "}\n";
    outOfOrder = outOfOrder || !inRule;
    inRule = false;
    ++ended;
  }
  void OnImport(std::string_view url) noexcept override {
    lines += "I " + std::string(url) + "\n";
    outOfOrder = outOfOrder || inRule;
  }
  void OnDropped(CssDropped dropped, std::string_view text) noexcept override {
    lines += std::string("X ") + bn::epub::CssDroppedName(dropped) + ": " + std::string(text.substr(0, 60)) + "\n";
  }

  std::string lines;
  int selectors = 0;
  std::uint32_t begun = 0;  // Rules begun, and rules ended.
  std::uint32_t ended = 0;
  bool inRule = false;
  bool declared = false;    // The rule in hand has set something.
  bool outOfOrder = false;  // Something was told at a time it cannot be.
};

// A source that gives out its bytes a few at a time, and counts.
class PiecewiseSource final : public bn::core::ByteSource {
 public:
  PiecewiseSource(std::string_view data, std::size_t pieceBytes) : data_(data), pieceBytes_(pieceBytes) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    ++reads;
    if (failAfterBytes >= 0 && position_ >= static_cast<std::size_t>(failAfterBytes)) {
      return kError;
    }
    const std::size_t count = std::min({dst.size(), pieceBytes_, data_.size() - position_});
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(position_), count, dst.begin());
    position_ += count;
    return static_cast<std::ptrdiff_t>(count);
  }

  std::size_t Taken() const { return position_; }

  int reads = 0;
  std::ptrdiff_t failAfterBytes = -1;  // Fails once this many bytes have been given. -1: never.

 private:
  std::string_view data_;
  std::size_t pieceBytes_;
  std::size_t position_ = 0;
};

// What a reader says of `sheet`, read `pieceBytes` at a time.
std::string Read(std::string_view sheet, std::size_t pieceBytes = 4096) {
  PiecewiseSource source(sheet, pieceBytes);
  Recorder recorder;
  CssReader reader;
  EXPECT_EQ(reader.Read(source, recorder), CssStatus::kOk);
  EXPECT_TRUE(reader.Finished());
  return recorder.lines;
}

// What it says of one declaration, in a rule of its own.
std::string Declared(std::string_view declaration) {
  const std::string lines = Read("a{" + std::string(declaration) + "}");
  const std::string_view begin = "{\nS 0,0,1 a\n";
  const std::string_view end = "}\n";
  EXPECT_TRUE(lines.starts_with(begin) && lines.ends_with(end)) << lines;
  return lines.substr(begin.size(), lines.size() - begin.size() - end.size());
}

TEST(CssReader, TellsOfARulesSelectorsAndWhatItSets) {
  EXPECT_EQ(Read("p.first, h1 { text-indent: 0; font-weight: bold }\n.note { margin-left: 2em; }"),
            "{\n"
            "S 0,1,1 p.first\n"
            "S 0,0,1 h1\n"
            "D text-indent: 0px\n"
            "D font-weight: bold\n"
            "}\n"
            "{\n"
            "S 0,1,0 .note\n"
            "D margin-left: 2em\n"
            "}\n");
  EXPECT_EQ(Read(""), "");
  EXPECT_EQ(Read("   \n\t "), "");
  EXPECT_EQ(Read("p{}"), "{\nS 0,0,1 p\n}\n");
}

TEST(CssReader, ReadsSelectorsByElementClassAndIdJoinedByDescentOrAsChild) {
  struct Checker final : bn::epub::CssHandler {
    void OnRuleBegin() noexcept override {}
    void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
      ASSERT_EQ(selector.compounds.size(), 4U);
      EXPECT_EQ(selector.compounds[0].combinator, CssCombinator::kFirst);
      EXPECT_EQ(selector.compounds[0].element, "div");
      EXPECT_EQ(selector.compounds[0].id, "main");
      EXPECT_TRUE(selector.compounds[0].classes.empty());
      EXPECT_EQ(selector.compounds[1].combinator, CssCombinator::kDescendant);
      EXPECT_EQ(selector.compounds[1].element, "");
      ASSERT_EQ(selector.compounds[1].classes.size(), 2U);
      EXPECT_EQ(selector.compounds[1].classes[0], "a");
      EXPECT_EQ(selector.compounds[1].classes[1], "b");
      EXPECT_EQ(selector.compounds[2].combinator, CssCombinator::kChild);
      EXPECT_EQ(selector.compounds[2].element, "p");
      EXPECT_EQ(selector.compounds[3].combinator, CssCombinator::kChild);
      EXPECT_EQ(selector.compounds[3].element, "");
      EXPECT_EQ(selector.compounds[3].id, "");
      EXPECT_TRUE(selector.compounds[3].classes.empty());
      EXPECT_FALSE(selector.firstLetter);
      EXPECT_EQ(selector.specificity, (1U << 16U) | (2U << 8U) | 2U);
      ++seen;
    }
    void OnDeclaration(const bn::epub::CssDeclaration& /*declaration*/) noexcept override {}
    void OnRuleEnd() noexcept override {}
    void OnImport(std::string_view /*url*/) noexcept override {}
    void OnDropped(CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}
    int seen = 0;
  };
  Checker checker;
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>("div#main .a.b>p  >  * {}"), 24});
  CssReader reader;
  EXPECT_EQ(reader.Read(source, checker), CssStatus::kOk);
  EXPECT_EQ(checker.seen, 1);

  // Spaces and line ends around the parts make no difference.
  EXPECT_EQ(Read("\n div\t#main\n>p.x { }"), "{\nS 1,1,2 div #main > p.x\n}\n");
}

TEST(CssReader, ReadsTheElementStraightAfterAnother) {
  EXPECT_EQ(Read("h2 + p, h1+p.first, div > h2 + p i, a+b+c {}"),
            "{\nS 0,0,2 h2 + p\nS 0,1,2 h1 + p.first\nS 0,0,4 div > h2 + p i\nS 0,0,3 a + b + c\n}\n");
  // The sign is not part of a number here, nor of anything in brackets.
  EXPECT_EQ(Read("li:nth-child(2n+1), [title=\"a+b\"] {}"),
            "X pseudo-class: :nth-child\n{\nS 0,1,0 [title=\"a+b\"]\n}\n");
  // It joins two parts, and has to have both.
  for (const char* bad : {"+ p", "p +", "p + + q", "p + > q", "p > + q"}) {
    EXPECT_EQ(Read(std::string(bad) + " , b {}"), "X selector: " + std::string(bad) + "\n{\nS 0,0,1 b\n}\n") << bad;
  }
}

TEST(CssReader, ReadsWhatASelectorAsksOfAnAttribute) {
  struct Checker final : bn::epub::CssHandler {
    void OnRuleBegin() noexcept override {}
    void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
      ASSERT_EQ(selector.compounds.size(), 2U);
      const bn::epub::CssCompound& section = selector.compounds[0];
      EXPECT_EQ(section.element, "section");
      ASSERT_EQ(section.attributes.size(), 2U);
      EXPECT_EQ(section.attributes[0].name, "epub:type");
      EXPECT_EQ(section.attributes[0].test, CssAttributeTest::kWord);
      EXPECT_EQ(section.attributes[0].value, "chapter");
      EXPECT_FALSE(section.attributes[0].anyCase);
      EXPECT_FALSE(section.attributes[0].anyPrefix);
      EXPECT_EQ(section.attributes[1].name, "hidden");
      EXPECT_EQ(section.attributes[1].test, CssAttributeTest::kPresent);
      EXPECT_EQ(section.attributes[1].value, "");
      const bn::epub::CssCompound& last = selector.compounds[1];
      EXPECT_EQ(last.combinator, CssCombinator::kChild);
      EXPECT_EQ(last.element, "");
      EXPECT_TRUE(last.classes.empty());
      ASSERT_EQ(last.attributes.size(), 1U);
      EXPECT_EQ(last.attributes[0].name, "lang");
      EXPECT_EQ(last.attributes[0].test, CssAttributeTest::kDash);
      EXPECT_EQ(last.attributes[0].value, "EN");
      EXPECT_TRUE(last.attributes[0].anyCase);
      EXPECT_TRUE(last.attributes[0].anyPrefix);
      // Each thing asked counts as a class does.
      EXPECT_EQ(selector.specificity, (3U << 8U) | 1U);
      ++seen;
    }
    void OnDeclaration(const bn::epub::CssDeclaration& /*declaration*/) noexcept override {}
    void OnRuleEnd() noexcept override {}
    void OnImport(std::string_view /*url*/) noexcept override {}
    void OnDropped(CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}
    int seen = 0;
  };
  Checker checker;
  const std::string_view sheet = "section[epub|type~=\"chapter\"][hidden] > [*|lang|=EN i] {}";
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>(sheet.data()), sheet.size()});
  CssReader reader;
  EXPECT_EQ(reader.Read(source, checker), CssStatus::kOk);
  EXPECT_EQ(checker.seen, 1);

  // Every way of holding a value against an attribute's, with the value
  // in either kind of quotes or as a name, and spaces wherever they may be.
  EXPECT_EQ(Read("[a], [a=b], [a~='b'], [a|=\"b\"], [a^=b], [a$=b], [a*=b], [ a = b ], [a='b'i], [a=b S] {}"),
            "{\nS 0,1,0 [a]\nS 0,1,0 [a=\"b\"]\nS 0,1,0 [a~=\"b\"]\nS 0,1,0 [a|=\"b\"]\nS 0,1,0 [a^=\"b\"]\n"
            "S 0,1,0 [a$=\"b\"]\nS 0,1,0 [a*=\"b\"]\nS 0,1,0 [a=\"b\"]\nS 0,1,0 [a=\"b\" i]\nS 0,1,0 [a=\"b\"]\n}\n");
  // With the rest of a part, in any order; and a value may be empty.
  EXPECT_EQ(Read("a.x[href]#y[rel=\"\"]:link, td[colspan=\"2\"] p {}"),
            "{\nS 1,4,1 a#y.x[href][rel=\"\"]\nS 0,1,2 td[colspan=\"2\"] p\n}\n");
}

TEST(CssReader, WritesAnAttributesNameAsADocumentHasIt) {
  // In small letters, with a `:` after a namespace's prefix; `|` with no
  // prefix is no namespace, which is what a plain name is in; `*|` is any.
  EXPECT_EQ(Read("[EPUB|Type~=\"Note\"], [xml|lang], [|title], [*|HREF], [DATA-x=A] {}"),
            "{\nS 0,1,0 [epub:type~=\"Note\"]\nS 0,1,0 [xml:lang]\nS 0,1,0 [title]\nS 0,1,0 [*|href]\n"
            "S 0,1,0 [data-x=\"A\"]\n}\n");
  // A prefix is told from the sign that begins with the same mark.
  EXPECT_EQ(Read("[a|b|=c], [a|=c], [*|a|=c] {}"),
            "{\nS 0,1,0 [a:b|=\"c\"]\nS 0,1,0 [a|=\"c\"]\nS 0,1,0 [*|a|=\"c\"]\n}\n");
  // Backslashes are taken out of both halves before they are joined.
  EXPECT_EQ(Read(R"([\65 pub|t\79pe], [e\70 ub\:type] {})"), "{\nS 0,1,0 [epub:type]\nS 0,1,0 [epub:type]\n}\n");
}

TEST(CssReader, TakesTheQuotesAndBackslashesOutOfAnAttributesValue) {
  // Either kind of quotes, with the other kind inside as it is; a quote, a
  // character by its number and a line's end after a backslash; and
  // whatever else is inside quotes is only itself.
  EXPECT_EQ(Read(R"([a="x y"], [a='say "hi"'], [a="say \"hi\""], [a='\41 b\63'], [a="1\
2"], [a=\31 0], [a="]{,;"] {})"),
            "{\nS 0,1,0 [a=\"x y\"]\nS 0,1,0 [a=\"say \"hi\"\"]\nS 0,1,0 [a=\"say \"hi\"\"]\n"
            "S 0,1,0 [a=\"Abc\"]\nS 0,1,0 [a=\"12\"]\nS 0,1,0 [a=\"10\"]\nS 0,1,0 [a=\"]{,;\"]\n}\n");
}

TEST(CssReader, DropsBracketsThatDoNotHoldATestOfAnAttribute) {
  for (const char* bad :
       {"[]",     "[ ]",  "[a=]", "[a=1]",   "[=b]",   "[a b]", "[a=b c]", "[a=b ii]", "[a=b i i]",  "[a==b]", "[a~b]",
        "[a!=b]", "[1a]", "[a|]", "[a|b|c]", "[|a|b]", "[*|]",  "[*a]",    "[a='b'x]", "[a='b''c']", "p[a][]"}) {
    EXPECT_EQ(Read(std::string(bad) + " , b {}"), "X attribute selector: " + std::string(bad) + "\n{\nS 0,0,1 b\n}\n")
        << bad;
  }
  // What is dropped is told as the sheet had it, its name not yet made
  // small or joined.
  EXPECT_EQ(Read("[Epub|TYPE=\"x\" y], i {}"), "X attribute selector: [Epub|TYPE=\"x\" y]\n{\nS 0,0,1 i\n}\n");
  // Brackets that are not closed take the rest of the list with them, as a
  // comma inside them is not between two selectors.
  EXPECT_EQ(Read("[a , b {} i {}"), "X attribute selector: [a , b\nX rule: \n{\nS 0,0,1 i\n}\n");
  EXPECT_EQ(Read("p[a=b, b {} i {}"), "X attribute selector: p[a=b, b\nX rule: \n{\nS 0,0,1 i\n}\n");
  // A string ends with its line if it was not closed, and is then no
  // value, whether or not a quote comes later; nor is one that the text
  // ends in the middle of, after a backslash.
  EXPECT_EQ(Read("[a=\"b\n], b {} i {}"), "X attribute selector: [a=\"b\n], b\nX rule: \n{\nS 0,0,1 i\n}\n");
  EXPECT_EQ(Read("[a=\"b\nc\"]\n, i {}"), "X attribute selector: [a=\"b\nc\"]\n{\nS 0,0,1 i\n}\n");
  EXPECT_EQ(Read("[a=\"b\\ \n{} i {}"), "X attribute selector: [a=\"b\\\nX rule: \n{\nS 0,0,1 i\n}\n");
}

TEST(CssReader, WritesElementNamesSmallAndLeavesClassesAndIdsAsTheyAre) {
  EXPECT_EQ(Read("DIV.Note P#Top{}"), "{\nS 1,1,2 div.Note p#Top\n}\n");
}

TEST(CssReader, TakesTheBackslashesOutOfNames) {
  // A backslash before a character that could not otherwise be in a name;
  // and a character by its number, with the space that ends the number.
  EXPECT_EQ(Read(R"(.a\:b, .\31 0, #x\2e y, .caf\e9, .\1F600 z {})"),
            "{\nS 0,1,0 .a:b\nS 0,1,0 .10\nS 1,0,0 #x.y\nS 0,1,0 .caf\xC3\xA9\nS 0,1,0 .\xF0\x9F\x98\x80z\n}\n");
  // A number that is no character becomes the mark for one that is
  // missing; or a `?`, where it was written in fewer bytes than the mark
  // takes and there is nowhere to put it.
  EXPECT_EQ(Read(R"(.\0z, .x\0, .\0\0 y {})"), "{\nS 0,1,0 .?z\nS 0,1,0 .x?\nS 0,1,0 .?\xEF\xBF\xBDy\n}\n");
  EXPECT_EQ(Read(R"(.\0 a, .\D800 b, .\110000 c {})"),
            "{\nS 0,1,0 .\xEF\xBF\xBD"
            "a\nS 0,1,0 .\xEF\xBF\xBD"
            "b\nS 0,1,0 .\xEF\xBF\xBD"
            "c\n}\n");
}

TEST(CssReader, ReadsFirstLetterAndTakesLinkAsAlwaysSo) {
  EXPECT_EQ(Read("p::first-letter, p.x:first-letter, a:link, a.ref:LINK {}"),
            "{\nS 0,0,2 p ::first-letter\nS 0,1,2 p.x ::first-letter\nS 0,1,1 a\nS 0,2,1 a.ref\n}\n");
  // Nothing can come after it, in its own part of the selector or in
  // another.
  EXPECT_EQ(Read("p::first-letter span {}"), "X selector: p::first-letter span\nX rule: \n");
  EXPECT_EQ(
      Read("p::first-letter::first-letter, p::first-letter.x, p:first-letter:link, p::first-letter[x] {}"),
      "X selector: p::first-letter::first-letter\nX selector: p::first-letter.x\nX selector: p:first-letter:link\n"
      "X selector: p::first-letter[x]\nX rule: \n");
}

TEST(CssReader, DropsASelectorItDoesNotReadAndKeepsTheOthersOfItsRule) {
  EXPECT_EQ(Read("h1 ~ p, p[lang=], a:hover, p::before, li:nth-child(2n + 1), h2 ~ p, q:after, p { margin-top: 0 }"),
            "X sibling selector: h1 ~ p\n"
            "X attribute selector: p[lang=]\n"
            "X pseudo-class: :hover\n"
            "X pseudo-element: ::before\n"
            "X pseudo-class: :nth-child\n"
            "X sibling selector: h2 ~ p\n"
            "X pseudo-element: :after\n"
            "{\n"
            "S 0,0,1 p\n"
            "D margin-top: 0px\n"
            "}\n");
  // A comma inside brackets is not between two selectors.
  EXPECT_EQ(Read("p:not(.a, .b), [title=\"a,b\"] x, i {}"),
            "X pseudo-class: :not\n{\nS 0,1,1 [title=\"a,b\"] x\nS 0,0,1 i\n}\n");
  // What is no selector at all.
  for (const char* bad :
       {"> p", "p >", "p > > q", "p..a", ".5a", "#", "p#a#b", "svg|rect", "p*", "*p", "p$", "1a", ""}) {
    EXPECT_EQ(Read(std::string(bad) + " , b {}"), "X selector: " + std::string(bad) + "\n{\nS 0,0,1 b\n}\n") << bad;
  }
}

TEST(CssReader, DropsARuleNoneOfWhoseSelectorsCouldBeRead) {
  PiecewiseSource source("p ~ p, a:visited { margin: 0; color: red } b { font-style: italic }", 4096);
  Recorder recorder;
  CssReader reader;
  EXPECT_EQ(reader.Read(source, recorder), CssStatus::kOk);
  // Nothing of what it sets is told, or counted.
  EXPECT_EQ(recorder.lines,
            "X sibling selector: p ~ p\nX pseudo-class: :visited\nX rule: \n{\nS 0,0,1 b\nD font-style: italic\n}\n");
  EXPECT_EQ(reader.Rules(), 1U);
  EXPECT_EQ(reader.Declarations(), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kRule), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kProperty), 0U);
}

TEST(CssReader, DropsASelectorWithMorePartsClassesOrAttributesThanThereIsRoomFor) {
  std::string parts = "a";
  for (std::size_t i = 1; i < CssReader::kMaxCompounds; ++i) {
    parts += " > a";
  }
  EXPECT_EQ(Read(parts + "{}"), "{\nS 0,0,8 " + parts + "\n}\n");
  EXPECT_EQ(Read(parts + " b, i {}"), "X selector: " + (parts + " b").substr(0, 60) + "\n{\nS 0,0,1 i\n}\n");

  std::string classes = "p";
  for (std::size_t i = 0; i < CssReader::kMaxClasses; ++i) {
    classes += ".c";
  }
  EXPECT_EQ(Read(classes + "{}"), "{\nS 0,12,1 " + classes + "\n}\n");
  EXPECT_EQ(Read(classes + ".d, i {}"), "X selector: " + classes + ".d\n{\nS 0,0,1 i\n}\n");

  // What is asked of attributes is counted over the whole selector, as
  // classes are.
  std::string attributes = "p";
  for (std::size_t i = 0; i < CssReader::kMaxAttributes; ++i) {
    attributes += i % 2 == 0 ? "[a]" : " q[b]";
  }
  EXPECT_EQ(Read(attributes + "{}"), "{\nS 0,8,5 " + attributes + "\n}\n");
  EXPECT_EQ(Read(attributes + "[c], i {}"), "X selector: " + attributes + "[c]\n{\nS 0,0,1 i\n}\n");
}

TEST(CssReader, ReadsEachPropertyItKnows) {
  const std::vector<std::pair<const char*, const char*>> cases = {
      {"font-style: italic", "D font-style: italic\n"},
      {"font-style: oblique", "D font-style: oblique\n"},
      {"font-weight: bold", "D font-weight: bold\n"},
      {"font-weight: 600", "D font-weight: 600\n"},
      {"font-weight: lighter", "D font-weight: lighter\n"},
      {"font-variant: small-caps", "D font-variant: small-caps\n"},
      {"font-family: Georgia, \"Times New Roman\" , serif", "D font-family: georgia, times new roman, serif\n"},
      {"font-size: 1.25em", "D font-size: 1.25em\n"},
      {"font-size: x-small", "D font-size: x-small\n"},
      {"font-size: larger", "D font-size: larger\n"},
      {"font-size: 90%", "D font-size: 90%\n"},
      {"line-height: 1.4", "D line-height: 1.4\n"},
      {"line-height: 120%", "D line-height: 120%\n"},
      {"line-height: normal", "D line-height: normal\n"},
      {"text-align: justify", "D text-align: justify\n"},
      {"text-align: start", "D text-align: start\n"},
      {"text-indent: -1.5em", "D text-indent: -1.5em\n"},
      {"text-decoration: line-through", "D text-decoration: line-through\n"},
      {"text-decoration: none", "D text-decoration: none\n"},
      {"text-decoration: underline dotted red", "D text-decoration: underline\n"},
      {"text-decoration-line: overline", "D text-decoration: overline\n"},
      {"vertical-align: super", "D vertical-align: super\n"},
      {"vertical-align: -0.25em", "D vertical-align: -0.25em\n"},
      {"vertical-align: text-bottom", "D vertical-align: text-bottom\n"},
      {"margin-top: auto", "D margin-top: auto\n"},
      {"margin-right: -2px", "D margin-right: -2px\n"},
      {"margin-bottom: 5%", "D margin-bottom: 5%\n"},
      {"margin-left: 1rem", "D margin-left: 1rem\n"},
      {"padding-top: 0", "D padding-top: 0px\n"},
      {"padding-right: .5em", "D padding-right: 0.5em\n"},
      {"padding-bottom: 3px", "D padding-bottom: 3px\n"},
      {"padding-left: 10%", "D padding-left: 10%\n"},
      {"display: none", "D display: none\n"},
      {"display: inline-block", "D display: inline-block\n"},
      {"display: list-item", "D display: list-item\n"},
      {"float: left", "D float: left\n"},
      {"float: none", "D float: none\n"},
      {"page-break-before: always", "D break-before: always\n"},
      {"page-break-before: right", "D break-before: always\n"},
      {"page-break-after: avoid", "D break-after: avoid\n"},
      {"page-break-inside: avoid", "D break-inside: avoid\n"},
      {"break-before: page", "D break-before: always\n"},
      {"break-after: auto", "D break-after: auto\n"},
      {"break-inside: avoid-page", "D break-inside: avoid\n"},
      {"width: 50%", "D width: 50%\n"},
      {"height: auto", "D height: auto\n"},
      // In capitals, and with space about the colon.
      {"FONT-STYLE :\tITALIC", "D font-style: italic\n"},
      {"Margin-Left:2EM", "D margin-left: 2em\n"},
      // The words every property takes.
      {"font-size: inherit", "D font-size: inherit\n"},
      {"display: initial", "D display: initial\n"},
      {"text-align: unset", "D text-align: inherit\n"},
  };
  for (const auto& [declaration, lines] : cases) {
    EXPECT_EQ(Declared(declaration), lines) << declaration;
  }
}

TEST(CssReader, TakesMarginAndPaddingApartIntoTheFourSides) {
  EXPECT_EQ(Declared("margin: 1em"),
            "D margin-top: 1em\nD margin-right: 1em\nD margin-bottom: 1em\nD margin-left: 1em\n");
  EXPECT_EQ(Declared("margin: 1em auto"),
            "D margin-top: 1em\nD margin-right: auto\nD margin-bottom: 1em\nD margin-left: auto\n");
  EXPECT_EQ(Declared("margin: 1px 2px 3px"),
            "D margin-top: 1px\nD margin-right: 2px\nD margin-bottom: 3px\nD margin-left: 2px\n");
  EXPECT_EQ(Declared("padding: 1px 2px 3px 4px"),
            "D padding-top: 1px\nD padding-right: 2px\nD padding-bottom: 3px\nD padding-left: 4px\n");
  EXPECT_EQ(Declared("margin: inherit"),
            "D margin-top: inherit\nD margin-right: inherit\nD margin-bottom: inherit\nD margin-left: inherit\n");
  // All of it is read, or none.
  EXPECT_EQ(Declared("margin: 1px 2px 3px 4px 5px"), "X value: margin: 1px 2px 3px 4px 5px\n");
  EXPECT_EQ(Declared("margin: 1px red"), "X value: margin: 1px red\n");
  EXPECT_EQ(Declared("margin: 1px inherit"), "X value: margin: 1px inherit\n");
  EXPECT_EQ(Declared("padding: 1px -2px"), "X value: padding: 1px -2px\n");
  EXPECT_EQ(Declared("padding: auto"), "X value: padding: auto\n");
  EXPECT_EQ(Declared("margin:"), "X value: margin:\n");
}

TEST(CssReader, TakesFontApart) {
  EXPECT_EQ(Declared("font: 12px serif"),
            "D font-style: normal\nD font-variant: normal\nD font-weight: normal\nD font-size: 12px\n"
            "D line-height: normal\nD font-family: serif\n");
  EXPECT_EQ(Declared("font: italic small-caps bold 1.2em/1.5 \"Gill Sans\", sans-serif"),
            "D font-style: italic\nD font-variant: small-caps\nD font-weight: bold\nD font-size: 1.2em\n"
            "D line-height: 1.5\nD font-family: gill sans, sans-serif\n");
  EXPECT_EQ(Declared("font: 700 normal large/120% Georgia"),
            "D font-style: normal\nD font-variant: normal\nD font-weight: 700\nD font-size: large\n"
            "D line-height: 120%\nD font-family: georgia\n");
  EXPECT_EQ(Declared("font: inherit"),
            "D font-style: inherit\nD font-variant: inherit\nD font-weight: inherit\nD font-size: inherit\n"
            "D line-height: inherit\nD font-family: inherit\n");
  // With no size, or no family, it is not a font; nor is one of the
  // names for a system's own.
  EXPECT_EQ(Declared("font: bold serif"), "X value: font: bold serif\n");
  EXPECT_EQ(Declared("font: bold 2em"), "X value: font: bold 2em\n");
  EXPECT_EQ(Declared("font: caption"), "X value: font: caption\n");
}

TEST(CssReader, TurnsEveryUnitOfLengthIntoOneOfFour) {
  EXPECT_EQ(Declared("width: 12pt"), "D width: 16px\n");
  EXPECT_EQ(Declared("width: 1pc"), "D width: 16px\n");
  EXPECT_EQ(Declared("width: 0.5in"), "D width: 48px\n");
  EXPECT_EQ(Declared("width: 2.54cm"), "D width: 96px\n");
  EXPECT_EQ(Declared("width: 25.4mm"), "D width: 96px\n");
  EXPECT_EQ(Declared("width: 101.6Q"), "D width: 96px\n");
  EXPECT_EQ(Declared("width: 10pt"), "D width: 13.333px\n");
  EXPECT_EQ(Declared("width: 3ex"), "D width: 1.5em\n");
  EXPECT_EQ(Declared("width: 2ch"), "D width: 1em\n");
  EXPECT_EQ(Declared("width: 2REM"), "D width: 2rem\n");
  EXPECT_EQ(Declared("width: +4px"), "D width: 4px\n");
  // Past the third place after the point, a number is cut.
  EXPECT_EQ(Declared("width: 1.23456em"), "D width: 1.234em\n");
  EXPECT_EQ(Declared("width: 0.0004em"), "D width: 0em\n");
}

TEST(CssReader, HoldsANumberToTheMostItCanBe) {
  EXPECT_EQ(Declared("width: 99999999999999999999999999px"), "D width: 100000px\n");
  EXPECT_EQ(Declared("text-indent: -99999999999999999999999999em"), "D text-indent: -100000em\n");
  EXPECT_EQ(Declared("width: 99999in"), "D width: 100000px\n");
  EXPECT_EQ(Declared("width: 100000.999px"), "D width: 100000px\n");
}

TEST(CssReader, DropsAValueItDoesNotUnderstand) {
  for (const char* bad : {"width: 50vh",
                          "width: calc(100% - 2em)",
                          "width: -1px",
                          "width: 12",
                          "width: 1 em",
                          "width: em",
                          "width: 1.px",
                          "width: 1e3px",
                          "width: 2e-1em",
                          "width: 12furlongs",
                          "font-size: -1em",
                          "font-size: normal",
                          "font-weight: nomal",
                          "font-weight: 0",
                          "font-weight: 1001",
                          "font-weight: 400.5",
                          "font-weight: 50%",
                          "font-style: bold",
                          "font-variant: all-small-caps",
                          "font-family:",
                          "line-height: -1",
                          "text-align: disc",
                          "text-indent: 1em hanging",
                          "text-decoration: #333",
                          "vertical-align: sup",
                          "display: table-cell",
                          "float: center",
                          "page-break-before: column",
                          "page-break-inside: always",
                          "margin-top: 1em 2em",
                          "padding-left: auto",
                          "padding-left: -1px",
                          "height: revert"}) {
    EXPECT_EQ(Declared(bad), "X value: " + std::string(bad) + "\n") << bad;
  }
}

TEST(CssReader, DropsAPropertyItDoesNotKnowByItsName) {
  EXPECT_EQ(Declared("color: #333; -webkit-hyphens: auto; Text-Transform: uppercase; _height: 1px"),
            "X property: color\nX property: -webkit-hyphens\nX property: Text-Transform\nX property: _height\n");
  // And what is no declaration.
  EXPECT_EQ(Declared("margin; : 1em; *zoom: 1; a b: c; ok"),
            "X declaration: margin\nX declaration: : 1em\nX declaration: *zoom: 1\nX declaration: a b: c\n"
            "X declaration: ok\n");
}

TEST(CssReader, MarksWhatIsImportant) {
  EXPECT_EQ(Declared("margin-top: 1em !important"), "D margin-top: 1em !important\n");
  EXPECT_EQ(Declared("margin-top: 1em!IMPORTANT"), "D margin-top: 1em !important\n");
  EXPECT_EQ(Declared("margin: 1em 2em ! important"),
            "D margin-top: 1em !important\nD margin-right: 2em !important\nD margin-bottom: 1em !important\n"
            "D margin-left: 2em !important\n");
  EXPECT_EQ(Declared("font-family: Arial !important"), "D font-family: arial !important\n");
  EXPECT_EQ(Declared("margin-top: 1em important"), "X value: margin-top: 1em important\n");
  EXPECT_EQ(Declared("margin-top: !important"), "X value: margin-top: !important\n");
}

TEST(CssReader, PassesOverComments) {
  EXPECT_EQ(
      Read("/* one */ p /* two */ { /* three */ margin-top /* four */ : 1em /* five */ ; /* six */ } /* seven */"),
      "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  // A comment with marks in it that would mean something outside one; one
  // that is never closed; and what only looks like the start of one.
  EXPECT_EQ(Read("p { /* } ; { \" ' */ margin-top: 1em } /** stars **/ b {} /* i { }"),
            "{\nS 0,0,1 p\nD margin-top: 1em\n}\n{\nS 0,0,1 b\n}\n");
  EXPECT_EQ(Read("p { margin-top: 1em } /"), "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_EQ(Read("p / b {}"), "X selector: p / b\nX rule: \n");
}

TEST(CssReader, KeepsWhatIsInsideQuotesAndBracketsAsText) {
  // A brace, a semicolon and a comment's marks inside quotes; a semicolon
  // inside brackets.
  EXPECT_EQ(Read("p { font-family: \"a;b}c/*d*/\", 'e{f'; content: \"}\"; x: url(data:image/png;base64,AA==); "
                 "margin-top: 1em }"),
            "{\nS 0,0,1 p\nD font-family: a;b}c/*d*/, e{f\nX property: content\nX property: x\nD margin-top: 1em\n}\n");
  // A quote with a backslash before it does not end what it is in.
  EXPECT_EQ(Read("p { font-family: \"a\\\"}\"; margin-top: 1em }"),
            "{\nS 0,0,1 p\nD font-family: a\"}\nD margin-top: 1em\n}\n");
  // Quotes left open end with the line, so that a brace after it is a
  // brace again.
  EXPECT_EQ(Read("p { font-family: \"open;\n margin-top: 1em }\nb {}"),
            "{\nS 0,0,1 p\nD font-family: open; margin-top: 1em\n}\n{\nS 0,0,1 b\n}\n");
}

TEST(CssReader, ReadsTheRulesOfAMediaBlockOnlyIfItIsForAReader) {
  EXPECT_EQ(Read("@media screen { p { margin-top: 1em } b {} } i {}"),
            "{\nS 0,0,1 p\nD margin-top: 1em\n}\n{\nS 0,0,1 b\n}\n{\nS 0,0,1 i\n}\n");
  EXPECT_EQ(Read("@media print { p { margin-top: 1em } @page { margin: 0 } b {} } i {}"),
            "X media block: print\n{\nS 0,0,1 i\n}\n");
  // One inside another.
  EXPECT_EQ(Read("@media all { @media not amzn-mobi { p {} } @media amzn-kf8 { b {} } u {} } i {}"),
            "{\nS 0,0,1 p\n}\nX media block: amzn-kf8\n{\nS 0,0,1 u\n}\n{\nS 0,0,1 i\n}\n");

  for (const char* yes : {"", "all", "screen", "SCREEN", "only screen", "not print", "not amzn-mobi", "print, screen",
                          "amzn-kf8, all", "screen and (max-width: 400px), screen"}) {
    EXPECT_TRUE(bn::epub::CssMediaApplies(yes)) << yes;
  }
  for (const char* no : {"print", "amzn-mobi", "amzn-kf8", "handheld", "speech", "not screen", "not all",
                         "screen and (max-width: 420px)", "all and (prefers-color-scheme: dark)", "(max-width: 400px)",
                         "not screen and (color)", "not print and (color)", "only", "not", "screen and", "screen print",
                         "print, amzn-kf8", ",", "amzn-kf8 and (max-width: 420px)"}) {
    EXPECT_FALSE(bn::epub::CssMediaApplies(no)) << no;
  }
}

TEST(CssReader, TellsOfAnImport) {
  EXPECT_EQ(Read("@import url(a.css);\n@import url(\"b c.css\");\n@import 'd.css';\n@import \"e.css\" screen;\n"
                 "@import url( f.css ) not amzn-mobi;\n@IMPORT \"g.css\""),
            "I a.css\nI b c.css\nI d.css\nI e.css\nI f.css\nI g.css\n");
  // Not one for a printer, nor one that names nothing, nor one with a
  // block.
  EXPECT_EQ(Read("@import \"p.css\" print; @import; @import x.css; @import \"y.css\" { p {} } b {}"),
            "X at-rule: import\nX at-rule: import\nX at-rule: import\nX at-rule: import\n{\nS 0,0,1 b\n}\n");
  // A bracket left open in one goes on to the next block, and not to the
  // end of the sheet.
  EXPECT_EQ(Read("@import url(a.css; p { margin-top: 1em } b {}"), "X at-rule: import\n{\nS 0,0,1 b\n}\n");
  // A semicolon inside brackets is part of what is named.
  EXPECT_EQ(Read("@import url(data:text/css;base64,AA==); b {}"), "I data:text/css;base64,AA==\n{\nS 0,0,1 b\n}\n");
}

TEST(CssReader, PassesOverTheOtherAtRulesWithTheirBlocks) {
  EXPECT_EQ(Read("@charset \"utf-8\";\n"
                 "@namespace epub \"http://www.idpf.org/2007/ops\";\n"
                 "@font-face { font-family: \"X\"; src: url(x.ttf) }\n"
                 "@page :left { margin: 1em; @top-left { content: \"}\" } }\n"
                 "@supports (display: grid) { p { margin-top: 1em } @media screen { b {} } }\n"
                 "@-webkit-keyframes spin { from { width: 0 } to { width: 1px } }\n"
                 "@unknown;\n"
                 "p { margin-top: 2em }"),
            "X at-rule: charset\nX at-rule: namespace\nX at-rule: font-face\nX at-rule: page\nX at-rule: supports\n"
            "X at-rule: -webkit-keyframes\nX at-rule: unknown\n{\nS 0,0,1 p\nD margin-top: 2em\n}\n");
  EXPECT_EQ(Read("@media screen; p {}"), "X at-rule: media\n{\nS 0,0,1 p\n}\n");
  EXPECT_EQ(Read("@ { p {} } b {}"), "X at-rule: \n{\nS 0,0,1 b\n}\n");
}

TEST(CssReader, GoesOnAfterAMistake) {
  // A brace that closes nothing; a declaration with no semicolon after
  // it; a rule with a block inside it; a semicolon where a rule should be.
  EXPECT_EQ(Read("} p { margin-top: 1em }"), "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_EQ(Read("p { margin-top: 1em margin-bottom: 2em; text-indent: 0 }"),
            "{\nS 0,0,1 p\nX value: margin-top: 1em margin-bottom: 2em\nD text-indent: 0px\n}\n");
  EXPECT_EQ(Read("p { b { margin: 0 } text-indent: 1em; float: left } i {}"),
            "{\nS 0,0,1 p\nX declaration: b { margin: 0 } text-indent: 1em\nD float: left\n}\n{\nS 0,0,1 i\n}\n");
  EXPECT_EQ(Read("p; b { float: left }"), "X selector: p; b\nX rule: \n");
  // Brackets left open in a value end with the rule, and not with the
  // sheet.
  EXPECT_EQ(Read("p { width: calc(1em; float: left } b {}"),
            "{\nS 0,0,1 p\nX value: width: calc(1em; float: left\n}\n{\nS 0,0,1 b\n}\n");
  // What a sheet inside a page of HTML may have round it, and the mark
  // some files begin with.
  EXPECT_EQ(Read("\xEF\xBB\xBF<!-- p {} --> b {}"), "{\nS 0,0,1 p\n}\n{\nS 0,0,1 b\n}\n");
  EXPECT_EQ(Read("\xEF\xBB\xBF@import \"a.css\"; p {}"), "I a.css\n{\nS 0,0,1 p\n}\n");
}

TEST(CssReader, EndsWhatIsOpenAtTheEndOfTheSheet) {
  EXPECT_EQ(Read("p { margin-top: 1em"), "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_EQ(Read("p { margin-top: 1em;"), "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_EQ(Read("@media screen { p { margin-top: 1em"), "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_EQ(Read("p { font-family: \"unclosed"), "{\nS 0,0,1 p\nD font-family: unclosed\n}\n");
  // A rule that never got to its block was never a rule.
  EXPECT_EQ(Read("p { } b, i"), "{\nS 0,0,1 p\n}\n");
  EXPECT_EQ(Read("@font-face { src: url(x)"), "X at-rule: font-face\n");
}

TEST(CssReader, DropsWhatIsLongerThanThereIsRoomForAndGoesOn) {
  std::string selectors;
  while (selectors.size() <= CssReader::kMaxPrelude) {
    selectors += "p.some-class-or-other, ";
  }
  selectors += 'p';
  std::string lines = Read(selectors + " { margin-top: 1em } b {}");
  EXPECT_EQ(lines, "X too long: " + selectors.substr(0, 60) + "\n{\nS 0,0,1 b\n}\n");
  // One byte fewer than too many is read.
  const std::string fits = "p" + std::string(CssReader::kMaxPrelude - 2, ' ') + "b";
  EXPECT_EQ(Read(fits + "{}"), "{\nS 0,0,2 p b\n}\n");
  EXPECT_EQ(Read(fits + " {}"), "X too long: " + fits.substr(0, 60) + "\n");

  const std::string family = "font-family: " + std::string(CssReader::kMaxDeclaration, 'x');
  lines = Read("p { " + family + "; margin-top: 1em } b {}");
  EXPECT_EQ(lines, "{\nS 0,0,1 p\nX too long: " + family.substr(0, 60) + "\nD margin-top: 1em\n}\n{\nS 0,0,1 b\n}\n");
}

// A sheet with something of everything in it.
std::string EverySort() {
  std::string sheet = "\xEF\xBB\xBF@charset \"utf-8\";\n@import url(\"more.css\");\n/* a comment */\n";
  for (int i = 0; i < 60; ++i) {
    sheet += "p.c" + std::to_string(i) + ", div#d" + std::to_string(i) +
             " > span.a.b, h1 + p, a:link {\n"
             "  margin: " +
             std::to_string(i) +
             "px 1em; font: italic bold 12pt/1.5 \"A Face\", serif;\n"
             "  color: #" +
             std::to_string(100000 + i) + "; text-indent: -" + std::to_string(i) +
             ".5em !important;\n"
             "  content: \"}{;\"; width: calc(100% - " +
             std::to_string(i) +
             "px);\n}\n"
             "@media not amzn-mobi { .k" +
             std::to_string(i) +
             "::first-letter { float: left; font-size: 3em } }\n"
             "@media print { .k { display: none } }\n"
             "@font-face { font-family: F; src: url(data:font/ttf;base64,AAAA) }\n";
  }
  return sheet;
}

TEST(CssReader, SaysTheSameHoweverTheSheetIsCutUp) {
  const std::string sheet = EverySort();
  const std::string whole = Read(sheet, sheet.size());
  EXPECT_GT(whole.size(), 20'000U);
  for (const std::size_t pieceBytes : {1U, 2U, 3U, 7U, 64U, 1000U, 1024U}) {
    EXPECT_EQ(Read(sheet, pieceBytes), whole) << pieceBytes;
  }
}

TEST(CssReader, ReadsOnePieceAtAStepAndNoMore) {
  const std::string sheet = EverySort();
  PiecewiseSource source(sheet, 4096);
  Recorder recorder;
  CssReader reader;
  reader.Begin(source, recorder);
  EXPECT_EQ(source.reads, 0) << "nothing is read until the first step";
  std::size_t steps = 0;
  bool more = true;
  while (more) {
    const std::size_t takenBefore = source.Taken();
    const int readsBefore = source.reads;
    more = reader.Step();
    ++steps;
    EXPECT_EQ(source.reads - readsBefore, 1);
    EXPECT_LE(source.Taken() - takenBefore, CssReader::kChunkBytes);
    EXPECT_EQ(reader.Finished(), !more);
  }
  EXPECT_EQ(steps, ((sheet.size() + CssReader::kChunkBytes - 1) / CssReader::kChunkBytes) + 1);
  // Once it is over, a step does nothing.
  EXPECT_FALSE(reader.Step());
  EXPECT_EQ(static_cast<std::size_t>(source.reads), steps);
}

TEST(CssReader, CountsWhatItToldOfAndWhatItPassedOver) {
  const std::string sheet =
      "p, b:hover, i[x=], u ~ s, q::after, 1 { margin: 0; color: red; width: 1vh; oops }\n"
      "a:visited { }\n@font-face { }\n@media print { }\n"
      "em { font-family: " +
      std::string(600, 'x') + " }";
  PiecewiseSource source(sheet, 4096);
  Recorder recorder;
  CssReader reader;
  EXPECT_EQ(reader.Read(source, recorder), CssStatus::kOk);
  EXPECT_EQ(reader.Rules(), 2U);
  EXPECT_EQ(reader.Declarations(), 4U);
  EXPECT_EQ(reader.Dropped(CssDropped::kSelectorPseudoClass), 2U);
  EXPECT_EQ(reader.Dropped(CssDropped::kSelectorAttribute), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kSelectorCombinator), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kSelectorPseudoElement), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kSelectorOther), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kRule), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kProperty), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kValue), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kDeclaration), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kAtRule), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kMedia), 1U);
  EXPECT_EQ(reader.Dropped(CssDropped::kTooLong), 1U);
  // Every kind has a name of its own.
  std::vector<std::string> names;
  for (std::size_t kind = 0; kind < bn::epub::kCssDroppedKinds; ++kind) {
    names.emplace_back(bn::epub::CssDroppedName(static_cast<CssDropped>(kind)));
    EXPECT_NE(names.back(), "unknown");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

TEST(CssReader, DoesNothingUntilItIsBegunAndCanReadOneSheetAfterAnother) {
  CssReader reader;
  EXPECT_EQ(reader.Status(), CssStatus::kNotStarted);
  EXPECT_FALSE(reader.Step());
  EXPECT_FALSE(reader.Finished());

  Recorder recorder;
  // The first is left in the middle of everything it can be in the middle of.
  PiecewiseSource first("@media screen { p { font-family: \"a /* b", 4096);
  EXPECT_EQ(reader.Read(first, recorder), CssStatus::kOk);
  EXPECT_EQ(reader.Rules(), 1U);
  recorder.lines.clear();
  PiecewiseSource second("b { float: left }", 4096);
  EXPECT_EQ(reader.Read(second, recorder), CssStatus::kOk);
  EXPECT_EQ(recorder.lines, "{\nS 0,0,1 b\nD float: left\n}\n");
  EXPECT_EQ(reader.Rules(), 1U);
  EXPECT_EQ(reader.Declarations(), 1U);
  EXPECT_STREQ(bn::epub::CssStatusName(CssStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::CssStatusName(CssStatus::kNotStarted), "not started");
}

TEST(CssReader, ReportsASourceThatFails) {
  const std::string sheet = "p { margin-top: 1em }\n" + std::string(3000, ' ') + "b { float: left }";
  PiecewiseSource source(sheet, 1000);
  source.failAfterBytes = 2000;
  Recorder recorder;
  CssReader reader;
  EXPECT_EQ(reader.Read(source, recorder), CssStatus::kSourceFailed);
  EXPECT_FALSE(reader.Finished());
  EXPECT_EQ(source.reads, 3);
  // What came before the failure was told.
  EXPECT_EQ(recorder.lines, "{\nS 0,0,1 p\nD margin-top: 1em\n}\n");
  EXPECT_FALSE(reader.Step());
  EXPECT_STREQ(bn::epub::CssStatusName(reader.Status()), "source failed");
}

TEST(CssDeclarations, AreReadFromAStyleAttribute) {
  Recorder recorder;
  bn::epub::ReadCssDeclarations(
      "margin-left: 2em; font-weight: 700 !important; color: red; text-align: CENTER;;"
      " font-family: \"a;b\"; width: calc(1px; height: 2px",
      recorder);
  EXPECT_EQ(recorder.lines,
            "D margin-left: 2em\nD font-weight: 700 !important\nX property: color\nD text-align: center\n"
            "D font-family: a;b\nX value: width: calc(1px; height: 2px\n");
  recorder.lines.clear();
  bn::epub::ReadCssDeclarations("", recorder);
  bn::epub::ReadCssDeclarations(" ; ;; ", recorder);
  EXPECT_EQ(recorder.lines, "");
}

TEST(CssFamilies, AreTakenOffAListOneAtATime) {
  const auto names = [](std::string_view list, std::size_t room = 64) {
    std::vector<char> name(room);
    std::string out;
    for (std::string_view one = bn::epub::CssNextFamily(list, name); !one.empty();
         one = bn::epub::CssNextFamily(list, name)) {
      out += "[" + std::string(one) + "]";
    }
    return out;
  };
  EXPECT_EQ(names("Georgia, \"Times New Roman\", 'Gill  Sans' ,serif"), "[georgia][times new roman][gill sans][serif]");
  // Spaces are made one, and none is kept at either end; a comma inside
  // quotes is part of the name; names of nothing are passed over.
  EXPECT_EQ(names("  Helvetica   Neue  ,, \"a, b\" , ,x"), "[helvetica neue][a, b][x]");
  EXPECT_EQ(names(""), "");
  EXPECT_EQ(names(" , ,"), "");
  EXPECT_EQ(names("\"unclosed, serif"), "[unclosed, serif]");
  EXPECT_EQ(names("\"a\\\"b\""), "[a\"b]");
  // A name longer than there is room for is cut, and the next is found.
  EXPECT_EQ(names("abcdefghij, k", 4), "[abcd][k]");
  // Letters past ASCII are left as they are.
  EXPECT_EQ(names("\"\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\", Mincho"),
            "[\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E][mincho]");
}

TEST(CssReader, EndsWhateverBytesItIsGiven) {
  // Sheets with bytes changed, cut out and put in at random, and bytes
  // that never were a sheet: each is read to its end, a piece at a step.
  const std::string sheet = EverySort().substr(0, 3000);
  std::mt19937 rng(20261115);  // NOLINT(bugprone-random-generator-seed): the same run every time.
  const auto below = [&rng](std::size_t limit) -> std::size_t { return rng() % limit; };
  const std::string_view marks = "{}()[];:,\"'\\/*@!.#> \n";
  for (int round = 0; round < 400; ++round) {
    std::string damaged = round % 8 == 7 ? std::string(below(2000), '\0') : sheet;
    const std::size_t changes = 1 + below(12);
    for (std::size_t change = 0; change < changes && !damaged.empty(); ++change) {
      const std::size_t at = below(damaged.size());
      switch (below(4)) {
        case 0:
          damaged[at] = static_cast<char>(below(256));
          break;
        case 1:
          damaged[at] = marks[below(marks.size())];
          break;
        case 2:
          damaged.erase(at, below(40));
          break;
        default:
          damaged.insert(at, std::string(1 + below(3), marks[below(marks.size())]));
          break;
      }
    }
    if (round % 8 == 7) {
      for (char& ch : damaged) {
        ch = static_cast<char>(below(256));
      }
    }
    PiecewiseSource source(damaged, 1 + below(1500));
    Recorder recorder;
    CssReader reader;
    reader.Begin(source, recorder);
    std::size_t steps = 0;
    while (reader.Step()) {
      ++steps;
      ASSERT_LE(steps, damaged.size() + 2) << round;
    }
    ASSERT_EQ(reader.Status(), CssStatus::kOk) << round;
    ASSERT_TRUE(reader.Finished()) << round;
    // Rules are told whole and in order, and counted as they were told.
    ASSERT_FALSE(recorder.outOfOrder) << round;
    ASSERT_FALSE(recorder.inRule) << round;
    ASSERT_EQ(recorder.begun, recorder.ended) << round;
    ASSERT_EQ(reader.Rules(), recorder.ended) << round;
  }
}

}  // namespace
