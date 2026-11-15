#include "layout/hyphenator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hyphen_patterns.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;
using bn::layout::HyphenStatusName;
using bn::test::BrokenByTable;
using bn::test::OtherForm;
using bn::test::PlacesByHand;
using bn::test::PlacesByTable;
using bn::test::PlainHyphenTable;
using bn::test::ShownBroken;

using Table = std::vector<std::uint8_t>;
using Patterns = std::vector<std::string>;

// The patterns that Liang's own example word meets, from the ones TeX has
// for English.
const Patterns kHyphenation = {"hy3ph", "he2n", "hena4", "hen5at", "1na", "n2at", "1tio", "2io", "o2n"};

// A break before every letter there is a pattern for.
const Patterns kBeforeEach = {"1a", "1b", "1c", "1d", "1e"};

// Where the pieces of a table are: see the header.
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kListedCount = 8;
constexpr std::size_t kEdgeCount = 10;
constexpr std::size_t kListedBytes = 4;

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

// A word of `length` letters from the first `letters` of the alphabet.
std::string MadeUpWord(Dice& dice, std::size_t length, std::uint32_t letters) {
  std::string word;
  for (std::size_t i = 0; i < length; ++i) {
    word += static_cast<char>('a' + dice.Below(letters));
  }
  return word;
}

// A pattern of up to six of those letters, with a number at some places and
// now and then a word's end at either end of it.
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

TEST(Hyphenator, BreaksAWordWhereItsPatternsSay) {
  const Table table = PlainHyphenTable(kHyphenation, 2, 3);
  EXPECT_EQ(BrokenByTable(table, "hyphenation"), "hy-phen-ation");
  // The same patterns say nothing of these.
  EXPECT_EQ(BrokenByTable(table, "hyphen"), "hy-phen");
  EXPECT_EQ(BrokenByTable(table, "queue"), "queue");
  EXPECT_EQ(BrokenByTable(table, ""), "");
}

TEST(Hyphenator, GivesEachPlaceAsTheBytesBeforeIt) {
  const Table table = PlainHyphenTable(kHyphenation, 2, 3);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);
  EXPECT_EQ(PlacesByTable(hyphenator, "hyphenation"), (std::vector<std::size_t>{2, 6}));
  EXPECT_EQ(hyphenator.LeftMin(), 2);
  EXPECT_EQ(hyphenator.RightMin(), 3);
}

TEST(Hyphenator, LetsTheLargestNumberAtAPlaceDecide) {
  // One pattern allows a break between a and b, and a longer one, which has
  // a c after them, forbids it again.
  const Table table = PlainHyphenTable({"a1b", "a2bc", "x3y", "x4yz", "x5yzz"}, 1, 1);
  EXPECT_EQ(BrokenByTable(table, "ddabdd"), "dda-bdd");
  EXPECT_EQ(BrokenByTable(table, "ddabcd"), "ddabcd");
  // And a longer one still allows it.
  EXPECT_EQ(BrokenByTable(table, "dxyd"), "dx-yd");
  EXPECT_EQ(BrokenByTable(table, "dxyzd"), "dxyzd");
  EXPECT_EQ(BrokenByTable(table, "dxyzzd"), "dx-yzzd");
}

TEST(Hyphenator, KnowsTheEndsOfAWord) {
  const Table table = PlainHyphenTable({".ab1", "1cd."}, 1, 1);
  EXPECT_EQ(BrokenByTable(table, "abeecd"), "ab-ee-cd");
  // The same letters in the middle of a word are not the same patterns.
  EXPECT_EQ(BrokenByTable(table, "eabecde"), "eabecde");
  // A pattern that is a whole word.
  const Table whole = PlainHyphenTable({".a1b1c."}, 1, 1);
  EXPECT_EQ(BrokenByTable(whole, "abc"), "a-b-c");
  EXPECT_EQ(BrokenByTable(whole, "abcd"), "abcd");
  EXPECT_EQ(BrokenByTable(whole, "dabc"), "dabc");
}

TEST(Hyphenator, LeavesTheFewestLettersTheLanguageWantsOnEachSide) {
  EXPECT_EQ(BrokenByTable(PlainHyphenTable(kBeforeEach, 1, 1), "abcde"), "a-b-c-d-e");
  EXPECT_EQ(BrokenByTable(PlainHyphenTable(kBeforeEach, 2, 3), "abcde"), "ab-cde");
  EXPECT_EQ(BrokenByTable(PlainHyphenTable(kBeforeEach, 3, 1), "abcde"), "abc-d-e");
  // A word with too few letters for both sides has no place at all.
  EXPECT_EQ(BrokenByTable(PlainHyphenTable(kBeforeEach, 2, 3), "abcd"), "abcd");
  EXPECT_EQ(BrokenByTable(PlainHyphenTable(kBeforeEach, 40, 40), "abcdeabcdeabcde"), "abcdeabcdeabcde");
}

