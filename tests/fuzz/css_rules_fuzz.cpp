// The fuzz target for the table of a book's rules and the cascade
// (epub/css_rules.h).
//
// What it is given:
//
//   byte 0   bits 0 to 1: how much memory the table has
//            bits 2 to 3: how much the cascade has, for the open elements
//            bits 4 to 5: how the document's sheets are ordered, and
//                         whether one is left out of what it is given
//   the rest up to four sheets and then a document, a byte of zero between
//            one and the next: whatever comes last is the document
//
// A document is not XML, which has a target of its own, but a line for
// each thing a parser would tell of: `<name|attribute|value|...` for an
// element that opens, with as many attributes as it has, and `>` for one
// that closes. Any other line is passed over.
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
//     never told of the last sheet.

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
#include "epub/xml.h"
#include "fuzz_support.h"

namespace {

using bn::epub::CssCascade;
using bn::epub::CssKeyword;
using bn::epub::CssReader;
using bn::epub::CssRules;
using bn::epub::CssStyle;
using bn::epub::CssUnit;
using bn::epub::CssValue;

constexpr std::array<std::size_t, 4> kTableBytes = {65535, 4096, 300, 40};
constexpr std::array<std::size_t, 4> kCascadeBytes = {4096, 96, 6, 0};
constexpr std::size_t kMostSheets = 4;
// A document of more than this is cut off, and an element is given no
// more attributes: what is being tried is the table, not how long a test
// can be made to take.
constexpr std::size_t kMostElements = 200;
constexpr std::size_t kMostAttributes = 24;

// One element of a document: its name and attributes, or that it closes.
struct Step {
  bool opens = false;
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
      BN_FUZZ_CHECK(value.keyword <= CssKeyword::kLarger, "a keyword there is not");
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

// Goes through the document with a cascade over `rules`; and with
// `reference` beside it, if that is given, which has to say the same.
Said Go(const CssRules& rules, std::span<const std::uint8_t> sheets, const std::vector<Step>& steps,
        std::size_t cascadeBytes, bn::test::CssReference* reference) {
  std::vector<std::uint8_t> memory(cascadeBytes);
  CssCascade cascade(rules, memory);
  cascade.Begin(sheets);
  if (reference != nullptr) {
    reference->Begin({sheets.begin(), sheets.end()}, cascade.Levels());
  }
  Said said;
  for (const Step& step : steps) {
    if (!step.opens) {
      cascade.Close();
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

  const Said all = Go(rules, given, steps, cascadeBytes, whole ? &reference : nullptr);

  // Back to before the last sheet: the table is as it was, and says of
  // the document what a table that never had that sheet says.
  if (!sheets.empty()) {
    rules.BackTo(place);
    CheckTable(rules, tableBytes);
    BN_FUZZ_CHECK(rules.Bytes() == bytesBefore && rules.Sheets() == sheets.size() - 1,
                  "a table taken back is not the size it was");
    const Said back = Go(rules, given, steps, cascadeBytes, nullptr);

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
    const Said never = Go(other, given, steps, cascadeBytes, nullptr);
    BN_FUZZ_CHECK(back.digest == never.digest,
                  "a table taken back says something else of a document than one never told of the last sheet");

    // And it takes the sheet again, to the same end as the first time.
    std::uint8_t number = 0xFF;
    BN_FUZZ_CHECK(rules.BeginSheet(number) && number == sheets.size() - 1, "a table taken back does not take a sheet");
    Read(sheets.back(), rules);
    rules.EndSheet();
    const Said again = Go(rules, given, steps, cascadeBytes, nullptr);
    BN_FUZZ_CHECK(again.digest == all.digest,
                  "a sheet read again after the table was taken back came to something else");
  }
  return 0;
}
