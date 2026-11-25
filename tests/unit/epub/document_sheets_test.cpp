#include "epub/document_sheets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "book_fakes.h"
#include "core/byte_source.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/xml.h"
#include "sheets_reference.h"

namespace {

using bn::epub::CssRules;
using bn::epub::DocumentSheets;
using bn::epub::SheetLeftOut;
using bn::epub::SheetsStatus;
using bn::test::FakeBook;

// A page of XHTML with that in its head and that in its body.
std::string Page(const std::string& head, const std::string& body = "<p>Text.</p>") {
  return "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml'><head><title>A "
         "page</title>" +
         head + "</head><body>" + body + "</body></html>";
}

std::string LinkTo(const std::string& href, const std::string& more = "") {
  return "<link rel='stylesheet' type='text/css' href='" + href + "'" + (more.empty() ? "" : " " + more) + "/>";
}

// Who is told: what was left out, as "kind: text", and the sheets read.
struct Told final : public bn::epub::SheetsListener {
  void OnSheetRead(std::string_view path, std::uint8_t sheet, const bn::epub::CssReader& reader) noexcept override {
    read.emplace_back(std::string(path) + " as " + std::to_string(sheet) + " with " + std::to_string(reader.Rules()) +
                      " rules");
  }
  void OnLeftOut(SheetLeftOut what, std::string_view text) noexcept override {
    leftOut.emplace_back(std::string(bn::epub::SheetLeftOutName(what)) + ": " + std::string(text));
  }
  std::vector<std::string> read;
  std::vector<std::string> leftOut;
};

struct Fixture {
  explicit Fixture(std::size_t namesBytes = DocumentSheets::kNamesBytes,
                   std::size_t styleBytes = DocumentSheets::kStyleBytes)
      : names(namesBytes), style(styleBytes) {
    sheets.SetListener(&told);
    sheets.BeginBook();
  }

  // The document's list, as the paths of its sheets, with "<style>" for
  // its own.
  std::vector<std::string> Find(std::string_view document) {
    EXPECT_EQ(sheets.Find(document), SheetsStatus::kOk) << document;
    return Listed();
  }
  // The same for a document handed over open: by the book's own one
  // source for whatever is open, as a book in a ZIP file has one reader
  // of entries.
  std::vector<std::string> FindGiven(std::string_view document) {
    bn::core::ByteSource* const source = book.Open(document);
    EXPECT_NE(source, nullptr) << document;
    if (source == nullptr) {
      return {};
    }
    EXPECT_EQ(sheets.Find(document, *source), SheetsStatus::kOk) << document;
    return Listed();
  }
  std::vector<std::string> Listed() const {
    std::vector<std::string> paths;
    const std::span<const std::uint8_t> list = sheets.Sheets();
    for (std::size_t i = 0; i < list.size(); ++i) {
      const std::string_view path = sheets.PathOf(list[i]);
      const bool own = sheets.HasOwnStyle() && i + 1 == list.size();
      EXPECT_EQ(path.empty(), own) << "sheet " << static_cast<int>(list[i]);
      paths.emplace_back(own ? "<style>" : std::string(path));
    }
    return paths;
  }
  std::uint32_t LeftOut(SheetLeftOut what) const { return sheets.LeftOut(what); }
  std::uint32_t LeftOutInAll() const {
    std::uint32_t all = 0;
    for (std::size_t kind = 0; kind < bn::epub::kSheetLeftOutKinds; ++kind) {
      all += sheets.LeftOut(static_cast<SheetLeftOut>(kind));
    }
    return all;
  }

  FakeBook book;
  std::vector<std::uint8_t> parserMemory = std::vector<std::uint8_t>(262144);
  bn::epub::XmlParser parser{parserMemory};
  bn::epub::CssReader reader;
  std::vector<std::uint8_t> tableMemory = std::vector<std::uint8_t>(CssRules::kMaxBytes);
  CssRules rules{tableMemory};
  std::vector<char> names;
  std::vector<std::uint8_t> style;
  Told told;
  DocumentSheets sheets{book, parser, reader, rules, names, style};
};

using Paths = std::vector<std::string>;

TEST(DocumentSheets, GivesADocumentTheSheetsItLinksInTheOrderOfItsLinks) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("b.css") + LinkTo("a.css")));
  f.book.Put("a.css", "p { text-indent: 1em }");
  f.book.Put("b.css", "p { text-indent: 2em } h1 { font-weight: bold }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"b.css", "a.css"}));
  EXPECT_TRUE(f.sheets.Finished());
  EXPECT_EQ(f.rules.Sheets(), 2U);
  EXPECT_EQ(f.rules.Rules(), 3U);
  EXPECT_EQ(f.sheets.SheetsRead(), 2U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
  EXPECT_EQ(f.told.read, (Paths{"b.css as 0 with 2 rules", "a.css as 1 with 1 rules"}));
}

TEST(DocumentSheets, FindsASheetFromTheDocumentsFolderWhetherAManifestListsItOrNot) {
  Fixture f;
  f.book.Put("OEBPS/text/ch1.xhtml", Page(LinkTo("../css/book.css") + LinkTo("ch1.css") + LinkTo("/top.css#x")));
  f.book.Put("OEBPS/css/book.css", "p { margin-left: 0 }");
  f.book.Put("OEBPS/text/ch1.css", "p { margin-left: 0 }");
  f.book.Put("top.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("OEBPS/text/ch1.xhtml"), (Paths{"OEBPS/css/book.css", "OEBPS/text/ch1.css", "top.css"}));
}

TEST(DocumentSheets, ALinkCountsIfItIsAStyleSheetThatIsNotAnAlternateAndIsCss) {
  Fixture f;
  f.book.Put("page.xhtml",
             Page("<link rel='StyleSheet' href='caps.css'/>"
                  "<link rel='alternate stylesheet' href='night.css'/>"
                  "<link rel='stylesheet\talternate' href='night.css'/>"
                  "<link rel='icon' href='icon.css'/>"
                  "<link href='norel.css'/>"
                  "<link rel='stylesheet'/>"
                  "<link rel='stylesheet' type=' TEXT/CSS ' href='typed.css'/>"
                  "<link rel='stylesheet' type='application/vnd.adobe-page-template+xml' href='template.xpgt'/>"
                  "<link rel='  author   stylesheet  ' href='words.css'/>"
                  "<xhtml:link xmlns:xhtml='http://www.w3.org/1999/xhtml' rel='stylesheet' href='prefixed.css'/>"));
  for (const char* name :
       {"caps.css", "night.css", "icon.css", "norel.css", "typed.css", "template.xpgt", "words.css", "prefixed.css"}) {
    f.book.Put(name, "p { margin-left: 0 }");
  }
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"caps.css", "typed.css", "words.css", "prefixed.css"}));
  // What does not count is not left out: it was never asked for.
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, PassesOverALinkForAnotherMediumByTheRuleOfMediaBlocks) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("kindle.css", "media='amzn-mobi'") + LinkTo("all.css", "media='all'") +
                                LinkTo("screen.css", "media='screen'") + LinkTo("others.css", "media='not amzn-mobi'") +
                                LinkTo("newer.css", "media='(min-device-width: 0px)'") +
                                LinkTo("speech.css", "media='speech'") + LinkTo("empty.css", "media=''")));
  for (const char* name :
       {"kindle.css", "all.css", "screen.css", "others.css", "newer.css", "speech.css", "empty.css"}) {
    f.book.Put(name, "p { margin-left: 0 }");
  }
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"all.css", "screen.css", "others.css", "empty.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kLinkMedia), 3U);
  EXPECT_EQ(f.told.leftOut, (Paths{"link for another medium: kindle.css", "link for another medium: newer.css",
                                   "link for another medium: speech.css"}));
  // A sheet that is passed over is not opened.
  EXPECT_EQ(std::count(f.book.opened.begin(), f.book.opened.end(), "kindle.css"), 0);
}

