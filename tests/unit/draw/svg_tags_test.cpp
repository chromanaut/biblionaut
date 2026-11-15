#include "draw/svg_tags.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace {

using bn::draw::SvgTagReader;
using bn::draw::SvgTagStatus;
using bn::draw::SvgTagToken;
using Kind = bn::draw::SvgTagToken::Kind;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// What a reader made of a document.
struct Reading {
  // Everything it told, written out again in one way: `<a b="c">` for an
  // element and its attributes, `/>` where it was empty, `{text}` for
  // text, `</a>` for its end. Pieces of one value, or of one run of text,
  // are joined.
  std::string said;
  SvgTagStatus status = SvgTagStatus::kOk;
  std::uint32_t errorOffset = 0;
  std::uint32_t unknownEntities = 0;
  std::size_t calls = 0;
};

// Reads `document`, given to the reader `piece` bytes at a time.
Reading Read(std::string_view document, std::size_t piece = SIZE_MAX) {
  Reading reading;
  SvgTagReader reader;
  bool inValue = false;
  bool inText = false;
  const auto close = [&reading, &inValue, &inText]() {
    if (inValue) {
      reading.said += '"';
    }
    if (inText) {
      reading.said += '}';
    }
    inValue = false;
    inText = false;
  };
  for (std::size_t at = 0; at < document.size();) {
    std::string_view bytes = document.substr(at, piece);
    at += bytes.size();
    while (!bytes.empty()) {
      SvgTagToken token;
      const std::size_t taken = reader.Next(bytes, token);
      ++reading.calls;
      EXPECT_LE(taken, bytes.size());
      if (token.kind == Kind::kNone) {
        EXPECT_EQ(taken, bytes.size());
      }
      if (token.ofInput) {
        // A piece of the input is the last of the bytes just taken.
        EXPECT_EQ(token.text.data() + token.text.size(), bytes.data() + taken);
      }
      bytes.remove_prefix(std::min(taken, bytes.size()));
      switch (token.kind) {
        case Kind::kNone:
          break;
        case Kind::kOpen:
          close();
          reading.said += "<" + std::string(token.text);
          break;
        case Kind::kAttribute:
          close();
          reading.said += " " + std::string(token.text) + "=";
          break;
        case Kind::kValue:
          if (!inValue) {
            reading.said += '"';
            inValue = true;
          }
          reading.said += token.text;
          break;
        case Kind::kOpened:
          close();
          reading.said += token.empty ? "/>" : ">";
          break;
        case Kind::kClose:
          close();
          reading.said += "</" + std::string(token.text) + ">";
          break;
        case Kind::kText:
          if (!inText) {
            reading.said += '{';
            inText = true;
          }
          reading.said += token.text;
          break;
      }
    }
  }
  close();
  reading.status = reader.Finish();
  reading.errorOffset = reader.ErrorOffset();
  reading.unknownEntities = reader.UnknownEntities();
  return reading;
}

std::string Said(std::string_view document) {
  const Reading reading = Read(document);
  EXPECT_EQ(reading.status, SvgTagStatus::kOk) << document;
  return reading.said;
}

