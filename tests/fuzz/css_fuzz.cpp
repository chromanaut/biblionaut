// The fuzz target for the reader of CSS (epub/css.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes the source gives at a call
//   the rest a style sheet, or what passes for one
//
// The sheet is read twice, once from a source that gives all it is asked
// for and once in the pieces byte 0 chooses; then the same bytes are read
// as a `style` attribute, and as a media query and a list of font
// families. What has to hold, whatever the bytes are:
//
//   - the reading ends, having asked its source once at each step for no
//     more than kChunkBytes;
//   - a rule is told whole and in order: its beginning, one selector or
//     more, what it sets, its end, with nothing of another between;
//   - a selector has from one part to kMaxCompounds, no more classes than
//     kMaxClasses and no more asked of attributes than kMaxAttributes, an
//     element's name and an attribute's in small letters, every name and
//     value all there, and a rank that its parts account for;
//   - a declaration is of a property and a keyword there are, a number is
//     within kCssMaxNumber, and is not below zero where the property
//     cannot be;
//   - the reader's counts are the counts of what it told;
//   - the two readings tell the same;
//   - a `style` attribute gives declarations and nothing else.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/byte_source.h"
#include "epub/css.h"
#include "fuzz_support.h"

namespace {

using bn::epub::CssAttributeTest;
using bn::epub::CssCombinator;
using bn::epub::CssDropped;
using bn::epub::CssKeyword;
using bn::epub::CssProperty;
using bn::epub::CssReader;
using bn::epub::CssStatus;
using bn::epub::CssUnit;
using bn::epub::CssValue;

constexpr std::array<std::size_t, 8> kSourcePieces = {1024, 1, 2, 7, 64, 255, 1000, 1023};

// Checks everything it is told, and adds it up for setting one reading
// beside another.
class Listener final : public bn::epub::CssHandler {
 public:
  // `hasRules` is false for a `style` attribute, which has none.
  explicit Listener(bool hasRules) noexcept : rules_(hasRules) {}

  void OnRuleBegin() noexcept override {
    BN_FUZZ_CHECK(rules_ && !inRule_, "a rule began inside another, or where there are no rules");
    inRule_ = true;
    selectorsOfRule_ = 0;
    declared_ = false;
    Add(1);
  }

  void OnSelector(const bn::epub::CssSelector& selector) noexcept override {
    BN_FUZZ_CHECK(inRule_ && !declared_, "a selector was told outside a rule, or after what the rule sets");
    ++selectorsOfRule_;
    BN_FUZZ_CHECK(!selector.compounds.empty() && selector.compounds.size() <= CssReader::kMaxCompounds,
                  "a selector has no parts, or more than there is room for");
    std::uint32_t ids = 0;
    std::uint32_t classes = 0;
    std::uint32_t attributes = 0;
    std::uint32_t elements = 0;
    const auto hasCapital = [](std::string_view name) {
      return std::any_of(name.begin(), name.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; });
    };
    for (std::size_t i = 0; i < selector.compounds.size(); ++i) {
      const bn::epub::CssCompound& part = selector.compounds[i];
      BN_FUZZ_CHECK(
          (part.combinator == CssCombinator::kFirst) == (i == 0) && part.combinator <= CssCombinator::kAdjacent,
          "a selector's first part is joined to something, or a later one to nothing");
      BN_FUZZ_CHECK(!hasCapital(part.element), "an element's name has a capital in it");
      Add(bn::fuzz::Touch(part.element) + bn::fuzz::Touch(part.id) + static_cast<std::uint32_t>(part.combinator));
      ids += part.id.empty() ? 0 : 1;
      elements += part.element.empty() ? 0 : 1;
      for (const std::string_view name : part.classes) {
        BN_FUZZ_CHECK(!name.empty(), "a class with no name");
        Add(bn::fuzz::Touch(name));
        ++classes;
      }
      for (const bn::epub::CssAttribute& attribute : part.attributes) {
        BN_FUZZ_CHECK(!attribute.name.empty() && !hasCapital(attribute.name),
                      "an attribute with no name, or with a capital in its name");
        BN_FUZZ_CHECK(attribute.test <= CssAttributeTest::kContains,
                      "a way of asking of an attribute that there is not");
        BN_FUZZ_CHECK(attribute.test != CssAttributeTest::kPresent || (attribute.value.empty() && !attribute.anyCase),
                      "a value to hold an attribute against, where only its being there was asked");
        Add(bn::fuzz::Touch(attribute.name) + bn::fuzz::Touch(attribute.value) +
            static_cast<std::uint32_t>(attribute.test) + (attribute.anyPrefix ? 16U : 0U) +
            (attribute.anyCase ? 32U : 0U));
        ++attributes;
      }
    }
    BN_FUZZ_CHECK(classes <= CssReader::kMaxClasses, "a selector has more classes than there is room for");
    BN_FUZZ_CHECK(attributes <= CssReader::kMaxAttributes, "a selector asks more of attributes than there is room for");
    // The rank counts each id, each class, each thing asked of an
    // attribute and each element's name, and may count more: `:link` with
    // the classes, `::first-letter` with the elements.
    BN_FUZZ_CHECK((selector.specificity >> 16U) == ids &&
                      ((selector.specificity >> 8U) & 0xFFU) >= classes + attributes &&
                      (selector.specificity & 0xFFU) == elements + (selector.firstLetter ? 1U : 0U),
                  "a selector's rank is not what its parts come to");
    Add(selector.specificity + (selector.firstLetter ? 7U : 0U));
  }

