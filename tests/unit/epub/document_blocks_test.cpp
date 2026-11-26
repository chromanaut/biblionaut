#include "epub/document_blocks.h"

#include <gtest/gtest.h>

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
#include "epub/style_stack.h"
#include "epub/xml.h"

// A document's blocks (epub/document_blocks.h).

namespace {

using bn::epub::BlockChange;
using bn::epub::BlockKind;
using bn::epub::BlockRecord;
using bn::epub::DocumentBlocks;
using bn::epub::ElementStyle;
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

// With `*` after what the book said.
std::string Mark(const std::string& what, bool fromBook) { return fromBook ? what + "*" : what; }

// How text is set: "plain", or what is not plain of it with spaces
// between, a `*` after what the book said.
std::string Style(const TextStyle& text, const StyleStack& stack) {
  constexpr std::array<const char*, 7> kNames = {"bold",       "italic", "underline", "line-through",
                                                 "small-caps", "raised", "lowered"};
  std::string out;
  const auto add = [&out](const std::string& part) {
    out += out.empty() ? "" : " ";
    out += part;
  };
  if (text.size != 1000 || text.sizeFromBook) {
    add(Mark(std::to_string(text.size), text.sizeFromBook));
  }
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    if (((std::uint32_t{text.bits} >> i) & 1U) != 0) {
      add(Mark(kNames.at(i), ((std::uint32_t{text.fromBook} >> i) & 1U) != 0));
    }
  }
  if (text.family != bn::epub::CssFamilyClass::kUnset) {
    add(Mark(bn::epub::CssFamilyClassName(text.family), text.familyFromBook));
  }
  if (text.language != 0) {
    add("in-" + std::string(stack.Language(text.language)));
  }
  return out.empty() ? "plain" : out;
}

// A block's record, as the parts of it that are not as a block that says
// nothing has them, each under a name.
using Parts = std::vector<std::pair<std::string, std::string>>;

Parts Record(const BlockRecord& record, const StyleStack& stack) {
  Parts parts;
  if (record.align != bn::epub::StyleAlign::kUnset || record.alignFromBook) {
    parts.emplace_back("align", Mark(bn::epub::StyleAlignName(record.align), record.alignFromBook));
  }
  if (record.rightToLeft) {
    parts.emplace_back("direction", "right-to-left");
  }
  if (record.spacesAsWritten) {
    parts.emplace_back("spaces", "as-written");
  }
  if (record.size != 1000 || record.sizeFromBook) {
    parts.emplace_back("size", Mark(std::to_string(record.size), record.sizeFromBook));
  }
  if (record.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kMultiple) {
    parts.emplace_back("lines", Mark("x" + std::to_string(record.lineHeight.multiple), record.lineHeightFromBook));
  } else if (record.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kLength) {
    parts.emplace_back("lines", Mark(Length(record.lineHeight.length), record.lineHeightFromBook));
  } else if (record.lineHeightFromBook) {
    parts.emplace_back("lines", "normal*");
  }
  if (record.hasIndent) {
    parts.emplace_back("indent", Mark(Length(record.indent), record.indentFromBook));
  }
  if (!record.marginLeft.IsZero() || record.marginLeftFromBook) {
    parts.emplace_back("left", Mark(Length(record.marginLeft), record.marginLeftFromBook));
  }
  if (!record.marginRight.IsZero() || record.marginRightFromBook) {
    parts.emplace_back("right", Mark(Length(record.marginRight), record.marginRightFromBook));
  }
  if (!record.spaceBefore.IsZero() || record.spaceBeforeFromBook) {
    parts.emplace_back("above", Mark(Length(record.spaceBefore), record.spaceBeforeFromBook));
  }
  std::string breaks;
  if (record.breakBefore) {
    breaks += "page-before ";
  }
  if (record.keepWithNext || record.keepWithNextFromBook) {
    breaks += Mark(record.keepWithNext ? "with-next" : "not-with-next", record.keepWithNextFromBook) + " ";
  }
  if (record.keepTogether) {
    breaks += "together ";
  }
  if (!breaks.empty()) {
    breaks.pop_back();
    parts.emplace_back("breaks", breaks);
  }
  if (record.floated) {
    parts.emplace_back("floated", "yes");
  }
  if (record.centred) {
    parts.emplace_back("centred", Length(record.width));
  }
  if (record.quoteDepth != 0) {
    parts.emplace_back("quotes", std::to_string(record.quoteDepth));
  }
  if (record.listDepth != 0) {
    parts.emplace_back("lists", std::to_string(record.listDepth));
  }
  if (record.hasMarker) {
    parts.emplace_back("marker", Mark(bn::epub::StyleMarkerName(record.marker), record.markerFromBook) + " " +
                                     std::to_string(record.number));
  }
  if (record.hasFirstLetter) {
    parts.emplace_back("letter",
                       std::string(record.firstLetterFloated ? "floated " : "") + Style(record.firstLetter, stack));
  }
  if (record.beginsFloated) {
    parts.emplace_back("begins", "floated");
  }
  return parts;
}

// What going through a document came to.
struct Walked {
  // A line for each block as it begins, "kind" or "heading N" and after
  // ": " the parts asked for of its record, each as its name and what it
  // is with "; " between; then on the same line, in their order, what is
  // in it: `[style]` where text set another way begins, if styles were
  // asked for, `/` for a line's end, `#` for a picture, `<` where the
  // floated text it began with ends, and `.` where the block ends.
  std::string lines;
  std::uint32_t mostWork = 0;
  bool withinWork = true;
  std::size_t blocks = 0;
};

