// The fuzz target for the reader of an SVG file's own style sheet
// (draw/svg_sheet.h).
//
// What it is given:
//
//   byte 0    bits 0 to 2: how big a piece of the sheet the reader is given
//             at a time, for the second reading
//   the rest  the text of a sheet, or what passes for one
//
// What has to hold, whatever the bytes are:
//
//   - a call takes no more than it was given, and takes a byte or tells
//     something; so there are no more calls than twice the bytes and two,
//     and nothing is told that the bytes did not bring;
//   - a rule is told as its selectors, then what it sets, then its end:
//     never something it sets, or an end, without a selector before it;
//   - a selector is a class or an element's name, made of the letters,
//     digits, `_`, `-` and bytes beyond ASCII that a name may have, and
//     not beginning with a digit;
//   - a property's name is in small letters, has no space in it, and is
//     no longer than kLongestProperty; a value has no space at either end
//     and is no longer than kLongestValue;
//   - the sheet cut up is read as it is whole: the same things, in the
//     same order, and the same said of what was dropped.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "draw/svg_sheet.h"
#include "fuzz_support.h"

namespace {

using bn::draw::SvgSelector;
using bn::draw::SvgSheetReader;
using bn::draw::SvgSheetToken;
using Kind = bn::draw::SvgSheetToken::Kind;

constexpr std::size_t kPieces[] = {1, 2, 3, 5, 8, 17, 64, SIZE_MAX};

bool IsCssSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool InName(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         static_cast<unsigned char>(c) >= 0x80U;
}

struct Reading {
  std::string told;
  std::uint8_t dropped = 0;
};

Reading Read(std::string_view sheet, std::size_t piece) {
  Reading reading;
  SvgSheetReader reader;
  std::size_t calls = 0;
  bool inRule = false;
  bool setting = false;
  for (std::size_t at = 0; at < sheet.size();) {
    std::string_view bytes = sheet.substr(at, piece);
    at += bytes.size();
    for (;;) {
      SvgSheetToken token;
      const std::size_t taken = reader.Next(bytes, token);
      ++calls;
      BN_FUZZ_CHECK(taken <= bytes.size(), "a call took more than it was given");
      BN_FUZZ_CHECK(taken > 0 || token.kind != Kind::kNone || bytes.empty(), "a call took nothing and told nothing");
      bytes.remove_prefix(taken);
      switch (token.kind) {
        case Kind::kNone:
          BN_FUZZ_CHECK(bytes.empty(), "the reader stopped with bytes left");
          break;
        case Kind::kSelector: {
          BN_FUZZ_CHECK(!setting, "a selector came after what its rule sets");
          const std::string_view name = token.selector.name;
          BN_FUZZ_CHECK(!name.empty() && name.size() <= SvgSheetReader::kLongestSelectors,
                        "a selector's name is empty or too long");
          BN_FUZZ_CHECK(name.front() < '0' || name.front() > '9', "a selector's name begins with a digit");
          for (const char c : name) {
            BN_FUZZ_CHECK(InName(c), "a selector's name has in it what a name cannot");
          }
          reading.told += token.selector.kind == SvgSelector::Kind::kClass ? "." : "<";
          reading.told.append(name).append(" ");
          inRule = true;
          break;
        }
        case Kind::kDeclaration: {
          BN_FUZZ_CHECK(inRule, "a rule set something before any selector");
          setting = true;
          const std::string_view property = token.property;
          BN_FUZZ_CHECK(!property.empty() && property.size() <= SvgSheetReader::kLongestProperty,
                        "a property's name is empty or too long");
          for (const char c : property) {
            BN_FUZZ_CHECK((c < 'A' || c > 'Z') && !IsCssSpace(c), "a property's name has a capital or a space in it");
          }
          const std::string_view value = token.value;
          BN_FUZZ_CHECK(value.size() <= SvgSheetReader::kLongestValue, "a value is too long");
          BN_FUZZ_CHECK(value.empty() || (!IsCssSpace(value.front()) && !IsCssSpace(value.back())),
                        "a value has space at an end");
          reading.told.append("{").append(property).append("=").append(value).append("}");
          break;
        }
        case Kind::kRuleEnd:
          BN_FUZZ_CHECK(inRule, "a rule ended that had no selector");
          inRule = false;
          setting = false;
          reading.told += ';';
          break;
      }
      if (token.kind == Kind::kNone) {
        break;
      }
    }
  }
  BN_FUZZ_CHECK(calls <= (2 * sheet.size()) + 2, "there were more calls than twice the bytes");
  reader.Finish();
  reading.dropped = reader.Dropped();
  return reading;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  const std::string_view sheet(reinterpret_cast<const char*>(rest.data()), rest.size());

  const Reading whole = Read(sheet, SIZE_MAX);
  const Reading cut = Read(sheet, kPieces[choices & 7U]);
  BN_FUZZ_CHECK(cut.told == whole.told, "a sheet cut up was read differently from the same sheet whole");
  BN_FUZZ_CHECK(cut.dropped == whole.dropped, "a sheet cut up was said to drop other things than the same sheet whole");
  return 0;
}