TEST(DocumentSheets, CountsALinkToNoFileOfTheBookForEachDocumentThatHasIt) {
  Fixture f;
  const std::string head = LinkTo("https://example.org/a.css") + LinkTo("gone.css") + LinkTo("here.css") + LinkTo("");
  f.book.Put("one.xhtml", Page(head));
  f.book.Put("two.xhtml", Page(head));
  f.book.Put("here.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("one.xhtml"), (Paths{"here.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kLinkNotInBook), 3U);
  EXPECT_EQ(f.told.leftOut, (Paths{"link to no file of the book: https://example.org/a.css",
                                   "link to no file of the book: ", "link to no file of the book: gone.css"}));
  EXPECT_EQ(f.Find("two.xhtml"), (Paths{"here.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kLinkNotInBook), 6U);
  // The file that is not there was looked for once.
  EXPECT_EQ(std::count(f.book.opened.begin(), f.book.opened.end(), "gone.css"), 1);
}

TEST(DocumentSheets, FollowsTheFirstLinksOfADocumentAndCountsTheRest) {
  Fixture f;
  std::string head;
  Paths first;
  for (int i = 0; i < 11; ++i) {
    const std::string name = "s" + std::to_string(i) + ".css";
    head += LinkTo(name);
    f.book.Put(name, "p { margin-left: 0 }");
    if (i < 8) {
      first.push_back(name);
    }
  }
  f.book.Put("page.xhtml", Page(head));
  EXPECT_EQ(f.Find("page.xhtml"), first);
  EXPECT_EQ(DocumentSheets::kMaxLinks, 8U);
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kLinkTooMany), 3U);
  EXPECT_EQ(f.told.leftOut.back(), "link after the most followed: s10.css");
}

TEST(DocumentSheets, PutsTheSheetsASheetImportsBeforeItAndTheirsBeforeThem) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("css/a.css") + LinkTo("z.css")));
  f.book.Put("css/a.css", "@import 'b.css'; @import url(more/c.css); p { margin-left: 0 }");
  f.book.Put("css/b.css", "@import \"../d.css\"; p { margin-left: 0 }");
  f.book.Put("css/more/c.css", "p { margin-left: 0 }");
  f.book.Put("d.css", "p { margin-left: 0 }");
  f.book.Put("z.css", "p { margin-left: 0 }");
  // An import is found from the folder of the sheet that names it.
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"d.css", "css/b.css", "css/more/c.css", "css/a.css", "z.css"}));
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, AnImportForAnotherMediumIsNotFollowed) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", "@import 'print.css' print; @import 'screen.css' screen; p { margin-left: 0 }");
  f.book.Put("print.css", "p { margin-left: 0 }");
  f.book.Put("screen.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"screen.css", "a.css"}));
}

TEST(DocumentSheets, FollowsImportsSoFarDownFromALinkAndNoFurther) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("s0.css")));
  for (int i = 0; i < 7; ++i) {
    f.book.Put("s" + std::to_string(i) + ".css", "@import 's" + std::to_string(i + 1) + ".css'; p { margin-left: 0 }");
  }
  EXPECT_EQ(DocumentSheets::kMaxDepth, 4U);
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"s4.css", "s3.css", "s2.css", "s1.css", "s0.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportTooDeep), 1U);
  EXPECT_EQ(f.told.leftOut, (Paths{"import too deep: s5.css"}));
  // What is too far down is not read.
  EXPECT_EQ(std::count(f.book.opened.begin(), f.book.opened.end(), "s5.css"), 0);

  // Linked itself, the sheet that was fourth down is the first, and those
  // under it are near enough to be read now.
  f.book.Put("deep.xhtml", Page(LinkTo("s4.css")));
  EXPECT_EQ(f.Find("deep.xhtml"), (Paths{"s6.css", "s5.css", "s4.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportTooDeep), 1U);
  // s6.css imports s7.css, which the book has not.
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportNotInBook), 1U);
}

TEST(DocumentSheets, PassesOverAnImportOfASheetThatIsOnTheWayToIt) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + LinkTo("self.css")));
  f.book.Put("a.css", "@import 'b.css'; p { margin-left: 0 }");
  f.book.Put("b.css", "@import 'a.css'; p { margin-left: 0 }");
  f.book.Put("self.css", "@import 'self.css'; p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"b.css", "a.css", "self.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportLoop), 2U);
  EXPECT_EQ(f.told.leftOut,
            (Paths{"import of a sheet on the way to it: a.css", "import of a sheet on the way to it: self.css"}));
}

