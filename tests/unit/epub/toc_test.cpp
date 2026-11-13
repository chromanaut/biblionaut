#include "epub/toc.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/inflate.h"
#include "epub/package.h"
#include "epub/toc_digest.h"
#include "storage_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::Package;
using bn::epub::PackageStatus;
using bn::epub::TocEntry;
using bn::epub::TocKind;
using bn::epub::TocReader;
using bn::epub::TocStatus;
using bn::test::FakeStorage;
using bn::test::ZipBuilder;
using Bytes = std::vector<std::uint8_t>;
using Lines = std::vector<std::string>;

// The files of a book, in the order they are to go into the ZIP file:
// (path, contents).
using Files = std::vector<std::pair<std::string, std::string>>;

Bytes BytesOf(const std::string& text) { return {text.begin(), text.end()}; }

// What a book of these documents has in it. `spine` is the documents in
// reading order, as paths from the folder the package is in (OEBPS). The
// NCX and the nav, each left out if it is empty, go at the paths given,
// which are from that folder too.
struct BookPlan {
  std::vector<std::string> spine = {"a.xhtml", "b.xhtml", "c.xhtml"};
  // The two below are given an initializer they do not need, because
  // without one the compiler warns of every `{.ncx = ...}` that leaves the
  // other out.
  // NOLINTBEGIN(readability-redundant-member-init)
  std::string ncx{};
  std::string nav{};
  // NOLINTEND(readability-redundant-member-init)
  std::string ncxPath = "toc.ncx";
  std::string navPath = "nav.xhtml";
  bool navInSpine = false;
  // Files the manifest lists that are no part of the spine.
  std::vector<std::string> others = {"style.css"};
};

Files BookOf(const BookPlan& plan) {
  std::string manifest;
  std::string spine;
  Files files = {{"META-INF/container.xml",
                  "<container><rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"},
                 {"OEBPS/content.opf", ""}};
  std::vector<std::string> placed;
  int number = 0;
  for (const std::string& document : plan.spine) {
    // A document that is in the spine twice is one file, listed once.
    std::size_t item = 0;
    while (item < placed.size() && placed[item] != document) {
      ++item;
    }
    if (item == placed.size()) {
      placed.push_back(document);
      manifest +=
          "<item id=\"d" + std::to_string(item) + "\" href=\"" + document + "\" media-type=\"application/xhtml+xml\"/>";
      files.emplace_back("OEBPS/" + document, "<html>" + std::to_string(number++) + "</html>");
    }
    spine += "<itemref idref=\"d" + std::to_string(item) + "\"/>";
  }
  for (const std::string& other : plan.others) {
    manifest += "<item id=\"o" + std::to_string(number++) + "\" href=\"" + other + "\" media-type=\"text/css\"/>";
    files.emplace_back("OEBPS/" + other, "other");
  }
  if (!plan.ncx.empty()) {
    manifest += "<item id=\"ncx\" href=\"" + plan.ncxPath + "\" media-type=\"application/x-dtbncx+xml\"/>";
    files.emplace_back("OEBPS/" + plan.ncxPath, plan.ncx);
  }
  if (!plan.nav.empty()) {
    manifest +=
        "<item id=\"nav\" href=\"" + plan.navPath + "\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>";
    files.emplace_back("OEBPS/" + plan.navPath, plan.nav);
    if (plan.navInSpine) {
      spine += "<itemref idref=\"nav\"/>";
    }
  }
  files[1].second = "<package><metadata><title>T</title></metadata><manifest>" + manifest +
                    "</manifest><spine toc=\"ncx\">" + spine + "</spine></package>";
  return files;
}

// An NCX with this in its navMap.
std::string Ncx(const std::string& navMap) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
         "<head><meta name=\"dtb:uid\" content=\"x\"/></head>\n"
         "<docTitle><text>The Title, Which Is No Entry</text></docTitle>\n"
         "<navMap>\n" +
         navMap + "</navMap>\n</ncx>\n";
}

// One navPoint: its label, where it points, and the navPoints under it.
std::string Point(const std::string& label, const std::string& src, const std::string& under = "") {
  return "<navPoint id=\"p\" playOrder=\"1\"><navLabel><text>" + label + "</text></navLabel><content src=\"" + src +
         "\"/>" + under + "</navPoint>\n";
}

// A nav document with this in its body.
std::string NavDocument(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
         "<head><title>Contents</title></head>\n<body>\n" +
         body + "</body>\n</html>\n";
}

// The nav that is the table of contents, with this list in it.
std::string TocNav(const std::string& list) {
  return "<nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1>\n" + list + "</nav>\n";
}

// Keeps what it is told, an entry a line: how far in, the document, the
// place within it, and the label, with "-" for no document and for no
// place, and "(cut)" after a label that was cut.
class Collector final : public bn::epub::TocHandler {
 public:
  bool OnEntry(const TocEntry& entry) noexcept override {
    lines.push_back(std::to_string(entry.depth) + " " +
                    (entry.document == Package::kNotInSpine ? "-" : std::to_string(entry.document)) + " " +
                    (entry.fragment.empty() ? "-" : std::string(entry.fragment)) + " " + std::string(entry.label) +
                    (entry.labelCut ? " (cut)" : ""));
    return stopAfter == 0 || lines.size() < stopAfter;
  }
  Lines lines;
  std::size_t stopAfter = 0;  // Asks for no more once it has this many; 0: never.
};

