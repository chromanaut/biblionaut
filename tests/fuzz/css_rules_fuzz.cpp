// The fuzz target for the table of a book's rules and the cascade
// (epub/css_rules.h), for what each element then comes to
// (epub/style_stack.h), and for the blocks a document is made into
// (epub/document_blocks.h).
//
// What it is given:
//
//   byte 0   bits 0 to 1: how much memory the table has
//            bits 2 to 3: how much the cascade has, for the open elements
//            bits 4 to 5: how the document's sheets are ordered, and
//                         whether one is left out of what it is given
//            bits 6 to 7: how many open elements the stack of styles keeps,
//                         and what finds the document's blocks
//   the rest up to four sheets and then a document, a byte of zero between
//            one and the next: whatever comes last is the document
//
// A document is not XML, which has a target of its own, but a line for
// each thing a parser would tell of: `<name|attribute|value|...` for an
// element that opens, with as many attributes as it has, `>` for one that
// closes, `"` and then some text for text, and `#` for a picture. Any
// other line is passed over.
//
// The sheets are read into a table, all but the last; where the table then
// stands is kept, the last is read, and the document is gone through with
// a cascade beside the plain working-out of the same rules (the reference
// in tests/support). Then the table is taken back to where it stood and
// the document gone through again. What has to hold, whatever the bytes:
//
//   - the table stays inside its memory and its counts are of what it
//     holds: no more nodes or sheets than it says it takes;
//   - if it left no rule out, the cascade gives every element exactly what
//     the plain working-out gives it, and so for its first letter, with
//     however little memory the cascade has;
//   - no element takes more work than MostWorkFor() says;
//   - what is set is of a property, a keyword and a unit there are, and a
//     number within kCssMaxNumber;
//   - a table taken back is as it was: the same size, and the document
//     gone through with it comes to what it would with a table that was
//     never told of the last sheet;
//   - a stack of styles beside the cascade counts the elements that are
//     open, keeps no more of them and no more languages than it has room
//     for, counts each element deeper than that, and does no more work
//     for an element than the most it says; what it says of an
//     element is of kinds and bits there are; an element inside one that
//     is nothing is nothing; a block's text is neither raised nor lowered,
//     and no text is both; a distance between lines has an amount of its
//     own kind and no other; a first letter is told of when a rule set
//     something for it and not otherwise; what it says an open element
//     passes to those inside it is what it said of that element as it
//     opened; and the document gone through again comes to the same;
//   - what finds the document's blocks says of each element what the
//     stack of styles says of it; a block ends only where one is open, and
//     only as an element opens or closes; one begins only where none is
//     open; white space begins none, and other text and a picture are
//     always part of one unless they are inside what is nothing, where
//     nothing happens at all; a block's record is of kinds there are, a
//     heading alone has a level, and a rule has no marker and no first
//     letter; the floated text a block began with ends only for a block
//     that began so; and no call does more work than the most it says.

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
#include "css_reference.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_blocks.h"
#include "epub/style_stack.h"
#include "epub/xml.h"
#include "fuzz_support.h"

