#include "core/utf8.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using bn::core::Character;
using bn::core::CharacterAt;

// A code point written as UTF-8 the plain way, whatever it is: surrogates
// and all.
std::string Written(std::uint32_t codePoint) {
  std::string out;
  if (codePoint < 0x80U) {
    out += static_cast<char>(codePoint);
  } else if (codePoint < 0x800) {
    out += static_cast<char>(0xC0U | (codePoint >> 6U));
    out += static_cast<char>(0x80U | (codePoint & 0x3FU));
  } else if (codePoint < 0x10000) {
    out += static_cast<char>(0xE0U | (codePoint >> 12U));
    out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (codePoint & 0x3FU));
  } else {
    out += static_cast<char>(0xF0U | (codePoint >> 18U));
    out += static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (codePoint & 0x3FU));
  }
  return out;
}

TEST(Utf8, ReadsEveryCodePointThereIs) {
  for (std::uint32_t codePoint = 0; codePoint <= bn::core::kLargestCodePoint; ++codePoint) {
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
      continue;
    }
    const std::string text = Written(codePoint) + "z";
    const Character character = CharacterAt(text, 0);
    ASSERT_TRUE(character.good) << codePoint;
    ASSERT_EQ(character.codePoint, codePoint);
    ASSERT_EQ(character.bytes, text.size() - 1) << codePoint;
  }
}

TEST(Utf8, ReadsTheCharacterThatBeginsWhereItIsAsked) {
  const std::string_view text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";  // a, é, the euro sign, a face, z.
  std::size_t at = 0;
  for (const std::uint32_t wanted : {0x61U, 0xE9U, 0x20ACU, 0x1F600U, 0x7AU}) {
    const Character character = CharacterAt(text, at);
    ASSERT_TRUE(character.good);
    EXPECT_EQ(character.codePoint, wanted);
    at += character.bytes;
  }
  EXPECT_EQ(at, text.size());
}

TEST(Utf8, TakesAByteThatBeginsNoCharacterAsOneByteThatIsNone) {
  using namespace std::string_view_literals;
  for (const std::string_view bad : {
           "\x80z"sv,              // Goes on with a character, and begins none.
           "\xBFz"sv,              //
           "\xC0\xAFz"sv,          // A slash the long way round,
           "\xC1\xBFz"sv,          //
           "\xE0\x80\xAFz"sv,      // in three bytes,
           "\xE0\x9F\xBFz"sv,      // and the last such.
           "\xF0\x80\x80\xAFz"sv,  // In four.
           "\xF0\x8F\xBF\xBFz"sv,  //
           "\xED\xA0\x80z"sv,      // Half of a pair: the first there is,
           "\xED\xBF\xBFz"sv,      // and the last.
           "\xF4\x90\x80\x80z"sv,  // Past the last code point,
           "\xF5\x80\x80\x80z"sv,  // by a long way,
           "\xF8\x88\x80\x80\x80z"sv,
           "\xFEz"sv,
           "\xFFz"sv,
           "\xC3z"sv,          // Cut short by another character,
           "\xE2\x82z"sv,      //
           "\xF0\x9F\x98z"sv,  //
           "\xC3"sv,           // and by the end of the text.
           "\xE2\x82"sv,
           "\xF0\x9F\x98"sv,
       }) {
    const Character character = CharacterAt(bad, 0);
    EXPECT_FALSE(character.good) << static_cast<int>(static_cast<unsigned char>(bad[0]));
    EXPECT_EQ(character.bytes, 1U);
  }
}

TEST(Utf8, IsWorkedOutWhenTheProgramIsCompiledToo) {
  static constexpr std::string_view kText = "\xC3\xA9";
  static constexpr Character kRead = CharacterAt(kText, 0);
  static_assert(kRead.good && kRead.codePoint == 0xE9 && kRead.bytes == 2);
  SUCCEED();
}

}  // namespace