// Everything needed to read the table of contents of a book built for
// the test.
struct TocFixture {
  TocFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Makes a ZIP file of `files`, every one compressed, and reads its
  // package.
  PackageStatus Open(const Files& files) {
    ZipBuilder builder;
    for (const auto& [path, contents] : files) {
      builder.Add(path, BytesOf(contents), {.deflate = true});
    }
    return OpenZip(builder.Finish());
  }

  PackageStatus OpenZip(const Bytes& zip) {
    directory.Close();
    entries.Close();
    storage.Put("/book.epub", zip);
    EXPECT_EQ(directory.Open(storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(entries.Open(storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(archive.Open(directory), bn::epub::ZipStatus::kOk);
    return package.Read(archive, entries, reader, xml);
  }

  // Reads a table of contents of the book that is open, into `collector`.
  TocStatus Read(TocKind kind = TocKind::kBest) {
    collector.lines.clear();
    const TocStatus status = toc.Read(package, entries, reader, xml, collector, kind);
    EXPECT_EQ(status, toc.Status());
    EXPECT_EQ(toc.Entries(), collector.lines.size());
    return status;
  }

  // Opens a book and reads its table of contents: the entries, or one
  // line that says how the reading failed.
  Lines EntriesOf(const BookPlan& plan, TocKind kind = TocKind::kBest) {
    EXPECT_EQ(Open(BookOf(plan)), PackageStatus::kOk);
    const TocStatus status = Read(kind);
    if (status != TocStatus::kOk) {
      return {std::string("(") + bn::epub::TocStatusName(status) + ")"};
    }
    return collector.lines;
  }

  FakeStorage storage;
  std::array<std::uint8_t, 4096> directoryBuffer{};
  std::array<std::uint8_t, 4096> entriesBuffer{};
  bn::core::BlockFile directory{directoryBuffer};
  bn::core::BlockFile entries{entriesBuffer};
  bn::epub::ZipArchive archive;
  std::vector<std::uint8_t> window = std::vector<std::uint8_t>(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader{window};
  std::vector<std::uint8_t> xmlMemory = std::vector<std::uint8_t>(32'768);
  bn::epub::XmlParser xml{xmlMemory};
  std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(16'384);
  Package package{memory};
  TocReader toc;
  Collector collector;
};

TEST(TocReader, ReadsAnNcxEntryByEntry) {
  TocFixture f;
  const std::string ncx = Ncx(Point("Beginning", "a.xhtml") +
                              Point("\n   Part  One\n", "b.xhtml",
                                    Point("Chapter 1", "b.xhtml#ch1", Point("A Section", "b.xhtml#ch1-s1")) +
                                        Point("Chapter 2", "b.xhtml#ch2")) +
                              Point("End &amp; After", "c.xhtml#end"));
  EXPECT_EQ(f.EntriesOf({.ncx = ncx}), (Lines{"0 0 - Beginning", "0 1 - Part One", "1 1 ch1 Chapter 1",
                                              "2 1 ch1-s1 A Section", "1 1 ch2 Chapter 2", "0 2 end End & After"}));
  EXPECT_EQ(f.toc.Kind(), TocKind::kNcx);
  EXPECT_FALSE(f.toc.CutShort());
}

TEST(TocReader, TakesFromAnNcxOnlyThePointsOfItsMap) {
  TocFixture f;
  // Besides the map: a title, a list of pages and a list of pictures,
  // each with labels and targets of its own. In the map: a note about it,
  // and points with more than one label or target, or with none.
  const std::string ncx =
      "<ncx><docTitle><text>Title</text></docTitle>"
      "<navPoint><navLabel><text>Before the map</text></navLabel><content src=\"a.xhtml\"/></navPoint>"
      "<navMap><navInfo><text>About this map</text></navInfo>" +
      Point("One", "a.xhtml") +
      "<navPoint><navLabel><text>Two</text></navLabel><navLabel><text>Deux</text></navLabel>"
      "<content src=\"b.xhtml\"/><content src=\"c.xhtml\"/></navPoint>"
      "<navPoint><content src=\"c.xhtml#x\"/></navPoint>"
      "<navPoint><navLabel><text>No target</text></navLabel></navPoint>"
      "<navPoint><navLabel><text>No src</text></navLabel><content/><content src=\"c.xhtml\"/></navPoint>"
      "</navMap>"
      "<pageList><pageTarget type=\"normal\"><navLabel><text>1</text></navLabel><content src=\"a.xhtml#p1\"/>"
      "</pageTarget></pageList>"
      "<navList><navTarget><navLabel><text>Figure</text></navLabel><content src=\"b.xhtml#f\"/></navTarget></navList>"
      "<navMap>" +
      Point("A second map", "a.xhtml") + "</navMap></ncx>";
  EXPECT_EQ(f.EntriesOf({.ncx = ncx}), (Lines{"0 0 - One", "0 1 - Two", "0 2 x ", "0 - - No target", "0 2 - No src"}));
}

TEST(TocReader, WorksOutWhatEachEntryPointsAt) {
  TocFixture f;
  BookPlan plan;
  plan.spine = {"text/a.xhtml", "text/b c.xhtml", "text/a.xhtml", "top.xhtml"};
  plan.ncxPath = "nav/toc.ncx";
  const std::string longId(TocReader::kMaxFragment, 'i');
  plan.ncx = Ncx(Point("Up and over", "../text/a.xhtml") +                      //
                 Point("With a space, escaped", "../text/b%20c.xhtml#s%201") +  //
                 Point("With a dot", "./../top.xhtml?x=1#t") +                  //
                 Point("From the top", "/OEBPS/top.xhtml") +                    //
                 Point("In the manifest, not the spine", "../style.css#x") +    //
                 Point("Not in the book", "../text/gone.xhtml#x") +             //
                 Point("Somewhere else", "https://example.com/a.xhtml#x") +     //
                 Point("Nowhere", "") +                                         //
                 Point("In the list itself", "#here") +                         //
                 Point("An id as long as is kept", "../top.xhtml#" + longId) +  //
                 Point("An id longer than that", "../top.xhtml#" + longId + "i"));
  EXPECT_EQ(f.EntriesOf(plan),
            (Lines{"0 0 - Up and over", "0 1 s 1 With a space, escaped", "0 3 t With a dot", "0 3 - From the top",
                   "0 - - In the manifest, not the spine", "0 - - Not in the book", "0 - - Somewhere else",
                   "0 - - Nowhere", "0 - - In the list itself", "0 3 " + longId + " An id as long as is kept",
                   "0 3 - An id longer than that"}))
      << "a document that is in the spine twice is its first place there";
}

TEST(TocReader, ReadsANavEntryByEntry) {
  TocFixture f;
  // The nav that is the table of contents, between two that are not.
  const std::string nav = NavDocument(
      "<nav epub:type=\"landmarks\"><ol><li><a epub:type=\"cover\" href=\"a.xhtml\">Cover</a></li></ol></nav>\n" +
      TocNav("<ol>\n"
             "<li><a href=\"a.xhtml\">Beginning</a></li>\n"
             "<li><a href=\"b.xhtml\">\n  Part <i>One</i>:<br/>The  <b><small>Start</small></b>\n</a>\n"
             "  <ol><li><a href=\"b.xhtml#ch1\">Chapter 1</a>\n"
             "        <ol hidden=\"\"><li><a href=\"b.xhtml#ch1-s1\">A Section</a></li></ol></li>\n"
             "      <li><a href=\"b.xhtml#ch2\">Chapter 2</a></li></ol>\n"
             "</li>\n"
             "<li><a href=\"c.xhtml#end\">End &amp; After</a></li>\n"
             "</ol>\n") +
      "<nav epub:type=\"page-list\"><ol><li><a href=\"a.xhtml#p1\">1</a></li></ol></nav>\n");
  EXPECT_EQ(f.EntriesOf({.nav = nav}), (Lines{"0 0 - Beginning", "0 1 - Part One:The Start", "1 1 ch1 Chapter 1",
                                              "2 1 ch1-s1 A Section", "1 1 ch2 Chapter 2", "0 2 end End & After"}));
  EXPECT_EQ(f.toc.Kind(), TocKind::kNav);
}

TEST(TocReader, TakesFromANavOnlyTheLinksOfItsLists) {
  TocFixture f;
  const std::string nav =
      NavDocument(TocNav("<p>See also <a href=\"a.xhtml\">the start</a>, which is in no list.</p>\n"
                         "<ol>\n"
                         "<li>Text of the item's own <a href=\"a.xhtml\">One</a> and after it</li>\n"
                         // A heading with no link, and the entries under it. A list of the other kind.
                         "<li><span>Part</span>\n<ul><li><a href=\"b.xhtml\">Two</a></li></ul></li>\n"
                         // A span after the link is not another entry, and nor is one inside it.
                         "<li><a href=\"b.xhtml#x\"><span class=\"n\">3.</span> Three</a> <span>(new)</span></li>\n"
                         // A heading that turns out to hold a link points where the link does.
                         "<li><span><a href=\"c.xhtml#y\">Four</a> or <a href=\"a.xhtml\">so</a></span></li>\n"
                         // A link with nowhere to go, and one inside something else.
                         "<li><a>Five</a></li>\n"
                         "<li><div><a href=\"c.xhtml\">Six</a></div></li>\n"
                         "</ol>\n"));
  EXPECT_EQ(f.EntriesOf({.nav = nav}), (Lines{"0 0 - One", "0 - - Part", "1 1 - Two", "0 1 x 3. Three",
                                              "0 2 y Four or so", "0 - - Five", "0 2 - Six"}));
}

TEST(TocReader, FindsTheNavThatSaysItIsTheTableOfContents) {
  TocFixture f;
  const std::string list = "<ol><li><a href=\"a.xhtml\">One</a></li></ol>";
  const std::string other = "<ol><li><a href=\"b.xhtml\">Other</a></li></ol>";
  // Among other words, and with EPUB's names under another prefix.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<nav epub:type=\"landmarks\">" + other +
                                            "</nav><nav epub:type=\"x toc\ny\">" + list + "</nav>")}),
            Lines{"0 0 - One"});
  EXPECT_EQ(f.EntriesOf({.nav = "<html xmlns:ops=\"http://www.idpf.org/2007/ops\"><body><nav ops:type=\"toc\">" + list +
                                "</nav></body></html>"}),
            Lines{"0 0 - One"});
  // Of two that say so, the first.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument(TocNav(list) + TocNav(other))}), Lines{"0 0 - One"});
  // It is the type that has to say so, not the nav's id or its class.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<nav epub:type=\"landmarks\" id=\"toc\" class=\"toc\">" + other +
                                            "</nav>" + TocNav(list))}),
            Lines{"0 0 - One"});
  // "toc" has to be a word of it, and the element a nav.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<nav epub:type=\"tocs\">" + list + "</nav>")}),
            Lines{"(not a table of contents)"});
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<div epub:type=\"toc\">" + list + "</div>")}),
            Lines{"(not a table of contents)"});
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<nav>" + list + "</nav>")}), Lines{"(not a table of contents)"});
  // One that is, with nothing in it, is a table of contents with no entries.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument(TocNav(""))}), Lines{});
}