namespace {

using bn::epub::CssCascade;
using bn::epub::CssReader;
using bn::epub::CssRules;
using bn::epub::CssStyle;
using bn::epub::CssUnit;
using bn::epub::CssValue;

constexpr std::array<std::size_t, 4> kTableBytes = {65535, 4096, 300, 40};
constexpr std::array<std::size_t, 4> kCascadeBytes = {4096, 96, 6, 0};
constexpr std::array<std::size_t, 4> kStyleLevels = {32, 3, 1, 0};
constexpr std::size_t kMostSheets = 4;
// A document of more than this is cut off, and an element is given no
// more attributes: what is being tried is the table, not how long a test
// can be made to take.
constexpr std::size_t kMostElements = 200;
constexpr std::size_t kMostAttributes = 24;

// One element of a document: its name and attributes, or that it closes.
struct Step {
  bool opens = false;
  bool text = false;     // Text: `name` is the text.
  bool picture = false;  // A picture.
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
};

std::vector<Step> StepsOf(std::string_view document) {
  std::vector<Step> steps;
  std::size_t opened = 0;
  std::size_t depth = 0;
  while (!document.empty()) {
    const std::size_t end = std::min(document.find('\n'), document.size());
    std::string_view line = document;
    line.remove_suffix(document.size() - end);
    document.remove_prefix(std::min(end + 1, document.size()));
    if (line == ">" && depth > 0) {
      steps.push_back({});
      --depth;
    } else if (!line.empty() && line.front() == '"') {
      Step step;
      step.text = true;
      step.name = line.substr(1);
      steps.push_back(std::move(step));
    } else if (line == "#") {
      Step step;
      step.picture = true;
      steps.push_back(std::move(step));
    } else if (!line.empty() && line.front() == '<' && opened < kMostElements) {
      Step step;
      step.opens = true;
      line.remove_prefix(1);
      std::vector<std::string> fields;
      while (true) {
        const std::size_t bar = std::min(line.find('|'), line.size());
        std::string_view field = line;
        field.remove_suffix(line.size() - bar);
        fields.emplace_back(field);
        if (bar == line.size()) {
          break;
        }
        line.remove_prefix(bar + 1);
      }
      step.name = fields.front();
      for (std::size_t i = 1; i + 1 < fields.size() && step.attributes.size() < kMostAttributes; i += 2) {
        // A parser gives an attribute of one name once.
        const bool again = std::any_of(step.attributes.begin(), step.attributes.end(),
                                       [&](const auto& has) { return has.first == fields[i]; });
        if (!again) {
          step.attributes.emplace_back(fields[i], fields[i + 1]);
        }
      }
      steps.push_back(std::move(step));
      ++opened;
      ++depth;
    }
  }
  // Whatever is open at the end closes.
  for (; depth > 0; --depth) {
    steps.push_back({});
  }
  return steps;
}

void Read(std::string_view sheet, bn::epub::CssHandler& handler) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>(sheet.data()), sheet.size()});
  CssReader reader;
  (void)reader.Read(source, handler);
}

void CheckValues(const CssStyle& style) {
  BN_FUZZ_CHECK((style.set >> bn::epub::kCssProperties) == 0, "a property set that there is not");
  for (std::size_t property = 0; property < bn::epub::kCssProperties; ++property) {
    if (((style.set >> property) & 1U) == 0) {
      continue;
    }
    const CssValue& value = style.values[property];
    BN_FUZZ_CHECK(value.kind <= CssValue::Kind::kFamilies, "a kind of value that there is not");
    if (value.kind == CssValue::Kind::kKeyword) {
      BN_FUZZ_CHECK(static_cast<std::size_t>(value.keyword) < bn::epub::kCssKeywords, "a keyword there is not");
    } else if (value.kind == CssValue::Kind::kLength) {
      BN_FUZZ_CHECK(value.unit <= CssUnit::kPercent, "a unit there is not");
      BN_FUZZ_CHECK(value.number >= -bn::epub::kCssMaxNumber && value.number <= bn::epub::kCssMaxNumber,
                    "a number further from zero than a number can be");
    } else {
      BN_FUZZ_CHECK(value.familyClass >= bn::epub::CssFamilyClass::kSerif &&
                        value.familyClass <= bn::epub::CssFamilyClass::kOther,
                    "a list of families that comes to no kind of face");
      BN_FUZZ_CHECK(value.families.empty(), "a cascade gave the names of a list of families");
    }
  }
}

// Everything the cascade said of a document, added up, to set one going
// through beside another.
struct Said {
  std::uint32_t digest = 0;
  void Add(std::uint32_t value) noexcept { digest = (digest * 31U) + value; }
  void Add(const CssStyle& style) noexcept {
    Add(style.set);
    for (std::size_t property = 0; property < bn::epub::kCssProperties; ++property) {
      if (((style.set >> property) & 1U) != 0) {
        const CssValue& value = style.values[property];
        Add(static_cast<std::uint32_t>(value.kind) + static_cast<std::uint32_t>(value.keyword) +
            static_cast<std::uint32_t>(value.number) + static_cast<std::uint32_t>(value.unit) +
            static_cast<std::uint32_t>(value.familyClass));
      }
    }
  }
};

