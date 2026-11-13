#include "core/fixed_text.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using bn::core::FixedText;

TEST(FixedText, HoldsWhatFits) {
  FixedText<16> text;
  EXPECT_TRUE(text.Empty());
  EXPECT_EQ(text.View(), "");
  EXPECT_EQ(FixedText<16>::Capacity(), 16u);
  text.Append("one");
  text.Append(", two");
  EXPECT_EQ(text.View(), "one, two");
  EXPECT_EQ(text.Size(), 8u);
  EXPECT_FALSE(text.Truncated());
  text.Append(" & three");  // Exactly to the brim.
  EXPECT_EQ(text.View(), "one, two & three");
  EXPECT_FALSE(text.Truncated());
  text.Append("");
  EXPECT_FALSE(text.Truncated());

  text.Clear();
  EXPECT_TRUE(text.Empty());
  EXPECT_FALSE(text.Truncated());
}

TEST(FixedText, CutsWhatDoesNotFitAndThenTakesNoMore) {
  FixedText<8> text;
  text.Append("abcdef");
  text.Append("ghijkl");
  EXPECT_EQ(text.View(), "abcdefgh");
  EXPECT_TRUE(text.Truncated());
  // Not even something that would fit, had there been room: the end has
  // been lost, and nothing may be put after where it was.
  text.Clear();
  text.Append("abcdefg");
  text.Append("xyz");
  EXPECT_EQ(text.View(), "abcdefgx");
  text.Append("!");
  EXPECT_EQ(text.View(), "abcdefgx");
  EXPECT_TRUE(text.Truncated());
  // Nor in the room a character left when it was cut whole.
  text.Clear();
  text.Append("abcdef");
  text.Append("\xE2\x80\x94");  // Three bytes, with room for two.
  EXPECT_EQ(text.View(), "abcdef");
  text.Append("!");
  EXPECT_EQ(text.View(), "abcdef");
  EXPECT_TRUE(text.Truncated());
}

TEST(FixedText, NeverCutsInsideACharacter) {
  // Characters of two, three and four bytes, with every amount of room
  // from none to all of it.
  const std::string characters[] = {"\xC3\xA9", "\xE2\x80\x94", "\xF0\x9F\x98\x80"};
  for (const std::string& character : characters) {
    for (std::size_t before = 0; before <= 8; ++before) {
      FixedText<8> text;
      text.Append(std::string(before, 'a'));
      text.Append(character + "z");
      const std::size_t room = 8 - before;
      if (room > character.size()) {
        EXPECT_EQ(text.View(), std::string(before, 'a') + character + "z") << before;
        EXPECT_FALSE(text.Truncated());
      } else if (room == character.size()) {
        EXPECT_EQ(text.View(), std::string(before, 'a') + character) << before;
        EXPECT_TRUE(text.Truncated());
      } else {
        EXPECT_EQ(text.View(), std::string(before, 'a')) << before << " " << character.size();
        EXPECT_TRUE(text.Truncated());
      }
    }
  }
}

TEST(FixedText, MakesRunsOfSpaceOneSpace) {
  FixedText<64> text;
  text.AppendCollapsed("  \n\t The   Book\r\n of \t\t Sand  ");
  EXPECT_EQ(text.View(), "The Book of Sand ");
  text.RemoveTrailingSpace();
  EXPECT_EQ(text.View(), "The Book of Sand");
  text.RemoveTrailingSpace();
  EXPECT_EQ(text.View(), "The Book of Sand");

  // Text that comes in pieces is the same text as if it came whole,
  // wherever the pieces are cut.
  const std::string whole = "  A \n tale   of two\tcities \n";
  for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
    FixedText<64> pieces;
    pieces.AppendCollapsed(whole.substr(0, cut));
    pieces.AppendCollapsed(whole.substr(cut));
    pieces.RemoveTrailingSpace();
    EXPECT_EQ(pieces.View(), "A tale of two cities") << cut;
  }

  FixedText<64> blank;
  blank.AppendCollapsed(" \n\t ");
  EXPECT_TRUE(blank.Empty());
  blank.RemoveTrailingSpace();
  EXPECT_TRUE(blank.Empty());
}

TEST(FixedText, CutsCollapsedTextAtACharacterToo) {
  // Room for "one two caf" and one byte more, which is half of the next
  // character: it is left off whole.
  FixedText<12> text;
  text.AppendCollapsed("one  two  caf\xC3\xA9 more");
  EXPECT_EQ(text.View(), "one two caf");
  EXPECT_TRUE(text.Truncated());
  text.AppendCollapsed("x");
  EXPECT_EQ(text.View(), "one two caf");

  FixedText<9> exact;
  exact.AppendCollapsed("one  two  x");
  EXPECT_EQ(exact.View(), "one two x");
  EXPECT_FALSE(exact.Truncated());
}

}  // namespace