TEST(TocReader, FollowsALinkToAPlaceInTheNavItself) {
  TocFixture f;
  BookPlan plan;
  plan.nav =
      NavDocument(TocNav("<ol><li><a href=\"#toc\">Contents</a></li><li><a href=\"a.xhtml\">One</a></li>"
                         "<li><a href=\"\">Nowhere</a></li><li><a href=\"https://example.com/#x\">Elsewhere</a></li>"
                         "<li><a href=\"?q#x\">A question</a></li></ol>"));
  // A nav that is not a document of the spine is nowhere a reader can go.
  EXPECT_EQ(f.EntriesOf(plan),
            (Lines{"0 - - Contents", "0 0 - One", "0 - - Nowhere", "0 - - Elsewhere", "0 - - A question"}));
  // One that is, is where a link that names only a place leads. A link
  // that names no file for any other reason still leads nowhere.
  plan.navInSpine = true;
  EXPECT_EQ(f.EntriesOf(plan),
            (Lines{"0 3 toc Contents", "0 0 - One", "0 - - Nowhere", "0 - - Elsewhere", "0 - - A question"}));
}

TEST(TocReader, TakesTheSpaceThatDoesNotBreakForASpace) {
  TocFixture f;
  // Written the way HTML writes it, which a document that names a DTD
  // may. The doctype goes after the document's first line.
  const auto withDoctype = [](const std::string& document, const std::string& doctype) {
    const std::size_t firstLineEnds = document.find('\n') + 1;
    return document.substr(0, firstLineEnds) + doctype + "\n" + document.substr(firstLineEnds);
  };
  const std::string nav = withDoctype(
      NavDocument(TocNav("<ol><li><a href=\"a.xhtml\">Chapter&nbsp;1&mdash;The&nbsp;&nbsp;Start</a></li></ol>")),
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">");
  EXPECT_EQ(f.EntriesOf({.nav = nav}), Lines{"0 0 - Chapter 1The Start"});
  const std::string ncx = withDoctype(
      Ncx(Point("Chapter&nbsp;1", "a.xhtml")),
      "<!DOCTYPE ncx PUBLIC \"-//NISO//DTD ncx 2005-1//EN\" \"http://www.daisy.org/z3986/2005/ncx-2005-1.dtd\">");
  EXPECT_EQ(f.EntriesOf({.ncx = ncx}), Lines{"0 0 - Chapter 1"});
  // The character itself is kept as the character it is.
  EXPECT_EQ(f.EntriesOf({.ncx = Ncx(Point("Chapter&#160;1", "a.xhtml"))}), Lines{"0 0 - Chapter\xC2\xA0"
                                                                                 "1"});
}

