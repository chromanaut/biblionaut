#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "epub/css.h"

// The kind of face a font-family value asks for (epub/css.h).

namespace {

using bn::epub::CssClassOfFamilies;
using bn::epub::CssClassOfFamily;
using bn::epub::CssFamilyClass;
using bn::epub::CssFamilyHow;

// The kind of face a whole value comes to, by its name.
std::string Class(std::string_view families) { return bn::epub::CssFamilyClassName(CssClassOfFamilies(families)); }

// The kind of face of one name, written as a book might write it, and
// what said so: "serif name", "mono keyword", or "-" if nothing says.
std::string One(std::string_view written) {
  std::vector<char> name(bn::epub::kCssMostFamilyName);
  std::string_view rest = written;
  const std::string_view taken = bn::epub::CssNextFamily(rest, name);
  CssFamilyHow how = CssFamilyHow::kKeyword;
  const CssFamilyClass familyClass = CssClassOfFamily(taken, how);
  if (how == CssFamilyHow::kNothing) {
    EXPECT_EQ(familyClass, CssFamilyClass::kUnset) << written;
    return "-";
  }
  EXPECT_NE(familyClass, CssFamilyClass::kUnset) << written;
  return std::string(bn::epub::CssFamilyClassName(familyClass)) +
         (how == CssFamilyHow::kKeyword ? " keyword" : " name");
}

TEST(CssFamilyClass, IsThatOfTheFirstNameThatSays) {
  EXPECT_EQ(Class("Georgia, serif"), "serif");
  EXPECT_EQ(Class("\"Helvetica Neue\", Arial"), "sans");
  // The first name wins, as a reader takes the first face it has.
  EXPECT_EQ(Class("\"Courier New\", serif"), "mono");
  EXPECT_EQ(Class("cursive"), "script");
  // A name nothing knows is passed over for the next.
  EXPECT_EQ(Class("\"No Such Face\", sans-serif"), "sans");
  EXPECT_EQ(Class("\"No Such Face\", 'Nor This', \"Letter Gothic\", Georgia"), "mono");
}

TEST(CssFamilyClass, IsOtherWhenNothingSays) {
  // The book did name something, so it is not as if nothing were set.
  EXPECT_EQ(Class("\"No Such Face\""), "other");
  EXPECT_EQ(Class("Foo, Bar Baz, \"q\""), "other");
  EXPECT_EQ(Class(""), "other");
  EXPECT_EQ(Class(" , ,"), "other");
  // A face of pictures, or of letters drawn for show, is another kind: the
  // names after it are not looked at.
  EXPECT_EQ(Class("FontAwesome, sans-serif"), "other");
  EXPECT_EQ(Class("fantasy, serif"), "other");
}

TEST(CssFamilyClass, TakesEachOfCssOwnWordsForAKind) {
  EXPECT_EQ(One("serif"), "serif keyword");
  EXPECT_EQ(One("ui-serif"), "serif keyword");
  EXPECT_EQ(One("math"), "serif keyword");
  EXPECT_EQ(One("sans-serif"), "sans keyword");
  EXPECT_EQ(One("system-ui"), "sans keyword");
  EXPECT_EQ(One("ui-sans-serif"), "sans keyword");
  EXPECT_EQ(One("monospace"), "mono keyword");
  EXPECT_EQ(One("ui-monospace"), "mono keyword");
  EXPECT_EQ(One("cursive"), "script keyword");
  EXPECT_EQ(One("fantasy"), "other keyword");
  // In any case, since a name is given in small letters.
  EXPECT_EQ(One("Sans-Serif"), "sans keyword");
  // The words that set nothing apart are not kinds of face.
  EXPECT_EQ(One("emoji"), "-");
  EXPECT_EQ(One("normal"), "-");
}

TEST(CssFamilyClass, KnowsANameHoweverItIsWritten) {
  for (const char* written : {"Palatino", "\"Palatino LT Std\"", "PalatinoLTStd-Medium", "'Palatino Linotype'",
                              "PALATINO", "palatino_lt_std"}) {
    EXPECT_EQ(One(written), "serif name") << written;
  }
  EXPECT_EQ(One("AGaramondPro-Italic-0855"), "serif name");
  EXPECT_EQ(One("\"EB Garamond\""), "serif name");
  EXPECT_EQ(One("CormorantGaramond"), "serif name");
  EXPECT_EQ(One("Times_New_Roman"), "serif name");
  EXPECT_EQ(One("TimesNewRomanMTStd"), "serif name");
  EXPECT_EQ(One("\"Book Antiqua\""), "serif name");
  EXPECT_EQ(One("HelveticaLTStd-Blk"), "sans name");
  EXPECT_EQ(One("\"Helvetica (T1) Medium\""), "sans name");
  EXPECT_EQ(One("TradeGothicLTStd-BdCn20"), "sans name");
  EXPECT_EQ(One("\"Courier New\""), "mono name");
  EXPECT_EQ(One("BendixHandScript"), "script name");
  EXPECT_EQ(One("\"ITC Zapf Chancery Pro\""), "script name");
  EXPECT_EQ(One("\"Standard Blackletter\""), "other name");
}

TEST(CssFamilyClass, TakesAWordForAKindOfFaceWhereverItIs) {
  EXPECT_EQ(One("\"Noto Serif\""), "serif name");
  EXPECT_EQ(One("FreeFontSerif"), "serif name");
  EXPECT_EQ(One("ZillaSlab"), "serif name");
  EXPECT_EQ(One("\"PT Sans\""), "sans name");
  EXPECT_EQ(One("DejaVuSans"), "sans name");
  EXPECT_EQ(One("FreeFontMono"), "mono name");
  EXPECT_EQ(One("Monospaced"), "mono name");
  EXPECT_EQ(One("\"American Typewriter\""), "mono name");
  EXPECT_EQ(One("PatrickHand"), "script name");
  EXPECT_EQ(One("\"MS Mincho\""), "serif name");
  EXPECT_EQ(One("\"MS Gothic\""), "sans name");
}

TEST(CssFamilyClass, TakesTheLongestNameItFinds) {
  // A family of several kinds is told apart by the longer name.
  EXPECT_EQ(One("Ubuntu"), "sans name");
  EXPECT_EQ(One("\"Ubuntu Mono\""), "mono name");
  EXPECT_EQ(One("Roboto"), "sans name");
  EXPECT_EQ(One("\"Roboto Slab\""), "serif name");
  EXPECT_EQ(One("RobotoMono-Regular"), "mono name");
  EXPECT_EQ(One("\"Lucida Grande\""), "sans name");
  EXPECT_EQ(One("\"Lucida Bright\""), "serif name");
  EXPECT_EQ(One("\"Lucida Console\""), "mono name");
  EXPECT_EQ(One("\"Lucida Sans Typewriter\""), "mono name");
  EXPECT_EQ(One("\"Lucida Handwriting\""), "script name");
  EXPECT_EQ(One("\"Century Schoolbook\""), "serif name");
  EXPECT_EQ(One("Century_Gothic"), "sans name");
  EXPECT_EQ(One("\"Letter Gothic\""), "mono name");
  EXPECT_EQ(One("\"Merriweather Sans\""), "sans name");
  // "Sans serif" is without serifs, and a monospaced sans is monospaced.
  EXPECT_EQ(One("\"Sans Serif\""), "sans name");
  EXPECT_EQ(One("\"Microsoft Sans Serif\""), "sans name");
  EXPECT_EQ(One("\"DejaVu Sans Mono\""), "mono name");
  EXPECT_EQ(One("\"Comic Sans MS\""), "script name");
  EXPECT_EQ(One("\"Segoe UI\""), "sans name");
  EXPECT_EQ(One("\"Segoe Script\""), "script name");
}

TEST(CssFamilyClass, OfTwoNamesAsLongTakesTheLater) {
  EXPECT_EQ(One("\"Verdana Courier\""), "mono name");
  EXPECT_EQ(One("\"Courier Verdana\""), "sans name");
  // By where each is found last, for a name that counts as a whole word
  // as for any other.
  EXPECT_EQ(One("\"Arial Times Arial\""), "sans name");
  EXPECT_EQ(One("\"Times Arial Times\""), "serif name");
  EXPECT_EQ(One("\"Verdana Courier Verdana\""), "sans name");
  // And of the same name found twice, either: it is one kind.
  EXPECT_EQ(One("\"Courier Courier\""), "mono name");
}

TEST(CssFamilyClass, CountsAShortNameOnlyAsAWholeWord) {
  EXPECT_EQ(One("Arial"), "sans name");
  EXPECT_EQ(One("Arial-R"), "sans name");
  EXPECT_EQ(One("\"Arial Narrow\""), "sans name");
  EXPECT_EQ(One("\"arial (bold) 2\""), "sans name");
  EXPECT_EQ(One("Aerial"), "-");
  EXPECT_EQ(One("Arialish"), "-");
  EXPECT_EQ(One("Times"), "serif name");
  EXPECT_EQ(One("Sometimes"), "-");
  EXPECT_EQ(One("Lato-Light"), "sans name");
  EXPECT_EQ(One("Latona"), "-");
  EXPECT_EQ(One("\"DIN Next LT Pro\""), "sans name");
  EXPECT_EQ(One("Dinner"), "-");
  // Written as one word with something else, it is known only where that
  // way of writing it is.
  EXPECT_EQ(One("ArialMT"), "sans name");
  EXPECT_EQ(One("TimesLTStd"), "serif name");
  EXPECT_EQ(One("ArialFoo"), "-");
  // A short name inside a longer one that is known is that longer one.
  EXPECT_EQ(One("\"Material Icons\""), "other name");
}

TEST(CssFamilyClass, IsNotMisledByAMakersName) {
  EXPECT_EQ(One("\"Monotype Sorts\""), "-");
  EXPECT_EQ(One("Monotype"), "-");
  EXPECT_EQ(One("\"Monotype Corsiva\""), "script name");
  EXPECT_EQ(One("\"Monotype Garamond\""), "serif name");
  EXPECT_EQ(One("Handel"), "-");
  EXPECT_EQ(One("\"Handel Gothic\""), "sans name");
  EXPECT_EQ(Class("\"Monotype Sorts\", Georgia"), "serif");
  // Only what is inside the maker's name is hidden: the same word standing
  // by itself still says.
  EXPECT_EQ(One("\"Mono Monotype\""), "mono name");
  EXPECT_EQ(One("\"Monotype Mono\""), "mono name");
  EXPECT_EQ(One("\"Monotype Monotype\""), "-");
  EXPECT_EQ(One("\"Hand of Handel\""), "script name");
}

TEST(CssFamilyClass, PassesOverWhatIsNotALatinLetter) {
  // Digits and marks part words and are nothing themselves.
  EXPECT_EQ(One("\"Garamond 3 LT Std\""), "serif name");
  EXPECT_EQ(One("G4a5r6a7m8o9n0d"), "serif name");
  EXPECT_EQ(One("\"M+ 1mn\""), "-");
  EXPECT_EQ(One("\"123\""), "-");
  // Letters of other alphabets are not looked at.
  EXPECT_EQ(One("\"\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE6\x98\x8E\xE6\x9C\x9D Pro W3\""), "-");
  EXPECT_EQ(One("\"Caf\xC3\xA9 Sans\""), "sans name");
  EXPECT_EQ(One("G\xC3\xA5ramond"), "-");
}

TEST(CssFamilyClass, LooksAtSoMuchOfANameAndNoMore) {
  // The letters after the first kCssMostFamilyLetters are not looked at.
  const std::string within = std::string(bn::epub::kCssMostFamilyLetters - 8, 'x') + "garamond";
  const std::string beyond = std::string(bn::epub::kCssMostFamilyLetters - 7, 'x') + "garamond";
  EXPECT_EQ(One(within), "serif name");
  EXPECT_EQ(One(beyond), "-");
  // More words than are told apart: a short name among the later ones is
  // not found, and one that counts anywhere is.
  std::string words;
  for (int i = 0; i < 20; ++i) {
    words += "q ";
  }
  EXPECT_EQ(One("\"" + words + "arial\""), "-");
  EXPECT_EQ(One("\"" + words + "garamond\""), "serif name");
  // Just so many words are told apart: the sixteenth is one of them, and
  // the seventeenth is not.
  const std::string fifteen = "a b c d e f g h i j k l m n o ";
  EXPECT_EQ(One("\"" + fifteen + "arial\""), "sans name");
  EXPECT_EQ(One("\"" + fifteen + "p arial\""), "-");
  // A name is taken off a list to kCssMostFamilyName bytes, and the next
  // name is found after it.
  std::string spaced;
  while (spaced.size() < bn::epub::kCssMostFamilyName) {
    spaced += "1 ";
  }
  EXPECT_EQ(Class("\"" + spaced + "courier\", Georgia"), "serif");
  EXPECT_EQ(Class("\"1 2 3 courier\", Georgia"), "mono");
}

TEST(CssFamilyClass, SaysWhatSaidSo) {
  CssFamilyHow how = CssFamilyHow::kNothing;
  EXPECT_EQ(CssClassOfFamilies("Foo, serif", how), CssFamilyClass::kSerif);
  EXPECT_EQ(how, CssFamilyHow::kKeyword);
  EXPECT_EQ(CssClassOfFamilies("Foo, Georgia, serif", how), CssFamilyClass::kSerif);
  EXPECT_EQ(how, CssFamilyHow::kName);
  EXPECT_EQ(CssClassOfFamilies("Foo, Bar", how), CssFamilyClass::kOther);
  EXPECT_EQ(how, CssFamilyHow::kNothing);
  EXPECT_EQ(CssClassOfFamilies("fantasy", how), CssFamilyClass::kOther);
  EXPECT_EQ(how, CssFamilyHow::kKeyword);
  EXPECT_EQ(CssClassOfFamilies("Wingdings", how), CssFamilyClass::kOther);
  EXPECT_EQ(how, CssFamilyHow::kName);
}

TEST(CssFamilyClass, HasEveryKnownNameOnceInOrderAndFindsEachByItself) {
  const auto known = bn::epub::CssKnownFamilies();
  ASSERT_GT(known.size(), 100U);
  std::size_t wholeWords = 0;
  for (std::size_t i = 0; i < known.size(); ++i) {
    const std::string name(known[i].name);
    if (i != 0) {
      EXPECT_LT(known[i - 1].name, known[i].name) << name;
    }
    CssFamilyHow how = CssFamilyHow::kNothing;
    EXPECT_EQ(CssClassOfFamily(name, how), known[i].familyClass) << name;
    EXPECT_EQ(how == CssFamilyHow::kNothing, known[i].familyClass == CssFamilyClass::kUnset) << name;
    EXPECT_FALSE(known[i].familyClass == CssFamilyClass::kUnset && known[i].wholeWord) << name;
    // With letters before and after it, a name that counts anywhere is
    // still found, and one that counts as a whole word is not.
    const CssFamilyClass inside = CssClassOfFamily("zq" + name + "qz", how);
    if (known[i].wholeWord) {
      ++wholeWords;
      EXPECT_EQ(inside, CssFamilyClass::kUnset) << name;
    } else {
      EXPECT_EQ(inside, known[i].familyClass) << name;
    }
  }
  EXPECT_GT(wholeWords, 10U);
}

TEST(CssFamilyClass, HasANameForEachKind) {
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kUnset), "unset");
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kSerif), "serif");
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kSans), "sans");
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kMono), "mono");
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kScript), "script");
  EXPECT_STREQ(bn::epub::CssFamilyClassName(CssFamilyClass::kOther), "other");
  EXPECT_EQ(bn::epub::kCssFamilyClasses, 6U);
}