TEST(DocumentSheets, CountsAnImportOfNoFileOfTheBookForEachDocumentThatComesToIt) {
  Fixture f;
  f.book.Put("one.xhtml", Page(LinkTo("a.css")));
  f.book.Put("two.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", "@import 'gone.css'; @import url(http://example.org/fonts.css); p { margin-left: 0 }");
  EXPECT_EQ(f.Find("one.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(f.told.leftOut, (Paths{"import of no file of the book: gone.css",
                                   "import of no file of the book: http://example.org/fonts.css"}));
  EXPECT_EQ(f.Find("two.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportNotInBook), 4U);
}

TEST(DocumentSheets, ASheetNamedTwiceStandsWhereItWasNamedLast) {
  Fixture f;
  f.book.Put("links.xhtml", Page(LinkTo("a.css") + LinkTo("b.css") + LinkTo("a.css")));
  f.book.Put("imports.xhtml", Page(LinkTo("c.css") + LinkTo("d.css")));
  f.book.Put("a.css", "p { margin-left: 0 }");
  f.book.Put("b.css", "p { margin-left: 0 }");
  f.book.Put("c.css", "@import 'shared.css'; p { margin-left: 0 }");
  f.book.Put("d.css", "@import 'shared.css'; p { margin-left: 0 }");
  f.book.Put("shared.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("links.xhtml"), (Paths{"b.css", "a.css"}));
  // Before the second sheet that imports it, and so after the first.
  EXPECT_EQ(f.Find("imports.xhtml"), (Paths{"c.css", "shared.css", "d.css"}));
}

TEST(DocumentSheets, ReadsEachSheetOnceForTheWholeBook) {
  Fixture f;
  f.book.Put("one.xhtml", Page(LinkTo("a.css") + LinkTo("b.css")));
  f.book.Put("two.xhtml", Page(LinkTo("b.css") + LinkTo("a.css")));
  f.book.Put("a.css", "@import 'c.css'; p { margin-left: 0 }");
  f.book.Put("b.css", "p { margin-left: 0 }");
  f.book.Put("c.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("one.xhtml"), (Paths{"c.css", "a.css", "b.css"}));
  EXPECT_EQ(f.book.opens, 4U);
  const std::uint32_t rules = f.rules.Rules();
  EXPECT_EQ(f.Find("two.xhtml"), (Paths{"b.css", "c.css", "a.css"}));
  // The second document was opened, and nothing else.
  EXPECT_EQ(f.book.opened, (Paths{"one.xhtml", "a.css", "c.css", "b.css", "two.xhtml"}));
  EXPECT_EQ(f.rules.Rules(), rules);
  EXPECT_EQ(f.rules.Sheets(), 3U);
  EXPECT_EQ(f.sheets.SheetsRead(), 3U);
}

TEST(DocumentSheets, KeepsADocumentsOwnStyleAfterItsLinkedSheetsAndOnlyForThatDocument) {
  Fixture f;
  // The `<style>` stands before the link, and is still read after it.
  f.book.Put("styled.xhtml",
             Page("<style type='text/css'>p { text-indent: 2em } h1 { font-weight: bold }</style>" + LinkTo("a.css")));
  f.book.Put("plain.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", "p { text-indent: 1em }");
  EXPECT_EQ(f.Find("styled.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_TRUE(f.sheets.HasOwnStyle());
  EXPECT_EQ(f.rules.Sheets(), 2U);
  EXPECT_EQ(f.rules.Rules(), 3U);

  EXPECT_EQ(f.Find("plain.xhtml"), (Paths{"a.css"}));
  EXPECT_FALSE(f.sheets.HasOwnStyle());
  EXPECT_EQ(f.rules.Sheets(), 1U);
  EXPECT_EQ(f.rules.Rules(), 1U);

  // And again, the same as the first time.
  EXPECT_EQ(f.Find("styled.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.rules.Rules(), 3U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, TheCascadeGivesWhatTheListsOrderSays) {
  Fixture f;
  f.book.Put("page.xhtml",
             Page(LinkTo("first.css") + LinkTo("second.css") + "<style>p.own { text-indent: 4em }</style>"));
  f.book.Put("first.css", "@import 'imported.css'; p { text-indent: 2em; margin-left: 2em }");
  f.book.Put("imported.css", "p { text-indent: 1em; margin-left: 1em; margin-right: 1em }");
  f.book.Put("second.css", "p { text-indent: 3em } p.own { text-indent: 3.5em }");
  ASSERT_EQ(f.Find("page.xhtml"), (Paths{"imported.css", "first.css", "second.css", "<style>"}));

  std::vector<std::uint8_t> memory(4096);
  bn::epub::CssCascade cascade(f.rules, memory);
  cascade.Begin(f.sheets.Sheets());
  const auto indent = [&](const char* className) {
    const char* const pairs[] = {"class", className, nullptr};
    bn::epub::CssStyle style;
    bn::epub::CssStyle firstLetter;
    cascade.Open("p", bn::epub::XmlAttributes(className == nullptr ? nullptr : pairs), style, firstLetter);
    cascade.Close();
    return std::pair(style[bn::epub::CssProperty::kTextIndent].number,
                     style[bn::epub::CssProperty::kMarginLeft].number);
  };
  // The later sheet over the earlier, the importing over the imported,
  // and the document's own over them all.
  EXPECT_EQ(indent(nullptr), std::pair(3000, 2000));
  EXPECT_EQ(indent("own"), std::pair(4000, 2000));
}

TEST(DocumentSheets, ReadsSeveralStyleElementsAsOneSheetEachEndingWhereItEnds) {
  Fixture f;
  // The first is not closed: its end closes it, as the end of a sheet
  // does, and the second is read as written.
  f.book.Put("page.xhtml", Page("<style>p { text-indent: 1em</style><style>h1 { font-weight: bold }</style>"));
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"<style>"}));
  EXPECT_EQ(f.rules.Sheets(), 1U);
  EXPECT_EQ(f.rules.Rules(), 2U);
}

TEST(DocumentSheets, LeavesOutAStyleThatDoesNotFitAndKeepsThoseThatDo) {
  Fixture f(DocumentSheets::kNamesBytes, 64);
  const std::string longOne = "p { text-indent: 1em } " + std::string(60, ' ');
  f.book.Put("page.xhtml", Page("<style>h1 { font-weight: bold }</style><style>" + longOne +
                                "</style><style>h2 { font-style: italic }</style>"));
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"<style>"}));
  EXPECT_EQ(f.rules.Rules(), 2U);
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kStyleTooLong), 1U);

  // To the byte: one of 63 fits in 64 with the byte that ends it, and one
  // of 64 does not.
  Fixture exact(DocumentSheets::kNamesBytes, 64);
  exact.book.Put("fits.xhtml", Page("<style>" + std::string(63, ' ') + "</style>"));
  exact.book.Put("over.xhtml", Page("<style>" + std::string(64, ' ') + "</style>"));
  EXPECT_EQ(exact.Find("fits.xhtml"), (Paths{"<style>"}));
  EXPECT_EQ(exact.LeftOut(SheetLeftOut::kStyleTooLong), 0U);
  EXPECT_EQ(exact.Find("over.xhtml"), (Paths{}));
  EXPECT_EQ(exact.LeftOut(SheetLeftOut::kStyleTooLong), 1U);
}