// Goes through `document` with a cascade over the rules of `sheet` and a
// DocumentBlocks that keeps `levels` open elements. `only` is the names
// of the parts to tell of, with spaces between, "style" among them for
// how text is set; all of them if it is empty.
Walked Walk(std::string_view only, std::string_view sheet, std::string_view document, std::size_t levels = 32) {
  class Walker final : public bn::epub::XmlHandler {
   public:
    Walker(bn::epub::CssCascade& cascade, DocumentBlocks& blocks, Walked& walked, std::string_view only)
        : cascade_(cascade), blocks_(blocks), walked_(walked), only_(" " + std::string(only) + " ") {}

    void OnStartElement(std::string_view name, const bn::epub::XmlAttributes& attributes) noexcept override {
      bn::epub::CssStyle style;
      bn::epub::CssStyle firstLetter;
      cascade_.Open(name, attributes, style, firstLetter);
      const std::size_t depth = blocks_.Depth();
      ElementStyle said;
      Tell(blocks_.Open(name, attributes, style, firstLetter, said), 0, attributes.Count(), 0);
      EXPECT_EQ(blocks_.Depth(), depth + 1);
      if (name == "img") {
        Tell(blocks_.Picture(), '#', 0, 0);
      }
    }
    void OnEndElement(std::string_view /*name*/) noexcept override {
      cascade_.Close();
      Tell(blocks_.Close(), 0, 0, 0);
    }
    void OnText(std::string_view text) noexcept override { Tell(blocks_.Text(text), 't', 0, text.size()); }
    void OnEntity(std::string_view /*name*/) noexcept override { Tell(blocks_.Text("&"), 't', 0, 1); }

   private:
    bool Asked(const std::string& part) const {
      return only_ == "  " || only_.find(" " + part + " ") != std::string::npos;
    }

    void Tell(const BlockChange& change, char content, std::size_t attributes, std::size_t textBytes) {
      walked_.mostWork = std::max(walked_.mostWork, blocks_.Work());
      walked_.withinWork =
          walked_.withinWork && blocks_.Work() <= DocumentBlocks::MostWorkFor(attributes, textBytes, blocks_.Levels());
      std::string& out = walked_.lines;
      if (change.ends) {
        EXPECT_TRUE(open_);
        out += ".\n";
        open_ = false;
      }
      if (change.begins) {
        EXPECT_FALSE(open_);
        EXPECT_TRUE(blocks_.HasBlock());
        open_ = true;
        styled_ = false;
        ++walked_.blocks;
        const BlockRecord& record = blocks_.Record();
        out += bn::epub::BlockKindName(record.kind);
        if (record.headingLevel != 0) {
          out += " " + std::to_string(record.headingLevel);
        }
        std::string told;
        for (const auto& [part, what] : Record(record, blocks_.Styles())) {
          if (Asked(part)) {
            told += told.empty() ? "" : "; ";
            told += part;
            told += ' ';
            told += what;
          }
        }
        out += ":" + (told.empty() ? "" : " " + told) + " |";
      }
      EXPECT_EQ(blocks_.HasBlock(), open_);
      if (change.lineEnds) {
        EXPECT_TRUE(open_);
        out += " /";
      }
      if (change.floatEnds) {
        EXPECT_TRUE(open_);
        out += " <";
      }
      if (content == '#' && change.shown) {
        out += " #";
      }
      if (content == 't' && change.shown) {
        EXPECT_TRUE(open_);
        const TextStyle style = blocks_.TextNow();
        if (Asked("style") && (!styled_ || !(style == style_))) {
          out += " [" + Style(style, blocks_.Styles()) + "]";
        }
        style_ = style;
        styled_ = true;
      }
      if (content != 0 && !change.shown) {
        EXPECT_FALSE(change.begins);
      }
    }

    bn::epub::CssCascade& cascade_;
    DocumentBlocks& blocks_;
    Walked& walked_;
    std::string only_;
    bool open_ = false;
    bool styled_ = false;
    TextStyle style_;
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
  std::vector<std::uint8_t> memory(levels == 0 ? 0
                                               : StyleStack::kLanguagesBytes + (levels * DocumentBlocks::kLevelBytes));
  DocumentBlocks blocks(memory);
  EXPECT_EQ(blocks.Levels(), levels);
  blocks.Begin();

  Walked walked;
  std::vector<std::uint8_t> parserMemory(65536);
  bn::epub::XmlParser parser(parserMemory);
  bn::core::MemoryByteSource source(Bytes(document));
  Walker walker(cascade, blocks, walked, only);
  EXPECT_EQ(parser.Parse(source, walker), bn::epub::XmlStatus::kOk) << document;
  EXPECT_EQ(blocks.Depth(), 0U);
  EXPECT_TRUE(walked.withinWork);
  return walked;
}

std::string Blocks(std::string_view only, std::string_view sheet, std::string_view document) {
  return Walk(only, sheet, document).lines;
}

TEST(DocumentBlocks, ABlockIsMadeByWhatIsInItAndNotByAnElement) {
  // Text straight in a division is a block; so is the text before a block
  // inside it, and the text after; an element with only blocks inside
  // makes none of its own, and one with nothing in it makes none.
  EXPECT_EQ(Blocks("none", "",
                   "<html><body><div>one</div><div><p>two</p><p>three</p></div><div>four<p>five</p>six</div>"
                   "<div></div><p> </p><div><p/><p>seven</p></div></body></html>"),
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n");
}

TEST(DocumentBlocks, WhatIsPartOfALineNeitherBeginsNorEndsABlock) {
  EXPECT_EQ(Blocks("style", "", "<body><p>one <i>two <b>three</b></i> four<span/> five</p> <i>six</i> seven</body>"),
            "paragraph: | [plain] [italic] [bold italic] [plain].\n"
            // White space between blocks begins nothing, and the text after it does.
            "paragraph: | [italic] [plain].\n");
}

TEST(DocumentBlocks, AnElementTheBookMakesABlockEndsAndBeginsBlocks) {
  // A line of verse written as a `span` that is a block, in a paragraph.
  EXPECT_EQ(Blocks("none", "span { display: block } div { display: inline }",
                   "<body><p><span>one</span><span>two</span> three</p><div>four</div> five</body>"),
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            // What the book makes part of a line is that: one block with what follows.
            "paragraph: |.\n");
}

TEST(DocumentBlocks, ABlockThatIsOpenWhenTheDocumentEndsIsNotEnded) {
  // Nothing that is a block closes after it.
  EXPECT_EQ(Blocks("none", "", "<span>one</span>"), "paragraph: |");
  EXPECT_EQ(Walk("none", "", "<span> </span>").blocks, 0U);
}

TEST(DocumentBlocks, ItsKindIsFromTheNearestThingAroundItThatNamesOne) {
  EXPECT_EQ(Blocks("none", "",
                   "<body><h1>a</h1><h2>b</h2><h3>c</h3><h4>d</h4><h5>e</h5><H6>f</H6><h7>g</h7><h0>g</h0>"
                   "<h2><div>h</div><span>i</span></h2><div><h3>j</h3>k</div><p>l</p></body>"),
            "heading 1: |.\n"
            "heading 2: |.\n"
            "heading 3: |.\n"
            "heading 4: |.\n"
            "heading 5: |.\n"
            "heading 6: |.\n"
            // There is no seventh, and none before the first: they are elements nothing knows, parts of a line.
            "paragraph: |.\n"
            // Every block inside a heading is one.
            "heading 2: |.\n"
            "heading 2: |.\n"
            "heading 3: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n");
}

TEST(DocumentBlocks, TheFirstBlockBegunInsideAnItemIsAListItemAndHasItsMarker) {
  EXPECT_EQ(Blocks("marker", "",
                   "<body><ul><li>one</li><li><p>two</p><p>three</p></li><li><div><div>four</div></div>five</li>"
                   "<li><ul><li>six</li></ul>seven</li><li><h3>eight</h3><p>nine</p></li><li><hr/>ten</li>"
                   "<li></li><li> </li><li>eleven</li></ul></body>"),
            "list-item: marker disc 1 |.\n"
            "list-item: marker disc 2 |.\n"
            // The later blocks of an item are paragraphs.
            "paragraph: |.\n"
            // However deep in the item the first one begins.
            "list-item: marker disc 3 |.\n"
            "paragraph: |.\n"
            // An item's first block inside an item is the first of both, and has the marker of the nearer.
            "list-item: marker circle 1 |.\n"
            "paragraph: |.\n"
            // A heading is nearer than the item it is in, and has the item's marker.
            "heading 3: marker disc 5 |.\n"
            "paragraph: |.\n"
            // A rule takes no marker: the text after it does.
            "rule: |.\n"
            "list-item: marker disc 6 |.\n"
            // Items with nothing in them make no block, and are counted as items.
            "list-item: marker disc 9 |.\n");
}

TEST(DocumentBlocks, TheFirstBlockBegunInsideACellIsATablesCell) {
  EXPECT_EQ(Blocks("marker", "td.inline { display: inline }",
                   "<body><table><tr><th>one</th><td>two<p>three</p>four</td></tr>"
                   "<tr><td><ul><li>five</li></ul></td><td><h2>six</h2></td><td/>"
                   "<td class='inline'>seven</td></tr></table></body>"),
            "table-cell: |.\n"
            "table-cell: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            // An item in a cell is nearer than the cell.
            "list-item: marker disc 1 |.\n"
            "heading 2: |.\n"
            // A cell the book makes part of a line is no cell: its text is in the row.
            "paragraph: |.\n");
}

TEST(DocumentBlocks, ARuleIsABlockByItself) {
  EXPECT_EQ(Blocks("above", "hr.none { display: none } hr.inline { display: inline }",
                   "<body><p>one</p><hr/><p>two</p>three<hr class='inline'/>four<hr class='none'/>five<HR/></body>"),
            "paragraph: above 1em |.\n"
            "rule: above 1em |.\n"
            "paragraph: above 1em |.\n"
            "paragraph: above 1em |.\n"
            // A rule is one whatever the book says it is to a page, unless it says it is nothing.
            "rule: above 0.5em |.\n"
            "paragraph: above 0.5em |.\n"
            // In capitals too.
            "rule: above 0.5em |.\n");
}

TEST(DocumentBlocks, AnItemHasTheNextNumberOfItsListOrWhatItsValueSays) {
  EXPECT_EQ(
      Blocks("marker", ".item { display: list-item } .gone { display: none }",
             "<body><ol><li>a</li><li>b</li><li value='7'>c</li><li>d</li><li value='x'>e</li>"
             "<li class='gone'>f</li><li value='-3'>g</li><li>h</li><div><li>i</li></div>"
             "<ol><li>j</li></ol><li>k</li></ol>"
             "<ol start='5'><li>l</li><li>m</li></ol><ol start='-1'><li>n</li><li>o</li><li>p</li></ol>"
             "<ol start='1x'><li>q</li></ol><ol start='+12'><li>r</li></ol><ol start='0000000010'><li>s</li></ol>"
             "<ol start='999999999'><li>t</li><li>u</li></ol><ul start='3'><li>v</li></ul>"
             "<ol start=''><li>a</li><li value='-'>b</li><li value='1.5'>c</li><li value='+'>d</li></ol>"
             "<li>w</li><li value='4'>x</li><p class='item' value='2'>y</p><ol><p class='item'>z</p></ol></body>"),
      "list-item: marker decimal 1 |.\n"
      "list-item: marker decimal 2 |.\n"
      "list-item: marker decimal 7 |.\n"
      "list-item: marker decimal 8 |.\n"
      // A value that is no whole number says nothing.
      "list-item: marker decimal 9 |.\n"
      // An item that is nothing takes no number.
      "list-item: marker decimal -3 |.\n"
      "list-item: marker decimal -2 |.\n"
      // An item is its list's however deep in it.
      "list-item: marker decimal -1 |.\n"
      // A list inside counts for itself, and the one around goes on.
      "list-item: marker decimal 1 |.\n"
      "list-item: marker decimal 0 |.\n"
      "list-item: marker decimal 5 |.\n"
      "list-item: marker decimal 6 |.\n"
      "list-item: marker decimal -1 |.\n"
      "list-item: marker decimal 0 |.\n"
      "list-item: marker decimal 1 |.\n"
      // A start that is no whole number of nine digits at the most is 1.
      "list-item: marker decimal 1 |.\n"
      "list-item: marker decimal 12 |.\n"
      "list-item: marker decimal 1 |.\n"
      "list-item: marker decimal 999999999 |.\n"
      "list-item: marker decimal 1000000000 |.\n"
      "list-item: marker disc 3 |.\n"
      // Nothing, a sign alone, and a number with a part are no whole numbers.
      "list-item: marker decimal 1 |.\n"
      "list-item: marker decimal 2 |.\n"
      "list-item: marker decimal 3 |.\n"
      "list-item: marker decimal 4 |.\n"
      // An item in no list is the first, unless it says otherwise.
      "list-item: marker disc 1 |.\n"
      "list-item: marker disc 4 |.\n"
      // What the book makes an item is one.
      "list-item: marker disc 2 |.\n"
      "list-item: marker decimal 1 |.\n");
}

TEST(DocumentBlocks, BrEndsALineAndWhereNoBlockIsOpenBeginsOne) {
  EXPECT_EQ(Blocks("none", "br.none { display: none } br.block { display: block; margin: 1em }",
                   "<body><p>one<br/>two<br/><br/>three</p><p><br/>four</p><div><br/></div><br/>"
                   "<p>five<br class='none'/>six<br class='block'/>seven</p><p><br class='none'/></p></body>"),
            "paragraph: | / / /.\n"
            // An empty line, and the text after it in the same block.
            "paragraph: | /.\n"
            "paragraph: | /.\n"
            "paragraph: | /.\n"
            // One the book hides is nothing; and one is a line's end whatever else the book says it is.
            "paragraph: | /.\n");
}

TEST(DocumentBlocks, OfItsLinesABlockHasWhatTheNearestBlockAroundItHas) {
  EXPECT_EQ(Blocks("align size lines indent direction spaces style",
                   "div { text-align: center; line-height: 1.5; text-indent: 2em; font-size: 2em } "
                   "span { text-align: right; line-height: 3; text-indent: 0; font-size: 0.5em } "
                   "p { text-align: left }",
                   "<body><div>one <span>two</span><p>three</p></div><div><span>four</span></div>"
                   "<pre>five <b>six</b></pre><p dir='rtl'>seven</p><span>eight</span></body>"),
            // What something that is part of a line says of lines is not the block's.
            "paragraph: align center*; size 2000*; lines x1500*; indent 4em* | [2000*] [1000*].\n"
            "paragraph: align left*; size 2000*; lines x1500*; indent 4em* | [2000*].\n"
            "paragraph: align center*; size 2000*; lines x1500*; indent 4em* | [1000*].\n"
            "paragraph: spaces as-written | [mono] [bold mono].\n"
            "paragraph: align left*; direction right-to-left | [plain].\n"
            // With no block around it but the page, its lines are as the page's.
            "paragraph: | [500*].\n");
}

TEST(DocumentBlocks, ToEachSideItHasTheMarginAndThePaddingOfEveryBlockItIsInside) {
  EXPECT_EQ(Blocks("left right",
                   "body { margin: 0 10% } .a { margin: 0 1em 0 2em; padding: 0 3px 0 5px } .b { margin-left: 5% } "
                   ".c { margin-left: auto; margin-right: -1em } span { margin: 0 4em; padding: 0 4em } "
                   ".z { margin: 0; padding: 0 } .big { margin-left: 500em } .small { margin-left: -900em }",
                   "<html><body><p>one</p><div class='a'>two<div class='b'>three <span>four</span></div></div>"
                   "<blockquote>five<ul><li>six</li></ul></blockquote><div class='c'>seven</div>"
                   "<div class='z'>eight<p>nine</p></div><span>ten</span>"
                   "<div class='big'><div class='big'>eleven<div class='small'>twelve</div></div></div></body></html>"),
            // The page's own margins are left out.
            "paragraph: |.\n"
            "paragraph: left 2em+5px*; right 1em+3px* |.\n"
            // Added up in their three units; and what is part of a line adds nothing.
            "paragraph: left 2em+5px+5%*; right 1em+3px* |.\n"
            "paragraph: left 40px; right 40px |.\n"
            "list-item: left 80px; right 40px |.\n"
            // `auto` is nothing, as the book's; and a margin below nothing takes away.
            "paragraph: left 0*; right -1em* |.\n"
            // Nothing, said by the book, is the book's, and stays so for what is inside.
            "paragraph: left 0*; right 0* |.\n"
            "paragraph: left 0*; right 0* |.\n"
            "paragraph: |.\n"
            // Each amount is held to what sixteen bits take, as the sums are made.
            "paragraph: left 511.984375em* |.\n"
            "paragraph: left -0.015625em* |.\n");
}

TEST(DocumentBlocks, AboveItHasTheLargestOfTheMarginsThatMeetThere) {
  EXPECT_EQ(Blocks("above",
                   ".two { margin: 2em 0 } .three { margin: 3em 0 0 } .px { margin: 20px 0 } .pc { margin: 4% 0 } "
                   ".none { margin: 0 }",
                   "<html><body><div class='two'><div class='three'><p>one</p></div></div><p>two</p><p>three</p>"
                   "<div class='two'>four</div><p class='px'>five</p><p class='pc'>six</p><p class='px'>seven</p>"
                   "<p class='none'>eight</p><p class='none'>nine</p><div>ten</div><div>eleven<span>twelve</span>"
                   "</div></body></html>"),
            // An element's and that of the first thing inside it meet, at a document's start as anywhere.
            "paragraph: above 3em* |.\n"
            // The bottom margins of what closed and the top margin of what opened: the largest, not their sum.
            "paragraph: above 2em* |.\n"
            "paragraph: above 1em |.\n"
            "paragraph: above 2em* |.\n"
            "paragraph: above 2em* |.\n"
            // Which is the largest is judged with 16 pixels to the em and a page 30 em wide: 20 pixels are more
            // than 4 hundredths of the page, which are 19.2; and the one that wins is given as it was written.
            "paragraph: above 20px* |.\n"
            "paragraph: above 20px* |.\n"
            "paragraph: above 20px* |.\n"
            // Where nothing but nothing meets, it is the book's if the book said any of it: after the last
            // paragraph the book gave no margin, too.
            "paragraph: above 0* |.\n"
            "paragraph: above 0* |.\n"
            "paragraph: |.\n");
}

TEST(DocumentBlocks, AMarginBelowNothingIsAddedToTheLargestAboveIt) {
  EXPECT_EQ(Blocks("above", ".up { margin-top: -1em } .far { margin-top: -3em } .down { margin-bottom: 2em }",
                   "<body><div class='up'>zero</div><p>one</p><p class='up'>two</p><div class='far'>"
                   "<p class='up'>three</p></div>"
                   "<div class='down'><p class='down'>four</p></div><div class='far'><div class='up'>five</div></div>"
                   "</body>"),
            // With none above nothing beside it, it is the space.
            "paragraph: above -1em* |.\n"
            "paragraph: above 1em |.\n"
            "paragraph: above 0* |.\n"
            // The one furthest below nothing, of two.
            "paragraph: above -2em* |.\n"
            "paragraph: above 1em |.\n"
            "paragraph: above -1em* |.\n");
}

TEST(DocumentBlocks, PaddingIsAddedAndKeepsApartTheMarginsToEitherSideOfIt) {
  EXPECT_EQ(Blocks("above",
                   ".pad { margin: 2em 0; padding: 5px 0 7px } .in { margin: 3em 0 } .zero { padding: 0 } "
                   "span { padding: 9em 0; margin: 9em 0 }",
                   "<body><div class='pad'><p class='in'>one</p></div><p>two</p>"
                   "<div class='zero'><p class='in'>three</p></div><span><p class='in'>four</p></span></body>"),
            "paragraph: above 5em+5px* |.\n"
            "paragraph: above 5em+7px* |.\n"
            // A padding of nothing keeps nothing apart; nor does that of something that is part of a line.
            "paragraph: above 3em* |.\n"
            "paragraph: above 3em* |.\n");
}

TEST(DocumentBlocks, AnElementWithNothingInItMakesNoBlockAndItsMarginsMeetThoseAroundIt) {
  EXPECT_EQ(Blocks("above",
                   ".gap { margin: 3em 0 1em } .tall { height: 2em; margin: 1em 0 } .in { height: 10px } "
                   "span { height: 9em }",
                   "<body><p>one</p><div class='gap'></div><p>two</p><div class='gap'> <span> </span></div><p>three</p>"
                   "<div class='tall'></div><p>four</p><div class='tall'>five</div><p>six</p>"
                   "<div class='tall'><div class='in'></div></div><p>seven</p><div class='tall'><span/></div>"
                   "<p>eight</p></body>"),
            "paragraph: above 1em |.\n"
            "paragraph: above 3em* |.\n"
            "paragraph: above 3em* |.\n"
            // A height the book gave it is added, as padding is: the margins above it, it, and those below.
            "paragraph: above 4em* |.\n"
            // A height on what has something in it is no space. Of two margins as large as each other the
            // first is the one given: the paragraph's own before, and the book's after.
            "paragraph: above 1em |.\n"
            "paragraph: above 1em* |.\n"
            // One with only a height inside it has something in it.
            "paragraph: above 2em+10px* |.\n"
            "paragraph: above 4em* |.\n");
  // A height is the book's, whatever the margins beside it are.
  EXPECT_EQ(Blocks("above", ".in { height: 10px }", "<body><div>one</div><div class='in'/><div>two</div></body>"),
            "paragraph: |.\nparagraph: above 10px* |.\n");
}

TEST(DocumentBlocks, APageThatEndsAfterAnElementEndsBeforeTheBlockThatFollows) {
  EXPECT_EQ(Blocks("breaks",
                   ".before { page-break-before: always } .after { page-break-after: always } "
                   "span { page-break-before: always }",
                   "<body><p>one</p><div class='before'><div><p>two</p></div><p>three</p></div>"
                   "<p class='after'>four</p><div><div><p>five</p></div></div><div class='after'><p>six</p>"
                   "<p>seven</p></div><div class='after'></div><hr/><p>eight <span>nine</span></p></body>"),
            "paragraph: |.\n"
            // Before the first block inside it, however deep.
            "paragraph: breaks page-before |.\n"
            "paragraph: |.\n"
            // The block a page ends after says only that it is not kept with what follows, as the book's.
            "paragraph: breaks not-with-next* |.\n"
            "paragraph: breaks page-before |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n"
            // After one with nothing in it, too; and a rule is a block that follows.
            "rule: breaks page-before |.\n"
            "paragraph: |.\n");
}

TEST(DocumentBlocks, WhatIsToBeKeptOnOnePageIsByTheNearestBlockAroundIt) {
  EXPECT_EQ(Blocks("breaks", ".with { break-after: avoid } .together { break-inside: avoid } h3 { break-after: auto }",
                   "<body><h2>one</h2><h3>two</h3><p class='with together'>three <i>four</i></p>"
                   "<div class='together'><p>five</p>six</div><div class='with'><span>seven</span></div></body>"),
            "heading 2: breaks with-next |.\n"
            "heading 3: breaks not-with-next* |.\n"
            "paragraph: breaks with-next* together |.\n"
            "paragraph: |.\n"
            "paragraph: breaks together |.\n"
            "paragraph: breaks with-next* |.\n");
}

TEST(DocumentBlocks, SaysWhatIsFloatedAndWhatIsCentredByItsMargins) {
  EXPECT_EQ(Blocks("floated centred",
                   ".side { float: right } .mid { width: 60%; margin: 0 auto } .left { width: 60%; margin-left: auto } "
                   ".wide { margin: 0 auto } .narrow { width: 60% }",
                   "<body><div class='side'>one<p>two</p></div><p class='mid'>three</p><div class='mid'><p>four</p>five"
                   "</div><p class='left'>six</p><p class='wide'>seven</p><p class='narrow'>eight</p></body>"),
            // Everything in a block that was floated.
            "paragraph: floated yes |.\n"
            "paragraph: floated yes |.\n"
            "paragraph: centred 60% |.\n"
            // Only the text that stands straight in what is centred.
            "paragraph: |.\n"
            "paragraph: centred 60% |.\n"
            // `auto` to one side, or no width, or a width alone, centres nothing.
            "paragraph: |.\n"
            "paragraph: |.\n"
            "paragraph: |.\n");
}

TEST(DocumentBlocks, SaysHowDeepInQuotationsAndInListsABlockIs) {
  EXPECT_EQ(Blocks("quotes lists", "blockquote.inline { display: inline }",
                   "<body><blockquote>one<blockquote><p>two</p></blockquote><ul><li>three<ol><li>four</li></ol></li>"
                   "</ul></blockquote><p>five</p><blockquote class='inline'>six</blockquote>"
                   "<BLOCKQUOTE>seven</BLOCKQUOTE></body>"),
            "paragraph: quotes 1 |.\n"
            "paragraph: quotes 2 |.\n"
            "list-item: quotes 1; lists 1 |.\n"
            "list-item: quotes 1; lists 2 |.\n"
            "paragraph: |.\n"
            // A quotation the book makes part of a line is none.
            "paragraph: |.\n"
            "paragraph: quotes 1 |.\n");
}

TEST(DocumentBlocks, ABlockHasTheFirstLettersStyleOfTheElementItIsTheFirstBlockOf) {
  EXPECT_EQ(Blocks("letter",
                   "p.drop::first-letter { float: left; font-size: 3em; font-weight: bold } "
                   "div.cap::first-letter { font-variant: small-caps } "
                   "span::first-letter { font-size: 9em }",
                   "<body><p class='drop'>one<br/>two</p><p>three</p><div class='cap'><p>four</p><p>five</p></div>"
                   "<div class='cap'></div><p>six</p><div class='cap'><p class='drop'>seven</p>eight</div>"
                   "<div class='cap'><p class='drop'></p>nine</div><div class='cap'><hr/>ten</div>"
                   "<p><span>eleven</span></p></body>"),
            "paragraph: letter floated 3000* bold* | /.\n"
            "paragraph: |.\n"
            // Of an element around it, if it is the first block in that.
            "paragraph: letter small-caps* |.\n"
            "paragraph: |.\n"
            // Not of one that closed with nothing in it.
            "paragraph: |.\n"
            // Of the nearest that has one.
            "paragraph: letter floated 3000* bold* |.\n"
            "paragraph: |.\n"
            // The nearer one closed with nothing in it, and that of the one around it is not kept beside it.
            "paragraph: |.\n"
            // A rule has no letter; the text after it has the element's.
            "rule: |.\n"
            "paragraph: letter small-caps* |.\n"
            // What is part of a line has no first letter of its own.
            "paragraph: |.\n");
}

TEST(DocumentBlocks, ABlockBeginsFloatedIfItsFirstTextStandsInAFloatedPartOfALine) {
  EXPECT_EQ(Blocks("begins", ".drop { float: left } div.drop { float: right } br { float: left }",
                   "<body><p><span class='drop'>T</span>he first.</p><p>The <span class='drop'>s</span>econd.</p>"
                   "<p><b><span class='drop'>T</span></b>hird.</p><p><span class='drop'><i>F</i></span>ourth.</p>"
                   "<p><span class='drop'><img/></span>ifth.</p><p><img class='drop'/>ixth.</p>"
                   "<p><span class='drop'></span>Seventh.</p><p><span class='drop'><p>E</p></span>ighth.</p>"
                   "<p><span class='drop'><br/>N</span>inth.</p><div class='drop'>Tenth.</div>"
                   "<p><span class='drop'><span class='drop'>E</span>l</span>eventh.</p><p><br/>Twelfth.</p></body>"),
            // `<` is where the floated text ends.
            "paragraph: begins floated | <.\n"
            // Not where a block is open already.
            "paragraph: |.\n"
            "paragraph: begins floated | <.\n"
            "paragraph: begins floated | <.\n"
            // A picture in one, and a picture that is one.
            "paragraph: begins floated | # <.\n"
            "paragraph: begins floated | # <.\n"
            // One with nothing in it floats nothing.
            "paragraph: |.\n"
            // A block inside one begins floated, and ends before the floated text does.
            "paragraph: begins floated |.\n"
            "paragraph: |.\n"
            "paragraph: begins floated | / <.\n"
            // What is a block and floated is that, and not the beginning of one.
            "paragraph: |.\n"
            // It ends where the outermost that is floated does.
            "paragraph: begins floated | <.\n"
            // A `br` is a line's end, floated or not.
            "paragraph: | /.\n");
}

TEST(DocumentBlocks, SaysOfTextWhetherItIsPartOfABlockAndHowItIsSet) {
  EXPECT_EQ(Blocks("style", ".gone { display: none } i { font-size: 2em }",
                   "<html lang='en'><body> <p> one <b> </b><i>two</i> <span class='gone'>three <b>four</b></span>"
                   " five&#160;</p> <p class='gone'>six</p><p lang='fr'><b>seven</b></p></body></html>"),
            // White space in a block is part of it, and may be set its own way; what is nothing is no part.
            "paragraph: | [in-en] [bold in-en] [2000* italic in-en] [in-en].\n"
            "paragraph: | [bold in-fr].\n");
}

TEST(DocumentBlocks, APictureIsAsTextThatIsMoreThanWhiteSpace) {
  EXPECT_EQ(Blocks("none", "img.block { display: block } .gone { display: none }",
                   "<body><p>one <img/> two</p><div><img/></div><img/> <img/><img class='block'/>"
                   "<img class='gone'/><p class='gone'><img/></p></body>"),
            "paragraph: | #.\n"
            "paragraph: | #.\n"
            "paragraph: | # #.\n"
            // One the book makes a block is a block of its own.
            "paragraph: | #.\n");
}

TEST(DocumentBlocks, AnElementDeeperThanIsKeptStillEndsBlocksAndHasItsTopMargin) {
  const std::string_view sheet = "div { margin: 2em 0; padding: 0 1em } p { margin: 3em 0 }";
  // The `i` is as deep as the `p` that closed before it, and is part of a line.
  const std::string_view document =
      "<a><div><div>one<p>two</p>three<i>four</i> more<h2>five</h2></div>six</div>seven</a>";
  EXPECT_EQ(Walk("above left", sheet, document).lines,
            "paragraph: left 2em*; above 2em* |.\n"
            "paragraph: left 2em*; above 3em* |.\n"
            "paragraph: left 2em*; above 3em* |.\n"
            "heading 2: left 2em*; above 1.25em |.\n"
            "paragraph: left 1em*; above 2em* |.\n"
            "paragraph: above 2em* |");
  // With room for two, the second `div` and what is inside it are not
  // kept: they still end blocks, and what opens still has its top margin,
  // but nothing is known of them when they close, and a block inside them
  // is as one straight inside the first `div`.
  EXPECT_EQ(Walk("above left", sheet, document, 2).lines,
            "paragraph: left 1em*; above 2em* |.\n"
            "paragraph: left 1em*; above 3em* |.\n"
            "paragraph: left 1em* |.\n"
            "paragraph: left 1em*; above 1.25em |.\n"
            "paragraph: left 1em* |.\n"
            "paragraph: above 2em* |");
}

TEST(DocumentBlocks, ABlockMoreThan64DeeperThanIsKeptEndsABlockOnlyAsItOpens) {
  const auto document = [](std::size_t deep) {
    std::string text = "<a>";
    for (std::size_t i = 0; i < deep; ++i) {
      text += "<i>";
    }
    text += "one<div>two</div>three";
    for (std::size_t i = 0; i < deep; ++i) {
      text += "</i>";
    }
    return text + "</a>";
  };
  // One element is kept. The `div` is the 64th beyond it, and then the
  // 65th, which is not known to be a block when it closes: the text
  // after it is in the block that began inside it.
  EXPECT_EQ(Walk("none", "", document(63), 1).lines, "paragraph: |.\nparagraph: |.\nparagraph: |");
  EXPECT_EQ(Walk("none", "", document(64), 1).lines, "paragraph: |.\nparagraph: |");
}

TEST(DocumentBlocks, WithNoMemoryBlocksStillBeginAndEndAndSayNothingOfThemselves) {
  const Walked walked = Walk("", "p { margin: 1em 2em; text-align: center } li { font-weight: bold }",
                             "<body><p>one</p><ul><li>two<br/>three</li></ul><hr/><h2>four</h2></body>", 0);
  EXPECT_EQ(walked.lines,
            // The first paragraph's top margin; the list's, the paragraph's bottom one not being kept; the
            // rule's; and the heading's, which is not known to be one.
            "paragraph: above 1em* | [plain].\n"
            "paragraph: above 1em | [plain] /.\n"
            "rule: above 0.5em |.\n"
            "paragraph: above 1.25em | [plain].\n");
}

// A style that sets one property to a word.
bn::epub::CssStyle Setting(bn::epub::CssProperty property, bn::epub::CssKeyword keyword) {
  bn::epub::CssStyle style;
  bn::epub::CssValue& value = style.values.at(static_cast<std::size_t>(property));
  value.kind = bn::epub::CssValue::Kind::kKeyword;
  value.keyword = keyword;
  style.set = 1U << static_cast<unsigned>(property);
  return style;
}

// And one that sets a property to a length.
bn::epub::CssStyle Setting(bn::epub::CssProperty property, std::int32_t number, bn::epub::CssUnit unit) {
  bn::epub::CssStyle style;
  bn::epub::CssValue& value = style.values.at(static_cast<std::size_t>(property));
  value.kind = bn::epub::CssValue::Kind::kLength;
  value.number = number;
  value.unit = unit;
  style.set = 1U << static_cast<unsigned>(property);
  return style;
}

TEST(DocumentBlocks, WhiteSpaceIsASpaceATabALineEndAReturnOrAFormFeed) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (2 * DocumentBlocks::kLevelBytes));
  DocumentBlocks blocks(memory);
  blocks.Begin();
  BlockChange change = blocks.Text(" \t\n\r\f");
  EXPECT_FALSE(change.begins || change.shown);
  EXPECT_FALSE(blocks.Text("").begins);
  // A space that may not be broken at, and a vertical tab, are text.
  for (const std::string_view text : {"\xC2\xA0", "\v", " \f\rx"}) {
    blocks.Begin();
    change = blocks.Text(text);
    EXPECT_TRUE(change.begins && change.shown) << text;
  }
}