TEST(TocReader, CutsALabelThatIsTooLongAtACharacter) {
  TocFixture f;
  // 254 bytes, then a character of three: the cut falls inside it.
  const std::string start(254, 'l');
  const std::string label = start + "\xE2\x80\x94 and more";
  EXPECT_EQ(f.EntriesOf({.ncx = Ncx(Point(label, "a.xhtml") + Point("Next", "b.xhtml"))}),
            (Lines{"0 0 - " + start + " (cut)", "0 1 - Next"}));
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument(TocNav("<ol><li><a href=\"a.xhtml\">" + label + "</a></li></ol>"))}),
            Lines{"0 0 - " + start + " (cut)"});
  // Exactly as long as is kept is not cut.
  const std::string whole(TocReader::kMaxLabel, 'w');
  EXPECT_EQ(f.EntriesOf({.ncx = Ncx(Point(whole, "a.xhtml"))}), Lines{"0 0 - " + whole});
}

TEST(TocReader, ReadsTheNavOfABookThatHasBothUnlessAskedForTheNcx) {
  TocFixture f;
  BookPlan both;
  both.ncx = Ncx(Point("From the NCX", "a.xhtml"));
  both.nav = NavDocument(TocNav("<ol><li><a href=\"b.xhtml\">From the nav</a></li></ol>"));
  EXPECT_EQ(f.EntriesOf(both), Lines{"0 1 - From the nav"});
  EXPECT_EQ(f.toc.Kind(), TocKind::kNav);
  EXPECT_EQ(f.EntriesOf(both, TocKind::kNav), Lines{"0 1 - From the nav"});
  EXPECT_EQ(f.EntriesOf(both, TocKind::kNcx), Lines{"0 0 - From the NCX"});
  EXPECT_EQ(f.toc.Kind(), TocKind::kNcx);

  // A book with one kind has that kind, and not the other.
  BookPlan old;
  old.ncx = both.ncx;
  EXPECT_EQ(f.EntriesOf(old), Lines{"0 0 - From the NCX"});
  EXPECT_EQ(f.toc.Kind(), TocKind::kNcx);
  EXPECT_EQ(f.EntriesOf(old, TocKind::kNav), Lines{"(no table of contents)"});
  EXPECT_EQ(f.toc.Kind(), TocKind::kBest);
  BookPlan modern;
  modern.nav = both.nav;
  EXPECT_EQ(f.EntriesOf(modern), Lines{"0 1 - From the nav"});
  EXPECT_EQ(f.EntriesOf(modern, TocKind::kNcx), Lines{"(no table of contents)"});

  // And one with neither has none.
  EXPECT_EQ(f.EntriesOf({}), Lines{"(no table of contents)"});
  EXPECT_EQ(f.toc.Entries(), 0u);
}