// How text is set, held to what there is and added up.
void CheckText(const bn::epub::TextStyle& text, const bn::epub::StyleStack& stack, Said& said) {
  constexpr auto kNoSuchBits = static_cast<std::uint16_t>(~bn::epub::kTextBits);
  BN_FUZZ_CHECK((text.bits & kNoSuchBits) == 0 && (text.fromBook & kNoSuchBits) == 0,
                "a bit of how text is set that there is not");
  BN_FUZZ_CHECK((text.bits & bn::epub::kTextRaised) == 0 || (text.bits & bn::epub::kTextLowered) == 0,
                "text that is both raised and lowered");
  BN_FUZZ_CHECK(text.family <= bn::epub::CssFamilyClass::kOther, "a kind of face there is not");
  BN_FUZZ_CHECK(text.language <= stack.Languages(), "a language that is not kept");
  said.Add(text.bits + (std::uint32_t{text.fromBook} << 8U) + (std::uint32_t{text.size} << 16U));
  said.Add(static_cast<std::uint32_t>(text.family) + (text.familyFromBook ? 16U : 0U) + (text.sizeFromBook ? 32U : 0U) +
           (std::uint32_t{text.language} << 8U));
}

void Add(const bn::epub::StyleLength& length, Said& said) {
  said.Add(static_cast<std::uint16_t>(length.em) + (std::uint32_t{static_cast<std::uint16_t>(length.px)} << 16U));
  said.Add(static_cast<std::uint16_t>(length.percent));
}

// What an element comes to, held to what its header says and added up.
// `insideNothing` is whether an element it is inside is nothing.
void CheckElement(const bn::epub::ElementStyle& element, const CssStyle& firstLetter, bool insideNothing,
                  const bn::epub::StyleStack& stack, Said& said) {
  using bn::epub::StyleDisplay;
  BN_FUZZ_CHECK(element.display <= StyleDisplay::kListItem, "a way of being on a page that there is not");
  BN_FUZZ_CHECK(!insideNothing || element.display == StyleDisplay::kNone,
                "an element inside one that is nothing is something");
  said.Add(static_cast<std::uint32_t>(element.display));
  if (element.display == StyleDisplay::kNone) {
    return;
  }
  CheckText(element.text, stack, said);
  constexpr auto kMoved = static_cast<std::uint16_t>(bn::epub::kTextRaised | bn::epub::kTextLowered);
  BN_FUZZ_CHECK(element.display == StyleDisplay::kInline || (element.text.bits & kMoved) == 0,
                "a block whose text is raised or lowered");
  BN_FUZZ_CHECK(element.align <= bn::epub::StyleAlign::kEnd && element.marker <= bn::epub::StyleMarker::kUpperRoman &&
                    element.lineHeight.kind <= bn::epub::StyleLineHeight::Kind::kLength &&
                    element.floated <= bn::epub::StyleFloat::kRight,
                "an alignment, a marker, a kind of line height or a side that there is not");
  BN_FUZZ_CHECK((element.marginFromBook & 0xF0U) == 0 && (element.paddingFromBook & 0xF0U) == 0,
                "a side that there is not");
  BN_FUZZ_CHECK(
      (element.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kMultiple || element.lineHeight.multiple == 0) &&
          (element.lineHeight.kind == bn::epub::StyleLineHeight::Kind::kLength || element.lineHeight.length.IsZero()),
      "a distance between lines that is of one kind and has an amount of another");
  BN_FUZZ_CHECK(element.hasFirstLetter == (firstLetter.set != 0),
                "a first letter told of that no rule set, or one that a rule set not told of");
  said.Add(static_cast<std::uint32_t>(element.align) + (static_cast<std::uint32_t>(element.marker) << 4U) +
           (static_cast<std::uint32_t>(element.lineHeight.kind) << 8U) +
           (static_cast<std::uint32_t>(element.floated) << 12U) + (std::uint32_t{element.lineHeight.multiple} << 16U));
  said.Add((element.alignFromBook ? 1U : 0U) + (element.hasIndent ? 2U : 0U) + (element.indentFromBook ? 4U : 0U) +
           (element.lineHeightFromBook ? 8U : 0U) + (element.markerFromBook ? 16U : 0U) +
           (element.rightToLeft ? 32U : 0U) + (element.spacesAsWritten ? 64U : 0U) +
           (element.marginLeftAuto ? 128U : 0U) + (element.marginRightAuto ? 256U : 0U) +
           (element.hasWidth ? 512U : 0U) + (element.hasHeight ? 1024U : 0U) + (element.breakBefore ? 2048U : 0U) +
           (element.breakAfter ? 4096U : 0U) + (element.keepWithNext ? 8192U : 0U) +
           (element.keepTogether ? 16384U : 0U) + (element.keepWithNextFromBook ? 32768U : 0U) +
           (std::uint32_t{element.marginFromBook} << 16U) + (std::uint32_t{element.paddingFromBook} << 24U));
  Add(element.indent, said);
  Add(element.lineHeight.length, said);
  Add(element.width, said);
  Add(element.height, said);
  for (std::size_t side = 0; side < 4; ++side) {
    Add(element.margin.at(side), said);
    Add(element.padding.at(side), said);
  }
  if (element.hasFirstLetter) {
    CheckText(element.firstLetter, stack, said);
    said.Add(element.firstLetterFloated ? 1U : 0U);
  }
}

