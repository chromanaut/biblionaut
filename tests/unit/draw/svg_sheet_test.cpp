#include "draw/svg_sheet.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace {

using bn::draw::SvgSelector;
using bn::draw::SvgSheetDropped;
using bn::draw::SvgSheetReader;
using bn::draw::SvgSheetToken;
using Kind = bn::draw::SvgSheetToken::Kind;

// What a reader made of a sheet, written out again in one way: `.a` or
// `p` for a selector, `{name=value}` for a declaration, `;` for the end of
// a rule.
struct Reading {
  std::string said;
  std::uint8_t dropped = 0;
  std::size_t calls = 0;
};

Reading Read(std::string_view sheet, std::size_t piece = SIZE_MAX) {
  Reading reading;
  SvgSheetReader reader;
  for (std::size_t at = 0; at < sheet.size();) {
    std::string_view bytes = sheet.substr(at, piece);
    at += bytes.size();
    for (;;) {
      SvgSheetToken token;
      const std::size_t taken = reader.Next(bytes, token);
      ++reading.calls;
      EXPECT_LE(taken, bytes.size());
      bytes.remove_prefix(taken);
      switch (token.kind) {
        case Kind::kNone:
          break;
        case Kind::kSelector:
          reading.said += token.selector.kind == SvgSelector::Kind::kClass ? "." : "";
          reading.said.append(token.selector.name).append(" ");
          break;
        case Kind::kDeclaration:
          reading.said.append("{").append(token.property).append("=").append(token.value).append("}");
          break;
        case Kind::kRuleEnd:
          reading.said += ';';
          break;
      }
      if (token.kind == Kind::kNone) {
        EXPECT_TRUE(bytes.empty());
        break;
      }
    }
  }
  reader.Finish();
  reading.dropped = reader.Dropped();
  return reading;
}

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

std::uint8_t Bits(std::initializer_list<SvgSheetDropped> all) {
  std::uint8_t bits = 0;
  for (const SvgSheetDropped what : all) {
    bits |= static_cast<std::uint8_t>(what);
  }
  return bits;
}

TEST(SvgSheet, ReadsRulesOfClassesAndElementsAndWhatTheySet) {
  const Reading reading = Read(".st0 { fill: #FF0000; stroke : none } path{fill-opacity:.5}");
  EXPECT_EQ(reading.said, ".st0 {fill=#FF0000}{stroke=none};path {fill-opacity=.5};");
  EXPECT_EQ(reading.dropped, 0u);
}

TEST(SvgSheet, GivesEverySelectorOfARuleBeforeWhatItSets) {
  EXPECT_EQ(Read(".a, .b,path ,\n.c-d_9{fill:red;}").said, ".a .b path .c-d_9 {fill=red};");
}

TEST(SvgSheet, TakesAPropertysNameInSmallLettersAndItsValueAsWritten) {
  EXPECT_EQ(Read(".a{ FILL : URL( #G ) !important ;}").said, ".a {fill=URL( #G ) !important};");
}

TEST(SvgSheet, PassesOverCommentsAndTheMarksASheetIsWrittenInside) {
  const Reading reading = Read("<!-- /* .x{fill:red} */ .a /*?*/ {fill:/* no */blue} --> /**/");
  EXPECT_EQ(reading.said, ".a {fill=blue};");
  EXPECT_EQ(reading.dropped, 0u);
}

TEST(SvgSheet, ASemicolonOrABraceInAStringOrBracketsDoesNotEndAValue) {
  EXPECT_EQ(Read(".a{font-family:\"x;}\\\"y\";fill:url(a;b);stroke:'}'}").said,
            ".a {font-family=\"x;}\\\"y\"}{fill=url(a;b)}{stroke='}'};");
}

TEST(SvgSheet, PassesOverARuleWithASelectorItDoesNotRead) {
  for (const std::string_view selector :
       {"g path", "g > path", "path.a", ".a.b", "#id", "[fill]", ".a:hover", "*", ".", "9a", "svg|path", ".a,"}) {
    const Reading reading = Read(std::string(selector) + "{fill:red}.b{fill:blue}");
    EXPECT_EQ(reading.said, ".b {fill=blue};") << selector;
    EXPECT_EQ(reading.dropped, Bits({SvgSheetDropped::kSelector})) << selector;
  }
  // One that cannot be read is enough to lose the rule, as in CSS.
  EXPECT_EQ(Read(".a, g path {fill:red}").said, "");
}