TEST(DocumentSheets, PassesOverAStyleOfAnotherTypeOrForAnotherMedium) {
  Fixture f;
  f.book.Put("page.xhtml", Page("<style type='text/x-other'>p { text-indent: 1em }</style>"
                                "<style media='print'>p { text-indent: 2em }</style>"
                                "<style media='screen' type='TEXT/CSS'>p { text-indent: 3em }</style>"
                                "<style></style>"));
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"<style>"}));
  EXPECT_EQ(f.rules.Rules(), 1U);
  EXPECT_EQ(f.told.leftOut, (Paths{"style for another medium: print"}));
}

TEST(DocumentSheets, DoesNotFollowAnImportInsideAStyleAndCountsIt) {
  Fixture f;
  f.book.Put("page.xhtml", Page("<style>@import 'a.css'; p { text-indent: 1em }</style>"));
  f.book.Put("a.css", "p { text-indent: 2em }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"<style>"}));
  EXPECT_EQ(f.told.leftOut, (Paths{"import in a style: a.css"}));
  EXPECT_EQ(f.book.opened, (Paths{"page.xhtml"}));
}

TEST(DocumentSheets, TextInsideACommentIsNotAStyle) {
  Fixture f;
  f.book.Put("page.xhtml", Page("<!--[if gte mso 9]><style>p { text-indent: 1em }</style><![endif]-->"));
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{}));
  EXPECT_EQ(f.rules.Rules(), 0U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, ReadsTheHeadAndNothingOfTheBody) {
  Fixture f;
  // A link and a style in the body are not seen, and a body of 200 KB is
  // not read.
  const std::string body =
      LinkTo("late.css") + "<style>p { text-indent: 1em }</style>" + std::string(200000, 'x') + "<p>The end.</p>";
  f.book.Put("page.xhtml", Page(LinkTo("a.css"), body));
  f.book.Put("a.css", "p { margin-left: 0 }");
  f.book.Put("late.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"a.css"}));
  EXPECT_LT(f.book.bytesRead, 4096U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, GivesADocumentThatIsNotHtmlNoSheets) {
  Fixture f;
  f.book.Put("page.svg",
             "<?xml-stylesheet href='a.css' type='text/css'?><svg xmlns='http://www.w3.org/2000/svg'>"
             "<link rel='stylesheet' href='a.css'/><style>p { text-indent: 1em }</style>" +
                 std::string(50000, ' ') + "</svg>");
  f.book.Put("a.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.svg"), (Paths{}));
  EXPECT_LT(f.book.bytesRead, 2048U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, CountsAHeadThatCannotBeReadThroughAndKeepsWhatCameBeforeIt) {
  Fixture f;
  f.book.Put("broken.xhtml", "<html><head>" + LinkTo("a.css") +
                                 "<meta charset='utf-8'><style>p { text-indent: 1em }"
                                 "</style></head><body/></html>");
  f.book.Put("cut.xhtml", "<html><head>" + LinkTo("a.css") + "<style>p { text-indent: 1em }");
  f.book.Put("a.css", "p { margin-left: 0 }");
  // `<meta>` is not closed, which shows at `</head>`: the style before
  // that was whole, and is kept.
  EXPECT_EQ(f.Find("broken.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.told.leftOut, (Paths{"head not read through: not well-formed"}));
  // A `<style>` that the head ends inside is not one.
  EXPECT_EQ(f.Find("cut.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kHeadNotRead), 2U);

  // A file that fails part way is the same.
  f.book.Put("failing.xhtml", Page(LinkTo("a.css") + std::string(3000, ' ') + LinkTo("b.css")));
  f.book.Put("b.css", "p { margin-left: 0 }");
  f.book.FailAfter("failing.xhtml", 2000);
  EXPECT_EQ(f.Find("failing.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(f.told.leftOut.back(), "head not read through: source failed");
}

TEST(DocumentSheets, AFileThatFailsIsCountedUnlessTheBodyHadBeenToldOf) {
  const std::string page = Page(LinkTo("a.css"));
  const std::size_t body = page.find("<body>") + 6;

  // Read in whole pieces, the body is told of in the piece that has it,
  // and the reading stops there: the file never comes to fail.
  Fixture whole;
  whole.book.Put("page.xhtml", page);
  whole.book.Put("a.css", "p { margin-left: 0 }");
  whole.book.FailAfter("page.xhtml", body + 3);
  EXPECT_EQ(whole.Find("page.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(whole.LeftOutInAll(), 0U);

  // A byte at a time, the parser has not looked again at the tag by the
  // time the file fails, three bytes after it: the head was not read
  // through, as far as anything could tell.
  Fixture bytes;
  bytes.book = whole.book;
  bytes.book.FailAfter("page.xhtml", body + 3);
  bytes.book.pieceBytes = 1;
  static_cast<void>(bytes.Find("page.xhtml"));
  EXPECT_EQ(bytes.told.leftOut.back(), "head not read through: source failed");
  EXPECT_EQ(bytes.LeftOutInAll(), 1U);

  // And a file that fails well after its head is not counted, however its
  // bytes come.
  Fixture later;
  later.book.Put("page.xhtml", Page(LinkTo("a.css"), std::string(2000, 'x')));
  later.book.Put("a.css", "p { margin-left: 0 }");
  later.book.FailAfter("page.xhtml", later.book.Contents("page.xhtml").size() - 20);
  later.book.pieceBytes = 1;
  EXPECT_EQ(later.Find("page.xhtml"), (Paths{"a.css"}));
  EXPECT_EQ(later.LeftOutInAll(), 0U);
}

TEST(DocumentSheets, TakesADocumentItsCallerHasOpenedAndOpensOnlyItsSheets) {
  Fixture f;
  const std::string page = Page(LinkTo("../css/a.css") + "<style>p { text-indent: 1em }</style>");
  f.book.Put("text/page.xhtml", page);
  f.book.Put("css/a.css", "@import 'b.css'; p { margin-left: 0 }");
  f.book.Put("css/b.css", "h1 { margin-left: 0 }");
  // The document is whatever its caller opened: the name says only where
  // its links are from, and is not looked for in the book.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>(page.data()), page.size()});
  EXPECT_EQ(f.sheets.Find("text/not-in-the-book.xhtml", source), SheetsStatus::kOk);
  EXPECT_TRUE(f.sheets.Finished());
  EXPECT_EQ(f.Listed(), (Paths{"css/b.css", "css/a.css", "<style>"}));
  EXPECT_EQ(f.book.opened, (Paths{"css/a.css", "css/b.css"}));
  EXPECT_EQ(f.LeftOutInAll(), 0U);

  // The same list as when it is opened by its path.
  Fixture byPath;
  byPath.book = f.book;
  EXPECT_EQ(byPath.Find("text/page.xhtml"), f.Listed());
  EXPECT_EQ(byPath.rules.Rules(), f.rules.Rules());
}

TEST(DocumentSheets, ADocumentGivenMayComeFromTheReaderItsSheetsAreOpenedWith) {
  // The book has one source for whatever is open, as a ZIP file's book has
  // one reader of entries: the document comes from it, and opening the
  // first sheet takes it away. The head has been read by then, and the
  // long body is never asked for.
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + LinkTo("b.css"), std::string(100'000, 'x')));
  f.book.Put("a.css", "p { margin-left: 0 }");
  f.book.Put("b.css", "h1 { margin-left: 0 }");
  bn::core::ByteSource* const document = f.book.Open("page.xhtml");
  ASSERT_NE(document, nullptr);
  f.sheets.Begin("page.xhtml", *document);
  // The first step reads nothing and opens nothing: the document is open.
  EXPECT_TRUE(f.sheets.Step());
  EXPECT_EQ(f.book.opens, 1U);
  EXPECT_EQ(f.book.bytesRead, 0U);
  while (f.sheets.Step()) {
  }
  EXPECT_EQ(f.Listed(), (Paths{"a.css", "b.css"}));
  EXPECT_EQ(f.book.opened, (Paths{"page.xhtml", "a.css", "b.css"}));
  EXPECT_LT(f.book.bytesRead, 3000U);
  EXPECT_EQ(f.rules.Rules(), 2U);
}

TEST(DocumentSheets, ADocumentGivenWithNoPathOrOneTooLongIsNoDocument) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", "p { margin-left: 0 }");
  bn::core::ByteSource* const document = f.book.Open("page.xhtml");
  ASSERT_NE(document, nullptr);
  EXPECT_EQ(f.sheets.Find("", *document), SheetsStatus::kNoDocument);
  EXPECT_EQ(f.sheets.Find(std::string(bn::epub::kMaxBookPath + 1, 'x'), *document), SheetsStatus::kNoDocument);
  EXPECT_TRUE(f.sheets.Sheets().empty());
  // Nothing of it was read, and no sheet was looked for.
  EXPECT_EQ(f.book.bytesRead, 0U);
  EXPECT_EQ(f.book.opens, 1U);
  // A path as long as a path may be is one.
  f.book.Put(std::string(bn::epub::kMaxBookPath - 6, 'x') + "/a.css", "p { margin-left: 0 }");
  bn::core::ByteSource* const again = f.book.Open("page.xhtml");
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(f.sheets.Find(std::string(bn::epub::kMaxBookPath - 6, 'x') + "/p.htm", *again), SheetsStatus::kOk);
  EXPECT_EQ(f.Listed().size(), 1U);
}

TEST(DocumentSheets, ADocumentGivenIsForgottenWhenAnotherIsBegun) {
  Fixture f;
  const std::string first = Page(LinkTo("a.css"));
  f.book.Put("b.xhtml", Page(LinkTo("b.css")));
  f.book.Put("a.css", "p { margin-left: 0 }");
  f.book.Put("b.css", "h1 { margin-left: 0 }");
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  bn::core::MemoryByteSource source({reinterpret_cast<const std::uint8_t*>(first.data()), first.size()});
  // Begun with a source, and then another document begun by its path
  // before a step was taken: that one is opened, and the source left.
  f.sheets.Begin("a.xhtml", source);
  EXPECT_EQ(f.Find("b.xhtml"), (Paths{"b.css"}));
  EXPECT_EQ(f.book.opened, (Paths{"b.xhtml", "b.css"}));
  // And a book begun leaves nothing to step.
  f.sheets.Begin("a.xhtml", source);
  f.sheets.BeginBook();
  EXPECT_FALSE(f.sheets.Step());
  EXPECT_EQ(f.sheets.Status(), SheetsStatus::kNotStarted);
  // The source is as it was: it can still be given.
  EXPECT_EQ(f.sheets.Find("a.xhtml", source), SheetsStatus::kOk);
  EXPECT_EQ(f.Listed(), (Paths{"a.css"}));
}

TEST(DocumentSheets, SaysWhenTheBookHasNoSuchDocument) {
  Fixture f;
  EXPECT_EQ(f.sheets.Status(), SheetsStatus::kNotStarted);
  EXPECT_FALSE(f.sheets.Step());
  EXPECT_EQ(f.sheets.Find("gone.xhtml"), SheetsStatus::kNoDocument);
  EXPECT_FALSE(f.sheets.Finished());
  EXPECT_TRUE(f.sheets.Sheets().empty());
  EXPECT_EQ(f.sheets.Find(""), SheetsStatus::kNoDocument);
  EXPECT_EQ(f.sheets.Find(std::string(bn::epub::kMaxBookPath + 1, 'a')), SheetsStatus::kNoDocument);
  EXPECT_STREQ(bn::epub::SheetsStatusName(SheetsStatus::kNoDocument), "no document");
  // And the next document is found as ever.
  f.book.Put("page.xhtml", Page(""));
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{}));
}

TEST(DocumentSheets, CountsASheetWhoseFileFailsAndKeepsWhatWasReadOfIt) {
  Fixture f;
  f.book.Put("one.xhtml", Page(LinkTo("a.css")));
  f.book.Put("two.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css",
             "@import 'b.css'; p { text-indent: 1em } " + std::string(2000, ' ') + " h1 { font-weight: bold }");
  f.book.Put("b.css", "p { margin-left: 0 }");
  f.book.FailAfter("a.css", 1500);
  EXPECT_EQ(f.Find("one.xhtml"), (Paths{"b.css", "a.css"}));
  EXPECT_EQ(f.told.leftOut, (Paths{"sheet not read to its end: a.css"}));
  EXPECT_EQ(f.rules.Rules(), 2U);
  // Said once: the sheet is not read again for the next document.
  EXPECT_EQ(f.Find("two.xhtml"), (Paths{"b.css", "a.css"}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kSheetNotRead), 1U);
}

TEST(DocumentSheets, AStepOpensOneFileOrReadsOnePieceAndNeverBoth) {
  Fixture f;
  std::string big = "@import 'b.css'; ";
  for (int i = 0; i < 400; ++i) {
    big += "p.c" + std::to_string(i) + " { text-indent: " + std::to_string(i) + "px }\n";
  }
  f.book.Put("page.xhtml", Page(std::string(5000, ' ') + LinkTo("a.css") + "<style>" + std::string(3000, ' ') +
                                "p { text-indent: 1em }</style>"));
  f.book.Put("a.css", big);
  f.book.Put("b.css", big);
  f.sheets.Begin("page.xhtml");
  int steps = 0;
  bool more = true;
  while (more) {
    const std::size_t opens = f.book.opens;
    const std::uint64_t bytes = f.book.bytesRead;
    more = f.sheets.Step();
    ++steps;
    const std::size_t opened = f.book.opens - opens;
    const std::uint64_t read = f.book.bytesRead - bytes;
    ASSERT_LE(opened, 1U) << "step " << steps;
    ASSERT_LE(read, DocumentSheets::kMostBytesPerStep) << "step " << steps;
    ASSERT_TRUE(opened == 0 || read == 0) << "step " << steps;
    ASSERT_LT(steps, 1000);
  }
  EXPECT_EQ(DocumentSheets::kMostBytesPerStep, 1024U);
  EXPECT_EQ(f.Listed(), (Paths{"b.css", "a.css", "<style>"}));
  // Many steps: the files are 30 KB between them.
  EXPECT_GT(steps, 30);
}

TEST(DocumentSheets, CountsASheetThereIsNoRoomForInTheTableOrAmongTheNames) {
  // Nine documents with eight sheets each: the table holds 64.
  Fixture f(8192);
  for (int document = 0; document < 9; ++document) {
    std::string head;
    for (int sheet = 0; sheet < 8; ++sheet) {
      const std::string name = "s" + std::to_string((document * 8) + sheet) + ".css";
      head += LinkTo(name);
      f.book.Put(name, "p { margin-left: 0 }");
    }
    f.book.Put("d" + std::to_string(document) + ".xhtml", Page(head));
  }
  for (int document = 0; document < 8; ++document) {
    EXPECT_EQ(f.Find("d" + std::to_string(document) + ".xhtml").size(), 8U);
  }
  EXPECT_EQ(f.LeftOutInAll(), 0U);
  EXPECT_EQ(f.rules.Sheets(), 64U);
  EXPECT_EQ(f.Find("d8.xhtml"), (Paths{}));
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kSheetTooMany), 8U);
  EXPECT_EQ(f.told.leftOut.back(), "sheet with no room: s71.css");
  // And a document's own rules, with the table full.
  f.book.Put("own.xhtml", Page("<style>p { text-indent: 1em }</style>"));
  EXPECT_EQ(f.Find("own.xhtml"), (Paths{}));
  EXPECT_EQ(f.told.leftOut.back(), "sheet with no room: style");

  // Room for the names of two sheets and no more.
  Fixture small(10);
  small.book.Put("page.xhtml", Page(LinkTo("a.css") + LinkTo("b.css") + LinkTo("c.css") + LinkTo("a.css")));
  for (const char* name : {"a.css", "b.css", "c.css"}) {
    small.book.Put(name, "p { margin-left: 0 }");
  }
  EXPECT_EQ(small.Find("page.xhtml"), (Paths{"b.css", "a.css"}));
  EXPECT_EQ(small.told.leftOut, (Paths{"sheet with no room: c.css"}));
  EXPECT_EQ(small.sheets.NamesBytes(), 10U);
}

TEST(DocumentSheets, RemembersSoManyImportsInABookAndCountsTheRest) {
  Fixture f(8192);
  std::string imports;
  for (int i = 0; i < 70; ++i) {
    imports += "@import 'i" + std::to_string(i % 35) + ".css';\n";
  }
  f.book.Put("page.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", imports);
  for (int i = 0; i < 35; ++i) {
    f.book.Put("i" + std::to_string(i) + ".css", "p { margin-left: 0 }");
  }
  EXPECT_EQ(DocumentSheets::kMaxImports, 64U);
  // The 35 it imports, and itself; the last six of its 70 imports are of
  // sheets it had imported already.
  EXPECT_EQ(f.Find("page.xhtml").size(), 36U);
  EXPECT_EQ(f.LeftOut(SheetLeftOut::kImportTooMany), 6U);
}

TEST(DocumentSheets, ADocumentBegunPartWayThroughAnotherLeavesTheTableWhole) {
  Fixture f;
  std::string big;
  for (int i = 0; i < 120; ++i) {
    big += "p.c" + std::to_string(i) + " { text-indent: " + std::to_string(i) + "px }\n";
  }
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + "<style>" + big + "</style>"));
  f.book.Put("a.css", "@import 'b.css';\n" + big);
  f.book.Put("b.css", "p { margin-left: 0 }");
  f.book.Put("other.xhtml", Page(""));
  // Given up after every number of steps there is: the next document
  // finds the table without the sheet that was half read, and the first
  // document found again comes to the same as it does when left alone.
  Fixture whole;
  whole.book = f.book;
  const Paths expected = whole.Find("page.xhtml");
  ASSERT_EQ(expected, (Paths{"b.css", "a.css", "<style>"}));
  for (int stop = 0; stop < 60; ++stop) {
    f.sheets.BeginBook();
    f.sheets.Begin("page.xhtml");
    for (int step = 0; step < stop; ++step) {
      static_cast<void>(f.sheets.Step());
    }
    ASSERT_EQ(f.Find("other.xhtml"), (Paths{})) << stop;
    const std::uint32_t rulesBetween = f.rules.Rules();
    ASSERT_EQ(f.rules.Sheets(), f.sheets.SheetsRead()) << stop;
    ASSERT_EQ(f.Find("page.xhtml"), expected) << stop;
    ASSERT_EQ(f.rules.Rules(), whole.rules.Rules()) << stop;
    ASSERT_EQ(f.rules.Bytes(), whole.rules.Bytes()) << stop;
    ASSERT_LE(rulesBetween, whole.rules.Rules()) << stop;
  }
}

TEST(DocumentSheets, ABookBegunForgetsEverySheetAndEveryCount) {
  Fixture f;
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + LinkTo("gone.css") + "<style>p { text-indent: 1em }</style>"));
  f.book.Put("a.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.LeftOutInAll(), 1U);
  f.sheets.BeginBook();
  EXPECT_EQ(f.rules.Sheets(), 0U);
  EXPECT_EQ(f.sheets.SheetsRead(), 0U);
  EXPECT_EQ(f.sheets.NamesBytes(), 0U);
  EXPECT_EQ(f.LeftOutInAll(), 0U);
  EXPECT_TRUE(f.sheets.Sheets().empty());
  f.book.opened.clear();
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.book.opened, (Paths{"page.xhtml", "a.css", "gone.css"}));
}

TEST(DocumentSheets, ABookBegunPartWayThroughADocumentStartsClean) {
  Fixture f;
  std::string big;
  for (int i = 0; i < 120; ++i) {
    big += "p.c" + std::to_string(i) + " { text-indent: " + std::to_string(i) + "px }\n";
  }
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + "<style>" + big + "</style>"));
  f.book.Put("a.css", big);
  Fixture whole;
  whole.book = f.book;
  const Paths expected = whole.Find("page.xhtml");
  // After every number of steps there is: in the head, in the sheet, and
  // in the document's own rules.
  for (int stop = 0; stop < 40; ++stop) {
    f.sheets.Begin("page.xhtml");
    for (int step = 0; step < stop; ++step) {
      static_cast<void>(f.sheets.Step());
    }
    f.sheets.BeginBook();
    ASSERT_EQ(f.rules.Sheets(), 0U) << stop;
    ASSERT_EQ(f.rules.Rules(), 0U) << stop;
    ASSERT_EQ(f.Find("page.xhtml"), expected) << stop;
    ASSERT_EQ(f.rules.Rules(), whole.rules.Rules()) << stop;
    ASSERT_EQ(f.rules.Bytes(), whole.rules.Bytes()) << stop;
  }
}