TEST(DocumentBlocks, ADocumentBegunStartsFromNothing) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (2 * DocumentBlocks::kLevelBytes));
  DocumentBlocks blocks(memory);
  blocks.Begin();
  const bn::epub::CssStyle nothing;
  const bn::epub::CssStyle bold = Setting(bn::epub::CssProperty::kFontWeight, bn::epub::CssKeyword::kBold);
  ElementStyle said;
  const auto open = [&](std::string_view name) {
    return blocks.Open(name, bn::epub::XmlAttributes(nullptr), nothing, nothing, said);
  };
  // A list, an item, a block open, an element deeper than is kept that
  // is a block, margins waiting.
  open("ol");
  open("li");
  open("blockquote");
  EXPECT_TRUE(blocks.Text("x").begins);
  EXPECT_TRUE(blocks.Record().hasMarker);
  EXPECT_TRUE(blocks.HasBlock());
  EXPECT_TRUE(blocks.Close().ends);
  open("p");

  // And with other things waiting: a space gathered, a margin that has
  // met nothing yet, a page to end, a first letter's style, floated text,
  // something that is nothing.
  blocks.Begin();
  blocks.Open("div", bn::epub::XmlAttributes(nullptr),
              Setting(bn::epub::CssProperty::kPaddingTop, 10000, bn::epub::CssUnit::kPx), nothing, said);
  blocks.Open("p", bn::epub::XmlAttributes(nullptr),
              Setting(bn::epub::CssProperty::kBreakBefore, bn::epub::CssKeyword::kAlways), bold, said);
  EXPECT_TRUE(said.hasFirstLetter);
  blocks.Open("span", bn::epub::XmlAttributes(nullptr),
              Setting(bn::epub::CssProperty::kFloat, bn::epub::CssKeyword::kLeft), nothing, said);
  blocks.Open("b", bn::epub::XmlAttributes(nullptr),
              Setting(bn::epub::CssProperty::kDisplay, bn::epub::CssKeyword::kNone), nothing, said);
  EXPECT_FALSE(blocks.Text("x").shown);

  blocks.Begin();
  EXPECT_EQ(blocks.Depth(), 0U);
  EXPECT_FALSE(blocks.HasBlock());
  open("p");
  open("i");
  open("u");
  const BlockChange change = blocks.Text("x");
  EXPECT_TRUE(change.begins);
  EXPECT_TRUE(change.shown);
  // The paragraph's own margin above it, and nothing of the last document's.
  EXPECT_EQ(blocks.Record().spaceBefore, (StyleLength{.em = 64, .px = 0, .percent = 0}));
  EXPECT_FALSE(blocks.Record().hasMarker);
  EXPECT_FALSE(blocks.Record().breakBefore);
  EXPECT_FALSE(blocks.Record().hasFirstLetter);
  EXPECT_FALSE(blocks.Record().beginsFloated);
  EXPECT_EQ(blocks.Record().kind, BlockKind::kParagraph);
  // The third element is part of a line, and closing it ends nothing,
  // though the last document had a block that deep.
  EXPECT_FALSE(blocks.Close().ends);
  EXPECT_TRUE(blocks.HasBlock());
}