TEST(Hyphenator, TakesCapitalsAsTheirSmallLetters) {
  const Table table = PlainHyphenTable(kHyphenation, 2, 3);
  EXPECT_EQ(BrokenByTable(table, "Hyphenation"), "Hy-phen-ation");
  EXPECT_EQ(BrokenByTable(table, "HYPHENATION"), "HY-PHEN-ATION");
}

TEST(Hyphenator, DividesAWordAtACharacterThatIsNoLetter) {
  const Table table = PlainHyphenTable(kBeforeEach, 1, 1);
  // Each side is a word of its own, and there is no break beside the
  // character between them.
  EXPECT_EQ(BrokenByTable(table, "abc3cde"), "a-b-c3c-d-e");
  EXPECT_EQ(BrokenByTable(table, "ab'cd"), "a-b'c-d");
  EXPECT_EQ(BrokenByTable(table, "ab--cd"), "a-b--c-d");
  EXPECT_EQ(BrokenByTable(table, "3ab"), "3a-b");
  EXPECT_EQ(BrokenByTable(table, "ab3"), "a-b3");
  EXPECT_EQ(BrokenByTable(table, "333"), "333");
  // A letter there is no pattern for is still a letter of the word.
  EXPECT_EQ(BrokenByTable(table, "abzcd"), "a-bz-c-d");

  // Each side has to have letters enough by itself.
  const Table wider = PlainHyphenTable(kBeforeEach, 2, 2);
  EXPECT_EQ(BrokenByTable(wider, "abc3abcd"), "abc3ab-cd");
}

TEST(Hyphenator, ReadsLettersOfMoreThanOneByte) {
  // e with an acute accent, the euro sign and a face stand for letters
  // here: two bytes, three and four.
  const std::vector<OtherForm> others = {{0xE9, 'e'}, {0x20AC, 'c'}, {0x1F600, 'd'}};
  const Table table = PlainHyphenTable(kBeforeEach, 1, 1, others);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);

  const std::string word = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
  EXPECT_EQ(PlacesByTable(hyphenator, word), (std::vector<std::size_t>{1, 3, 6}));
  EXPECT_EQ(ShownBroken(word, PlacesByTable(hyphenator, word)), "a-\xC3\xA9-\xE2\x82\xAC-\xF0\x9F\x98\x80z");
  // A character of several bytes that is no letter divides the word like
  // any other.
  EXPECT_EQ(BrokenByTable(table,
                          "ab\xC3\xA8"
                          "cd"),
            "a-b\xC3\xA8"
            "c-d");
}

TEST(Hyphenator, TakesBytesThatAreNotUtf8AsNoLetter) {
  const std::vector<OtherForm> others = {{0xE9, 'e'}};
  const Table table = PlainHyphenTable(kBeforeEach, 1, 1, others);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);
  const auto places = [&](std::string_view word) { return PlacesByTable(hyphenator, word); };

  // A byte that begins nothing, and one that only ever follows.
  EXPECT_EQ(places("ab\xFF"
                   "cd"),
            (std::vector<std::size_t>{1, 4}));
  EXPECT_EQ(places("ab\x80"
                   "cd"),
            (std::vector<std::size_t>{1, 4}));
  // A character cut short by the end of the word, and by a letter.
  EXPECT_EQ(places("ab\xC3"), (std::vector<std::size_t>{1}));
  EXPECT_EQ(places("ab\xC3"
                   "cd"),
            (std::vector<std::size_t>{1, 4}));
  // The letter e written in two bytes where one would do, and the e with an
  // accent in three where two would: neither is that letter.
  EXPECT_EQ(places("ab\xC1\xA5"
                   "cd"),
            (std::vector<std::size_t>{1, 5}));
  EXPECT_EQ(places("ab\xE0\x83\xA9"
                   "cd"),
            (std::vector<std::size_t>{1, 6}));
  // Half of a pair, and a code point past the last.
  EXPECT_EQ(places("ab\xED\xA0\x80"
                   "cd"),
            (std::vector<std::size_t>{1, 6}));
  EXPECT_EQ(places("ab\xF4\x90\x80\x80"
                   "cd"),
            (std::vector<std::size_t>{1, 7}));
}