TEST(DocumentSheets, CountsAHeadItHasNoMemoryToParse) {
  // A parser with no room to begin: the document is there, and nothing of
  // its head could be read.
  FakeBook book;
  book.Put("page.xhtml", Page(LinkTo("a.css")));
  book.Put("a.css", "p { margin-left: 0 }");
  std::vector<std::uint8_t> none(16);
  bn::epub::XmlParser parser(none);
  bn::epub::CssReader reader;
  std::vector<std::uint8_t> tableMemory(4096);
  CssRules rules(tableMemory);
  std::vector<char> names(DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style(DocumentSheets::kStyleBytes);
  DocumentSheets sheets(book, parser, reader, rules, names, style);
  Told told;
  sheets.SetListener(&told);
  sheets.BeginBook();
  EXPECT_EQ(sheets.Find("page.xhtml"), SheetsStatus::kOk);
  EXPECT_TRUE(sheets.Sheets().empty());
  EXPECT_EQ(told.leftOut, (Paths{"head not read through: no memory"}));
}

TEST(DocumentSheets, AnEntityInTheHeadIsPassedOver) {
  Fixture f;
  f.book.Put("page.xhtml",
             "<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd'>"
             "<html><head><title>A&nbsp;page</title>" +
                 LinkTo("a.css") +
                 "<style>p { text-indent:&nbsp;1em } h1 { font-weight: bold }</style>"
                 "</head><body/></html>");
  f.book.Put("a.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.LeftOutInAll(), 0U);
  // The entity is not text: the style's text is what stood around it, two
  // rules beside the sheet's one.
  EXPECT_EQ(f.rules.Rules(), 3U);
}

TEST(DocumentSheets, TakesTheTextOfAnElementInsideAStyleWithTheRest) {
  Fixture f;
  f.book.Put("page.xhtml",
             Page("<style><b>p { text-indent: 1em }</b> h1 { font-weight: bold }<i/></style>" + LinkTo("a.css")));
  f.book.Put("a.css", "p { margin-left: 0 }");
  // The link after it is still a link: the style ended where it ended.
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"a.css", "<style>"}));
  EXPECT_EQ(f.rules.Rules(), 3U);
}