TEST(TocReader, SaysWhenAFileIsNoTableOfContents) {
  TocFixture f;
  EXPECT_EQ(f.EntriesOf({.ncx = "this is not XML"}), Lines{"(not a table of contents)"});
  EXPECT_EQ(f.EntriesOf({.ncx = "<ncx><head/><docTitle><text>No map</text></docTitle></ncx>"}),
            Lines{"(not a table of contents)"});
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument("<p>A page with no nav in it.</p>")}), Lines{"(not a table of contents)"});
  // A map with no points is a table of contents with no entries.
  EXPECT_EQ(f.EntriesOf({.ncx = Ncx("")}), Lines{});
  // One that goes wrong before its first entry has given nothing to keep.
  EXPECT_EQ(f.EntriesOf({.ncx = "<ncx><navMap><navPoint><navLabel><text>Never ended"}),
            Lines{"(not a table of contents)"});
  EXPECT_FALSE(f.toc.CutShort());
}

TEST(TocReader, KeepsTheEntriesThatCameBeforeAFileWentWrong) {
  TocFixture f;
  const std::string whole = Ncx(Point("One", "a.xhtml") + Point("Two", "b.xhtml", Point("Under two", "b.xhtml#u")) +
                                Point("Three", "c.xhtml"));
  // Cut off inside the third entry: the first two, and what is under the
  // second, were whole.
  EXPECT_EQ(f.EntriesOf({.ncx = whole.substr(0, whole.find("Three"))}),
            (Lines{"0 0 - One", "0 1 - Two", "1 1 u Under two"}));
  EXPECT_TRUE(f.toc.CutShort());
  // It ended inside a label. The next table read is read from its start,
  // whichever kind it is.
  EXPECT_EQ(f.EntriesOf({.nav = NavDocument(TocNav("<ol><li><a href=\"a.xhtml\">First</a></li><li><span>Head</span>"
                                                   "<ol><li><a href=\"b.xhtml\">Under</a></li></ol></li></ol>"))}),
            (Lines{"0 0 - First", "0 - - Head", "1 1 - Under"}));
  EXPECT_FALSE(f.toc.CutShort());
  // Rubbish after the end loses nothing.
  EXPECT_EQ(f.EntriesOf({.ncx = whole + "<oops>"}).size(), 4u);
  EXPECT_TRUE(f.toc.CutShort());
  // And the next one read starts afresh.
  EXPECT_EQ(f.EntriesOf({.ncx = whole}).size(), 4u);
  EXPECT_FALSE(f.toc.CutShort());

  const std::string nav =
      NavDocument(TocNav("<ol><li><a href=\"a.xhtml\">One</a></li><li><a href=\"b.xhtml\">Two</a></li></ol>"));
  EXPECT_EQ(f.EntriesOf({.nav = nav.substr(0, nav.find("Two"))}), Lines{"0 0 - One"});
  EXPECT_TRUE(f.toc.CutShort());
}

