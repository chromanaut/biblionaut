#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hyphen/pack.h"
#include "hyphen_patterns.h"
#include "layout/hyphenator.h"

namespace {

using bn::hyphen_tool::Pack;
using bn::hyphen_tool::Packed;
using bn::hyphen_tool::PackOptions;
using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;
using bn::test::BrokenByTable;
using bn::test::PlacesByHand;
using bn::test::PlacesByTable;
using bn::test::PlainHyphenTable;

// Patterns as one text, a line each.
std::string Lines(const std::vector<std::string>& patterns) {
  std::string text;
  for (const std::string& pattern : patterns) {
    text += pattern + "\n";
  }
  return text;
}

Packed PackOf(std::string_view patterns, std::string_view exceptions = {}, std::string_view letters = {},
              std::uint8_t leftMin = 2, std::uint8_t rightMin = 3) {
  return Pack(patterns, exceptions, letters, PackOptions{.leftMin = leftMin, .rightMin = rightMin});
}

// Numbers that come out the same on every machine, which the standard
// library's distributions do not promise.
class Dice {
 public:
  explicit Dice(std::uint32_t seed) : state_(seed) {}

  // A number from 0 up to, and not including, `below`.
  std::uint32_t Below(std::uint32_t below) {
    state_ = (state_ * 1664525U) + 1013904223U;
    return (state_ >> 8U) % below;
  }
  std::uint32_t Between(std::uint32_t low, std::uint32_t high) { return low + Below(high - low + 1); }

 private:
  std::uint32_t state_;
};

std::string MadeUpWord(Dice& dice, std::size_t length, std::uint32_t letters) {
  std::string word;
  for (std::size_t i = 0; i < length; ++i) {
    word += static_cast<char>('a' + dice.Below(letters));
  }
  return word;
}

// A pattern of up to six letters, with a number at some places and now and
// then a word's end at either end of it.
std::string MadeUpPattern(Dice& dice, std::uint32_t letters) {
  std::string pattern;
  if (dice.Below(6) == 0) {
    pattern += '.';
  }
  const std::size_t length = dice.Between(1, 6);
  for (std::size_t i = 0; i <= length; ++i) {
    if (dice.Below(3) == 0) {
      pattern += static_cast<char>('0' + dice.Below(10));
    }
    if (i < length) {
      pattern += static_cast<char>('a' + dice.Below(letters));
    }
  }
  if (dice.Below(6) == 0) {
    pattern += '.';
  }
  return pattern;
}

TEST(HyphenPack, MakesATableThatBreaksWordsAsThePatternsSay) {
  const Packed packed = PackOf("hy3ph he2n hena4 hen5at\n1na n2at 1tio 2io o2n\n");
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.patterns, 9U);
  EXPECT_EQ(packed.exceptions, 0U);
  EXPECT_EQ(packed.letters, 9U);  // a e h i n o p t y
  EXPECT_EQ(packed.listed, 18U);  // and their capitals.
  EXPECT_EQ(packed.deepest, 5U);
  EXPECT_EQ(BrokenByTable(packed.table, "hyphenation"), "hy-phen-ation");
  EXPECT_EQ(BrokenByTable(packed.table, "Hyphenation"), "Hy-phen-ation");
}

TEST(HyphenPack, PutsTheFewestLettersOnEachSideIntoTheTable) {
  const Packed packed = PackOf("1a 1b 1c 1d 1e", {}, {}, 1, 4);
  ASSERT_TRUE(packed.ok) << packed.error;
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(packed.table), HyphenStatus::kOk);
  EXPECT_EQ(hyphenator.LeftMin(), 1);
  EXPECT_EQ(hyphenator.RightMin(), 4);
  EXPECT_EQ(BrokenByTable(packed.table, "abcdeabc"), "a-b-c-d-eabc");
}

