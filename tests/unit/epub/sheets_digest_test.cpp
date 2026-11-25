#include "epub/sheets_digest.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "book_fakes.h"
#include "core/checksum.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/xml.h"

namespace {

using bn::epub::DocumentSheets;
using bn::epub::SheetsDigest;
using bn::test::FakeBook;

std::string Page(const std::string& head) {
  return "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>A page</title>" + head +
         "</head><body><p>Text.</p></body></html>";
}

std::string LinkTo(const std::string& href, const std::string& more = "") {
  return "<link rel='stylesheet' href='" + href + "' " + more + "/>";
}

// The checksum of lines written out by hand.
std::uint32_t ChecksumOf(const std::string& lines) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  return bn::core::Crc32({reinterpret_cast<const std::uint8_t*>(lines.data()), lines.size()});
}

// A book, what finds its documents' sheets, and the digest of what they
// were given.
struct Fixture {
  Fixture() { sheets.BeginBook(); }

  void Find(std::string_view document) {
    (void)sheets.Find(document);
    digest.Add(document, sheets);
  }

  FakeBook book;
  std::vector<std::uint8_t> parserMemory = std::vector<std::uint8_t>(65536);
  bn::epub::XmlParser parser{parserMemory};
  bn::epub::CssReader reader;
  std::vector<std::uint8_t> tableMemory = std::vector<std::uint8_t>(bn::epub::CssRules::kMaxBytes);
  bn::epub::CssRules rules{tableMemory};
  std::vector<char> names = std::vector<char>(DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style = std::vector<std::uint8_t>(DocumentSheets::kStyleBytes);
  DocumentSheets sheets{book, parser, reader, rules, names, style};
  SheetsDigest digest;
};

// A book of three documents: the first with two sheets, one of them
// imported, and rules of its own; the second with things left out; the
// third with nothing.
void PutBook(FakeBook& book) {
  book.Put("text/one.xhtml", Page(LinkTo("../css/book.css") + "<style>p { margin-left: 0 }</style>"));
  book.Put("text/two.xhtml", Page(LinkTo("../css/book.css") + LinkTo("gone.css") +
                                  LinkTo("k.css", "media='amzn-mobi'") + LinkTo("also-gone.css")));
  book.Put("text/three.xhtml", Page(""));
  book.Put("css/book.css", "@import 'fonts.css'; p { text-indent: 1em }");
  book.Put("css/fonts.css", "body { font-family: serif }");
}

TEST(SheetsDigest, TakesEachDocumentAsLinesAndChecksumsThem) {
  Fixture f;
  PutBook(f.book);
  EXPECT_EQ(f.digest.Checksum(), 0U);
  f.Find("text/one.xhtml");
  EXPECT_EQ(f.digest.Checksum(), ChecksumOf("D text/one.xhtml\nS css/fonts.css\nS css/book.css\nS <style>\n"));
  f.Find("text/two.xhtml");
  f.Find("text/three.xhtml");
  // What was left out is a line for each kind, in the order of the kinds,
  // with how many: not in the order they were come to.
  EXPECT_EQ(f.digest.Checksum(), ChecksumOf("D text/one.xhtml\nS css/fonts.css\nS css/book.css\nS <style>\n"
                                            "D text/two.xhtml\nS css/fonts.css\nS css/book.css\n"
                                            "X link for another medium 1\nX link to no file of the book 2\n"
                                            "D text/three.xhtml\n"));
  EXPECT_EQ(f.digest.Documents(), 3U);
  EXPECT_EQ(f.digest.Unopened(), 0U);
  EXPECT_EQ(f.digest.Given(), 5U);
  EXPECT_EQ(f.digest.WithOwnStyle(), 1U);
}

TEST(SheetsDigest, CountsWhatWasLeftOutForEachDocumentAndNotSinceTheBookBegan) {
  Fixture f;
  f.book.Put("a.xhtml", Page(LinkTo("gone.css")));
  f.book.Put("b.xhtml", Page(LinkTo("gone.css") + LinkTo("gone.css")));
  f.book.Put("c.xhtml", Page(""));
  f.Find("a.xhtml");
  f.Find("b.xhtml");
  f.Find("c.xhtml");
  EXPECT_EQ(f.sheets.LeftOut(bn::epub::SheetLeftOut::kLinkNotInBook), 3U);
  EXPECT_EQ(f.digest.Checksum(), ChecksumOf("D a.xhtml\nX link to no file of the book 1\n"
                                            "D b.xhtml\nX link to no file of the book 2\n"
                                            "D c.xhtml\n"));
}

TEST(SheetsDigest, ADocumentThatCouldNotBeOpenedIsItsNameAndALineThatSaysSo) {
  Fixture f;
  PutBook(f.book);
  f.Find("text/one.xhtml");
  f.Find("text/no-such.xhtml");
  f.digest.AddUnopened("text/another.xhtml");
  f.Find("text/three.xhtml");
  EXPECT_EQ(f.digest.Checksum(), ChecksumOf("D text/one.xhtml\nS css/fonts.css\nS css/book.css\nS <style>\n"
                                            "D text/no-such.xhtml\nN\nD text/another.xhtml\nN\nD text/three.xhtml\n"));
  EXPECT_EQ(f.digest.Documents(), 4U);
  EXPECT_EQ(f.digest.Unopened(), 2U);
  EXPECT_EQ(f.digest.Given(), 3U);
}

TEST(SheetsDigest, FindingsThatDifferComeToDifferentChecksums) {
  // The same documents in another order; a sheet more; the same sheets in
  // another order; one thing more left out.
  const auto checksum = [](const std::vector<std::string>& order, const std::string& headOfOne) {
    Fixture f;
    f.book.Put("one.xhtml", Page(headOfOne));
    f.book.Put("two.xhtml", Page(LinkTo("b.css")));
    f.book.Put("a.css", "p { margin-left: 0 }");
    f.book.Put("b.css", "p { margin-left: 0 }");
    for (const std::string& document : order) {
      f.Find(document);
    }
    return f.digest.Checksum();
  };
  const std::string links = LinkTo("a.css") + LinkTo("b.css");
  const std::uint32_t first = checksum({"one.xhtml", "two.xhtml"}, links);
  EXPECT_EQ(checksum({"one.xhtml", "two.xhtml"}, links), first);
  EXPECT_NE(checksum({"two.xhtml", "one.xhtml"}, links), first);
  EXPECT_NE(checksum({"one.xhtml", "two.xhtml"}, LinkTo("a.css")), first);
  EXPECT_NE(checksum({"one.xhtml", "two.xhtml"}, LinkTo("b.css") + LinkTo("a.css")), first);
  EXPECT_NE(checksum({"one.xhtml", "two.xhtml"}, links + LinkTo("c.css")), first);
  EXPECT_NE(checksum({"one.xhtml"}, links), first);
}

TEST(SheetsDigest, StartsAgainFromNothingForAnotherBook) {
  Fixture f;
  f.book.Put("a.xhtml", Page(LinkTo("gone.css") + LinkTo("a.css")));
  f.book.Put("a.css", "p { margin-left: 0 }");
  f.Find("a.xhtml");
  const std::uint32_t once = f.digest.Checksum();
  EXPECT_EQ(once, ChecksumOf("D a.xhtml\nS a.css\nX link to no file of the book 1\n"));

  // The book again: its counts start again, and so does the digest's
  // memory of them.
  f.sheets.BeginBook();
  f.digest.Reset();
  EXPECT_EQ(f.digest.Checksum(), 0U);
  EXPECT_EQ(f.digest.Documents(), 0U);
  EXPECT_EQ(f.digest.Given(), 0U);
  f.Find("a.xhtml");
  EXPECT_EQ(f.digest.Checksum(), once);
}

TEST(SheetsDigest, TakesAPathAsItsBytesWhateverTheyAre) {
  Fixture f;
  const std::string odd = std::string("a\tb\\c d") + '\x01' + "\xc3\xa9.xhtml";
  f.book.Put(odd, Page(""));
  f.Find(odd);
  EXPECT_EQ(f.digest.Checksum(), ChecksumOf("D " + odd + "\n"));
}

}  // namespace