TEST(Hyphenator, GivesNoPlacesInAWordOfMoreThanSixtyThreeCharacters) {
  const Table table = PlainHyphenTable(kBeforeEach, 1, 1);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);
  const std::string longest(Hyphenator::kMaxLetters, 'a');
  EXPECT_EQ(PlacesByTable(hyphenator, longest).size(), Hyphenator::kMaxLetters - 1);
  EXPECT_TRUE(PlacesByTable(hyphenator, longest + "a").empty());
  EXPECT_EQ(hyphenator.LastWork(), 0U);
  // Characters are counted, not bytes: sixty-three of two bytes each.
  std::string accented;
  for (std::size_t i = 0; i < Hyphenator::kMaxLetters; ++i) {
    accented += "\xC3\xA9";
  }
  const Table withAccent = PlainHyphenTable(kBeforeEach, 1, 1, {{0xE9, 'e'}});
  ASSERT_EQ(hyphenator.Open(withAccent), HyphenStatus::kOk);
  const std::vector<std::size_t> places = PlacesByTable(hyphenator, accented);
  ASSERT_EQ(places.size(), Hyphenator::kMaxLetters - 1);
  EXPECT_EQ(places.front(), 2U);
  EXPECT_EQ(places.back(), 124U);
}

TEST(Hyphenator, GivesTheFirstPlacesWhenThereIsRoomForFewer) {
  const Table table = PlainHyphenTable(kBeforeEach, 1, 1);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);
  std::array<std::uint8_t, 2> two{};
  EXPECT_EQ(hyphenator.Find("abcde", two), 2U);
  EXPECT_EQ(two[0], 1);
  EXPECT_EQ(two[1], 2);
  EXPECT_EQ(hyphenator.Find("abcde", std::span<std::uint8_t>()), 0U);
}

TEST(Hyphenator, GivesNoPlacesWithoutATable) {
  Hyphenator hyphenator;
  EXPECT_EQ(hyphenator.Status(), HyphenStatus::kNotOpen);
  EXPECT_TRUE(PlacesByTable(hyphenator, "hyphenation").empty());
  EXPECT_EQ(hyphenator.MostWorkFor(10), 0U);

  // A table with no patterns in it is a table, and breaks nothing.
  const Table empty = PlainHyphenTable({}, 2, 3);
  ASSERT_EQ(hyphenator.Open(empty), HyphenStatus::kOk);
  EXPECT_TRUE(PlacesByTable(hyphenator, "hyphenation").empty());
}

TEST(Hyphenator, RefusesBytesThatAreNotATable) {
  const Table good = PlainHyphenTable(kHyphenation, 2, 3);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(good), HyphenStatus::kOk);

  // Cut short anywhere, or with a byte too many.
  for (std::size_t length = 0; length < good.size(); ++length) {
    ASSERT_EQ(hyphenator.Open(std::span(good).first(length)), HyphenStatus::kBadTable) << length;
    ASSERT_TRUE(PlacesByTable(hyphenator, "hyphenation").empty());
  }
  Table longer = good;
  longer.push_back(0);
  EXPECT_EQ(hyphenator.Open(longer), HyphenStatus::kBadTable);

  const auto with = [&](std::size_t at, std::uint8_t byte) {
    Table table = good;
    table[at] = byte;
    return hyphenator.Open(table);
  };
  EXPECT_EQ(with(0, 'X'), HyphenStatus::kBadTable);            // Not its four letters,
  EXPECT_EQ(with(4, 2), HyphenStatus::kBadTable);              // nor a version this code knows.
  EXPECT_EQ(with(15, 1), HyphenStatus::kBadTable);             // The byte that is nothing.
  EXPECT_EQ(with(5, 0), HyphenStatus::kBadTable);              // No letter before a break,
  EXPECT_EQ(with(6, 0), HyphenStatus::kBadTable);              // or none after.
  EXPECT_EQ(with(7, 0), HyphenStatus::kBadTable);              // Patterns of no letters,
  EXPECT_EQ(with(7, 66), HyphenStatus::kBadTable);             // or of more than a word can have.
  EXPECT_EQ(with(14, 0), HyphenStatus::kBadTable);             // States with no edges.
  EXPECT_EQ(with(kListedCount, 51), HyphenStatus::kBadTable);  // Counts that do not add up to its size.
  EXPECT_EQ(with(kEdgeCount, 1), HyphenStatus::kBadTable);
  EXPECT_EQ(with(12, 1), HyphenStatus::kBadTable);
  EXPECT_EQ(with(7, 65), HyphenStatus::kOk);  // The longest a pattern may be.

  // The letter list out of order, with a character twice, naming the end of
  // a word or no letter, or with a character past the last there is.
  const std::size_t second = kHeaderBytes + kListedBytes;
  EXPECT_EQ(with(second, 0x7F), HyphenStatus::kBadTable);
  EXPECT_EQ(with(second, good[kHeaderBytes]), HyphenStatus::kBadTable);
  EXPECT_EQ(with(second + 3, 0), HyphenStatus::kBadTable);
  EXPECT_EQ(with(second + 3, 0xFF), HyphenStatus::kBadTable);
  const std::size_t last = kHeaderBytes + (51 * kListedBytes);
  EXPECT_EQ(with(last + 2, 0x11), HyphenStatus::kBadTable);
  EXPECT_EQ(with(last + 2, 0x10), HyphenStatus::kOk);

  // A table that was refused leaves no table open.
  EXPECT_EQ(with(0, 'X'), HyphenStatus::kBadTable);
  EXPECT_EQ(hyphenator.Status(), HyphenStatus::kBadTable);
  EXPECT_TRUE(PlacesByTable(hyphenator, "hyphenation").empty());
}