TEST(HyphenPack, AgreesWithLayingEveryPatternOverEveryPlace) {
  Dice dice(29);
  std::size_t placesInAll = 0;
  std::size_t edges = 0;
  std::size_t edgesUnshared = 0;
  for (int round = 0; round < 300; ++round) {
    const std::uint32_t letters = dice.Between(2, 5);
    std::vector<std::string> patterns;
    const std::size_t count = dice.Between(0, 80);
    patterns.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      patterns.push_back(MadeUpPattern(dice, letters));
    }
    const auto leftMin = static_cast<std::uint8_t>(dice.Between(1, 3));
    const auto rightMin = static_cast<std::uint8_t>(dice.Between(1, 3));
    const Packed packed = PackOf(Lines(patterns), {}, {}, leftMin, rightMin);
    ASSERT_TRUE(packed.ok) << "round " << round << ": " << packed.error;
    Hyphenator hyphenator;
    ASSERT_EQ(hyphenator.Open(packed.table), HyphenStatus::kOk) << "round " << round;
    // And with the same patterns in a table written the plain way.
    const std::vector<std::uint8_t> plain = PlainHyphenTable(patterns, leftMin, rightMin);
    Hyphenator plainly;
    ASSERT_EQ(plainly.Open(plain), HyphenStatus::kOk) << "round " << round;
    EXPECT_LE(packed.table.size(), plain.size()) << "round " << round;

    for (int n = 0; n < 60; ++n) {
      const std::string word = MadeUpWord(dice, dice.Between(1, 40), letters);
      const std::vector<std::size_t> places = PlacesByTable(hyphenator, word);
      ASSERT_EQ(places, PlacesByHand(patterns, word, leftMin, rightMin)) << "round " << round << ", " << word;
      ASSERT_EQ(places, PlacesByTable(plainly, word)) << "round " << round << ", " << word;
      ASSERT_LE(hyphenator.LastWork(), hyphenator.MostWorkFor(word.size())) << "round " << round << ", " << word;
      placesInAll += places.size();
    }
    edges += packed.edges;
    edgesUnshared += packed.edgesUnshared;
  }
  EXPECT_GT(placesInAll, 10000U);
  // Keeping like branches once is worth something even on patterns made up
  // with no likeness meant.
  EXPECT_LT(edges, edgesUnshared);
}

TEST(HyphenPack, KeepsBranchesThatAreAlikeOnce) {
  // Twenty beginnings, each going on in the same four ways.
  std::vector<std::string> patterns;
  for (char first = 'a'; first < 'a' + 20; ++first) {
    for (const std::string_view rest : {"xy1z", "x2yy", "zz3z", "z1x"}) {
      patterns.push_back(std::string(1, first) + std::string(rest));
    }
  }
  const Packed packed = PackOf(Lines(patterns), {}, {}, 1, 1);
  ASSERT_TRUE(packed.ok) << packed.error;
  // The plain tree has, under each of the 20 beginnings, the same eight
  // branches: x, xy, xyz, xyy, z, zz, zzz and zx. Kept once, those are
  // eight edges in all.
  EXPECT_EQ(packed.edgesUnshared, 20U * 9U);
  EXPECT_EQ(packed.edges, 20U + 8U);
  EXPECT_EQ(packed.widest, 20U);
  EXPECT_EQ(BrokenByTable(packed.table, "qaxyzq"), "qaxy-zq");
  EXPECT_EQ(BrokenByTable(packed.table, "qtzxq"), "qtz-xq");
  EXPECT_EQ(BrokenByTable(packed.table, "qtxyyq"), "qtxyyq");
}

TEST(HyphenPack, LetsNoPatternChangeAWordWrittenOutWithItsBreaks) {
  const std::string patterns = "1a 1b 1c 1d 1e";
  const Packed plain = PackOf(patterns, {}, {}, 1, 1);
  ASSERT_TRUE(plain.ok) << plain.error;
  EXPECT_EQ(BrokenByTable(plain.table, "abcde"), "a-b-c-d-e");
  EXPECT_EQ(BrokenByTable(plain.table, "edcba"), "e-d-c-b-a");

  const Packed packed = PackOf(patterns, "ab-cde\nedcba % not to be broken at all\n", {}, 1, 1);
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.patterns, 5U);
  EXPECT_EQ(packed.exceptions, 2U);
  EXPECT_EQ(BrokenByTable(packed.table, "abcde"), "ab-cde");
  EXPECT_EQ(BrokenByTable(packed.table, "Abcde"), "Ab-cde");
  EXPECT_EQ(BrokenByTable(packed.table, "edcba"), "edcba");
  // Only those words: not ones they are part of, or that are part of them.
  EXPECT_EQ(BrokenByTable(packed.table, "abcdea"), "a-b-c-d-e-a");
  EXPECT_EQ(BrokenByTable(packed.table, "bcde"), "b-c-d-e");
  // Between two characters that are not letters it is the word itself.
  EXPECT_EQ(BrokenByTable(packed.table, "3abcde3"), "3ab-cde3");

  // The language's fewest letters on each side still hold.
  const Packed wider = PackOf(patterns, "a-b-c-d-e", {}, 2, 2);
  ASSERT_TRUE(wider.ok) << wider.error;
  EXPECT_EQ(BrokenByTable(wider.table, "abcde"), "ab-c-de");
}