  void OnDeclaration(const bn::epub::CssDeclaration& declaration) noexcept override {
    BN_FUZZ_CHECK(inRule_ == rules_, "something was set outside a rule, or a `style` attribute had a rule");
    declared_ = true;
    ++declarations;
    const CssValue& value = declaration.value;
    BN_FUZZ_CHECK(declaration.property <= CssProperty::kHeight, "a property there is not");
    Add(static_cast<std::uint32_t>(declaration.property) + (declaration.important ? 100U : 0U));
    switch (value.kind) {
      case CssValue::Kind::kKeyword:
        BN_FUZZ_CHECK(value.keyword <= CssKeyword::kLarger, "a keyword there is not");
        Add(static_cast<std::uint32_t>(value.keyword));
        break;
      case CssValue::Kind::kLength: {
        BN_FUZZ_CHECK(value.unit <= CssUnit::kPercent, "a unit there is not");
        BN_FUZZ_CHECK(value.number >= -bn::epub::kCssMaxNumber && value.number <= bn::epub::kCssMaxNumber,
                      "a number further from zero than a number can be");
        const CssProperty property = declaration.property;
        const bool mayBeBelow = property == CssProperty::kTextIndent || property == CssProperty::kVerticalAlign ||
                                (property >= CssProperty::kMarginTop && property <= CssProperty::kMarginLeft);
        BN_FUZZ_CHECK(mayBeBelow || value.number >= 0, "a number below zero for a property that cannot be");
        if (property == CssProperty::kFontWeight) {
          BN_FUZZ_CHECK(value.unit == CssUnit::kNumber && value.number >= 1000 && value.number <= 1'000'000 &&
                            value.number % 1000 == 0,
                        "a weight that is not a whole number from 1 to 1000");
        }
        Add(static_cast<std::uint32_t>(value.number) + static_cast<std::uint32_t>(value.unit));
        break;
      }
      case CssValue::Kind::kFamilies: {
        BN_FUZZ_CHECK(declaration.property == CssProperty::kFontFamily, "a list of families for another property");
        // The names come off the list one at a time until there are no
        // more, which is before the list's bytes run out.
        std::array<char, 24> name{};
        std::string_view rest = value.families;
        std::size_t names = 0;
        for (std::string_view one = bn::epub::CssNextFamily(rest, name); !one.empty();
             one = bn::epub::CssNextFamily(rest, name)) {
          BN_FUZZ_CHECK(one.size() <= name.size() && ++names <= value.families.size(),
                        "a family's name is too long, or the names do not end");
          Add(bn::fuzz::Touch(one));
        }
        break;
      }
    }
  }

  void OnRuleEnd() noexcept override {
    BN_FUZZ_CHECK(inRule_ && selectorsOfRule_ > 0, "a rule ended that had not begun, or that had no selector");
    inRule_ = false;
    ++rules;
    Add(2);
  }

