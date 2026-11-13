#include "epub/xml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "core/metrics.h"

namespace {

using bn::epub::XmlAttributes;
using bn::epub::XmlParser;
using bn::epub::XmlStatus;

// expat's numbers for the mistakes the tests make (its XML_Error).
constexpr int kSyntax = 2;
constexpr int kNoElements = 3;
constexpr int kInvalidToken = 4;
constexpr int kUnclosedToken = 5;
constexpr int kTagMismatch = 7;
constexpr int kDuplicateAttribute = 8;
constexpr int kJunkAfterDocument = 9;
constexpr int kUndefinedEntity = 11;
constexpr int kUnknownEncoding = 18;

std::span<const std::uint8_t> BytesOf(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// Writes down everything a parser says, one line an event:
//
//   (name          an element begins
//   Aname value    one of its attributes
//   )name          an element ends
//   -text          text, the pieces it came in joined up
//   &name          an entity that was not expanded
//
// with the line ends, tabs and backslashes in text and values written as
// \n, \t and \\.
class Recorder final : public bn::epub::XmlHandler {
 public:
  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override {
    FlushText();
    events += "(" + std::string(name) + "\n";
    for (std::size_t i = 0; i < attributes.Count(); ++i) {
      events += "A" + std::string(attributes.Name(i)) + " " + Escaped(attributes.Value(i)) + "\n";
    }
    if (stopAt == name && parser != nullptr) {
      parser->Stop();
    }
    if (onStart) {
      onStart(name);
    }
  }
  void OnEndElement(std::string_view name) noexcept override {
    FlushText();
    events += ")" + std::string(name) + "\n";
  }
  void OnText(std::string_view piece) noexcept override {
    text_ += piece;
    pieces.emplace_back(piece);
  }
  void OnEntity(std::string_view name) noexcept override {
    FlushText();
    events += "&" + std::string(name) + "\n";
  }

  // Everything so far.
  std::string Events() {
    FlushText();
    return events;
  }

  std::string events;
  std::vector<std::string> pieces;  // Text exactly as it came.
  // Stops the parser when an element of this name begins.
  std::string stopAt;
  XmlParser* parser = nullptr;
  void (*onStart)(std::string_view name) = nullptr;

 private:
  static std::string Escaped(std::string_view raw) {
    std::string out;
    for (const char ch : raw) {
      if (ch == '\n') {
        out += "\\n";
      } else if (ch == '\t') {
        out += "\\t";
      } else if (ch == '\\') {
        out += "\\\\";
      } else {
        out += ch;
      }
    }
    return out;
  }
  void FlushText() {
    if (!text_.empty()) {
      events += "-" + Escaped(text_) + "\n";
      text_.clear();
    }
  }
  std::string text_;
};

// A source that gives out its bytes a few at a time.
class PiecewiseSource final : public bn::core::ByteSource {
 public:
  PiecewiseSource(std::string_view data, std::size_t pieceBytes) : data_(data), pieceBytes_(pieceBytes) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    ++reads;
    if (failAfterBytes >= 0 && position_ >= static_cast<std::size_t>(failAfterBytes)) {
      return kError;
    }
    const std::size_t count = std::min({dst.size(), pieceBytes_, data_.size() - position_});
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(position_), count, dst.begin());
    position_ += count;
    largestAsked = std::max(largestAsked, dst.size());
    return static_cast<std::ptrdiff_t>(count);
  }

  int reads = 0;
  std::size_t largestAsked = 0;
  std::ptrdiff_t failAfterBytes = -1;  // Fails once this many bytes have been given. -1: never.

 private:
  std::string_view data_;
  std::size_t pieceBytes_;
  std::size_t position_ = 0;
};