TEST(SvgTags, TellsElementsAttributesAndText) {
  EXPECT_EQ(Said("<svg width=\"10\"><g><rect x='1' y=\"2\"/>hello</g></svg>"),
            "<svg width=\"10\"><g><rect x=\"1\" y=\"2\"/>{hello}</g></svg>");
  // Space is allowed in most places in a tag, and a line end is space.
  EXPECT_EQ(Said("<a\n  b = \"1\"\tc\r\n=\n'2'  >\n</a  >"), "<a b=\"1\" c=\"2\">{\n}</a>");
  // One quote may stand inside the other, and a `>` inside either.
  EXPECT_EQ(Said("<a b=\"it's\" c='say \"x > y\"'/>"), "<a b=\"it's\" c=\"say \"x > y\"\"/>");
  // Names with a prefix, with digits, dashes and points, and beyond ASCII.
  EXPECT_EQ(Said("<svg:path xlink:href='#a' stroke-width='1' a.b_c9='x'/>"),
            "<svg:path xlink:href=\"#a\" stroke-width=\"1\" a.b_c9=\"x\"/>");
  EXPECT_EQ(Said("<\xC3\xA9l\xC3\xA9ment \xC3\xA0='1'/>"), "<\xC3\xA9l\xC3\xA9ment \xC3\xA0=\"1\"/>");
  // An attribute with nothing in it, and one with no value at all.
  EXPECT_EQ(Said("<a b='' c d='1'/>"), "<a b= c= d=\"1\"/>");
  // Text before, between and after.
  EXPECT_EQ(Said("\xEF\xBB\xBF <a>one<b/>two</a>\n"), "{\xEF\xBB\xBF }<a>{one}<b/>{two}</a>{\n}");
}

TEST(SvgTags, SaysTheSameWhateverPiecesTheBytesComeIn) {
  const std::string_view document =
      "<?xml version=\"1.0\"?>\n<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"x.dtd\" [ <!ENTITY a \"b>c\"> ]>\n"
      "<!-- a comment -- with > and <tags/> in it -->\n"
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox='0 0 10 10'>\n"
      "  <style><![CDATA[ .a > .b { fill: red } ]] ]]]></style>\n"
      "  <path d=\"M0 0L5 5&#32;10 0z\" fill=\"&quot;&amp;&lt;&gt;&apos;\" title='&unknown;a&b;&#x41;'/>\n"
      "  <text>caf&#233; &amp; t&#xE9;&#x20AC;&#x1F600; & more</text>\n"
      "</svg>\n";
  const Reading whole = Read(document);
  ASSERT_EQ(whole.status, SvgTagStatus::kOk);
  EXPECT_EQ(whole.said,
            "{\n\n\n}<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">{\n  }"
            "<style>{ .a > .b { fill: red } ]] ]}</style>{\n  }"
            "<path d=\"M0 0L5 5 10 0z\" fill=\"\"&<>'\" title=\"aA\"/>{\n  }"
            "<text>{caf\xC3\xA9 & t\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80 & more}</text>{\n}</svg>{\n}");
  EXPECT_EQ(whole.unknownEntities, 2u);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{64}}) {
    const Reading cut = Read(document, piece);
    EXPECT_EQ(cut.said, whole.said) << piece;
    EXPECT_EQ(cut.status, SvgTagStatus::kOk) << piece;
    EXPECT_EQ(cut.unknownEntities, 2u) << piece;
  }
}

TEST(SvgTags, ReadsTheFiveEntitiesAndCharactersByNumber) {
  EXPECT_EQ(Said("<a b='&lt;&gt;&amp;&quot;&apos;'>&lt;&gt;&amp;&quot;&apos;</a>"), "<a b=\"<>&\"'\">{<>&\"'}</a>");
  // In tens and in sixteens, as one byte of UTF-8, two, three and four.
  EXPECT_EQ(Said("<a b='&#65;&#x42;&#X43;&#233;&#x20AC;&#x1F600;&#x10FFFF;'/>"),
            "<a b=\"ABC\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF\"/>");
  // The last character of each length, and the first of the next.
  EXPECT_EQ(Said("<a b='&#x7F;&#x80;&#x7FF;&#x800;&#xFFFF;&#x10000;'/>"),
            "<a b=\"\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\"/>");
}

TEST(SvgTags, AnEntityItDoesNotKnowStandsForNothing) {
  // One a document type could have declared, and numbers that are no
  // character: nothing, one of the pairs UTF-16 is written with, and one
  // past the last there is.
  for (const std::string_view entity :
       {"&ns_svg;", "&#0;", "&#xD800;", "&#xDFFF;", "&#x110000;", "&#99999999;", "&#;", "&#x;", "&#12a;", "&;"}) {
    const Reading reading = Read("<a b='x" + std::string(entity) + "y'>p" + std::string(entity) + "q</a>");
    EXPECT_EQ(reading.status, SvgTagStatus::kOk) << entity;
    EXPECT_EQ(reading.said, "<a b=\"xy\">{pq}</a>") << entity;
    EXPECT_EQ(reading.unknownEntities, 2u) << entity;
  }
}