TEST(TocReader, CanBeReadAPieceAtATime) {
  TocFixture f;
  // Long enough to be several pieces of the parser's.
  std::string points;
  Lines expected;
  for (int i = 0; i < 200; ++i) {
    points += Point("Chapter " + std::to_string(i), "b.xhtml#c" + std::to_string(i));
    expected.push_back("0 1 c" + std::to_string(i) + " Chapter " + std::to_string(i));
  }
  ASSERT_EQ(f.Open(BookOf({.ncx = Ncx(points)})), PackageStatus::kOk);
  EXPECT_EQ(f.toc.Status(), TocStatus::kNotStarted);
  EXPECT_FALSE(f.toc.Step()) << "nothing to do before it is begun";

  ASSERT_EQ(f.toc.Begin(f.package, f.entries, f.reader, f.xml, f.collector), TocStatus::kOk);
  EXPECT_TRUE(f.collector.lines.empty()) << "beginning tells nothing";
  EXPECT_EQ(f.toc.Kind(), TocKind::kNcx);
  int steps = 0;
  std::size_t mostInOneStep = 0;
  while (true) {
    const std::size_t before = f.collector.lines.size();
    const bool more = f.toc.Step();
    ++steps;
    mostInOneStep = std::max(mostInOneStep, f.collector.lines.size() - before);
    if (!more) {
      break;
    }
    EXPECT_EQ(f.toc.Status(), TocStatus::kOk);
    EXPECT_EQ(f.toc.Entries(), f.collector.lines.size());
  }
  EXPECT_EQ(f.toc.Status(), TocStatus::kOk);
  EXPECT_EQ(f.collector.lines, expected);
  EXPECT_GT(steps, 10);
  EXPECT_LT(mostInOneStep, 20u) << "a piece is 1 KB of the file";
  EXPECT_FALSE(f.toc.Step());
  EXPECT_EQ(f.collector.lines.size(), 200u);

  // All at once gives the same.
  EXPECT_EQ(f.Read(), TocStatus::kOk);
  EXPECT_EQ(f.collector.lines, expected);
}

TEST(TocReader, StopsWhenTheHandlerHasHadEnough) {
  TocFixture f;
  std::string points;
  for (int i = 0; i < 300; ++i) {
    points += Point("Chapter " + std::to_string(i), "a.xhtml", Point("Under it", "a.xhtml#u"));
  }
  ASSERT_EQ(f.Open(BookOf({.ncx = Ncx(points)})), PackageStatus::kOk);
  f.collector.stopAfter = 5;
  EXPECT_EQ(f.Read(), TocStatus::kStopped);
  EXPECT_EQ(f.collector.lines.size(), 5u) << "and is told no more";
  EXPECT_EQ(f.toc.Entries(), 5u);
  EXPECT_EQ(f.collector.lines.back(), "0 0 - Chapter 2");
  EXPECT_FALSE(f.toc.Step());

  // An entry that is given because the first one under it has begun, and
  // that one empty: the parser still tells of the empty one's end, and
  // that is not passed on.
  ASSERT_EQ(f.Open(BookOf({.ncx = "<ncx><navMap><navPoint><navLabel><text>Over</text></navLabel>"
                                  "<content src=\"a.xhtml\"/><navPoint/></navPoint>" +
                                  Point("After", "b.xhtml") + "</navMap></ncx>"})),
            PackageStatus::kOk);
  f.collector.stopAfter = 1;
  EXPECT_EQ(f.Read(), TocStatus::kStopped);
  EXPECT_EQ(f.collector.lines, Lines{"0 0 - Over"});

  // The same in a nav, where an entry ends at the end of its link.
  std::string items;
  for (int i = 0; i < 300; ++i) {
    items += "<li><a href=\"a.xhtml\">Chapter " + std::to_string(i) + "</a></li>";
  }
  ASSERT_EQ(f.Open(BookOf({.nav = NavDocument(TocNav("<ol>" + items + "</ol>"))})), PackageStatus::kOk);
  f.collector.stopAfter = 3;
  EXPECT_EQ(f.Read(), TocStatus::kStopped);
  EXPECT_EQ(f.collector.lines, (Lines{"0 0 - Chapter 0", "0 0 - Chapter 1", "0 0 - Chapter 2"}));
}