TEST(DocumentBlocks, AnElementClosedThatWasNeverOpenedIsNothing) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (2 * DocumentBlocks::kLevelBytes));
  DocumentBlocks blocks(memory);
  blocks.Begin();
  const BlockChange change = blocks.Close();
  EXPECT_FALSE(change.ends || change.begins || change.lineEnds || change.floatEnds || change.shown);
  EXPECT_EQ(blocks.Depth(), 0U);
  EXPECT_EQ(blocks.TextNow(), TextStyle{});
  // Text where no element is open is a block all the same.
  EXPECT_TRUE(blocks.Text("x").begins);
  EXPECT_EQ(blocks.Record().kind, BlockKind::kParagraph);
}

TEST(DocumentBlocks, KeepsNoMoreOpenElementsThanItsNumbersHaveRoomFor) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (300 * DocumentBlocks::kLevelBytes));
  const DocumentBlocks blocks(memory);
  EXPECT_EQ(blocks.Levels(), DocumentBlocks::kMaxLevels);
  std::array<std::uint8_t, StyleStack::kLanguagesBytes + DocumentBlocks::kLevelBytes - 1> small{};
  EXPECT_EQ(DocumentBlocks(small).Levels(), 0U);
  std::array<std::uint8_t, StyleStack::kLanguagesBytes + DocumentBlocks::kLevelBytes> one{};
  EXPECT_EQ(DocumentBlocks(one).Levels(), 1U);
}