// A block's record, held to what its header says and added up.
void CheckRecord(const bn::epub::BlockRecord& record, const bn::epub::StyleStack& stack, Said& said) {
  using bn::epub::BlockKind;
  BN_FUZZ_CHECK(record.kind <= BlockKind::kTableCell, "a kind of block that there is not");
  BN_FUZZ_CHECK(record.kind == BlockKind::kHeading ? record.headingLevel >= 1 && record.headingLevel <= 6
                                                   : record.headingLevel == 0,
                "a heading with no level, or a level on what is no heading");
  BN_FUZZ_CHECK(record.align <= bn::epub::StyleAlign::kEnd && record.marker <= bn::epub::StyleMarker::kUpperRoman &&
                    record.lineHeight.kind <= bn::epub::StyleLineHeight::Kind::kLength,
                "an alignment, a marker or a kind of line height that there is not");
  BN_FUZZ_CHECK(record.hasMarker ||
                    (record.marker == bn::epub::StyleMarker::kNone && record.number == 0 && !record.markerFromBook),
                "a marker's number on a block that has no marker");
  BN_FUZZ_CHECK(record.centred || record.width.IsZero(), "a width on a block that is not centred");
  BN_FUZZ_CHECK(record.hasFirstLetter || !record.firstLetterFloated, "a first letter floated that there is not");
  BN_FUZZ_CHECK(
      record.kind != BlockKind::kRule || (!record.hasMarker && !record.hasFirstLetter && !record.beginsFloated),
      "a rule with a marker, a first letter or floated text");
  said.Add(static_cast<std::uint32_t>(record.kind) + (std::uint32_t{record.headingLevel} << 4U) +
           (static_cast<std::uint32_t>(record.align) << 8U) + (static_cast<std::uint32_t>(record.marker) << 12U) +
           (static_cast<std::uint32_t>(record.lineHeight.kind) << 16U) + (std::uint32_t{record.quoteDepth} << 20U));
  said.Add((record.alignFromBook ? 1U : 0U) + (record.rightToLeft ? 2U : 0U) + (record.spacesAsWritten ? 4U : 0U) +
           (record.sizeFromBook ? 8U : 0U) + (record.lineHeightFromBook ? 16U : 0U) + (record.hasIndent ? 32U : 0U) +
           (record.indentFromBook ? 64U : 0U) + (record.marginLeftFromBook ? 128U : 0U) +
           (record.marginRightFromBook ? 256U : 0U) + (record.spaceBeforeFromBook ? 512U : 0U) +
           (record.breakBefore ? 1024U : 0U) + (record.keepWithNext ? 2048U : 0U) +
           (record.keepWithNextFromBook ? 4096U : 0U) + (record.keepTogether ? 8192U : 0U) +
           (record.floated ? 16384U : 0U) + (record.centred ? 32768U : 0U) + (record.hasMarker ? 65536U : 0U) +
           (record.markerFromBook ? 131072U : 0U) + (record.hasFirstLetter ? 262144U : 0U) +
           (record.firstLetterFloated ? 524288U : 0U) + (record.beginsFloated ? 1048576U : 0U) +
           (std::uint32_t{record.listDepth} << 24U));
  said.Add(record.size + (std::uint32_t{record.lineHeight.multiple} << 16U));
  said.Add(static_cast<std::uint32_t>(record.number));
  Add(record.lineHeight.length, said);
  Add(record.indent, said);
  Add(record.marginLeft, said);
  Add(record.marginRight, said);
  Add(record.spaceBefore, said);
  Add(record.width, said);
  if (record.hasFirstLetter) {
    CheckText(record.firstLetter, stack, said);
  }
}