TEST(SvgSheet, PassesOverAtRulesWithTheirBlocks) {
  const Reading reading =
      Read("@import url(x.css);@media print { .a { fill: red } @page {} } .b{fill:blue}@font-face{src:'}'}");
  EXPECT_EQ(reading.said, ".b {fill=blue};");
  EXPECT_EQ(reading.dropped, Bits({SvgSheetDropped::kAtRule}));
}

TEST(SvgSheet, ADeclarationWithNoValueOrABlockInItIsPassedOver) {
  EXPECT_EQ(Read(".a{fill;stroke:{x};:red;fill:blue;{q:1}opacity:1}").said, ".a {fill=blue}{opacity=1};");
}

TEST(SvgSheet, WhatIsTooLongToHoldIsPassedOverAndTheRestIsRead) {
  const std::string longName(SvgSheetReader::kLongestSelectors, 'n');
  const Reading selectors = Read("." + longName + "{fill:red}.b{fill:blue}");
  EXPECT_EQ(selectors.said, ".b {fill=blue};");
  EXPECT_EQ(selectors.dropped, Bits({SvgSheetDropped::kTooLong}));
  const std::string longest(SvgSheetReader::kLongestSelectors - 1, 'n');
  EXPECT_EQ(Read("." + longest + "{fill:red}").said, "." + longest + " {fill=red};");

  const std::string value(SvgSheetReader::kLongestValue + 1, 'v');
  const Reading declaration = Read(".a{fill:" + value + ";stroke:red}");
  EXPECT_EQ(declaration.said, ".a {stroke=red};");
  EXPECT_EQ(declaration.dropped, Bits({SvgSheetDropped::kTooLong}));
  const std::string property(SvgSheetReader::kLongestProperty + 1, 'p');
  EXPECT_EQ(Read(".a{" + property + ":1;fill:red}").said, ".a {fill=red};");
}

TEST(SvgSheet, SaysWhenTheSheetEndsInsideSomething) {
  for (const std::string_view sheet : {".a{fill:red", ".a{fill:red}/* x", ".a{fill:'red}", ".a", "@media{"}) {
    EXPECT_NE(Read(sheet).dropped & Bits({SvgSheetDropped::kUnended}), 0) << sheet;
  }
  EXPECT_EQ(Read(".a{fill:red} <!-- -->").dropped, 0u);
  // What was told before the end stands.
  EXPECT_EQ(Read(".a{fill:red}.b{stroke:none").said, ".a {fill=red};.b ");
}

TEST(SvgSheet, ComesOutTheSameInAnyPieces) {
  const std::string sheet =
      "<!--\n/* Inkscape */\n.st0{fill:#1D1D1B;}\n.st1 , path{fill : none ; stroke:#000;stroke-width:0.5}\n"
      "@media screen{.x{fill:red}}\ng path{fill:red}\n.st2{font-family:'A; B', serif;fill:url(#g) #fff}\n-->";
  const Reading whole = Read(sheet);
  EXPECT_EQ(whole.said,
            ".st0 {fill=#1D1D1B};.st1 path {fill=none}{stroke=#000}{stroke-width=0.5};"
            ".st2 {font-family='A; B', serif}{fill=url(#g) #fff};");
  for (std::size_t piece = 1; piece < 12; ++piece) {
    const Reading cut = Read(sheet, piece);
    EXPECT_EQ(cut.said, whole.said) << piece;
    EXPECT_EQ(cut.dropped, whole.dropped) << piece;
  }
}

TEST(SvgSheet, AnyBytesAtAllEndWithEveryCallDoingABoundedAmount) {
  constexpr std::string_view kParts[] = {".a", "{", "}", ";",  ":",    "fill", " ", ",", "'",   "\"", "\\", "/*",
                                         "*/", "(", ")", "@m", "<!--", "-->",  "g", "#", "red", "\n", "/",  "*"};
  std::mt19937 random = Repeatable(7);
  for (int round = 0; round < 3000; ++round) {
    std::string sheet;
    const std::size_t parts = random() % 40;
    for (std::size_t part = 0; part < parts; ++part) {
      sheet += kParts[random() % std::size(kParts)];
    }
    const Reading whole = Read(sheet);
    // Every call takes a byte or tells something, and nothing is told
    // without bytes to tell it from.
    EXPECT_LE(whole.calls, (2 * sheet.size()) + 2) << sheet;
    const Reading cut = Read(sheet, 1 + (random() % 5));
    EXPECT_EQ(cut.said, whole.said) << sheet;
    EXPECT_EQ(cut.dropped, whole.dropped) << sheet;
  }
}

}  // namespace