// A parser with memory of its own.
struct XmlFixture {
  explicit XmlFixture(std::size_t memoryBytes = 32'768) : memory(memoryBytes), parser(memory) {}

  // Parses `document`, read `pieceBytes` at a time, and returns the events.
  std::string Parse(std::string_view document, std::size_t pieceBytes = 4096) {
    recorder = Recorder();
    recorder.parser = &parser;
    PiecewiseSource source(document, pieceBytes);
    status = parser.Parse(source, recorder);
    EXPECT_EQ(status, parser.Status());
    EXPECT_TRUE(parser.Memory().IsConsistent());
    EXPECT_EQ(parser.Memory().MisuseCount(), 0u);
    return recorder.Events();
  }

  std::vector<std::uint8_t> memory;
  XmlParser parser;
  Recorder recorder;
  XmlStatus status = XmlStatus::kNotStarted;
};

TEST(XmlParser, TellsOfElementsAttributesAndText) {
  XmlFixture f;
  const std::string events = f.Parse(R"(<?xml version="1.0" encoding="UTF-8"?>
<package version="3.0" unique-identifier="id">
  <metadata><title>The Book</title></metadata>
  <item id="c1" href="text/chapter%201.xhtml" media-type="application/xhtml+xml"/>
</package>)");
  EXPECT_EQ(f.status, XmlStatus::kOk);
  EXPECT_TRUE(f.parser.Finished());
  EXPECT_EQ(events,
            "(package\n"
            "Aversion 3.0\n"
            "Aunique-identifier id\n"
            "-\\n  \n"
            "(metadata\n"
            "(title\n"
            "-The Book\n"
            ")title\n"
            ")metadata\n"
            "-\\n  \n"
            "(item\n"
            "Aid c1\n"
            "Ahref text/chapter%201.xhtml\n"
            "Amedia-type application/xhtml+xml\n"
            ")item\n"
            "-\\n\n"
            ")package\n");
  EXPECT_EQ(f.parser.ErrorCode(), 0);
  EXPECT_EQ(f.parser.Line(), 5u);
}

TEST(XmlParser, GivesNamesAsTheyAreWrittenPrefixAndAll) {
  XmlFixture f;
  // The epub prefix is never declared, which strict namespace processing
  // would refuse. Books do this.
  EXPECT_EQ(
      f.Parse(R"(<opf:package xmlns:opf="http://www.idpf.org/2007/opf" xmlns:dc="http://purl.org/dc/elements/1.1/">)"
              R"(<dc:title xml:lang="en">T</dc:title><nav epub:type="toc"/></opf:package>)"),
      "(opf:package\n"
      "Axmlns:opf http://www.idpf.org/2007/opf\n"
      "Axmlns:dc http://purl.org/dc/elements/1.1/\n"
      "(dc:title\n"
      "Axml:lang en\n"
      "-T\n"
      ")dc:title\n"
      "(nav\n"
      "Aepub:type toc\n"
      ")nav\n"
      ")opf:package\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);

  EXPECT_EQ(bn::epub::XmlLocalName("dc:title"), "title");
  EXPECT_EQ(bn::epub::XmlLocalName("title"), "title");
  EXPECT_EQ(bn::epub::XmlLocalName("a:b:c"), "c");
  EXPECT_EQ(bn::epub::XmlLocalName("odd:"), "");
  EXPECT_EQ(bn::epub::XmlLocalName(""), "");
}

TEST(XmlAttributes, AreFoundByNumberAndByName) {
  const std::array<const char*, 7> pairs = {"id", "c1", "href", "a b.xhtml", "empty", "", nullptr};
  const XmlAttributes attributes(pairs.data());
  ASSERT_EQ(attributes.Count(), 3u);
  EXPECT_EQ(attributes.Name(0), "id");
  EXPECT_EQ(attributes.Value(0), "c1");
  EXPECT_EQ(attributes.Name(2), "empty");
  EXPECT_EQ(attributes.Value(2), "");
  EXPECT_EQ(attributes.Name(3), "") << "past the end there is nothing";
  EXPECT_EQ(attributes.Value(3), "");

  std::string_view value = "untouched";
  EXPECT_TRUE(attributes.Find("href", value));
  EXPECT_EQ(value, "a b.xhtml");
  EXPECT_TRUE(attributes.Find("empty", value));
  EXPECT_EQ(value, "");
  value = "untouched";
  EXPECT_FALSE(attributes.Find("HREF", value)) << "names are matched exactly";
  EXPECT_FALSE(attributes.Find("hre", value));
  EXPECT_FALSE(attributes.Find("", value));
  EXPECT_EQ(value, "untouched");

  const XmlAttributes none(nullptr);
  EXPECT_EQ(none.Count(), 0u);
  EXPECT_FALSE(none.Find("id", value));
  const std::array<const char*, 1> empty = {nullptr};
  EXPECT_EQ(XmlAttributes(empty.data()).Count(), 0u);
}

TEST(XmlParser, GivesAttributeValuesAsXmlSaysTheyAre) {
  XmlFixture f;
  // A line end or a tab inside a value is a space; references are what
  // they stand for; either kind of quote will do.
  EXPECT_EQ(f.Parse("<a one=\"x\ny\tz\" two='say \"hi\"' three=\"Q&amp;A &#169; &#x2014;\" four=\"\"/>"),
            "(a\n"
            "Aone x y z\n"
            "Atwo say \"hi\"\n"
            "Athree Q&A \xC2\xA9 \xE2\x80\x94\n"
            "Afour \n"
            ")a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, GivesTheBuiltInEntitiesAndNumberedCharactersAsText) {
  XmlFixture f;
  EXPECT_EQ(f.Parse("<p>&lt;b&gt; &amp; &quot;q&quot; &apos;a&apos; &#160;|&#xA0;|&#8212;|&#x1F600;</p>"),
            "(p\n"
            "-<b> & \"q\" 'a' \xC2\xA0|\xC2\xA0|\xE2\x80\x94|\xF0\x9F\x98\x80\n"
            ")p\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, NamesAnEntityItDoesNotExpandWhenTheDocumentNamesADtd) {
  XmlFixture f;
  // The way XHTML in an EPUB 2 book begins.
  const std::string doctype = R"(<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" )"
                              R"("http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">)";
  EXPECT_EQ(f.Parse(doctype + "<html><p>one&nbsp;two&mdash;three &amp; four</p></html>"),
            "(html\n"
            "(p\n"
            "-one\n"
            "&nbsp\n"
            "-two\n"
            "&mdash\n"
            "-three & four\n"
            ")p\n"
            ")html\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);

  // A SYSTEM identifier alone is naming one too.
  EXPECT_EQ(f.Parse(R"(<!DOCTYPE html SYSTEM "about:legacy-compat"><p>&nbsp;</p>)"), "(p\n&nbsp\n)p\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, StopsAtAnEntityItDoesNotKnowWhenTheDocumentNamesNoDtd) {
  XmlFixture f;
  for (const char* start : {"", "<!DOCTYPE html>", "<?xml version=\"1.0\"?>\n"}) {
    const std::string events = f.Parse(std::string(start) + "<html><p>one&nbsp;two</p></html>");
    EXPECT_EQ(f.status, XmlStatus::kNotWellFormed) << start;
    EXPECT_EQ(f.parser.ErrorCode(), kUndefinedEntity) << start;
    EXPECT_FALSE(f.parser.Finished());
    // What came before it was still told.
    EXPECT_EQ(events, "(html\n(p\n-one\n") << start;
  }
}

TEST(XmlParser, DoesNotExpandEntitiesADocumentDeclaresForItself) {
  XmlFixture f;
  // The reference comes out as text, as it was written, whether or not
  // the document names a DTD as well.
  EXPECT_EQ(f.Parse(R"(<!DOCTYPE a [<!ENTITY me "a long piece of text">]><a>it is &me;.</a>)"),
            "(a\n-it is &me;.\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
  EXPECT_EQ(f.Parse(R"(<!DOCTYPE a PUBLIC "x" "y" [<!ENTITY me "text">]><a>&me;&nbsp;</a>)"), "(a\n-&me;\n&nbsp\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);

  // The well-known way to make a small document enormous comes to nothing:
  // nothing is expanded, so there is nothing to grow.
  const std::string events = f.Parse(R"(<!DOCTYPE a [<!ENTITY a "aaaaaaaaaa">)"
                                     R"(<!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">)"
                                     R"(<!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">]><a>&c;&c;&c;</a>)");
  EXPECT_EQ(events, "(a\n-&c;&c;&c;\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, HasNothingToSayOfTheEntitiesThatBelongToDtds) {
  XmlFixture f;
  // Parameter entities, declared and used inside the document's own DTD.
  EXPECT_EQ(f.Parse(R"(<!DOCTYPE a PUBLIC "p" "s" [<!ENTITY % pe "<!ENTITY x 'y'>"> %pe; %other; ]><a>t</a>)"),
            "(a\n-t\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, LeavesOutCommentsInstructionsAndDeclarations) {
  XmlFixture f;
  EXPECT_EQ(f.Parse("<?xml version=\"1.0\"?><!-- before --><?style x?><!DOCTYPE a><a><!-- in -->x<?pi y?>y"
                    "<![CDATA[<raw> & ]]]>z</a><!-- after -->"),
            "(a\n-xy<raw> & ]z\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, ReadsTheEncodingsItKnowsAndGivesUtf8) {
  XmlFixture f;
  const std::string expected = "(a\nAt caf\xC3\xA9\n-na\xC3\xAFve \xE2\x80\x94 \xE6\x96\x87\n)a\n";
  // UTF-8, with and without the mark some editors put in front.
  const std::string utf8 = "<a t=\"caf\xC3\xA9\">na\xC3\xAFve \xE2\x80\x94 \xE6\x96\x87</a>";
  EXPECT_EQ(f.Parse(utf8), expected);
  EXPECT_EQ(f.Parse("\xEF\xBB\xBF" + utf8), expected);
  EXPECT_EQ(f.status, XmlStatus::kOk);

  // UTF-16, either way round, known by its mark.
  const std::u16string wide = u"<a t=\"café\">naïve — 文</a>";
  std::string little = "\xFF\xFE";
  std::string big = "\xFE\xFF";
  for (const char16_t unit : wide) {
    little += static_cast<char>(unit & 0xFFU);
    little += static_cast<char>(unit >> 8U);
    big += static_cast<char>(unit >> 8U);
    big += static_cast<char>(unit & 0xFFU);
  }
  EXPECT_EQ(f.Parse(little), expected);
  EXPECT_EQ(f.status, XmlStatus::kOk);
  EXPECT_EQ(f.Parse(big, 3), expected);
  EXPECT_EQ(f.status, XmlStatus::kOk);

  // ISO-8859-1, by its declaration.
  EXPECT_EQ(f.Parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a t=\"caf\xE9\">na\xEFve</a>"),
            "(a\nAt caf\xC3\xA9\n-na\xC3\xAFve\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, RefusesAnEncodingItDoesNotKnow) {
  XmlFixture f;
  EXPECT_EQ(f.Parse("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a>caf\xE9</a>"), "");
  EXPECT_EQ(f.status, XmlStatus::kUnsupportedEncoding);
  EXPECT_EQ(f.parser.ErrorCode(), kUnknownEncoding);
  EXPECT_FALSE(f.parser.Finished());
}

TEST(XmlParser, SaysWhatIsWrongWithADocumentThatIsNotXmlAndWhere) {
  struct Case {
    const char* document;
    int code;
    std::uint32_t line;
    const char* eventsBefore;
  };
  const std::array<Case, 9> cases = {{
      {"<a>\n<b>text</c>\n</a>", kTagMismatch, 2, "(a\n-\\n\n(b\n-text\n"},
      {"<a><b></b>", kNoElements, 1, "(a\n(b\n)b\n"},
      {"", kNoElements, 1, ""},
      {"   \n  ", kNoElements, 2, ""},
      {"just some words", kSyntax, 1, ""},
      {"<a/>\n\n<b/>", kJunkAfterDocument, 3, "(a\n)a\n"},
      {"<a x=\"1\" x=\"2\"/>", kDuplicateAttribute, 1, ""},
      {"<a>one\n<b two", kUnclosedToken, 2, "(a\n-one\\n\n"},
      {"<a>bad \xC3\x28 bytes</a>", kInvalidToken, 1, "(a\n-bad \n"},
  }};
  XmlFixture f;
  for (const Case& test : cases) {
    const std::string events = f.Parse(test.document);
    EXPECT_EQ(f.status, XmlStatus::kNotWellFormed) << test.document;
    EXPECT_EQ(f.parser.ErrorCode(), test.code) << test.document;
    EXPECT_EQ(f.parser.Line(), test.line) << test.document;
    EXPECT_EQ(events, test.eventsBefore) << test.document;
    EXPECT_FALSE(f.parser.Finished());
  }
  // A mistake is where it is, in bytes too: here, at the name that does
  // not match.
  f.Parse("<a>12345</b>");
  EXPECT_EQ(f.parser.ByteOffset(), 10u);
}

TEST(XmlParser, TellsTheSameOfADocumentHoweverItIsCutUp) {
  const std::string doctype = R"(<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "xhtml11.dtd">)";
  const std::string document =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + doctype +
      "\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>\xE6\x96\x87 &amp; \xC3\xA9</title></head>\n"
      "<body class=\"main text\" id='b'><!-- a comment -->"
      "<p>na\xC3\xAFve&nbsp;\xE2\x80\x94 <i>caf\xC3\xA9</i> &#x1F600; <![CDATA[a < b]]></p>"
      "<img src=\"images/cover%20one.jpg\" alt=\"\"/><?pi here?></body></html>\n";
  XmlFixture f;
  const std::string whole = f.Parse(document, document.size());
  ASSERT_EQ(f.status, XmlStatus::kOk);
  ASSERT_NE(whole.find("&nbsp\n"), std::string::npos);
  for (std::size_t pieceBytes = 1; pieceBytes <= 70; ++pieceBytes) {
    EXPECT_EQ(f.Parse(document, pieceBytes), whole) << pieceBytes;
    EXPECT_EQ(f.status, XmlStatus::kOk) << pieceBytes;
  }
}

// Whether `text` is whole characters of UTF-8 and nothing else.
bool IsWholeUtf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    const std::size_t length = lead < 0x80            ? 1
                               : (lead >> 5U) == 0x6  ? 2
                               : (lead >> 4U) == 0xE  ? 3
                               : (lead >> 3U) == 0x1E ? 4
                                                      : 0;
    if (length == 0 || i + length > text.size()) {
      return false;
    }
    for (std::size_t k = 1; k < length; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) >> 6U) != 0x2) {
        return false;
      }
    }
    i += length;
  }
  return true;
}

TEST(XmlParser, NeverCutsTextInsideACharacter) {
  std::string text;
  for (int i = 0; i < 400; ++i) {
    text += "a\xC3\xA9\xE2\x80\x94\xF0\x9F\x98\x80";  // One, two, three and four bytes long.
  }
  XmlFixture f;
  for (const std::size_t pieceBytes : {1u, 2u, 3u, 5u, 7u, 1024u}) {
    EXPECT_EQ(f.Parse("<p>" + text + "</p>", pieceBytes), "(p\n-" + text + "\n)p\n");
    ASSERT_EQ(f.status, XmlStatus::kOk);
    ASSERT_GT(f.recorder.pieces.size(), 1u) << pieceBytes;
    for (const std::string& piece : f.recorder.pieces) {
      ASSERT_TRUE(IsWholeUtf8(piece)) << pieceBytes;
    }
  }
}

TEST(XmlParser, ReadsADocumentAPieceAtATime) {
  // Some 100 KB of chapter.
  std::string document = "<html><body>";
  for (int i = 0; i < 2'000; ++i) {
    document += "<p class=\"x\">paragraph " + std::to_string(i) + " of the chapter, with words in it</p>\n";
  }
  document += "</body></html>";
  XmlFixture f;
  PiecewiseSource source(document, 100'000);
  Recorder recorder;
  ASSERT_EQ(f.parser.Begin(source, recorder), XmlStatus::kOk);
  EXPECT_EQ(source.reads, 0) << "nothing is read until the first step";
  EXPECT_EQ(f.parser.Status(), XmlStatus::kOk);
  EXPECT_FALSE(f.parser.Finished());

  int steps = 0;
  std::uint32_t lastLine = 1;
  while (f.parser.Step()) {
    ++steps;
    EXPECT_EQ(source.reads, steps) << "one read of the source a step";
    EXPECT_EQ(f.parser.Status(), XmlStatus::kOk);
    EXPECT_FALSE(f.parser.Finished());
    EXPECT_GE(f.parser.Line(), lastLine);
    lastLine = f.parser.Line();
  }
  EXPECT_EQ(f.parser.Status(), XmlStatus::kOk);
  EXPECT_TRUE(f.parser.Finished());
  EXPECT_EQ(source.largestAsked, XmlParser::kChunkBytes);
  EXPECT_EQ(static_cast<std::size_t>(steps), (document.size() + XmlParser::kChunkBytes - 1) / XmlParser::kChunkBytes);
  EXPECT_EQ(f.parser.Line(), 2'001u);
  EXPECT_EQ(f.parser.ByteOffset(), document.size());
  const std::string events = recorder.Events();
  EXPECT_EQ(std::count(events.begin(), events.end(), '('), 2'002);
  EXPECT_NE(events.find("-paragraph 1999 of the chapter, with words in it\n"), std::string::npos);

  // Once it is over, a step does nothing.
  EXPECT_FALSE(f.parser.Step());
  EXPECT_EQ(source.reads, steps + 1);
  // A long document takes no more memory than a short one of the same shape.
  EXPECT_LT(f.parser.Memory().HighWater(), 12'288u);
}

TEST(XmlParser, DoesNothingUntilItIsBegun) {
  XmlFixture f;
  EXPECT_EQ(f.parser.Status(), XmlStatus::kNotStarted);
  EXPECT_FALSE(f.parser.Step());
  f.parser.Stop();
  EXPECT_EQ(f.parser.Status(), XmlStatus::kNotStarted);
  EXPECT_FALSE(f.parser.Finished());
  EXPECT_EQ(f.parser.Memory().Used(), 0u);
}

TEST(XmlParser, CanBeStoppedFromInsideAHandlerOrBetweenSteps) {
  XmlFixture f;
  // From inside: the handler that asked is the last one called.
  f.recorder = Recorder();
  f.recorder.parser = &f.parser;
  f.recorder.stopAt = "spine";
  const std::string_view document = "<package><manifest><item/><item/></manifest><spine><itemref/></spine></package>";
  bn::core::MemoryByteSource source(BytesOf(document));
  EXPECT_EQ(f.parser.Parse(source, f.recorder), XmlStatus::kStopped);
  EXPECT_EQ(f.recorder.Events(), "(package\n(manifest\n(item\n)item\n(item\n)item\n)manifest\n(spine\n");
  EXPECT_FALSE(f.parser.Finished());
  EXPECT_FALSE(f.parser.Step());
  EXPECT_EQ(f.parser.ByteOffset(), document.find("<spine>") + 7) << "just past the tag it stopped at";

  // Between steps.
  std::string longDocument = "<a>";
  for (int i = 0; i < 500; ++i) {
    longDocument += "<b>some text</b>";
  }
  longDocument += "</a>";
  PiecewiseSource pieces(longDocument, 4096);
  Recorder recorder;
  ASSERT_EQ(f.parser.Begin(pieces, recorder), XmlStatus::kOk);
  ASSERT_TRUE(f.parser.Step());
  const std::size_t eventsSoFar = recorder.Events().size();
  f.parser.Stop();
  EXPECT_EQ(f.parser.Status(), XmlStatus::kStopped);
  EXPECT_FALSE(f.parser.Step());
  EXPECT_EQ(pieces.reads, 1);
  EXPECT_EQ(recorder.Events().size(), eventsSoFar);

  // And it is ready for another document afterwards.
  EXPECT_EQ(f.Parse("<a>after</a>"), "(a\n-after\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, ReportsASourceThatFails) {
  XmlFixture f;
  const std::string document = "<a>" + std::string(3'000, 'x') + "</a>";
  PiecewiseSource source(document, 1'000);
  source.failAfterBytes = 2'000;
  Recorder recorder;
  EXPECT_EQ(f.parser.Parse(source, recorder), XmlStatus::kSourceFailed);
  EXPECT_FALSE(f.parser.Finished());
  EXPECT_EQ(source.reads, 3);
  EXPECT_EQ(f.parser.ErrorCode(), 0);

  // From the very first read.
  PiecewiseSource dead(document, 1'000);
  dead.failAfterBytes = 0;
  EXPECT_EQ(f.parser.Parse(dead, recorder), XmlStatus::kSourceFailed);
}

// A source that says it read more than there was room for.
class OverclaimingSource final : public bn::core::ByteSource {
 public:
  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    std::fill(dst.begin(), dst.end(), std::uint8_t{'x'});
    return static_cast<std::ptrdiff_t>(dst.size()) + 1;
  }
};

TEST(XmlParser, DoesNotBelieveASourceThatSaysItReadMoreThanItWasAskedFor) {
  XmlFixture f;
  OverclaimingSource source;
  Recorder recorder;
  EXPECT_EQ(f.parser.Parse(source, recorder), XmlStatus::kSourceFailed);
  EXPECT_EQ(recorder.Events(), "");
  EXPECT_TRUE(f.parser.Memory().IsConsistent());
}

TEST(XmlParser, RefusesADocumentThatNeedsMoreMemoryThanItHas) {
  // Not enough to make a parser at all.
  XmlFixture tiny(512);
  EXPECT_EQ(tiny.Parse("<a/>"), "");
  EXPECT_EQ(tiny.status, XmlStatus::kNoMemory);
  EXPECT_GT(tiny.parser.Memory().FailureCount(), 0u);

  XmlFixture f(16'384);
  // A tag longer than the memory: the whole of a tag has to be held at once.
  const std::string longTag = "<a><img src=\"data:" + std::string(40'000, 'A') + "\"/></a>";
  EXPECT_EQ(f.Parse(longTag, 1024), "(a\n");
  EXPECT_EQ(f.status, XmlStatus::kNoMemory);
  EXPECT_FALSE(f.parser.Finished());
  EXPECT_LE(f.parser.Memory().HighWater(), f.parser.Memory().Capacity());

  // Elements nested deeper than the memory can keep track of.
  std::string deep;
  for (int i = 0; i < 20'000; ++i) {
    deep += "<d>";
  }
  f.Parse(deep, 1024);
  EXPECT_EQ(f.status, XmlStatus::kNoMemory);

  // Text is not held, so any length of it is fine; and after a refusal
  // the parser is as good as new.
  const std::string longText = "<a>" + std::string(200'000, 't') + "</a>";
  EXPECT_EQ(f.Parse(longText, 1024).size(), 200'000u + 8);
  EXPECT_EQ(f.status, XmlStatus::kOk);
  // A long tag that does fit.
  const std::string value(3'000, 'A');
  EXPECT_EQ(f.Parse("<a><img src=\"" + value + "\"/></a>", 1024), "(a\n(img\nAsrc " + value + "\n)img\n)a\n");
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, ReadsOneDocumentAfterAnotherInTheSameMemory) {
  XmlFixture f(16'384);
  std::size_t first = 0;
  for (int i = 0; i < 50; ++i) {
    const std::string name = "e" + std::to_string(i);
    std::string document;
    document.append("<").append(name).append(" a=\"1\"><x/><y>t</y></").append(name).append(">");
    std::string expected;
    expected.append("(").append(name).append("\nAa 1\n(x\n)x\n(y\n-t\n)y\n)").append(name).append("\n");
    EXPECT_EQ(f.Parse(document), expected);
    ASSERT_EQ(f.status, XmlStatus::kOk) << i;
    if (i == 0) {
      first = f.parser.Memory().HighWater();
    }
  }
  // Fifty documents took no more than one: each began with all of it.
  EXPECT_EQ(f.parser.Memory().HighWater(), first);
  EXPECT_GT(first, 1'000u);
  EXPECT_EQ(f.parser.Memory().FailureCount(), 0u);
}

TEST(XmlParser, SaysHowMuchMemoryEachDocumentUsed) {
  XmlFixture f;
  f.Parse("<a>small</a>");
  const std::size_t small = f.parser.Memory().HighWaterSinceReset();
  const std::string value(3'000, 'A');
  f.Parse("<a b=\"" + value + "\"/>");
  const std::size_t large = f.parser.Memory().HighWaterSinceReset();
  EXPECT_GT(large, small + 3'000);
  // The figure is the document's own, not the most there has been.
  f.Parse("<a>small</a>");
  EXPECT_EQ(f.parser.Memory().HighWaterSinceReset(), small);
  EXPECT_EQ(f.parser.Memory().HighWater(), large);
}

TEST(XmlParser, KeepsUpWithManyAttributesLongNamesAndDeepNesting) {
  // More of each than the parser makes room for to begin with, so that it
  // has to make more as it goes.
  std::string document = "<root";
  std::string expected = "(root\n";
  for (int i = 0; i < 60; ++i) {
    const std::string name = "attribute-number-" + std::to_string(i);
    document.append(" ").append(name).append("=\"").append(std::to_string(i)).append("\"");
    expected.append("A").append(name).append(" ").append(std::to_string(i)).append("\n");
  }
  document += '>';
  const std::string longName = "an-element-with-a-name-" + std::string(200, 'n');
  for (int depth = 0; depth < 60; ++depth) {
    document.append("<").append(longName).append(std::to_string(depth)).append(">");
    expected.append("(").append(longName).append(std::to_string(depth)).append("\n");
  }
  document += "deep";
  expected += "-deep\n";
  for (int depth = 59; depth >= 0; --depth) {
    document.append("</").append(longName).append(std::to_string(depth)).append(">");
    expected.append(")").append(longName).append(std::to_string(depth)).append("\n");
  }
  document += "</root>";
  expected += ")root\n";
  // Every different name is kept for as long as the document lasts, so this
  // one needs far more memory than a book's chapter does.
  XmlFixture f(262'144);
  for (const std::size_t pieceBytes : {7u, 1024u}) {
    EXPECT_EQ(f.Parse(document, pieceBytes), expected);
    EXPECT_EQ(f.status, XmlStatus::kOk);
  }
  EXPECT_EQ(f.parser.Memory().FailureCount(), 0u);
}

TEST(XmlParser, GivesItsMemoryBackWhenItIsDestroyed) {
  std::vector<std::uint8_t> memory(16'384);
  {
    XmlParser parser(memory);
    Recorder recorder;
    bn::core::MemoryByteSource source(BytesOf("<a><b>unfinished"));
    ASSERT_EQ(parser.Begin(source, recorder), XmlStatus::kOk);
    ASSERT_TRUE(parser.Step());
    EXPECT_GT(parser.Memory().LivePieces(), 0u);
  }
  // In a build with AddressSanitizer this would fail if any of it were
  // still marked as not to be touched.
  std::fill(memory.begin(), memory.end(), std::uint8_t{0x5A});
  EXPECT_EQ(memory[8'000], 0x5A);
}

// A parser at work inside the handler of another, each with its own
// memory: the inner one must not take from the outer one's, nor leave the
// outer one without its own when it is done.
TEST(XmlParser, CanBeUsedFromInsideTheHandlerOfAnother) {
  static std::string innerEvents;
  static std::size_t innerUsed = 0;
  innerEvents.clear();
  XmlFixture outer;
  outer.recorder = Recorder();
  outer.recorder.onStart = [](std::string_view name) {
    if (name != "include") {
      return;
    }
    XmlFixture inner(16'384);
    innerEvents += inner.Parse("<inner><leaf a=\"b\"/>text</inner>");
    innerUsed = inner.parser.Memory().HighWater();
  };
  bn::core::MemoryByteSource source(BytesOf("<outer><include/><after x=\"y\">more</after><include/></outer>"));
  EXPECT_EQ(outer.parser.Parse(source, outer.recorder), XmlStatus::kOk);
  EXPECT_EQ(outer.recorder.Events(),
            "(outer\n(include\n)include\n(after\nAx y\n-more\n)after\n(include\n)include\n)outer\n");
  EXPECT_EQ(innerEvents, "(inner\n(leaf\nAa b\n)leaf\n-text\n)inner\n(inner\n(leaf\nAa b\n)leaf\n-text\n)inner\n");
  EXPECT_GT(innerUsed, 1'000u);
  EXPECT_TRUE(outer.parser.Memory().IsConsistent());
  EXPECT_EQ(outer.parser.Memory().MisuseCount(), 0u);
}

TEST(XmlParser, TellsTheSameWhateverItsSalt) {
  XmlFixture f;
  const std::string document = "<a one=\"1\" two=\"2\" three=\"3\"><b/><c/><d/><e/><f/><g/></a>";
  const std::string plain = f.Parse(document);
  std::array<std::uint8_t, 16> salt{};
  for (std::size_t i = 0; i < salt.size(); ++i) {
    salt[i] = static_cast<std::uint8_t>(0xA5U ^ (i * 37U));
  }
  f.parser.SetSalt(salt);
  EXPECT_EQ(f.Parse(document), plain);
  EXPECT_EQ(f.status, XmlStatus::kOk);
}

TEST(XmlParser, CountsItsDocumentsAndItsFailures) {
  const auto counter = [](const std::string& name) {
    std::array<char, 2048> line{};
    bn::core::DefaultMetrics().FormatLine(line);
    const std::string text(line.data());
    const std::size_t at = text.find(name + "=");
    EXPECT_NE(at, std::string::npos) << text;
    return at == std::string::npos ? 0UL : std::strtoul(text.c_str() + at + name.size() + 1, nullptr, 10);
  };
  XmlFixture f;
  const unsigned long documents = counter("xml_documents");
  const unsigned long failures = counter("xml_failures");
  f.Parse("<a/>");
  f.Parse("<a>");
  f.Parse("<b/>");
  EXPECT_EQ(counter("xml_documents"), documents + 3);
  EXPECT_EQ(counter("xml_failures"), failures + 1);
  EXPECT_GE(counter("xml_memory"), f.parser.Memory().HighWater());
}

TEST(XmlStatus, EveryStatusHasAName) {
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kNotStarted), "not started");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kNotWellFormed), "not well-formed");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kUnsupportedEncoding), "unsupported encoding");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kNoMemory), "no memory");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kSourceFailed), "source failed");
  EXPECT_STREQ(bn::epub::XmlStatusName(XmlStatus::kStopped), "stopped");
}

// A chapter with something of everything in it, for the two tests below.
std::string SampleChapter() {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"xhtml11.dtd\" [<!ENTITY me \"x\">]>\n"
         "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\"><head><title>T &amp; t</title></head>"
         "<body class='a b' id=\"c\"><!-- note --><p>na\xC3\xAFve&nbsp;\xE2\x80\x94 <i>x</i> &#x1F600;&me;"
         "<![CDATA[a < b]]></p><img src=\"i.jpg\" alt=\"\"/><?pi here?></body></html>\n";
}

// The document is not trusted: whatever is done to it, parsing ends with
// an answer, in bounded memory, and the memory's records stay in order.
// (In a build with AddressSanitizer, a byte read or written outside what
// the parser asked for also ends the test.)
TEST(XmlParser, SurvivesDamageAnywhereInADocument) {
  const std::string good = SampleChapter();
  XmlFixture f(12'288);
  f.Parse(good);
  ASSERT_EQ(f.status, XmlStatus::kOk);
  int refused = 0;
  // Every bit turned over.
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::string damaged = good;
      damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ (1U << bit));
      f.Parse(damaged, 64);
      refused += f.status == XmlStatus::kOk ? 0 : 1;
      ASSERT_TRUE(f.status == XmlStatus::kOk || f.status == XmlStatus::kNotWellFormed ||
                  f.status == XmlStatus::kUnsupportedEncoding)
          << at << " " << bit << " " << bn::epub::XmlStatusName(f.status);
    }
  }
  EXPECT_GT(refused, 1'000);
  // Cut short at every length.
  for (std::size_t length = 0; length < good.size(); ++length) {
    f.Parse(good.substr(0, length), 64);
    ASSERT_NE(f.status, XmlStatus::kNoMemory) << length;
    if (length < good.size() - 1) {
      ASSERT_EQ(f.status, XmlStatus::kNotWellFormed) << length;
    }
  }
  // A byte put in, or taken out, anywhere.
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const char extra : {'<', '&', '"', '\0', '\xFF', ']'}) {
      std::string damaged = good;
      damaged.insert(at, 1, extra);
      f.Parse(damaged, 64);
    }
    std::string shorter = good;
    shorter.erase(at, 1);
    f.Parse(shorter, 64);
  }
  EXPECT_EQ(f.parser.Memory().MisuseCount(), 0u);
}

TEST(XmlParser, SurvivesDocumentsThatWereNeverXml) {
  XmlFixture f(12'288);
  std::mt19937 rng(20261113);  // NOLINT(bugprone-random-generator-seed): the same documents every time.
  const auto below = [&rng](std::uint32_t limit) { return static_cast<std::uint32_t>(rng() % limit); };
  const std::string_view pieces[] = {"<",  ">",        "</", "/>",   "a",    " x=\"", "\"",        "'",
                                     "&",  ";",        "#",  "x41",  "<!--", "-->",   "<![CDATA[", "]]>",
                                     "<?", "?>",       "=",  " ",    "\n",   "b:c",   "<!DOCTYPE", "[",
                                     "]",  "<!ENTITY", "%",  "\xC3", "\xA9", "\xFF"};
  int wellFormed = 0;
  for (int document = 0; document < 4'000; ++document) {
    std::string text;
    const std::uint32_t length = below(60);
    for (std::uint32_t i = 0; i < length; ++i) {
      if (below(8) == 0) {
        text += static_cast<char>(below(256));
      } else {
        text += pieces[below(static_cast<std::uint32_t>(std::size(pieces)))];
      }
    }
    f.Parse(text, 1 + below(40));
    wellFormed += f.status == XmlStatus::kOk ? 1 : 0;
    ASSERT_NE(f.status, XmlStatus::kNotStarted);
    ASSERT_NE(f.status, XmlStatus::kSourceFailed);
    ASSERT_NE(f.status, XmlStatus::kStopped);
  }
  // Hardly any of it is XML, and it does not have to be.
  EXPECT_LT(wellFormed, 400);
  EXPECT_EQ(f.parser.Memory().MisuseCount(), 0u);
}

}  // namespace