TEST(TocReader, ReportsATableOfContentsItCannotRead) {
  // A letter of a label changed: it is still XML, and the entry's
  // checksum is found wrong at its end, when every entry has been told.
  ZipBuilder builder;
  const std::string ncx = Ncx(Point("One", "a.xhtml"));
  const Files book = BookOf({.ncx = ncx});
  std::size_t ncxAt = 0;
  for (std::size_t i = 0; i < book.size(); ++i) {
    builder.Add(book[i].first, BytesOf(book[i].second));
    ncxAt = book[i].first == "OEBPS/toc.ncx" ? i : ncxAt;
  }
  Bytes zip = builder.Finish();
  zip.at(builder.At(ncxAt).data + ncx.find("One")) ^= 0x01U;
  TocFixture f;
  ASSERT_EQ(f.OpenZip(zip), PackageStatus::kOk);
  EXPECT_EQ(f.Read(), TocStatus::kUnreadable);
  EXPECT_EQ(f.collector.lines, Lines{"0 0 - Nne"});

  // Storage that fails once the package has been read: it is found at
  // the beginning.
  TocFixture failing;
  ASSERT_EQ(failing.Open(book), PackageStatus::kOk);
  failing.storage.failReads = true;
  // (Not from what the file has in its buffer already.)
  ASSERT_EQ(failing.entries.Open(failing.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(failing.toc.Begin(failing.package, failing.entries, failing.reader, failing.xml, failing.collector),
            TocStatus::kUnreadable);
  EXPECT_EQ(failing.toc.Status(), TocStatus::kUnreadable);
  EXPECT_FALSE(failing.toc.Step());
  EXPECT_TRUE(failing.collector.lines.empty());

  // A tag longer than the parser has memory for, after two entries: they
  // were told, and the reading ends there.
  TocFixture longTag;
  ASSERT_EQ(longTag.Open(BookOf({.ncx = Ncx(Point("One", "a.xhtml") + Point("Two", "b.xhtml") + "<navPoint id=\"" +
                                            std::string(60'000, 'i') + "\"/>" + Point("Three", "c.xhtml"))})),
            PackageStatus::kOk);
  EXPECT_EQ(longTag.Read(), TocStatus::kNoMemory);
  EXPECT_EQ(longTag.collector.lines, (Lines{"0 0 - One", "0 1 - Two"}));
  EXPECT_FALSE(longTag.toc.CutShort());

  // A parser with too little memory to parse anything.
  TocFixture small;
  ASSERT_EQ(small.Open(book), PackageStatus::kOk);
  std::vector<std::uint8_t> little(512);
  bn::epub::XmlParser tiny(little);
  EXPECT_EQ(small.toc.Read(small.package, small.entries, small.reader, tiny, small.collector), TocStatus::kNoMemory);
  EXPECT_TRUE(small.collector.lines.empty());
}

TEST(TocReader, ReadsItsFileOnceAndKeepsNothingOfIt) {
  // 2,000 entries with long labels: about 300 KB of NCX, kept as it is.
  std::string points;
  for (int i = 0; i < 2'000; ++i) {
    points += Point("Chapter " + std::to_string(i) + " " + std::string(60, 'x'), "c.xhtml#c" + std::to_string(i));
  }
  const std::string ncx = Ncx(points);
  ASSERT_GT(ncx.size(), 300'000u);
  ZipBuilder builder;
  for (const auto& [path, contents] : BookOf({.ncx = ncx})) {
    builder.Add(path, BytesOf(contents));
  }
  TocFixture f;
  ASSERT_EQ(f.OpenZip(builder.Finish()), PackageStatus::kOk);
  const std::size_t before = f.storage.reads.size();
  EXPECT_EQ(f.Read(), TocStatus::kOk);
  EXPECT_EQ(f.toc.Entries(), 2'000u);
  EXPECT_EQ(f.collector.lines.back(), "0 2 c1999 Chapter 1999 " + std::string(60, 'x'));
  // The file's blocks, each once, and no more than two reads besides.
  EXPECT_LE(f.storage.reads.size() - before, (ncx.size() / 4096) + 3);
  EXPECT_LT(sizeof(TocReader), 1100u) << "about 1 KB, whatever the table of contents";
}

TEST(TocDigest, IsOfALineForEachEntry) {
  TocFixture f;
  ASSERT_EQ(f.Open(BookOf({.ncx = Ncx(Point("Beginning", "a.xhtml") +
                                      Point("Part  One", "b.xhtml", Point("Chapter 1", "b.xhtml#ch%201")) +
                                      "<navPoint><navLabel><text>No target</text></navLabel></navPoint>" +
                                      Point("", "c.xhtml#end"))})),
            PackageStatus::kOk);
  bn::epub::TocDigest digest;
  ASSERT_EQ(f.toc.Read(f.package, f.entries, f.reader, f.xml, digest), TocStatus::kOk);
  // How far in, the document, the place within it and the label, with a
  // tab between and "-" for a document or a place there is none of.
  const std::string lines =
      "0\t0\t-\tBeginning\n0\t1\t-\tPart One\n1\t1\tch 1\tChapter 1\n0\t-\t-\tNo target\n"
      "0\t2\tend\t\n";
  EXPECT_EQ(digest.Checksum(), bn::core::Crc32(BytesOf(lines))) << lines;
  EXPECT_EQ(digest.Entries(), 5u);
  EXPECT_EQ(digest.Levels(), 2u);
  EXPECT_EQ(digest.WithoutDocument(), 1u);

  // It goes on over whatever it is given next, until it is started again.
  ASSERT_EQ(f.toc.Read(f.package, f.entries, f.reader, f.xml, digest), TocStatus::kOk);
  EXPECT_EQ(digest.Checksum(), bn::core::Crc32(BytesOf(lines + lines)));
  EXPECT_EQ(digest.Entries(), 10u);
  EXPECT_EQ(digest.WithoutDocument(), 2u);
  digest.Reset();
  EXPECT_EQ(digest.Checksum(), 0u);
  EXPECT_EQ(digest.Entries(), 0u);
  EXPECT_EQ(digest.Levels(), 0u);
  EXPECT_EQ(digest.WithoutDocument(), 0u);
  ASSERT_EQ(f.toc.Read(f.package, f.entries, f.reader, f.xml, digest), TocStatus::kOk);
  EXPECT_EQ(digest.Checksum(), bn::core::Crc32(BytesOf(lines)));
}

TEST(TocDigest, WritesNumbersOfAnyLengthAndNeverAsksForNoMore) {
  bn::epub::TocDigest digest;
  // Numbers with every digit, and one with noughts inside it.
  EXPECT_TRUE(
      digest.OnEntry({.label = "Deep", .labelCut = true, .depth = 65'535, .document = 1'234'567'890, .fragment = "x"}));
  EXPECT_TRUE(digest.OnEntry({.label = "", .labelCut = false, .depth = 10, .document = 100, .fragment = ""}));
  EXPECT_EQ(digest.Checksum(), bn::core::Crc32(BytesOf("65535\t1234567890\tx\tDeep\n10\t100\t-\t\n")));
  EXPECT_EQ(digest.Entries(), 2u);
  EXPECT_EQ(digest.Levels(), 65'536u);
  EXPECT_EQ(digest.WithoutDocument(), 0u);
  // Anything a label holds changes it, a different label most of all.
  bn::epub::TocDigest other;
  EXPECT_TRUE(
      other.OnEntry({.label = "Deeq", .labelCut = true, .depth = 65'535, .document = 1'234'567'890, .fragment = "x"}));
  EXPECT_TRUE(other.OnEntry({.label = "", .labelCut = false, .depth = 10, .document = 100, .fragment = ""}));
  EXPECT_NE(other.Checksum(), digest.Checksum());
}

TEST(TocReader, NamesEveryStatusAndKind) {
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kNotStarted), "not started");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kNoToc), "no table of contents");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kNotToc), "not a table of contents");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kNoMemory), "no memory");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kUnreadable), "unreadable");
  EXPECT_STREQ(bn::epub::TocStatusName(TocStatus::kStopped), "stopped");
  EXPECT_STREQ(bn::epub::TocKindName(TocKind::kBest), "best");
  EXPECT_STREQ(bn::epub::TocKindName(TocKind::kNav), "nav");
  EXPECT_STREQ(bn::epub::TocKindName(TocKind::kNcx), "ncx");
}