TEST(SvgTags, AnAmpersandThatBeginsNoEntityIsItself) {
  EXPECT_EQ(Said("<a b='fish & chips' c='a&&b' d='x&y z'>R&D, &c. &</a>"),
            "<a b=\"fish & chips\" c=\"a&&b\" d=\"x&y z\">{R&D, &c. &}</a>");
  // What follows it is too long to be the name of one.
  EXPECT_EQ(Said("<a b='&abcdefghijklmnop;'/>"), "<a b=\"&abcdefghijklmnop;\"/>");
  // The quote that ends the value ends it, too.
  EXPECT_EQ(Said("<a b='x&amp' c=\"&\"/>"), "<a b=\"x&amp\" c=\"&\"/>");
  EXPECT_EQ(Read("<a b='&amp' c=\"&\"/>").unknownEntities, 0u);
}

TEST(SvgTags, PassesOverCommentsInstructionsAndADocumentType) {
  // An instruction ends at the first `?>`, wherever that is.
  EXPECT_EQ(Said("<?xml version='1.0'?><a><?php echo '?>'; ?>x</a>"), "<a>{'; ?>x}</a>");
  EXPECT_EQ(Said("<a><?b ? > ?? >?></a>"), "<a></a>");
  EXPECT_EQ(Said("<a><!----><!-- - -- > <b/> ---></a>"), "<a></a>");
  EXPECT_EQ(Said("<!DOCTYPE svg><a/>"), "<a/>");
  // What a document type declares for itself is in square brackets, and
  // may have a `>` in it, in quotes or not.
  EXPECT_EQ(Said("<!DOCTYPE svg PUBLIC '-//W3C//DTD SVG 1.1//EN' \"svg11.dtd\" [\n"
                 "  <!ENTITY ns_svg \"http://www.w3.org/2000/svg\">\n  <!ENTITY gt2 '>>'>\n]><a/>"),
            "<a/>");
  // Each may be cut anywhere.
  const std::string_view document = "<!-- a --><?b c?><!DOCTYPE d [ <!ENTITY e 'f'> ]><g/><!-- h -->";
  for (std::size_t piece = 1; piece < 12; ++piece) {
    EXPECT_EQ(Read(document, piece).said, "<g/>") << piece;
  }
}

TEST(SvgTags, TextMarkedAsTextIsGivenAsItIs) {
  EXPECT_EQ(Said("<style><![CDATA[a < b && c > d &amp; <e/>]]></style>"), "<style>{a < b && c > d &amp; <e/>}</style>");
  // Square brackets that are not its end are part of it.
  EXPECT_EQ(Said("<a><![CDATA[x]y]]z]]]]></a>"), "<a>{x]y]]z]]}</a>");
  EXPECT_EQ(Said("<a><![CDATA[]]></a>"), "<a></a>");
  EXPECT_EQ(Said("<a><![CDATA[]]]></a>"), "<a>{]}</a>");
  EXPECT_EQ(Said("<a>one<![CDATA[two]]>three</a>"), "<a>{onetwothree}</a>");
  for (std::size_t piece = 1; piece < 8; ++piece) {
    EXPECT_EQ(Read("<a><![CDATA[x]y]]z]]]]></a>", piece).said, "<a>{x]y]]z]]}</a>") << piece;
  }
}

TEST(SvgTags, ANameTooLongIsCutShort) {
  const std::string name(SvgTagReader::kLongestName, 'n');
  EXPECT_EQ(Said("<" + name + " " + name + "='1'></" + name + ">"), "<" + name + " " + name + "=\"1\"></" + name + ">");
  // One byte more, or a hundred: the same.
  EXPECT_EQ(Said("<" + name + "X " + name + "YZ='1'></" + name + std::string(100, 'W') + ">"),
            "<" + name + " " + name + "=\"1\"></" + name + ">");
}

