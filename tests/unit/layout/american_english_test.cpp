#include "hyphenation/american_english.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "layout/hyphenator.h"

namespace {

using bn::hyphenation::AmericanEnglish;
using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;

// `word` with a - at every place the table lets it be broken.
std::string Broken(Hyphenator& hyphenator, std::string_view word) {
  std::array<std::uint8_t, Hyphenator::kMaxLetters> places{};
  const std::size_t count = hyphenator.Find(word, places);
  std::string shown;
  std::size_t next = 0;
  for (std::size_t at = 0; at < word.size(); ++at) {
    if (next < count && places[next] == at) {
      shown += '-';
      ++next;
    }
    shown += word[at];
  }
  return shown;
}

TEST(AmericanEnglish, IsTheTableBnHyphenMadeByteForByte) {
  // The build packs the patterns into a file and then writes that file's
  // bytes out for the compiler: what is compiled in is what was packed.
  std::ifstream file(BN_AMERICAN_ENGLISH_TABLE, std::ios::binary);
  ASSERT_TRUE(file) << BN_AMERICAN_ENGLISH_TABLE;
  const std::vector<std::uint8_t> packed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::span<const std::uint8_t> table = AmericanEnglish().table;
  EXPECT_GT(packed.size(), 30000U);
  EXPECT_EQ(std::vector<std::uint8_t>(table.begin(), table.end()), packed);
}

TEST(AmericanEnglish, BreaksWordsAsTeXDoes) {
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(AmericanEnglish().table), HyphenStatus::kOk);
  EXPECT_EQ(Broken(hyphenator, "hyphenation"), "hy-phen-ation");
  EXPECT_EQ(Broken(hyphenator, "typography"), "ty-pog-ra-phy");
  EXPECT_EQ(Broken(hyphenator, "Typography"), "Ty-pog-ra-phy");
  EXPECT_EQ(Broken(hyphenator, "supercalifragilisticexpialidocious"), "su-per-cal-ifrag-ilis-tic-ex-pi-ali-do-cious");
  // Three of the words the patterns' file writes out with their breaks,
  // which no pattern changes.
  EXPECT_EQ(Broken(hyphenator, "associate"), "as-so-ciate");
  EXPECT_EQ(Broken(hyphenator, "table"), "ta-ble");
  EXPECT_EQ(Broken(hyphenator, "present"), "present");
}

TEST(AmericanEnglish, LeavesTwoLettersBeforeABreakAndThreeAfterOne) {
  Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(AmericanEnglish().table), HyphenStatus::kOk);
  EXPECT_EQ(hyphenator.LeftMin(), 2U);
  EXPECT_EQ(hyphenator.RightMin(), 3U);
}

TEST(AmericanEnglish, ComesWithItsCopyrightNotice) {
  const std::string_view notice = AmericanEnglish().notice;
  EXPECT_NE(notice.find("Copyright (C) 1990, 2004, 2005 Gerard D.C. Kuiken"), std::string_view::npos);
  EXPECT_NE(notice.find("provided the copyright notice and this notice are preserved"), std::string_view::npos);
}

}  // namespace