// What the reader of a sheet tells of each font-family it reads.
class Families final : public bn::epub::CssHandler {
 public:
  void OnRuleBegin() noexcept override {}
  void OnSelector(const bn::epub::CssSelector& /*selector*/) noexcept override {}
  void OnDeclaration(const bn::epub::CssDeclaration& declaration) noexcept override {
    if (declaration.property != bn::epub::CssProperty::kFontFamily) {
      return;
    }
    if (declaration.value.kind == bn::epub::CssValue::Kind::kFamilies) {
      said += std::string(bn::epub::CssFamilyClassName(declaration.value.familyClass)) + "[" +
              std::string(declaration.value.families) + "] ";
    } else {
      said += std::string(bn::epub::CssKeywordName(declaration.value.keyword)) + " ";
    }
  }
  void OnRuleEnd() noexcept override {}
  void OnImport(std::string_view /*url*/) noexcept override {}
  void OnDropped(bn::epub::CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}

  std::string said;
};

TEST(CssFamilyClass, IsToldWithTheNamesByTheReaderOfASheet) {
  const std::string sheet =
      "p { font-family: Foo, \"Courier New\", serif }\n"
      "h1 { font: bold 2em/1.2 'Gill Sans', sans-serif }\n"
      "i { font-family: \"No Such Face\" }\n"
      "b { font-family: inherit }\n";
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>(sheet.data()), sheet.size()});
  bn::epub::CssReader reader;
  Families families;
  EXPECT_EQ(reader.Read(source, families), bn::epub::CssStatus::kOk);
  // A word for every property, such as `inherit`, is no list of families.
  EXPECT_EQ(families.said,
            "mono[Foo, \"Courier New\", serif] sans['Gill Sans', sans-serif] other[\"No Such Face\"] inherit ");

  Families own;
  bn::epub::ReadCssDeclarations("font-family: Lekton; font-family: 'Zapf Chancery'", own);
  EXPECT_EQ(own.said, "mono[Lekton] script['Zapf Chancery'] ");
}

}  // namespace