TEST(Hyphenator, RefusesALetterListLongerThanItWillGoOver) {
  // A table that is all letter list: one entry more than the most, and then
  // the most.
  const auto listOf = [](std::size_t entries) {
    Table table = {'B',
                   'N',
                   'H',
                   'Y',
                   1,
                   2,
                   3,
                   1,
                   static_cast<std::uint8_t>(entries & 0xFFU),
                   static_cast<std::uint8_t>(entries >> 8U),
                   0,
                   0,
                   0,
                   0,
                   1,
                   0};
    for (std::size_t i = 0; i < entries; ++i) {
      table.push_back(static_cast<std::uint8_t>(i & 0xFFU));
      table.push_back(static_cast<std::uint8_t>(i >> 8U));
      table.push_back(0);
      table.push_back(1);
    }
    return table;
  };
  Hyphenator hyphenator;
  EXPECT_EQ(hyphenator.Open(listOf(Hyphenator::kMaxListed + 1)), HyphenStatus::kBadTable);
  EXPECT_EQ(hyphenator.Open(listOf(Hyphenator::kMaxListed)), HyphenStatus::kOk);
}

TEST(Hyphenator, HasANameForEachStatus) {
  EXPECT_EQ(std::string(HyphenStatusName(HyphenStatus::kOk)), "ok");
  EXPECT_EQ(std::string(HyphenStatusName(HyphenStatus::kNotOpen)), "not open");
  EXPECT_EQ(std::string(HyphenStatusName(HyphenStatus::kBadTable)), "bad table");
}

TEST(Hyphenator, AgreesWithLayingEveryPatternOverEveryPlace) {
  Dice dice(11);
  std::size_t placesInAll = 0;
  for (int round = 0; round < 300; ++round) {
    // Few letters, so that patterns meet in the words and in each other.
    const std::uint32_t letters = dice.Between(2, 5);
    Patterns patterns;
    const std::size_t count = dice.Between(0, 60);
    patterns.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      patterns.push_back(MadeUpPattern(dice, letters));
    }
    const auto leftMin = static_cast<std::uint8_t>(dice.Between(1, 3));
    const auto rightMin = static_cast<std::uint8_t>(dice.Between(1, 3));
    const Table table = PlainHyphenTable(patterns, leftMin, rightMin);
    Hyphenator hyphenator;
    ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk) << "round " << round;

    for (int n = 0; n < 60; ++n) {
      const std::string word = MadeUpWord(dice, dice.Between(1, 40), letters);
      const std::vector<std::size_t> places = PlacesByTable(hyphenator, word);
      ASSERT_EQ(places, PlacesByHand(patterns, word, leftMin, rightMin)) << "round " << round << ", " << word;
      ASSERT_LE(hyphenator.LastWork(), hyphenator.MostWorkFor(word.size())) << "round " << round << ", " << word;
      placesInAll += places.size();
    }
  }
  EXPECT_GT(placesInAll, 10000U);
}

TEST(Hyphenator, DoesNoMoreWorkThanItSaysItWill) {
  // Patterns made to cost a word the most: every run of the letter a, up to
  // the longest a table lets a pattern be, each with a number at every
  // place, and a word that is nothing but that letter. Every start in the
  // word then goes as deep as the patterns do.
  Patterns patterns;
  std::string run;
  for (std::size_t length = 1; length <= Hyphenator::kMaxDeepest; ++length) {
    run += "1a";
    patterns.push_back(run + "1");
  }
  const Table table = PlainHyphenTable(patterns, 1, 1);
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk);

  for (const std::size_t letters : {std::size_t{1}, std::size_t{8}, Hyphenator::kMaxLetters}) {
    const std::string word(letters, 'a');
    EXPECT_EQ(PlacesByTable(hyphenator, word).size(), letters - 1);
    EXPECT_LE(hyphenator.LastWork(), hyphenator.MostWorkFor(letters)) << letters;
    // The bound grows with the word, as far as words are looked at.
    if (letters < Hyphenator::kMaxLetters) {
      EXPECT_LT(hyphenator.MostWorkFor(letters), hyphenator.MostWorkFor(letters + 1));
    }
  }
  // It is the same for every word that is too long to look at.
  EXPECT_EQ(hyphenator.MostWorkFor(Hyphenator::kMaxLetters), hyphenator.MostWorkFor(1000));
}