  void OnImport(std::string_view url) noexcept override {
    BN_FUZZ_CHECK(rules_ && !inRule_ && !url.empty(), "an import inside a rule, or of nothing");
    Add(bn::fuzz::Touch(url) + 3);
  }

  void OnDropped(CssDropped what, std::string_view text) noexcept override {
    BN_FUZZ_CHECK(static_cast<std::size_t>(what) < bn::epub::kCssDroppedKinds,
                  "a kind of thing passed over that there is not");
    ++dropped[static_cast<std::size_t>(what)];
    Add(bn::fuzz::Touch(text) + static_cast<std::uint32_t>(what));
  }

  bool InRule() const noexcept { return inRule_; }

  std::uint32_t rules = 0;
  std::uint32_t declarations = 0;
  std::array<std::uint32_t, bn::epub::kCssDroppedKinds> dropped{};
  std::uint32_t digest = 0;  // Everything told, in order.

 private:
  void Add(std::uint32_t value) noexcept { digest = (digest * 31U) + value; }

  bool rules_;
  bool inRule_ = false;
  bool declared_ = false;
  std::uint32_t selectorsOfRule_ = 0;
};

// Reads `sheet`, given `pieceBytes` at a call, and returns what was told.
Listener Read(std::span<const std::uint8_t> sheet, std::size_t pieceBytes) {
  bn::fuzz::PiecewiseSource source(sheet, pieceBytes);
  Listener listener(true);
  CssReader reader;
  reader.Begin(source, listener);
  std::size_t steps = 0;
  bool more = true;
  while (more) {
    const std::size_t takenBefore = source.Taken();
    const std::size_t callsBefore = source.Calls();
    more = reader.Step();
    ++steps;
    BN_FUZZ_CHECK(source.Calls() - callsBefore == 1 && source.Taken() - takenBefore <= CssReader::kChunkBytes,
                  "a step did not ask its source once, or took more than one piece");
    BN_FUZZ_CHECK(steps <= sheet.size() + 2, "the reading of a sheet does not end");
  }
  BN_FUZZ_CHECK(reader.Status() == CssStatus::kOk && reader.Finished(),
                "a sheet from a source that works was not read");
  BN_FUZZ_CHECK(!reader.Step(), "a reading that was over went on");
  BN_FUZZ_CHECK(!listener.InRule(), "the sheet ended and a rule did not");
  BN_FUZZ_CHECK(reader.Rules() == listener.rules && reader.Declarations() == listener.declarations,
                "the reader's counts of rules and declarations are not what it told");
  for (std::size_t kind = 0; kind < bn::epub::kCssDroppedKinds; ++kind) {
    BN_FUZZ_CHECK(reader.Dropped(static_cast<CssDropped>(kind)) == listener.dropped[kind],
                  "the reader's count of what it passed over is not what it told");
  }
  return listener;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> sheet = input.Rest();

  const Listener whole = Read(sheet, CssReader::kChunkBytes);
  const Listener pieces = Read(sheet, kSourcePieces[choices & 7U]);
  BN_FUZZ_CHECK(
      whole.digest == pieces.digest && whole.rules == pieces.rules && whole.declarations == pieces.declarations,
      "a sheet was read one way in large pieces and another way in small ones");

  // The same bytes as a `style` attribute, a media query and a list of
  // families.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  const std::string_view text(reinterpret_cast<const char*>(sheet.data()), sheet.size());
  Listener attribute(false);
  bn::epub::ReadCssDeclarations(text, attribute);
  BN_FUZZ_CHECK(attribute.rules == 0, "a `style` attribute had a rule");
  (void)bn::epub::CssMediaApplies(text);
  std::array<char, 16> name{};
  std::string_view rest = text;
  std::size_t names = 0;
  while (!bn::epub::CssNextFamily(rest, name).empty()) {
    BN_FUZZ_CHECK(++names <= text.size(), "the names of a list of families do not end");
  }
  BN_FUZZ_CHECK(rest.empty(), "a list of families ended with something left of it");
  return 0;
}
