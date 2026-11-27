#include "layout/made_up_face.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace {

using bn::layout::MadeUpFace;

TEST(MadeUpFace, GivesEachKindOfCharacterItsWidth) {
  EXPECT_EQ(MadeUpFace::CharacterWidth('a', 0), 36);
  EXPECT_EQ(MadeUpFace::CharacterWidth('i', 0), 18);
  EXPECT_EQ(MadeUpFace::CharacterWidth('.', 0), 18);
  EXPECT_EQ(MadeUpFace::CharacterWidth('m', 0), 54);
  EXPECT_EQ(MadeUpFace::CharacterWidth('W', 0), 54);
  EXPECT_EQ(MadeUpFace::CharacterWidth('Q', 0), 44);
  EXPECT_EQ(MadeUpFace::CharacterWidth('-', 0), 22);
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xC3', 0), 40);  // The first byte of é.
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xE4', 0), 40);  // Of 中.
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xA9', 0), 0);   // A byte that goes on with a character.
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xCC', 0), 0);   // A mark set over a letter.
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xCD', 0), 0);
}

TEST(MadeUpFace, MakesEachStyleFourWiderThanTheOneBefore) {
  EXPECT_EQ(MadeUpFace::CharacterWidth('a', 1), 40);
  EXPECT_EQ(MadeUpFace::CharacterWidth('a', 3), 48);
  EXPECT_EQ(MadeUpFace::CharacterWidth('\xA9', 3), 0);
  MadeUpFace face;
  EXPECT_EQ(face.SpaceWidth(0), 18);
  EXPECT_EQ(face.SpaceWidth(2), 26);
  EXPECT_EQ(MadeUpFace::HyphenWidth(0), 22);
  EXPECT_EQ(MadeUpFace::HyphenWidth(1), 26);
}

TEST(MadeUpFace, MeasuresEachPieceOfAWordAndCountsTheBytes) {
  MadeUpFace face;
  // "hyph" "en" "ation": 4 + 2 + 5 bytes.
  const std::array<std::uint8_t, 3> ends = {4, 6, 11};
  std::array<std::uint16_t, 3> widths{};
  std::array<std::uint16_t, 3> hyphens{};
  face.Measure("hyphenation", 0, ends, widths, hyphens);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 3>{36 + 36 + 36 + 36, 36 + 36, 36 + 18 + 18 + 36 + 36}));
  // A hyphen is as wide after any piece.
  EXPECT_EQ(hyphens, (std::array<std::uint16_t, 3>{22, 22, 22}));
  EXPECT_EQ(face.BytesMeasured(), 11U);

  // A character of two bytes is one width, in the piece it begins.
  const std::array<std::uint8_t, 2> accented = {3, 4};
  std::array<std::uint16_t, 2> two{};
  std::array<std::uint16_t, 2> twoHyphens{};
  face.Measure("\x63\xC3\xA9s", 1, accented, two, twoHyphens);
  EXPECT_EQ(two, (std::array<std::uint16_t, 2>{40 + 44, 40}));
  EXPECT_EQ(twoHyphens, (std::array<std::uint16_t, 2>{26, 26}));
  EXPECT_EQ(face.BytesMeasured(), 15U);
}

}  // namespace