TEST(SvgTags, CountsTheElementsThatAreOpen) {
  SvgTagReader reader;
  SvgTagToken token;
  std::string_view bytes = "<a><b><c/></b>";
  std::uint32_t deepest = 0;
  while (!bytes.empty()) {
    bytes.remove_prefix(reader.Next(bytes, token));
    deepest = std::max(deepest, reader.Depth());
  }
  EXPECT_EQ(deepest, 2u);
  EXPECT_EQ(reader.Depth(), 1u);
  EXPECT_EQ(reader.Finish(), SvgTagStatus::kTruncated);
}

TEST(SvgTags, SaysWhereTagsGoWrong) {
  struct Case {
    std::string_view document;
    SvgTagStatus status;
    std::uint32_t offset;
    std::string_view said;  // What it told before that.
  };
  const Case cases[] = {
      {"<a>< b/></a>", SvgTagStatus::kMalformed, 4, "<a>"},
      {"<a><1/></a>", SvgTagStatus::kMalformed, 4, "<a>"},
      {"<a b=1/>", SvgTagStatus::kMalformed, 5, "<a b="},
      {"<a \"b\"/>", SvgTagStatus::kMalformed, 3, "<a"},
      {"<a/ >", SvgTagStatus::kMalformed, 3, "<a"},
      {"<a></></a>", SvgTagStatus::kMalformed, 5, "<a>"},
      {"<a></a b></a>", SvgTagStatus::kMalformed, 7, "<a>"},
      {"<a><!-x--></a>", SvgTagStatus::kMalformed, 6, "<a>"},
      {"<a><![CDATA x]]></a>", SvgTagStatus::kMalformed, 11, "<a>"},
      {"<a></a></a>", SvgTagStatus::kUnbalanced, 10, "<a></a>"},
      {"</a>", SvgTagStatus::kUnbalanced, 3, ""},
      {"<a><b></b>", SvgTagStatus::kTruncated, 10, "<a><b></b>"},
      {"<a", SvgTagStatus::kTruncated, 2, ""},
      {"<a b", SvgTagStatus::kTruncated, 4, "<a"},
      {"<a b='c", SvgTagStatus::kTruncated, 7, "<a b=\"c\""},
      {"<a><!-- b", SvgTagStatus::kTruncated, 9, "<a>"},
      {"<a>&am", SvgTagStatus::kTruncated, 6, "<a>"},
      {"<a><![CDATA[b]]", SvgTagStatus::kTruncated, 15, "<a>{b}"},
      {"<!DOCTYPE a [", SvgTagStatus::kTruncated, 13, ""},
      {"<?xml", SvgTagStatus::kTruncated, 5, ""},
  };
  for (const Case& one : cases) {
    for (const std::size_t piece : {SIZE_MAX, std::size_t{1}, std::size_t{3}}) {
      const Reading reading = Read(one.document, piece);
      EXPECT_EQ(reading.status, one.status) << one.document;
      EXPECT_EQ(reading.errorOffset, one.offset) << one.document;
      EXPECT_EQ(reading.said, one.said) << one.document;
    }
  }
  // Nothing at all, and nothing but space, are no mistake of the tags'.
  EXPECT_EQ(Read("").status, SvgTagStatus::kOk);
  EXPECT_EQ(Read(" \n ").status, SvgTagStatus::kOk);
  EXPECT_STREQ(bn::draw::SvgTagStatusName(SvgTagStatus::kUnbalanced), "unbalanced");
}