TEST(DocumentBlocks, DoesNoMoreWorkThanItSaysItMay) {
  std::vector<std::uint8_t> memory(StyleStack::kLanguagesBytes + (4 * DocumentBlocks::kLevelBytes));
  DocumentBlocks blocks(memory);
  blocks.Begin();
  const bn::epub::CssStyle nothing;
  ElementStyle said;
  const auto open = [&](std::string_view name, std::vector<const char*> pairs) {
    pairs.push_back(nullptr);
    blocks.Open(name, bn::epub::XmlAttributes(pairs.data()), nothing, nothing, said);
    return blocks.Work();
  };
  // An element's attributes once, and a list's or an item's once more for
  // its number.
  EXPECT_EQ(open("div", {"id", "a", "class", "b"}), 2U);
  EXPECT_EQ(open("ol", {"id", "a", "start", "3", "class", "b"}), 5U);
  EXPECT_EQ(open("li", {"id", "a", "class", "b", "value", "3"}), 6U);
  // White space is looked at to its end; other text as far as the first
  // of it that is not white space, and then each open element that is
  // kept, for the block that begins.
  blocks.Text("   ");
  EXPECT_EQ(blocks.Work(), 3U);
  blocks.Text("  x  yz");
  EXPECT_EQ(blocks.Work(), 3U + 3U);
  // In a block that is open, no element is looked at.
  blocks.Text("  x  yz");
  EXPECT_EQ(blocks.Work(), 3U);
  blocks.Close();
  EXPECT_EQ(blocks.Work(), 0U);
  blocks.Picture();
  EXPECT_EQ(blocks.Work(), 2U);
  EXPECT_EQ(DocumentBlocks::MostWorkFor(3, 0, 4), StyleStack::MostWorkFor(3) + 3 + 4);
  EXPECT_EQ(DocumentBlocks::MostWorkFor(0, 7, 4), StyleStack::MostWorkFor(0) + 7 + 4);
}

TEST(DocumentBlocks, NamesTheKindsOfBlock) {
  EXPECT_STREQ(bn::epub::BlockKindName(BlockKind::kParagraph), "paragraph");
  EXPECT_STREQ(bn::epub::BlockKindName(BlockKind::kHeading), "heading");
  EXPECT_STREQ(bn::epub::BlockKindName(BlockKind::kListItem), "list-item");
  EXPECT_STREQ(bn::epub::BlockKindName(BlockKind::kRule), "rule");
  EXPECT_STREQ(bn::epub::BlockKindName(BlockKind::kTableCell), "table-cell");
}

}  // namespace