TEST(Hyphenator, IsNotLedAstrayByATableThatHasBeenSpoiled) {
  // A good table with one byte of it changed, for every byte and several
  // changes. Whether or not it still opens, a word is answered within the
  // work the table says, and nothing outside it is read.
  Dice dice(3);
  Patterns patterns = kHyphenation;
  patterns.reserve(patterns.size() + 40);
  for (int i = 0; i < 40; ++i) {
    patterns.push_back(MadeUpPattern(dice, 4));
  }
  const Table good = PlainHyphenTable(patterns, 1, 1);
  const std::vector<std::string> words = {"hyphenation", "abcdabcdabcdabcd", "a", std::string(63, 'b'), "dcbadcba"};

  std::size_t opened = 0;
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      // On the heap and exactly as long as the table, so that a read past
      // its end is caught.
      std::vector<std::uint8_t> spoiled = good;
      spoiled[at] ^= change;
      Hyphenator hyphenator;
      if (hyphenator.Open(spoiled) != HyphenStatus::kOk) {
        continue;
      }
      ++opened;
      for (const std::string& word : words) {
        const std::vector<std::size_t> places = PlacesByTable(hyphenator, word);
        ASSERT_LE(hyphenator.LastWork(), hyphenator.MostWorkFor(word.size())) << "byte " << at;
        ASSERT_TRUE(std::is_sorted(places.begin(), places.end())) << "byte " << at;
        for (const std::size_t place : places) {
          ASSERT_GT(place, 0U) << "byte " << at;
          ASSERT_LT(place, word.size()) << "byte " << at;
        }
      }
    }
  }
  // Most changes leave something that still opens: the test is of those.
  EXPECT_GT(opened, good.size());
}

TEST(Hyphenator, IsNotLedAstrayByATableOfAnyBytesAtAll) {
  // A header that is right about its own sizes, and after it whatever comes.
  Dice dice(17);
  for (int round = 0; round < 400; ++round) {
    const std::size_t listed = dice.Below(8);
    const std::size_t edges = dice.Between(1, 60);
    const std::size_t values = dice.Below(80);
    Table table = {'B',
                   'N',
                   'H',
                   'Y',
                   1,
                   static_cast<std::uint8_t>(dice.Between(1, 3)),
                   static_cast<std::uint8_t>(dice.Between(1, 3)),
                   static_cast<std::uint8_t>(dice.Between(1, 65)),
                   static_cast<std::uint8_t>(listed),
                   0,
                   static_cast<std::uint8_t>(edges),
                   0,
                   static_cast<std::uint8_t>(values),
                   0,
                   static_cast<std::uint8_t>(dice.Between(1, 255)),
                   0};
    // The letters a to h, in order, each naming some letter or other.
    for (std::size_t i = 0; i < listed; ++i) {
      table.push_back(static_cast<std::uint8_t>('a' + i));
      table.push_back(0);
      table.push_back(0);
      table.push_back(static_cast<std::uint8_t>(dice.Between(1, 4)));
    }
    const std::size_t rest = (edges * 5) + values;
    for (std::size_t i = 0; i < rest; ++i) {
      // Small numbers often, so that edges lead to edges that are there.
      table.push_back(static_cast<std::uint8_t>(dice.Below(3) == 0 ? dice.Below(256) : dice.Below(6)));
    }
    Hyphenator hyphenator;
    ASSERT_EQ(hyphenator.Open(table), HyphenStatus::kOk) << "round " << round;
    for (int n = 0; n < 20; ++n) {
      const std::string word = MadeUpWord(dice, dice.Between(1, 63), 8);
      const std::vector<std::size_t> places = PlacesByTable(hyphenator, word);
      ASSERT_LE(hyphenator.LastWork(), hyphenator.MostWorkFor(word.size())) << "round " << round;
      for (const std::size_t place : places) {
        ASSERT_GT(place, 0U);
        ASSERT_LT(place, word.size());
      }
    }
  }
}

}  // namespace