// What a call made of a document's blocks, held to what the header says.
// `open` is whether a block was open before it, and is made whether one
// is after; `element` is whether the call was for an element, which alone
// can end a block; `floated` is whether the open block began floated.
void CheckChange(const bn::epub::BlockChange& change, const bn::epub::DocumentBlocks& blocks, bool element, bool& open,
                 bool& floated, Said& said) {
  BN_FUZZ_CHECK(!change.ends || (open && element),
                "a block ends where none is open, or where no element opens or closes");
  open = open && !change.ends;
  floated = floated && !change.ends;
  BN_FUZZ_CHECK(!change.begins || !open, "a block begins where one is open");
  if (change.begins) {
    open = true;
    CheckRecord(blocks.Record(), blocks.Styles(), said);
    floated = blocks.Record().beginsFloated;
  }
  BN_FUZZ_CHECK(blocks.HasBlock() == open, "whether a block is open is not what the calls said");
  BN_FUZZ_CHECK((!change.lineEnds && !change.shown) || open, "a line ends, or something is shown, in no block");
  BN_FUZZ_CHECK(!change.floatEnds || (open && floated), "floated text ends in a block that did not begin with any");
  floated = floated && !change.floatEnds;
  said.Add((change.ends ? 1U : 0U) + (change.begins ? 2U : 0U) + (change.lineEnds ? 4U : 0U) +
           (change.floatEnds ? 8U : 0U) + (change.shown ? 16U : 0U));
}