TEST(SvgTags, OnceItHasFailedItTakesEverythingAndTellsNothing) {
  SvgTagReader reader;
  SvgTagToken token;
  std::string_view bytes = "<a><<";
  while (!bytes.empty()) {
    bytes.remove_prefix(reader.Next(bytes, token));
  }
  ASSERT_EQ(reader.Status(), SvgTagStatus::kMalformed);
  EXPECT_EQ(reader.Next("<b c='d'/>", token), 10u);
  EXPECT_EQ(token.kind, Kind::kNone);
  EXPECT_EQ(reader.Finish(), SvgTagStatus::kMalformed);
  EXPECT_EQ(reader.ErrorOffset(), 4u);
}

TEST(SvgTags, TheEndOfAPieceCanBeHandedBackAndComesAgain) {
  // Whoever is reading takes at most five bytes of each piece of a value
  // or of text, and hands the rest back.
  const std::string value = "M0 0L10 10 20 20 30 30 40 40 50 50 60 60 70 70 80 80 90 90z";
  const std::string document = "<path d='" + value + "' e='" + value + "&amp;" + value + "'>" + value + "</path><";
  // The two values, one after the other, as they should come out.
  std::string both = value;
  both += value;
  both += '&';
  both += value;
  for (const std::size_t piece : {SIZE_MAX, std::size_t{4}, std::size_t{9}}) {
    SvgTagReader reader;
    std::string values;
    std::string text;
    for (std::size_t at = 0; at < document.size();) {
      std::string_view bytes = std::string_view(document).substr(at, piece);
      at += bytes.size();
      while (!bytes.empty()) {
        SvgTagToken token;
        std::size_t taken = reader.Next(bytes, token);
        if ((token.kind == Kind::kValue || token.kind == Kind::kText) && token.ofInput && token.text.size() > 5) {
          const std::size_t back = token.text.size() - 5;
          reader.GiveBack(back);
          taken -= back;
          token.text = token.text.substr(0, 5);
        }
        (token.kind == Kind::kValue ? values : text) += token.kind == Kind::kNone ? "" : std::string(token.text);
        if (token.kind != Kind::kValue && token.kind != Kind::kText) {
          text.clear();
        }
        if (token.kind == Kind::kClose) {
          EXPECT_EQ(values, both) << piece;
        }
        bytes.remove_prefix(taken);
      }
    }
    // Where it says the mistake at the end is has not been put out by the
    // handing back.
    EXPECT_EQ(reader.Finish(), SvgTagStatus::kTruncated);
    EXPECT_EQ(reader.ErrorOffset(), document.size()) << piece;
  }
}

TEST(SvgTags, AnyBytesAtAllEndInAStatus) {
  // Made of the bytes that mean something to it, so that every state it
  // has is reached, and of every byte there is.
  std::mt19937 random = Repeatable(31);
  const std::string_view meaningful = "<<<>>>//==\"\"''&&;;!!--[[]]??##xX09 \n\tabcCDATA";
  std::size_t failed = 0;
  for (int i = 0; i < 20000; ++i) {
    std::string document;
    const std::size_t length = random() % 60;
    for (std::size_t at = 0; at < length; ++at) {
      document += (i % 4 == 0) ? static_cast<char>(random() % 256) : meaningful[random() % meaningful.size()];
    }
    const std::size_t piece = (i % 3 == 0) ? SIZE_MAX : 1 + (random() % 5);
    const Reading reading = Read(document, piece);
    failed += reading.status == SvgTagStatus::kOk ? 0 : 1;
    // Each call takes a byte or tells something, so there are no more
    // calls than twice the bytes: it cannot be made to go round.
    ASSERT_LE(reading.calls, (2 * document.size()) + 2) << i;
    ASSERT_LE(reading.errorOffset, document.size()) << i;
    // And cut another way it comes to the same.
    const Reading whole = Read(document);
    ASSERT_EQ(reading.said, whole.said) << i;
    ASSERT_EQ(reading.status, whole.status) << i;
    ASSERT_EQ(reading.errorOffset, whole.errorOffset) << i;
  }
  EXPECT_GT(failed, 10000u);
}

}  // namespace