// Whatever is done to a table of contents, reading it ends with an
// answer, and tells of no more entries than a whole one has room for.
TEST(TocReader, SurvivesDamageAnywhereInATableOfContents) {
  const std::string ncx =
      Ncx(Point("One", "a.xhtml") + Point("Two &amp; a half", "b.xhtml#x", Point("Under", "../OEBPS/c.xhtml#y%20z")));
  const std::string nav = NavDocument(
      TocNav("<ol><li><a href=\"a.xhtml\">One</a></li><li><span>Two</span><ol><li><a href=\"b.xhtml#x\">Under "
             "<i>it</i></a></li></ol></li></ol>") +
      "<nav epub:type=\"landmarks\"><ol><li><a href=\"c.xhtml\">Start</a></li></ol></nav>");
  for (const bool isNav : {false, true}) {
    const std::string& whole = isNav ? nav : ncx;
    TocFixture f;
    const auto read = [&](const std::string& damaged) {
      BookPlan plan;
      (isNav ? plan.nav : plan.ncx) = damaged.empty() ? std::string(" ") : damaged;
      ZipBuilder builder;
      for (const auto& [path, contents] : BookOf(plan)) {
        builder.Add(path, BytesOf(contents));
      }
      ASSERT_EQ(f.OpenZip(builder.Finish()), PackageStatus::kOk);
      const TocStatus status = f.Read();
      EXPECT_TRUE(status == TocStatus::kOk || status == TocStatus::kNotToc) << bn::epub::TocStatusName(status);
      EXPECT_LE(f.collector.lines.size(), 64u);
    };
    // Every bit turned over, one at a time.
    for (std::size_t at = 0; at < whole.size(); ++at) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        std::string damaged = whole;
        damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ (1U << bit));
        read(damaged);
      }
    }
    // Every length it can be cut to.
    for (std::size_t length = 0; length < whole.size(); ++length) {
      read(whole.substr(0, length));
    }
  }
}

TEST(TocReader, IsNotUpsetByListsNestedVeryDeep) {
  TocFixture f;
  // Deeper than any book, and than the levels a nav's spans are told
  // apart to. The parser's memory is what gives out, if anything does.
  std::string open;
  std::string close;
  for (int i = 0; i < 60; ++i) {
    open += "<li><span>Level " + std::to_string(i) + "</span><ol>";
    close += "</ol></li>";
  }
  const Lines entries = f.EntriesOf({.nav = NavDocument(TocNav("<ol>" + open + close + "</ol>"))});
  ASSERT_EQ(entries.size(), 60u);
  EXPECT_EQ(entries[0], "0 - - Level 0");
  EXPECT_EQ(entries[59], "59 - - Level 59");

  std::string points;
  for (int i = 0; i < 60; ++i) {
    points = Point("Level " + std::to_string(59 - i), "a.xhtml", points);
  }
  const Lines nested = f.EntriesOf({.ncx = Ncx(points)});
  ASSERT_EQ(nested.size(), 60u);
  EXPECT_EQ(nested[59], "59 0 - Level 59");
}

}  // namespace