TEST(HyphenPack, TakesALettersOtherFormsFromTheListOfLetters) {
  // The patterns write an e with an acute accent; its capital and the plain
  // E are to count as it. With a list given, a to z get no capitals of
  // their own accord.
  const std::string patterns = "1\xC3\xA9 1b";
  const Packed packed = PackOf(patterns, {},
                               "\xC3\xA9\xC3\x89"
                               "E\nbB\nq\n",
                               1, 1);
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.letters, 3U);  // The two the patterns use, and q from the list.
  EXPECT_EQ(packed.listed, 6U);
  EXPECT_EQ(BrokenByTable(packed.table,
                          "b\xC3\xA9"
                          "b"),
            "b-\xC3\xA9-b");
  EXPECT_EQ(BrokenByTable(packed.table,
                          "B\xC3\x89"
                          "B"),
            "B-\xC3\x89-B");
  EXPECT_EQ(BrokenByTable(packed.table, "bEb"), "b-E-b");
  // q is a letter of the language with no pattern; x is not a letter of it,
  // and divides the word.
  EXPECT_EQ(BrokenByTable(packed.table, "bqb"), "bq-b");
  EXPECT_EQ(BrokenByTable(packed.table, "bbxbb"), "b-bxb-b");

  // Without a list, a to z have their capitals and nothing else has.
  const Packed unlisted = PackOf(patterns, {}, {}, 1, 1);
  ASSERT_TRUE(unlisted.ok) << unlisted.error;
  EXPECT_EQ(unlisted.letters, 2U);
  EXPECT_EQ(unlisted.listed, 3U);
  EXPECT_EQ(BrokenByTable(unlisted.table,
                          "B\xC3\xA9"
                          "B"),
            "B-\xC3\xA9-B");
  EXPECT_EQ(BrokenByTable(unlisted.table,
                          "b\xC3\x89"
                          "b"),
            "b\xC3\x89"
            "b");
}

TEST(HyphenPack, PassesOverWhatFollowsAPercentSign) {
  const Packed packed = PackOf("% Patterns for nothing in particular.\n1a % before an a\n  1b\t1c\r\n% 1d\n");
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.patterns, 3U);
  EXPECT_EQ(packed.letters, 3U);
}

TEST(HyphenPack, MakesATableOfNoPatterns) {
  const Packed packed = PackOf("% Nothing.\n");
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.patterns, 0U);
  EXPECT_EQ(packed.edges, 0U);
  EXPECT_EQ(packed.table.size(), 16U);
  EXPECT_EQ(BrokenByTable(packed.table, "hyphenation"), "hyphenation");
}

TEST(HyphenPack, TakesTheSamePatternTwiceAsTheLargerOfItsNumbers) {
  const Packed packed = PackOf("a2b a1b c1d c3d c2d", {}, {}, 1, 1);
  ASSERT_TRUE(packed.ok) << packed.error;
  EXPECT_EQ(packed.patterns, 5U);
  EXPECT_EQ(BrokenByTable(packed.table, "abcd"), "abc-d");
}

TEST(HyphenPack, SaysWhatIsWrongWithPatternsItCannotPack) {
  const auto errorOf = [](std::string_view patterns, std::string_view exceptions = {}, std::string_view letters = {}) {
    const Packed packed = PackOf(patterns, exceptions, letters);
    EXPECT_FALSE(packed.ok);
    EXPECT_TRUE(packed.table.empty());
    return packed.error;
  };
  EXPECT_EQ(errorOf("1a\na12b\n"), "patterns, line 2: two numbers at one place in a pattern");
  EXPECT_EQ(errorOf("1a\n\n\na.b\n"), "patterns, line 4: a `.` that is not at an end of its pattern");
  EXPECT_EQ(errorOf("1a 2 1b"), "patterns, line 1: a pattern with no letters");
  EXPECT_EQ(errorOf(".\n"), "patterns, line 1: a pattern with no letters");
  EXPECT_EQ(errorOf("1a\n1\xFF\n"), "patterns, line 2: not UTF-8");
  EXPECT_EQ(errorOf("1a", "ab-\n"), "exceptions, line 1: a `-` with no letter after it");
  EXPECT_EQ(errorOf("1a", "ta-ble\n-ab\n"), "exceptions, line 2: a `-` with no letter before it");
  EXPECT_EQ(errorOf("1a", "a1b"), "exceptions, line 1: an exception is a word, with `-` where it may be broken");
  EXPECT_EQ(errorOf("1a 1b", {}, "aA\nbA\n"), "letters, line 2: a character that stands for two letters");

  // More letters than a table can name: 255 of them.
  std::string many;
  for (std::uint32_t letter = 0; letter < 255; ++letter) {
    const std::uint32_t codePoint = 0x400 + letter;
    many += '1';
    many += static_cast<char>(0xC0U | (codePoint >> 6U));
    many += static_cast<char>(0x80U | (codePoint & 0x3FU));
    many += '\n';
  }
  EXPECT_EQ(errorOf(many), "too many letters: 255, and a table holds 254");
  // A pattern longer than a word that is looked at.
  EXPECT_EQ(errorOf(std::string(66, 'a')), "a pattern of 66 letters, and the longest a table takes is 65");

  const Packed noLeft = Pack("1a", {}, {}, PackOptions{.leftMin = 0, .rightMin = 1});
  EXPECT_FALSE(noLeft.ok);
  EXPECT_EQ(noLeft.error, "a break needs a letter before it and a letter after it at the least");
}

}  // namespace