// Goes through the document with a cascade over `rules`, with a stack of
// styles beside it that keeps `styleLevels` open elements, and with what
// finds the document's blocks, keeping as many; and with `reference`
// beside the cascade, if that is given, which has to say the same.
Said Go(const CssRules& rules, std::span<const std::uint8_t> sheets, const std::vector<Step>& steps,
        std::size_t cascadeBytes, std::size_t styleLevels, bn::test::CssReference* reference) {
  std::vector<std::uint8_t> memory(cascadeBytes);
  CssCascade cascade(rules, memory);
  cascade.Begin(sheets);
  if (reference != nullptr) {
    reference->Begin({sheets.begin(), sheets.end()}, cascade.Levels());
  }
  // The stack's memory to the byte, so that a level written past it is seen.
  std::vector<std::uint8_t> styleMemory(
      styleLevels == 0 ? 0 : bn::epub::StyleStack::kLanguagesBytes + (styleLevels * bn::epub::StyleStack::kLevelBytes));
  bn::epub::StyleStack stack(styleMemory);
  BN_FUZZ_CHECK(stack.Levels() == styleLevels,
                "the stack of styles keeps another number of elements than it has room for");
  stack.Begin();
  std::vector<std::uint8_t> blocksMemory(styleLevels == 0 ? 0
                                                          : bn::epub::StyleStack::kLanguagesBytes +
                                                                (styleLevels * bn::epub::DocumentBlocks::kLevelBytes));
  bn::epub::DocumentBlocks blocks(blocksMemory);
  BN_FUZZ_CHECK(blocks.Levels() == styleLevels,
                "what finds the blocks keeps another number of elements than it has room for");
  blocks.Begin();
  bool blockOpen = false;
  bool blockFloated = false;
  std::vector<bool> nothing;  // For each open element, whether it or one it is inside is nothing.
  std::uint32_t tooDeep = 0;
  Said said;
  for (const Step& step : steps) {
    if (step.text || step.picture) {
      const bool hidden = !nothing.empty() && nothing.back();
      const bool blank = step.picture ? false : step.name.find_first_not_of(" \t\n\r\f") == std::string::npos;
      const bool was = blockOpen;
      const bn::epub::BlockChange change = step.picture ? blocks.Picture() : blocks.Text(step.name);
      BN_FUZZ_CHECK(blocks.Work() <= bn::epub::DocumentBlocks::MostWorkFor(0, step.name.size(), blocks.Levels()),
                    "text took more work of what finds the blocks than the most it says");
      CheckChange(change, blocks, false, blockOpen, blockFloated, said);
      BN_FUZZ_CHECK(!change.lineEnds && !change.floatEnds, "a line or floated text ends at text");
      if (hidden) {
        BN_FUZZ_CHECK(!change.begins && !change.shown, "what is inside something that is nothing is shown");
      } else if (blank) {
        BN_FUZZ_CHECK(!change.begins && change.shown == was, "white space begins a block, or is shown in none");
      } else {
        BN_FUZZ_CHECK(change.shown && change.begins == !was, "text or a picture is not part of a block");
        const bn::epub::TextStyle now = blocks.TextNow();
        Said scratch;
        CheckText(now, blocks.Styles(), scratch);
        said.Add(scratch.digest);
      }
      continue;
    }
    if (!step.opens) {
      cascade.Close();
      const std::size_t open = stack.Depth();
      stack.Close();
      BN_FUZZ_CHECK(stack.Depth() == (open == 0 ? 0 : open - 1),
                    "an element closed and the stack of styles does not count it so");
      const bool hidden = !nothing.empty() && nothing.back();
      const bn::epub::BlockChange change = blocks.Close();
      BN_FUZZ_CHECK(blocks.Depth() == stack.Depth(), "what finds the blocks counts other elements open than there are");
      BN_FUZZ_CHECK(blocks.Work() <= bn::epub::DocumentBlocks::MostWorkFor(0, 0, blocks.Levels()),
                    "a closing took more work of what finds the blocks than the most it says");
      CheckChange(change, blocks, true, blockOpen, blockFloated, said);
      BN_FUZZ_CHECK(!hidden || (!change.ends && !change.floatEnds), "what is nothing ends a block as it closes");
      BN_FUZZ_CHECK(!change.begins && !change.lineEnds && !change.shown, "a closing begins something");
      if (!nothing.empty()) {
        nothing.pop_back();
      }
      if (reference != nullptr) {
        reference->Close();
      }
      continue;
    }
    std::vector<const char*> pairs;
    pairs.reserve((2 * step.attributes.size()) + 1);
    for (const auto& [name, value] : step.attributes) {
      pairs.push_back(name.c_str());
      pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    const bn::epub::XmlAttributes attributes(pairs.data());
    const std::size_t open = cascade.Depth();
    CssStyle style;
    CssStyle firstLetter;
    cascade.Open(step.name, attributes, style, firstLetter);
    BN_FUZZ_CHECK(cascade.Depth() == open + 1, "an element opened and the cascade does not count it open");
    BN_FUZZ_CHECK(cascade.Work() <= CssCascade::MostWorkFor(rules.Names(), rules.Nodes(), rules.Selectors(),
                                                            attributes.Count(), open),
                  "an element took more work than the most there can be");
    CheckValues(style);
    CheckValues(firstLetter);
    said.Add(style);
    said.Add(firstLetter);
    {
      const std::size_t deep = stack.Depth();
      bn::epub::ElementStyle element;
      stack.Open(step.name, attributes, style, firstLetter, element);
      BN_FUZZ_CHECK(stack.Depth() == deep + 1, "an element opened and the stack of styles does not count it open");
      BN_FUZZ_CHECK(stack.Work() <= bn::epub::StyleStack::MostWorkFor(attributes.Count()),
                    "an element took more work of the stack of styles than the most it says");
      tooDeep += deep >= styleLevels ? 1 : 0;
      BN_FUZZ_CHECK(stack.LeftOut(bn::epub::StyleLeftOut::kTooDeep) == tooDeep,
                    "the elements too deep to keep are not counted as they came");
      BN_FUZZ_CHECK(
          stack.Languages() <= bn::epub::StyleStack::kMaxLanguages && (styleLevels != 0 || stack.Languages() == 0),
          "more languages are kept than there is room for");
      for (std::size_t language = 1; language <= stack.Languages(); ++language) {
        const std::string_view tag = stack.Language(static_cast<std::uint8_t>(language));
        BN_FUZZ_CHECK(!tag.empty() && tag.size() <= bn::epub::StyleStack::kMaxLanguageBytes,
                      "a language's tag is empty, or longer than one may be");
        (void)bn::fuzz::Touch(tag);
      }
      const bool insideNothing = !nothing.empty() && nothing.back();
      CheckElement(element, firstLetter, insideNothing, stack, said);
      nothing.push_back(insideNothing || element.display == bn::epub::StyleDisplay::kNone);

      // What it says the element passes on is what it said of it, where the element is kept.
      if (element.display != bn::epub::StyleDisplay::kNone && deep < styleLevels) {
        bn::epub::ElementStyle passes;
        stack.Passes(deep, passes);
        BN_FUZZ_CHECK(
            passes.text == element.text && passes.align == element.align &&
                passes.alignFromBook == element.alignFromBook && passes.hasIndent == element.hasIndent &&
                passes.indent == element.indent && passes.indentFromBook == element.indentFromBook &&
                passes.lineHeight == element.lineHeight && passes.lineHeightFromBook == element.lineHeightFromBook &&
                passes.marker == element.marker && passes.markerFromBook == element.markerFromBook &&
                passes.rightToLeft == element.rightToLeft && passes.spacesAsWritten == element.spacesAsWritten,
            "what an open element is said to pass on is not what was said of it as it opened");
      }

      // And the blocks: the same is said of the element, and what the header says holds of the rest.
      bn::epub::ElementStyle again;
      const bn::epub::BlockChange change = blocks.Open(step.name, attributes, style, firstLetter, again);
      BN_FUZZ_CHECK(blocks.Depth() == stack.Depth(), "what finds the blocks counts other elements open than there are");
      BN_FUZZ_CHECK(blocks.Work() <= bn::epub::DocumentBlocks::MostWorkFor(attributes.Count(), 0, blocks.Levels()),
                    "an element took more work of what finds the blocks than the most it says");
      Said one;
      Said two;
      CheckElement(element, firstLetter, insideNothing, stack, one);
      CheckElement(again, firstLetter, insideNothing, blocks.Styles(), two);
      BN_FUZZ_CHECK(one.digest == two.digest,
                    "what finds the blocks says something else of an element than the stack of styles");
      CheckChange(change, blocks, true, blockOpen, blockFloated, said);
      BN_FUZZ_CHECK(!nothing.back() || (!change.ends && !change.begins && !change.lineEnds),
                    "what is nothing ends or begins a block as it opens");
      BN_FUZZ_CHECK(!change.floatEnds && !change.shown, "floated text ends, or text is shown, as an element opens");
    }
    if (reference != nullptr) {
      CssStyle plain;
      CssStyle plainFirstLetter;
      // The names as a parser would give them: up to a byte of zero.
      std::vector<std::pair<std::string, std::string>> given;
      given.reserve(attributes.Count());
      for (std::size_t i = 0; i < attributes.Count(); ++i) {
        given.emplace_back(attributes.Name(i), attributes.Value(i));
      }
      reference->Open(step.name, given, plain, plainFirstLetter);
      BN_FUZZ_CHECK(bn::test::CssReference::Same(style, plain),
                    "the cascade gives an element something else than the plain working-out does");
      BN_FUZZ_CHECK(bn::test::CssReference::Same(firstLetter, plainFirstLetter),
                    "the cascade gives a first letter something else than the plain working-out does");
    }
  }
  BN_FUZZ_CHECK(cascade.Depth() == 0, "the document ended and the cascade has an element open");
  for (std::size_t kind = 0; kind < bn::epub::kStyleLeftOutKinds; ++kind) {
    said.Add(stack.LeftOut(static_cast<bn::epub::StyleLeftOut>(kind)));
  }
  return said;
}

void CheckTable(const CssRules& rules, std::size_t bytes) {
  BN_FUZZ_CHECK(rules.Bytes() <= std::max<std::size_t>(bytes, 2) && rules.Capacity() <= CssRules::kMaxBytes,
                "the table says it holds more than it has room for");
  BN_FUZZ_CHECK(rules.Nodes() <= CssRules::kMaxNodes && rules.Sheets() <= CssRules::kMaxSheets,
                "the table has more nodes or sheets than it takes");
  BN_FUZZ_CHECK(rules.Selectors() >= rules.Rules(), "a rule is kept that has no selector");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  std::string_view text(reinterpret_cast<const char*>(rest.data()), rest.size());

  // The sheets, then the document.
  std::vector<std::string_view> sheets;
  while (sheets.size() < kMostSheets) {
    const std::size_t zero = text.find('\0');
    if (zero == std::string_view::npos) {
      break;
    }
    std::string_view sheet = text;
    sheet.remove_suffix(text.size() - zero);
    sheets.push_back(sheet);
    text.remove_prefix(zero + 1);
  }
  const std::vector<Step> steps = StepsOf(text);

  const std::size_t tableBytes = kTableBytes[choices & 3U];
  const std::size_t cascadeBytes = kCascadeBytes[(choices >> 2U) & 3U];
  const std::size_t styleLevels = kStyleLevels[(choices >> 6U) & 3U];
  std::vector<std::uint8_t> memory(tableBytes);
  CssRules rules(memory);
  bn::test::CssReference reference;

  // All but the last sheet; then where the table stands; then the last.
  std::vector<std::uint8_t> numbers;
  CssRules::Place place;
  std::size_t bytesBefore = 0;
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    if (i + 1 == sheets.size()) {
      place = rules.Here();
      bytesBefore = rules.Bytes();
    }
    std::uint8_t number = 0xFF;
    BN_FUZZ_CHECK(rules.BeginSheet(number) && number == i, "a sheet was not begun, or not given the next number");
    Read(sheets[i], rules);
    rules.EndSheet();
    CheckTable(rules, tableBytes);
    reference.BeginSheet();
    Read(sheets[i], reference);
    reference.EndSheet();
    numbers.push_back(number);
  }
  const bool whole = rules.LeftOutForRoom() == 0 && rules.LeftOutForNodes() == 0;

  // The order the document is given them in: as they were read, the other
  // way about, or with the first given again at the end; and with one
  // left out, or a number that is no sheet's put in.
  std::vector<std::uint8_t> given = numbers;
  switch ((choices >> 4U) & 3U) {
    case 1:
      std::reverse(given.begin(), given.end());
      break;
    case 2:
      if (!given.empty()) {
        given.push_back(given.front());
      }
      break;
    case 3:
      if (!given.empty()) {
        given.erase(given.begin());
      }
      given.push_back(200);
      break;
    default:
      break;
  }

  const Said all = Go(rules, given, steps, cascadeBytes, styleLevels, whole ? &reference : nullptr);
  BN_FUZZ_CHECK(Go(rules, given, steps, cascadeBytes, styleLevels, nullptr).digest == all.digest,
                "a document gone through again came to something else");

  // Back to before the last sheet: the table is as it was, and says of
  // the document what a table that never had that sheet says.
  if (!sheets.empty()) {
    rules.BackTo(place);
    CheckTable(rules, tableBytes);
    BN_FUZZ_CHECK(rules.Bytes() == bytesBefore && rules.Sheets() == sheets.size() - 1,
                  "a table taken back is not the size it was");
    const Said back = Go(rules, given, steps, cascadeBytes, styleLevels, nullptr);

    std::vector<std::uint8_t> otherMemory(tableBytes);
    CssRules other(otherMemory);
    for (std::size_t i = 0; i + 1 < sheets.size(); ++i) {
      std::uint8_t number = 0;
      (void)other.BeginSheet(number);
      Read(sheets[i], other);
      other.EndSheet();
    }
    BN_FUZZ_CHECK(other.Bytes() == rules.Bytes() && other.Names() == rules.Names() && other.Nodes() == rules.Nodes() &&
                      other.Rules() == rules.Rules() && other.Selectors() == rules.Selectors(),
                  "a table taken back does not hold what one never told of the last sheet holds");
    const Said never = Go(other, given, steps, cascadeBytes, styleLevels, nullptr);
    BN_FUZZ_CHECK(back.digest == never.digest,
                  "a table taken back says something else of a document than one never told of the last sheet");

    // And it takes the sheet again, to the same end as the first time.
    std::uint8_t number = 0xFF;
    BN_FUZZ_CHECK(rules.BeginSheet(number) && number == sheets.size() - 1, "a table taken back does not take a sheet");
    Read(sheets.back(), rules);
    rules.EndSheet();
    const Said again = Go(rules, given, steps, cascadeBytes, styleLevels, nullptr);
    BN_FUZZ_CHECK(again.digest == all.digest,
                  "a sheet read again after the table was taken back came to something else");
  }
  return 0;
}