TEST(DocumentSheets, CountsAnImportWhoseNameThereIsNoRoomFor) {
  Fixture f(12);
  f.book.Put("page.xhtml", Page(LinkTo("a.css")));
  f.book.Put("a.css", "@import 'a-longer-name.css'; @import 'b.css'; p { margin-left: 0 }");
  f.book.Put("a-longer-name.css", "p { margin-left: 0 }");
  f.book.Put("b.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{"b.css", "a.css"}));
  EXPECT_EQ(f.told.leftOut, (Paths{"sheet with no room: a-longer-name.css"}));
  EXPECT_EQ(f.sheets.NamesBytes(), 10U);
}

TEST(DocumentSheets, LeavesOutEverySheetWhenTheTableWasFilledBehindItsBack) {
  // The table is its own to add to. If something else has filled it, the
  // sheets a document asks for are left out, counted, and not opened.
  Fixture f;
  for (std::size_t i = 0; i < CssRules::kMaxSheets; ++i) {
    std::uint8_t number = 0;
    ASSERT_TRUE(f.rules.BeginSheet(number));
    f.rules.EndSheet();
  }
  f.book.Put("page.xhtml", Page(LinkTo("a.css") + "<style>p { text-indent: 1em }</style>"));
  f.book.Put("a.css", "p { margin-left: 0 }");
  EXPECT_EQ(f.Find("page.xhtml"), (Paths{}));
  EXPECT_EQ(f.told.leftOut, (Paths{"sheet with no room: a.css", "sheet with no room: style"}));
  EXPECT_EQ(f.book.opened, (Paths{"page.xhtml"}));
}

TEST(DocumentSheets, NamesEveryKindOfThingItLeavesOut) {
  EXPECT_STREQ(bn::epub::SheetsStatusName(SheetsStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::SheetsStatusName(SheetsStatus::kNotStarted), "not started");
  std::vector<std::string> names;
  for (std::size_t kind = 0; kind < bn::epub::kSheetLeftOutKinds; ++kind) {
    names.emplace_back(bn::epub::SheetLeftOutName(static_cast<SheetLeftOut>(kind)));
    EXPECT_NE(names.back(), "?");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

// ---- Books made at random, beside the plain working-out ----

std::string RandomHead(std::mt19937& random, int sheets) {
  const auto pick = [&](int below) { return static_cast<int>(random() % static_cast<unsigned>(below)); };
  std::string head;
  const int things = pick(7);
  for (int i = 0; i < things; ++i) {
    const int kind = pick(10);
    if (kind < 6) {
      static const char* const kRels[] = {"stylesheet", "stylesheet",           "stylesheet",
                                          "Stylesheet", "alternate stylesheet", "icon"};
      static const char* const kMore[] = {"",
                                          "",
                                          "",
                                          "type='text/css'",
                                          "media='screen'",
                                          "media='print'",
                                          "type='text/plain'",
                                          "media='(min-width: 1px)'"};
      const std::string href =
          pick(8) == 0 ? "http://example.org/x.css" : "s" + std::to_string(pick(sheets + 2)) + ".css";
      head += std::string("<link rel='") + kRels[pick(6)] + "' href='" + href + "' " + kMore[pick(8)] + "/>";
    } else if (kind < 8) {
      static const char* const kStyles[] = {"p { text-indent: 1em }", "", "@import 's0.css'; h1 { font-weight: bold }",
                                            "p { margin: 0", "h2 { font-style: italic } p { text-indent: 0 }"};
      head += std::string("<style") + (pick(5) == 0 ? " media='print'" : "") + ">" + kStyles[pick(5)] +
              std::string(static_cast<std::size_t>(pick(3) == 0 ? pick(200) : 0), ' ') + "</style>";
    } else if (kind == 8) {
      head += "<meta name='x' content='y'/>";
    } else {
      head += "<!-- a comment <style>p { margin: 0 }</style> -->";
    }
  }
  return head;
}

TEST(DocumentSheets, AgreesWithThePlainWorkingOutOnBooksMadeAtRandom) {
  std::mt19937 random(20261124);  // NOLINT(bugprone-random-generator-seed): the same run every time.
  const auto pick = [&](int below) { return static_cast<int>(random() % static_cast<unsigned>(below)); };
  std::uint32_t leftOut = 0;
  std::uint32_t ownRules = 0;
  std::size_t listed = 0;
  for (int book = 0; book < 300; ++book) {
    Fixture f(DocumentSheets::kNamesBytes, 160);
    f.book.pieceBytes = pick(3) == 0 ? static_cast<std::size_t>(1 + pick(700)) : FakeBook::kNever;
    const int sheets = 1 + pick(9);
    for (int sheet = 0; sheet < sheets; ++sheet) {
      std::string text;
      for (int imports = pick(4) == 0 ? 1 + pick(3) : 0; imports > 0; --imports) {
        text += pick(10) == 0
                    ? "@import 'http://example.org/y.css';"
                    : "@import 's" + std::to_string(pick(sheets + 1)) + ".css'" + (pick(6) == 0 ? " print" : "") + ";";
      }
      text += "p.s" + std::to_string(sheet) + " { text-indent: " + std::to_string(sheet) + "em }";
      f.book.Put("s" + std::to_string(sheet) + ".css", text);
      if (pick(12) == 0) {
        f.book.FailAfter("s" + std::to_string(sheet) + ".css", static_cast<std::size_t>(pick(40)));
      }
    }
    const int documents = 1 + pick(5);
    for (int document = 0; document < documents; ++document) {
      std::string page = Page(RandomHead(random, sheets));
      if (pick(10) == 0) {
        page.resize(static_cast<std::size_t>(pick(static_cast<int>(page.size()))));
      }
      f.book.Put("d" + std::to_string(document) + ".xhtml", page);
    }
    bn::test::SheetsReference reference(f.book, 160);
    for (int turn = 0; turn < documents + 2; ++turn) {
      const std::string name = "d" + std::to_string(pick(documents)) + ".xhtml";
      std::array<std::uint32_t, bn::epub::kSheetLeftOutKinds> before{};
      for (std::size_t kind = 0; kind < before.size(); ++kind) {
        before[kind] = f.sheets.LeftOut(static_cast<SheetLeftOut>(kind));
      }
      const bn::test::SheetsExpected expected = reference.For(name);
      ASSERT_TRUE(expected.document);
      // Every other one is handed over open, which comes to the same.
      ASSERT_EQ(turn % 2 == 0 ? f.Find(name) : f.FindGiven(name), expected.sheets)
          << "book " << book << ", " << name << ":\n"
          << f.book.Contents(name);
      for (std::size_t kind = 0; kind < before.size(); ++kind) {
        ASSERT_EQ(f.sheets.LeftOut(static_cast<SheetLeftOut>(kind)) - before[kind], expected.leftOut[kind])
            << "book " << book << ", " << name << ", " << bn::epub::SheetLeftOutName(static_cast<SheetLeftOut>(kind))
            << ":\n"
            << f.book.Contents(name);
        leftOut += expected.leftOut[kind];
      }
      listed += expected.sheets.size();
      // Its own rules, which beginning a document takes out again: those
      // of its `<style>` texts, each read to its own end.
      ASSERT_EQ(f.rules.LeftOutForRoom() + f.rules.LeftOutForNodes(), 0U);
      const std::uint32_t with = f.rules.Rules();
      f.sheets.Begin(name);
      ASSERT_EQ(with - f.rules.Rules(), bn::test::SheetsReference::OwnRules(expected))
          << "book " << book << ", " << name << ":\n"
          << f.book.Contents(name);
      ownRules += with - f.rules.Rules();
    }
    // Nothing here is so large as to want room there is not.
    ASSERT_EQ(f.LeftOut(SheetLeftOut::kSheetTooMany), 0U);
    ASSERT_EQ(f.LeftOut(SheetLeftOut::kImportTooMany), 0U);
  }
  // The books were worth making: sheets were given, and things left out.
  EXPECT_GT(listed, 500U);
  EXPECT_GT(leftOut, 300U);
  EXPECT_GT(ownRules, 100U);
}

}  // namespace
