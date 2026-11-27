#include "epub/chapter_import.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_fakes.h"
#include "core/block_file.h"
#include "core/byte_source.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/html_entities.h"
#include "epub/package.h"
#include "epub/segment.h"
#include "epub/xml.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::BlockStyle;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleMaker;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::ChapterImporter;
using bn::epub::ChapterState;
using bn::epub::HtmlEntity;
using bn::epub::ImportLeftOut;
using bn::epub::ImportReason;
using bn::epub::ImportStatus;
using bn::epub::ItemKind;
using bn::epub::SegmentAnchor;
using bn::epub::SegmentBlock;
using bn::epub::SegmentBlockKind;
using bn::epub::SegmentItem;
using bn::epub::SegmentLength;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::test::FakeBook;
using bn::test::FakeStorage;

const char* const kBundle = "/book.bundle";
const char* const kChapter = "text/chapter.xhtml";

// A document of the fake book, by its name.
class FakeDocument final : public bn::epub::ChapterDocument {
 public:
  FakeDocument(FakeBook& book, std::string name) : book_(book), name_(std::move(name)) {}

  [[nodiscard]] bn::core::ByteSource* Open(std::span<char> path, std::size_t& length) noexcept override {
    ++opens;
    if (name_.size() > path.size() || opens == failsAtOpen) {
      return nullptr;
    }
    std::copy(name_.begin(), name_.end(), path.begin());
    length = name_.size();
    return book_.Open(name_);
  }

  int opens = 0;
  int failsAtOpen = 0;  // The time it is asked for that it cannot be opened; 0 for never.

 private:
  FakeBook& book_;
  std::string name_;
};

// A page of XHTML with this in its body, which asks for the sheet the
// fixture puts in the book. It names a DTD, so that a name such as &nbsp;
// is reported and not a mistake.
std::string Page(std::string_view body, std::string_view html = "") {
  std::string page = "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"xhtml11.dtd\">\n<html";
  page += html;
  page += "><head><title>A title</title><link rel=\"stylesheet\" href=\"book.css\"/></head><body>";
  page += body;
  page += "</body></html>";
  return page;
}

// Whether some bits have one of them set.
bool Has(std::uint16_t bits, std::uint16_t bit) { return (std::uint32_t{bits} & bit) != 0; }

// A block as it was read back.
struct GotBlock {
  SegmentBlockKind kind = SegmentBlockKind::kParagraph;
  std::uint8_t level = 0;
  BlockStyle style;
  std::uint32_t source = 0;
  std::uint32_t characters = 0;
  // Its items as text: text as it is, with \n for a line feed; a style
  // item as {bits/the book's/family/language/size}, the family with a star
  // if the book named it; a picture as [the text that stands for it].
  std::string items;
};

struct ImportFixture {
  // With so much memory for the parser: a device's 24 KB, unless a test
  // has a document that asks for more.
  explicit ImportFixture(std::size_t parserBytes = 24576) : xmlMemory(parserBytes) {
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
    MakeBundle(4);
  }

  void MakeBundle(std::uint16_t documents) {
    BundleMaker maker(page);
    ASSERT_EQ(file.Open(storage, kBundle, OpenMode::kCreate), StorageStatus::kOk);
    ASSERT_EQ(maker.Begin(file), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kStrings), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kSpine), BundleStatus::kOk);
    for (std::uint32_t n = 0; n < documents; ++n) {
      ASSERT_EQ(maker.WriteFile(BundleFile{}), BundleStatus::kOk);
    }
    ASSERT_EQ(maker.BeginTable(BundleTable::kPictures), BundleStatus::kOk);
    ASSERT_EQ(maker.BeginTable(BundleTable::kContents), BundleStatus::kOk);
    BundleBook made;
    made.documents = documents;
    ASSERT_EQ(maker.Finish(made), BundleStatus::kOk);
    while (maker.Step()) {
    }
    ASSERT_EQ(maker.Status(), BundleStatus::kOk);
    file.Close();
    ASSERT_EQ(file.Open(storage, kBundle, OpenMode::kUpdate), StorageStatus::kOk);
    ASSERT_EQ(bundle.Open(file), BundleStatus::kOk);
  }

  // Imports a document as the chapter of that number, with that sheet as
  // the one it links, holding every step to what a step may read and
  // write; commits it and reads it back if it was imported.
  ImportStatus Import(std::string_view document, std::string_view sheet = {}, std::size_t number = 0) {
    book.Put(kChapter, document);
    book.Put("text/book.css", sheet);
    return ImportNamed(kChapter, number);
  }

  ImportStatus ImportNamed(const std::string& name, std::size_t number = 0) {
    if (newBook) {
      sheets.BeginBook();
    }
    FakeDocument document(book, name);
    blocks.clear();
    anchors.clear();
    languages.clear();
    steps = 0;
    mostPages = 0;
    mostRead = 0;
    const std::size_t writesBefore = storage.written.size();
    ImportStatus status = importer.Begin(document, sheets, xml, file, bundle.DataEnd(), language);
    for (bool more = status == ImportStatus::kOk; more;) {
      const std::size_t written = storage.written.size();
      const std::uint64_t read = book.bytesRead;
      const std::size_t opens = book.opens;
      more = importer.Step();
      ++steps;
      mostPages = std::max(mostPages, storage.written.size() - written);
      mostRead = std::max<std::size_t>(mostRead, static_cast<std::size_t>(book.bytesRead - read));
      EXPECT_LE(storage.written.size() - written, ChapterImporter::kMostPagesPerStep) << "step " << steps;
      EXPECT_LE(book.bytesRead - read, ChapterImporter::kMostBytesPerStep) << "step " << steps;
      // It opens a file or reads, and never both.
      EXPECT_TRUE(book.opens - opens == 0 || book.bytesRead == read) << "step " << steps;
      EXPECT_LE(book.opens - opens, 1U) << "step " << steps;
    }
    status = importer.Status();
    chapter = importer.Chapter();
    importWrites = storage.written.size() - writesBefore;
    if (status == ImportStatus::kOk) {
      EXPECT_EQ(bundle.Commit(number, chapter), BundleStatus::kOk);
      if (chapter.state == ChapterState::kWhole || chapter.state == ChapterState::kCutShort) {
        ReadBack();
      }
    }
    return status;
  }

  void ReadBack() {
    ASSERT_EQ(reader.Open(file, chapter), SegmentStatus::kOk);
    for (std::size_t number = 1; number <= reader.Languages(); ++number) {
      languages.emplace_back(reader.Language(number));
    }
    for (std::size_t anchorPage = 0; anchorPage < reader.AnchorPages(); ++anchorPage) {
      ASSERT_EQ(reader.BeginReadAnchors(anchorPage), SegmentStatus::kOk);
      while (reader.Step()) {
      }
      ASSERT_EQ(reader.Status(), SegmentStatus::kOk);
      SegmentAnchor anchor;
      while (reader.NextAnchor(anchor)) {
        anchors.push_back(std::string(anchor.name) + " " + std::to_string(anchor.block) + " " +
                          std::to_string(anchor.into));
      }
    }
    // Where each block began is in the index alone.
    std::vector<std::uint32_t> sources;
    for (std::uint32_t number = 0; number < reader.Blocks(); ++number) {
      ASSERT_EQ(reader.FindBlock(number), SegmentStatus::kOk);
      sources.push_back(reader.Place().source);
    }
    ASSERT_EQ(reader.SeekStart(), SegmentStatus::kOk);
    for (;;) {
      SegmentBlock block;
      SegmentStatus status = SegmentStatus::kMore;
      while (status == SegmentStatus::kMore) {
        status = reader.ReadBlock(block);
      }
      if (status == SegmentStatus::kEnd) {
        break;
      }
      ASSERT_EQ(status, SegmentStatus::kOk);
      GotBlock read;
      read.kind = static_cast<SegmentBlockKind>(block.kind);
      read.level = block.level;
      read.style = block.style;
      read.source = sources.at(blocks.size());
      for (bool ended = false; !ended;) {
        SegmentItem item;
        status = reader.NextItem(item);
        if (status == SegmentStatus::kMore) {
          continue;
        }
        ASSERT_EQ(status, SegmentStatus::kOk);
        switch (item.kind) {
          case ItemKind::kText:
            for (const char ch : item.text) {
              read.items += ch == '\n' ? std::string("\\n") : std::string(1, ch);
            }
            break;
          case ItemKind::kStyle:
            read.items += "{" + std::to_string(item.style.bits) + "/" + std::to_string(item.style.fromBook) + "/" +
                          std::to_string(item.style.familyClass) + (item.style.familyFromBook ? "*" : "") + "/" +
                          std::to_string(item.style.language) + "/" + std::to_string(item.style.size) + "}";
            break;
          case ItemKind::kPicture:
            EXPECT_EQ(item.picture, bn::epub::kSegmentNoPicture);
            EXPECT_EQ(item.pictureBits, bn::epub::kPictureInLine);
            read.items += "[" + std::string(item.text) + "]";
            break;
          case ItemKind::kEnd:
            read.characters = item.characters;
            ended = true;
            break;
          default:
            ADD_FAILURE() << "an item that importing does not write";
            break;
        }
      }
      blocks.push_back(read);
    }
    EXPECT_EQ(blocks.size(), reader.Blocks());
    EXPECT_EQ(chapter.blocks, reader.Blocks());
    reader.Close();
  }

  // Every block's items, a block to a string.
  std::vector<std::string> Texts() const {
    std::vector<std::string> texts;
    texts.reserve(blocks.size());
    for (const GotBlock& block : blocks) {
      texts.push_back(block.items);
    }
    return texts;
  }

  // How many of its first characters a block says are a dropped capital.
  unsigned Dropped(std::size_t block) const {
    return (std::uint32_t{blocks.at(block).style.breaking} & bn::epub::kBreakDropped) >> bn::epub::kBreakDroppedShift;
  }

  FakeStorage storage;
  FakeBook book;
  alignas(4) std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  alignas(4) std::array<std::uint8_t, 4096> readerPage{};
  BlockFile file{fileBuffer};
  Bundle bundle{page};
  SegmentReader reader{readerPage};

  std::vector<std::uint8_t> xmlMemory;
  bn::epub::XmlParser xml{xmlMemory};
  std::vector<std::uint8_t> rulesMemory = std::vector<std::uint8_t>(16384);
  bn::epub::CssRules rules{rulesMemory};
  bn::epub::CssReader css;
  std::vector<char> names = std::vector<char>(bn::epub::DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style = std::vector<std::uint8_t>(bn::epub::DocumentSheets::kStyleBytes);
  bn::epub::DocumentSheets sheets{book, xml, css, rules, names, style};
  alignas(4) std::array<std::uint8_t, ChapterImporter::kWorkBytes> work{};
  ChapterImporter importer{rules, work};

  std::string language = "en";
  bool newBook = true;  // Each document is of a book of its own: its sheet is read afresh.
  BundleChapter chapter;
  std::vector<GotBlock> blocks;
  std::vector<std::string> anchors;  // "name block characters-into-it"
  std::vector<std::string> languages;
  std::size_t steps = 0;
  std::size_t mostPages = 0;
  std::size_t mostRead = 0;
  std::size_t importWrites = 0;  // How many writes the importing made, before the chapter was committed.
};

using Strings = std::vector<std::string>;

// ---- White space.

TEST(ChapterImport, MakesOneSpaceOfARunOfWhiteSpace) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Runs  of \t white\n   space\r\n are\n\n one.</p><p>a&#13;b &#13;&#13; c&#9;d</p>")),
            ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kWhole);
  // A return and a tab written by their numbers are white space too.
  EXPECT_EQ(f.Texts(), Strings({"Runs of white space are one.", "a b c d"}));
  EXPECT_EQ(f.blocks.at(0).characters, 28U);
  EXPECT_EQ(f.chapter.characters, 35U);
}

TEST(ChapterImport, KeepsNoWhiteSpaceAtABlocksStartOrEnd) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>  \n One. </p>\n<p><b> </b> Two<i> </i></p> <div> Three\n</div>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"One.", "Two", "Three"}));
}

TEST(ChapterImport, CombinesWhiteSpaceAcrossElementsAndSetsTheSpaceAsItsFirstWas) {
  ImportFixture f;
  // The space after "one" is bold, as the first of its white space is; the
  // one after "two" is plain, though white space in italics follows.
  ASSERT_EQ(f.Import(Page("<p>one<b> </b> <i> two</i> <i> </i>three</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"one{1/0/0/0/1000} {2/0/0/0/1000}two{0/0/0/0/1000} three"}));
}

TEST(ChapterImport, WritesALineFeedForABrAndNoWhiteSpaceBesideIt) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>One <br/> two<br/><br/>three<br/></p><p><br/>After an empty line.</p>"
                          "<div><br/></div><div> <br/> </div>")),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"One\\ntwo\\n\\nthree\\n", "\\nAfter an empty line.", "\\n", "\\n"}));
  EXPECT_EQ(f.blocks.at(2).characters, 1U);
}

TEST(ChapterImport, KeepsTextAsWrittenInAPreButForWhatComesBeforeItsFirstText) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<pre>\n\n  first line\n    second\tline  \n\n  third\n</pre><pre>one</pre>"
                          "<pre>   <b>bold</b>  </pre><pre>  </pre><pre>\n</pre>")),
            ImportStatus::kOk);
  // Of the white space before the first text, what follows the last line
  // end; everything after that, white space at the end as well.
  EXPECT_EQ(f.Texts(), Strings({"{0/0/3/0/1000}  first line\\n    second\tline  \\n\\n  third\\n", "{0/0/3/0/1000}one",
                                "{1/0/3/0/1000}   bold{0/0/3/0/1000}  "}));
  EXPECT_TRUE(Has(f.blocks.at(0).style.breaking, bn::epub::kBreakSpaceAsWritten));
}

TEST(ChapterImport, HoldsOnlySoMuchWhiteSpaceBeforeAPresFirstTextAndCountsTheRest) {
  ImportFixture f;
  const std::string spaces(ChapterImporter::kMostHeld + 5, ' ');
  ASSERT_EQ(f.Import(Page("<pre>" + spaces + "text</pre>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"{0/0/3/0/1000}" + std::string(ChapterImporter::kMostHeld, ' ') + "text"}));
  EXPECT_EQ(f.importer.LeftOut(ImportLeftOut::kHeld), 5U);
  EXPECT_EQ(f.chapter.leftOut, 5U);

  // What is never written was not left out for want of room: white space
  // before a line end, and in a block that has no text.
  ASSERT_EQ(f.Import(Page("<pre>" + spaces + "\n  text</pre><pre>" + spaces + "</pre><pre>" + spaces + "<br/>x</pre>")),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"{0/0/3/0/1000}  text", "\\n{0/0/3/0/1000}x"}));
  EXPECT_EQ(f.importer.LeftOut(ImportLeftOut::kHeld), 0U);
  EXPECT_EQ(f.chapter.leftOut, 0U);
}

TEST(ChapterImport, DoesNotCarryHeldWhiteSpacePastAnElementThatIsABlock) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<pre>   <div>in a block</div>   <p>and another</p>   </pre>after it")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"{0/0/3/0/1000}in a block", "{0/0/3/0/1000}and another", "after it"}));
}

TEST(ChapterImport, PutsASpaceBetweenTextAsWrittenAndTextThatIsNot) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Before <pre class='in'>  x  </pre>  after <pre class='in'>y</pre></p>"),
                     ".in { display: inline }"),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"Before {0/0/3/0/1000}  x  {0/0/0/0/1000} after {0/0/3/0/1000}y"}));
}

TEST(ChapterImport, GivesTheSameChapterWhateverPiecesTheDocumentComesIn) {
  const std::string document = Page(
      "<p>Runs  of   white <b> space </b> and <i>more</i>.</p><pre>\n   as\n  written  \n</pre>"
      "<p class='f'>&ldquo;A capital, and&nbsp;a name.</p><p><span class='d'>T</span>he end. <br/> </p>");
  const std::string_view sheet = ".f::first-letter { float: left; font-size: 3em } .d { float: left }";
  ImportFixture whole;
  ASSERT_EQ(whole.Import(document, sheet), ImportStatus::kOk);
  ASSERT_EQ(whole.blocks.size(), 4U);
  for (const std::size_t piece : {1U, 2U, 3U, 7U, 64U}) {
    ImportFixture f;
    f.book.pieceBytes = piece;
    ASSERT_EQ(f.Import(document, sheet), ImportStatus::kOk) << piece;
    EXPECT_EQ(f.Texts(), whole.Texts()) << piece;
    EXPECT_EQ(f.anchors, whole.anchors) << piece;
    for (std::size_t block = 0; block < f.blocks.size() && block < whole.blocks.size(); ++block) {
      EXPECT_TRUE(f.blocks[block].style == whole.blocks[block].style) << piece << " " << block;
      EXPECT_EQ(f.blocks[block].source, whole.blocks[block].source) << piece << " " << block;
    }
  }
}

// ---- Characters.

TEST(ChapterImport, WritesTheCharacterAnHtmlNameStandsForAndCountsANameThatIsNone) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>caf&eacute; &mdash; &ldquo;so&rdquo;&hellip; &euro;5 &nosuch; &Eacute;&amp;&lt;&#233;</p>"
                          "<p>&nbsp;</p><p>&nosuch;</p><p> &nbsp; </p>")),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"caf\xC3\xA9 \xE2\x80\x94 \xE2\x80\x9Cso\xE2\x80\x9D\xE2\x80\xA6 \xE2\x82\xAC"
                                "5 \xC3\x89&<\xC3\xA9",
                                "\xC2\xA0", "\xC2\xA0"}));
  // A name that is none is no text: it begins no block.
  EXPECT_EQ(f.importer.LeftOut(ImportLeftOut::kEntity), 2U);
  EXPECT_EQ(f.chapter.leftOut, 2U);
}

TEST(ChapterImport, KeepsASoftHyphenAndANoBreakSpaceAsTheyAre) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>hy&#173;phen  no&#160; &#160;break &#x2003; wide</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"hy\xC2\xADphen no\xC2\xA0 \xC2\xA0"
                                "break \xE2\x80\x83 wide"}));
}

// ---- How text is set.

TEST(ChapterImport, WritesAStyleItemWhereTheSettingOfTextChanges) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Plain <b>bold <i>both</i></b> <span class='s'>small capitals</span> <code>code</code>"
                          " x<sup>2</sup> <u>under</u><u>lined</u></p><h2>A heading</h2>"),
                     ".s { font-variant: small-caps } code { font-family: Courier, monospace }"),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(),
            Strings({"Plain {1/0/0/0/1000}bold {3/0/0/0/1000}both{0/0/0/0/1000} {16/16/0/0/1000}small capitals"
                     "{0/0/0/0/1000} {0/0/3*/0/1000}code{0/0/0/0/1000} x{32/0/0/0/833}2{0/0/0/0/1000} "
                     "{4/0/0/0/1000}underlined",
                     "{1/0/0/0/1000}A heading"}));
}

TEST(ChapterImport, GivesARunsSizeAsSoMuchOfItsBlocks) {
  ImportFixture f;
  ASSERT_EQ(
      f.Import(Page("<p class='big'>Twice, <span class='half'>once,</span> <span class='big'>four times</span></p>"
                    "<p class='more'>Half as big again, <span class='own'>the reader's own</span></p>"),
               ".big { font-size: 2em } .half { font-size: 50% } .more { font-size: 1.5em } .own { font-size: 1rem }"),
      ImportStatus::kOk);
  EXPECT_EQ(f.blocks.at(0).style.size, 2000);
  EXPECT_EQ(f.blocks.at(1).style.size, 1500);
  // Two thirds, to the nearest thousandth.
  EXPECT_EQ(f.Texts(), Strings({"Twice, {0/0/0/0/500}once,{0/0/0/0/1000} {0/0/0/0/2000}four times",
                                "Half as big again, {0/0/0/0/667}the reader's own"}));
}

TEST(ChapterImport, NumbersLanguagesAsTextInThemIsFirstWrittenAndTheBooksOwnAsNone) {
  ImportFixture f;
  f.language = "en-GB";
  ASSERT_EQ(f.Import(Page("<p>English <span xml:lang='de'> </span><span xml:lang='fr'>fran&ccedil;ais "
                          "<i lang='la'>latina</i></span> <span lang='EN-gb'>own</span> <span lang='en'>not own</span>"
                          "</p><p xml:lang='la'>Lingua latina.</p>",
                          " xml:lang='en-gb'")),
            ImportStatus::kOk);
  // German was named and nothing written in it: it has no number.
  EXPECT_EQ(f.languages, Strings({"fr", "la", "en"}));
  EXPECT_EQ(f.Texts(), Strings({"English {0/0/0/1/1000}fran\xC3\xA7"
                                "ais {2/0/0/2/1000}latina{0/0/0/0/1000} own {0/0/0/3/1000}not own",
                                "{0/0/0/2/1000}Lingua latina."}));
}

TEST(ChapterImport, NamesEveryLanguageWhereTheBookHasNoneOfItsOwn) {
  ImportFixture f;
  f.language = "";
  ASSERT_EQ(f.Import(Page("<p>English <span lang='fr'>fran&ccedil;ais</span></p>", " lang='en'")), ImportStatus::kOk);
  EXPECT_EQ(f.languages, Strings({"en", "fr"}));
  EXPECT_EQ(f.Texts(), Strings({"{0/0/0/1/1000}English {0/0/0/2/1000}fran\xC3\xA7"
                                "ais"}));
  // And a tag too long to be one is none: not even the language that is
  // as much of it as a tag may be.
  f.language = std::string(bn::epub::kSegmentMostLanguage + 1, 'x');
  const std::string longest(bn::epub::kSegmentMostLanguage, 'x');
  ASSERT_EQ(f.Import(Page("<p>Text</p><p lang='" + longest + "'>More</p>", " lang='en'")), ImportStatus::kOk);
  EXPECT_EQ(f.languages, Strings({"en", longest}));
  // One just as long as a tag may be is the book's.
  f.language = longest;
  ASSERT_EQ(f.Import(Page("<p>Text</p><p lang='" + longest + "'>More</p>", " lang='en'")), ImportStatus::kOk);
  EXPECT_EQ(f.languages, Strings({"en"}));
}

// ---- Ids and pictures.

TEST(ChapterImport, MakesAnAnchorOfEveryIdWhereItsElementOpens) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<h1 id='top'>One</h1><div id='outer'><p id='p'>Some <a id='a'/>text <b id='b'>here</b>"
                          "<a name='named'>.</a><a id='both' name='not'/><a id=''/><i name='no'/><A name='capital'/>"
                          "</p></div>"
                          "<p class='gone' id='hidden'>Not <i id='inside'>shown</i></p><hr id='rule'/>"
                          "<p>Last<br id='br'/>line<span id='end'/></p><a id='after'/>",
                          " id='html'"),
                     ".gone { display: none }"),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(),
            Strings({"{1/0/0/0/1000}One", "Some text {1/0/0/0/1000}here{0/0/0/0/1000}.", "", "Last\\nline"}));
  // An id before a block's text is at its start; one in what is not shown
  // is at the start of the block that follows; one after the last block
  // is at the chapter's end. An id stands before white space that has not
  // yet become a space, and one on a `br` before its line feed.
  EXPECT_EQ(f.anchors,
            Strings({"html 0 0", "top 0 0", "outer 1 0", "p 1 0", "a 1 4", "b 1 9", "named 1 14", "both 1 15",
                     "capital 1 15", "hidden 2 0", "inside 2 0", "rule 2 0", "br 3 4", "end 3 9", "after 4 0"}));
}

TEST(ChapterImport, LeavesOutAnIdTooLongToKeepAndCountsIt) {
  ImportFixture f;
  const std::string longest(bn::epub::kSegmentMostName, 'n');
  ASSERT_EQ(f.Import(Page("<p id='" + longest + "'>One</p><p id='" + longest + "x'>Two</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.anchors, Strings({longest + " 0 0"}));
  EXPECT_EQ(f.chapter.leftOut, 1U);
}

TEST(ChapterImport, WritesAPictureAsOneTheBookHasNotWithTheTextThatStandsForIt) {
  ImportFixture f;
  const std::string longAlt = "ab" + [] {
    std::string text;
    for (int i = 0; i < 127; ++i) {
      text += "\xC3\xA9";
    }
    return text;
  }();
  const std::string most(bn::epub::kSegmentMostName, 'm');
  ASSERT_EQ(f.Import(Page("<p>A <img src='a.png' alt='small picture'/> here, <img src='b.png'/> <IMG alt=''/>.</p>"
                          "<div> <img src='c.png' alt='alone'/> </div><p><img alt='" +
                          longAlt + "'/></p><p>None: <img class='gone' alt='hidden'/></p><p><img alt='" + most +
                          "'/></p><p><img alt='" + most + "x'/></p>"),
                     ".gone { display: none }"),
            ImportStatus::kOk);
  // The text that stands for one is cut where a character ends, and is
  // as long as an id may be at the most.
  EXPECT_EQ(f.Texts(), Strings({"A [small picture] here, [] [].", "[alone]", "[" + longAlt.substr(0, 254) + "]",
                                "None:", "[" + most + "]", "[" + most + "]"}));
  // A picture is a character of its block.
  EXPECT_EQ(f.blocks.at(1).characters, 1U);
  EXPECT_EQ(f.blocks.at(0).characters, 14U);
}

// ---- Blocks.

TEST(ChapterImport, GivesEachBlockItsKindAndItsRecord) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<h2>Heading</h2><p class='c'>Centred</p><ol start='4'><li>Four</li><li><p>Five</p></li></ol>"
                          "<blockquote><p>Quoted</p></blockquote><hr/><table><tr><td>Cell</td></tr></table>"
                          "<p class='w'>Narrow</p><p dir='rtl'>Right to left</p><pre>As written</pre>"
                          "<div class='pb'><p>On a new page</p></div><div class='fl'>Floated</div>"),
                     "p { text-indent: 1.5em; margin: 0 } .c { text-align: center; line-height: 1.5; font-size: 120% }"
                     ".w { width: 50%; margin: 1em auto } .pb { page-break-before: always; margin-left: 2em }"
                     ".fl { float: right; break-inside: avoid }"),
            ImportStatus::kOk);
  ASSERT_EQ(f.blocks.size(), 12U);
  const auto has = [&f](std::size_t block, std::uint16_t field) { return Has(f.blocks.at(block).style.has, field); };
  const auto book = [&f](std::size_t block, std::uint16_t field) {
    return Has(f.blocks.at(block).style.fromBook, field);
  };

  // A heading: its level, and kept with what follows because it is one.
  EXPECT_EQ(f.blocks[0].kind, SegmentBlockKind::kHeading);
  EXPECT_EQ(f.blocks[0].level, 2);
  EXPECT_EQ(f.blocks[0].style.breaking, bn::epub::kBreakKeepWithNext);
  EXPECT_FALSE(book(0, bn::epub::kBlockBreaking));
  EXPECT_FALSE(has(0, bn::epub::kBlockAlign));

  // What the book's sheet said of a paragraph.
  EXPECT_EQ(f.blocks[1].kind, SegmentBlockKind::kParagraph);
  EXPECT_EQ(f.blocks[1].level, 0);
  EXPECT_EQ(f.blocks[1].style.align, 2);
  EXPECT_TRUE(book(1, bn::epub::kBlockAlign));
  EXPECT_EQ(f.blocks[1].style.size, 1200);
  EXPECT_TRUE(book(1, bn::epub::kBlockSize));
  EXPECT_EQ(f.blocks[1].style.lineMultiple, 1500);
  // One and a half em of text a fifth larger than the reader's: 1.8 of the reader's, in 64ths.
  EXPECT_TRUE(f.blocks[1].style.indent == (SegmentLength{.em = 115, .px = 0, .percent = 0}));
  EXPECT_TRUE(book(1, bn::epub::kBlockIndent));
  EXPECT_TRUE(has(1, bn::epub::kBlockSpaceBefore));

  // Items of a list: the marker and the number, with the item's first
  // block whatever its kind.
  EXPECT_EQ(f.blocks[2].kind, SegmentBlockKind::kListItem);
  EXPECT_EQ(f.blocks[2].style.marker, 4);  // Decimal.
  EXPECT_EQ(f.blocks[2].style.markerNumber, 4U);
  EXPECT_EQ(f.blocks[2].style.listDepth, 1);
  EXPECT_FALSE(book(2, bn::epub::kBlockMarker));
  EXPECT_EQ(f.blocks[3].kind, SegmentBlockKind::kListItem);
  EXPECT_EQ(f.blocks[3].style.markerNumber, 5U);

  EXPECT_EQ(f.blocks[4].style.quoteDepth, 1);
  EXPECT_TRUE(has(4, bn::epub::kBlockMarginLeft));

  EXPECT_EQ(f.blocks[5].kind, SegmentBlockKind::kRule);
  EXPECT_EQ(f.blocks[5].items, "");
  EXPECT_EQ(f.blocks[5].characters, 1U);
  EXPECT_EQ(f.blocks[6].kind, SegmentBlockKind::kTableCell);

  EXPECT_TRUE(Has(f.blocks[7].style.breaking, bn::epub::kBreakCentred));
  EXPECT_TRUE(f.blocks[7].style.width == (SegmentLength{.em = 0, .px = 0, .percent = 3200}));
  EXPECT_TRUE(book(7, bn::epub::kBlockWidth));

  EXPECT_EQ(f.blocks[8].style.direction, 1);
  EXPECT_TRUE(has(8, bn::epub::kBlockDirection));
  EXPECT_FALSE(has(1, bn::epub::kBlockDirection));

  // White space as written is what a `pre` is, and not the book's.
  EXPECT_EQ(f.blocks[9].style.breaking, bn::epub::kBreakSpaceAsWritten);
  EXPECT_FALSE(book(9, bn::epub::kBlockBreaking));

  EXPECT_EQ(f.blocks[10].style.breaking, bn::epub::kBreakPageBefore);
  EXPECT_TRUE(book(10, bn::epub::kBlockBreaking));
  EXPECT_TRUE(f.blocks[10].style.marginLeft == (SegmentLength{.em = 128, .px = 0, .percent = 0}));
  EXPECT_TRUE(book(10, bn::epub::kBlockMarginLeft));

  EXPECT_EQ(f.blocks[11].style.breaking, std::uint32_t{bn::epub::kBreakFloated} | bn::epub::kBreakKeepTogether);
  EXPECT_TRUE(book(11, bn::epub::kBlockBreaking));
}

TEST(ChapterImport, GivesNothingSaidOfAlignmentAsTheSideLinesStartFromWhereTheBookSaidSo) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Nothing said</p><p class='i'>Said to be nothing</p><p class='s'>Start</p>"),
                     ".i { text-align: initial } .s { text-align: start }"),
            ImportStatus::kOk);
  EXPECT_FALSE(Has(f.blocks.at(0).style.has, bn::epub::kBlockAlign));
  EXPECT_TRUE(Has(f.blocks.at(1).style.has, bn::epub::kBlockAlign));
  EXPECT_EQ(f.blocks.at(1).style.align, 4);
  EXPECT_EQ(f.blocks.at(2).style.align, 4);
}

TEST(ChapterImport, WritesWhatTheBookSaidEvenWhereItSaidNothingNew) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Lines as the reader has them</p><p class='n'>Lines the book calls normal</p>"
                          "<h2>Kept with what follows</h2><h2 class='loose'>Let go by the book</h2><p>Text</p>"
                          "<p class='same'>The reader's own size, said by the book</p>"
                          "<p class='keep'>Kept with the next by the book</p><p class='nothing'>No size at all</p>"),
                     ".n { line-height: normal } .loose { page-break-after: auto } .same { font-size: 100% }"
                     ".keep { page-break-after: avoid } .nothing { font-size: 0 }"),
            ImportStatus::kOk);
  ASSERT_EQ(f.blocks.size(), 8U);
  // A size that is the reader's own is written if the book said it.
  EXPECT_FALSE(Has(f.blocks[4].style.has, bn::epub::kBlockSize));
  EXPECT_TRUE(Has(f.blocks[5].style.has, bn::epub::kBlockSize));
  EXPECT_TRUE(Has(f.blocks[5].style.fromBook, bn::epub::kBlockSize));
  EXPECT_EQ(f.blocks[5].style.size, 1000);
  // What the book keeps with the next is the book's, as a heading's own
  // keeping is not.
  EXPECT_EQ(f.blocks[6].style.breaking, bn::epub::kBreakKeepWithNext);
  EXPECT_TRUE(Has(f.blocks[6].style.fromBook, bn::epub::kBlockBreaking));
  // Text of no size is of no size its block's could be so much of: it
  // is written as plain.
  EXPECT_EQ(f.blocks[7].style.size, 0);
  EXPECT_EQ(f.blocks[7].items, "No size at all");
  // The book's "normal" is the reader's own distance, and the book's.
  EXPECT_FALSE(Has(f.blocks[0].style.has, bn::epub::kBlockLineHeight));
  EXPECT_TRUE(Has(f.blocks[1].style.has, bn::epub::kBlockLineHeight));
  EXPECT_TRUE(Has(f.blocks[1].style.fromBook, bn::epub::kBlockLineHeight));
  EXPECT_EQ(f.blocks[1].style.lineMultiple, 0);
  EXPECT_TRUE(f.blocks[1].style.lineHeight == SegmentLength{});
  // A heading the book lets go of is not kept, and that is the book's.
  EXPECT_EQ(f.blocks[2].style.breaking, bn::epub::kBreakKeepWithNext);
  EXPECT_FALSE(Has(f.blocks[2].style.fromBook, bn::epub::kBlockBreaking));
  EXPECT_TRUE(Has(f.blocks[3].style.has, bn::epub::kBlockBreaking));
  EXPECT_EQ(f.blocks[3].style.breaking, 0);
  EXPECT_TRUE(Has(f.blocks[3].style.fromBook, bn::epub::kBlockBreaking));
  EXPECT_FALSE(Has(f.blocks[4].style.has, bn::epub::kBlockBreaking));
}

TEST(ChapterImport, SaysWhereABlockBeganByTheTagBeforeIt) {
  ImportFixture f;
  const std::string document = Page(
      "<div>Straight in. <p>In  a paragraph</p> After it.<hr/><br/></div><p><i>x</i></p>"
      "<p><img alt='p'/></p>");
  ASSERT_EQ(f.Import(document), ImportStatus::kOk);
  const auto at = [&document](std::string_view what) { return static_cast<std::uint32_t>(document.find(what)); };
  ASSERT_EQ(f.blocks.size(), 7U);
  EXPECT_EQ(f.blocks[0].source, at("<div>"));
  EXPECT_EQ(f.blocks[1].source, at("<p>In"));
  EXPECT_EQ(f.blocks[2].source, at("</p> After"));
  EXPECT_EQ(f.blocks[3].source, at("<hr/>"));
  EXPECT_EQ(f.blocks[4].source, at("<br/>"));
  EXPECT_EQ(f.blocks[5].source, at("<i>"));
  EXPECT_EQ(f.blocks[6].source, at("<img"));
  EXPECT_EQ(f.chapter.blocks, 7U);
}

// ---- A capital that is dropped.

const std::string_view kCapitals =
    ".f::first-letter { float: left; font-size: 3em; font-weight: bold }"
    ".r::first-letter { font-size: 2em } .d { float: left; font-size: 3em }";

TEST(ChapterImport, SetsABlocksFirstLetterAsItsRuleSaysAndSaysItIsDroppedIfTheRuleFloatsIt) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p class='f'>The first.</p><p class='r'>Raised.</p><p>No rule.</p>"
                          "<div class='f'><p>Inside.</p><p>Second inside.</p></div>"),
                     kCapitals),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"{1/1/0/0/3000}T{0/0/0/0/1000}he first.", "{0/0/0/0/2000}R{0/0/0/0/1000}aised.",
                                "No rule.", "{1/1/0/0/3000}I{0/0/0/0/1000}nside.", "Second inside."}));
  EXPECT_EQ(f.Dropped(0), 1U);
  EXPECT_TRUE(Has(f.blocks[0].style.fromBook, bn::epub::kBlockBreaking));
  EXPECT_EQ(f.Dropped(1), 0U);  // Its rule does not float it.
  EXPECT_EQ(f.Dropped(2), 0U);
  EXPECT_EQ(f.Dropped(3), 1U);
  EXPECT_EQ(f.Dropped(4), 0U);
  EXPECT_EQ(f.importer.Dropped(), 2U);
}

TEST(ChapterImport, TakesOneOrTwoMarksBeforeAFirstLetterWithItAndADigitAsALetter) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p class='f'>&ldquo;Quoted.</p><p class='f'>&ldquo;&lsquo;Twice.</p>"
                          "<p class='f'>&ldquo;&lsquo;'Three.</p><p class='f'>1900.</p><p class='f'>&hellip;no.</p>"
                          "<p class='f'> <b> B</b>old.</p><p class='f'>&laquo;<b>S</b>et apart.</p>"
                          "<p class='f'>&#201;cole</p><p class='f'>&#937;mega</p><p class='f'>&#1046;ivago</p>"
                          "<p class='f'>&#215; is a sign</p><p class='f'>&#7842;</p><p class='f'>'</p>"),
                     kCapitals),
            ImportStatus::kOk);
  const std::string letter = "{1/1/0/0/3000}";
  const std::string plain = "{0/0/0/0/1000}";
  EXPECT_EQ(
      f.Texts(),
      Strings({letter + "\xE2\x80\x9CQ" + plain + "uoted.", letter + "\xE2\x80\x9C\xE2\x80\x98T" + plain + "wice.",
               "\xE2\x80\x9C\xE2\x80\x98'Three.", letter + "1" + plain + "900.", "\xE2\x80\xA6no.",
               letter + "B" + plain + "old.", "\xC2\xAB{1/0/0/0/1000}S" + plain + "et apart.",
               letter + "\xC3\x89" + plain + "cole", letter + "\xCE\xA9" + plain + "mega",
               letter + "\xD0\x96" + plain + "ivago", "\xC3\x97 is a sign", letter + "\xE1\xBA\xA2", "'"}));
  const std::vector<unsigned> dropped = {2, 3, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0};
  for (std::size_t block = 0; block < dropped.size(); ++block) {
    EXPECT_EQ(f.Dropped(block), dropped[block]) << block;
  }
}

TEST(ChapterImport, GivesAFirstLetterNoStyleWhereSomethingElseComesFirst) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p class='f'><img alt='p'/><a id='after'/>Picture.</p><p class='f'><br/>Line.</p>"
                          "<pre class='f'>  Held white space.</pre>"),
                     kCapitals),
            ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"[p]Picture.", "\\nLine.", "{0/0/3/0/1000}  Held white space."}));
  EXPECT_EQ(f.importer.Dropped(), 0U);
  // The block waits no longer once the picture is written: an id after it
  // is after it.
  EXPECT_EQ(f.anchors, Strings({"after 0 1"}));
}

TEST(ChapterImport, TakesEveryKindOfQuotationMarkBeforeAFirstLetter) {
  ImportFixture f;
  const std::vector<std::string> marks = {"&quot;",  "'",       "&#161;",  "&#171;",  "&#187;",  "&#191;",  "&#8216;",
                                          "&#8217;", "&#8218;", "&#8220;", "&#8221;", "&#8222;", "&#8249;", "&#8250;"};
  std::string body;
  for (const std::string& mark : marks) {
    body += "<p class='f'>" + mark + "A</p>";
  }
  // And what is no mark: a dash, an opening bracket, an asterisk.
  body += "<p class='f'>&#8212;A</p><p class='f'>(A</p><p class='f'>*A</p>";
  ASSERT_EQ(f.Import(Page(body), kCapitals), ImportStatus::kOk);
  ASSERT_EQ(f.blocks.size(), marks.size() + 3);
  for (std::size_t block = 0; block < f.blocks.size(); ++block) {
    EXPECT_EQ(f.Dropped(block), block < marks.size() ? 2U : 0U) << block;
  }
}

TEST(ChapterImport, TakesALetterOfTheLatinGreekOrCyrillicAlphabetsAndNoOther) {
  ImportFixture f;
  // The first and the last of each run of letters, and what stands just
  // outside each or is a sign among them.
  const std::vector<std::uint32_t> letters = {'A',   'Z',   'a',   'z',   0xC0,   0xD6,  0xD8,  0xF6,
                                              0xF8,  0x24F, 0x370, 0x37D, 0x37F,  0x386, 0x388, 0x3FF,
                                              0x400, 0x481, 0x48A, 0x52F, 0x1E00, 0x1FFF};
  const std::vector<std::uint32_t> others = {'@',   '[',   '`',   '{',   '~',   0xBE,   0xD7,   0xF7,   0x250,  0x36F,
                                             0x37E, 0x387, 0x482, 0x489, 0x530, 0x1DFF, 0x2000, 0x4E00, 0x1F600};
  std::string body;
  for (const std::vector<std::uint32_t>* group : {&letters, &others}) {
    for (const std::uint32_t character : *group) {
      body += "<p><span class='d'>&#" + std::to_string(character) + ";</span>text</p>";
    }
  }
  ASSERT_EQ(f.Import(Page(body), kCapitals), ImportStatus::kOk);
  ASSERT_EQ(f.blocks.size(), letters.size() + others.size());
  for (std::size_t block = 0; block < f.blocks.size(); ++block) {
    EXPECT_EQ(f.Dropped(block), block < letters.size() ? 1U : 0U) << block;
  }
}

TEST(ChapterImport, LooksAtFloatedTextBeforeAFirstLettersRuleWhereABlockHasBoth) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p class='f'><span class='d'>T</span>wo ways at once.</p>"
                          "<p class='f'><span class='d'>No</span> capital.</p>"),
                     kCapitals),
            ImportStatus::kOk);
  // The floated letter is set as its element is, and not as the rule says.
  EXPECT_EQ(f.Texts(),
            Strings({"{0/0/0/0/3000}T{0/0/0/0/1000}wo ways at once.", "{0/0/0/0/3000}No{0/0/0/0/1000} capital."}));
  EXPECT_EQ(f.Dropped(0), 1U);
  EXPECT_EQ(f.Dropped(1), 0U);
}

TEST(ChapterImport, SaysABlockBeginsWithADroppedCapitalWhenItsFloatedTextIsOneLetter) {
  ImportFixture f;
  ASSERT_EQ(
      f.Import(
          Page("<p><span class='d'>T</span>he letter.</p><p><span class='d'>&ldquo;T</span>he mark.</p>"
               "<p><span class='d'>T </span>he space.</p><p><span class='d'>Th</span>e two.</p>"
               "<p><span class='d'>7</span> is a digit.</p><p><span class='d'>&ldquo;</span>A mark alone.</p>"
               "<p><span class='d'></span>Nothing.</p><p><span class='d'><b>&ldquo;</b>S</span>et apart.</p>"
               "<p><span class='d'>E<br/></span>A line end.</p><p><span class='d'>F</span></p>"
               "<p>Not <span class='d'>A</span>t the start.</p><p><span class='d'>&ldquo;&ldquo;&ldquo;T</span></p>"),
          kCapitals),
      ImportStatus::kOk);
  const std::string big = "{0/0/0/0/3000}";
  const std::string plain = "{0/0/0/0/1000}";
  EXPECT_EQ(
      f.Texts(),
      Strings({big + "T" + plain + "he letter.", big + "\xE2\x80\x9CT" + plain + "he mark.",
               big + "T " + plain + "he space.", big + "Th" + plain + "e two.", big + "7" + plain + " is a digit.",
               big + "\xE2\x80\x9C" + plain + "A mark alone.", "Nothing.",
               "{1/0/0/0/3000}\xE2\x80\x9C" + big + "S" + plain + "et apart.", big + "E\\n" + plain + "A line end.",
               big + "F", "Not " + big + "A" + plain + "t the start.", big + "\xE2\x80\x9C\xE2\x80\x9C\xE2\x80\x9CT"}));
  const std::vector<unsigned> dropped = {1, 2, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0};
  for (std::size_t block = 0; block < dropped.size(); ++block) {
    EXPECT_EQ(f.Dropped(block), dropped[block]) << block;
  }
  EXPECT_EQ(f.importer.Dropped(), 4U);
}

TEST(ChapterImport, TakesAFloatedPictureAsACapitalIfItIsAllThatIsFloated) {
  ImportFixture f;
  const std::string longAlt(ChapterImporter::kMostFirst + 1, 'a');
  ASSERT_EQ(
      f.Import(
          Page("<p><span class='d'><img alt='J'/></span>ust.</p><p><img class='d' alt='K'/>Itself.</p>"
               "<p><span class='d'><img alt='L'/>x</span>More.</p><p><span class='d'><img alt='M'/><img/></span></p>"
               "<p><span class='d'><img alt='" +
               longAlt +
               "'/></span>Long.</p><p><span class='d'> <img/> </span>Spaces.</p>"
               "<p><span class='d'>&ldquo;<img alt='N'/></span>After a mark.</p>"
               "<p><span class='d'><img/><img alt='P'/></span></p>"
               "<pre>  <span class='d'><img alt='Q'/></span>after white space that is held</pre>"),
          kCapitals),
      ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"[J]ust.", "[K]Itself.", "[L]{0/0/0/0/3000}x{0/0/0/0/1000}More.", "[M][]",
                                "[" + longAlt + "]Long.", "[]{0/0/0/0/3000} {0/0/0/0/1000}Spaces.",
                                "{0/0/0/0/3000}\xE2\x80\x9C[N]{0/0/0/0/1000}After a mark.", "[][P]",
                                "{0/0/3/0/3000}  [Q]{0/0/3/0/1000}after white space that is held"}));
  const std::vector<unsigned> dropped = {1, 1, 0, 0, 0, 1, 0, 0, 0};
  for (std::size_t block = 0; block < dropped.size(); ++block) {
    EXPECT_EQ(f.Dropped(block), dropped[block]) << block;
  }
}

TEST(ChapterImport, PutsAnIdAtTheStartOfABlockWhoseFirstCharactersMayYetBeACapital) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p class='f' id='p'>&ldquo;<a id='a'/>A<a id='b'/> capital.</p>"
                          "<p><span class='d' id='s'>D<a id='c'/></span><a id='d'/>ropped.</p>"
                          "<p class='f'>&ldquo;<a id='e'/>&hellip;<a id='g'/>none.</p>"),
                     kCapitals),
            ImportStatus::kOk);
  EXPECT_EQ(f.Dropped(0), 2U);
  EXPECT_EQ(f.Dropped(1), 1U);
  EXPECT_EQ(f.Dropped(2), 0U);
  EXPECT_EQ(f.anchors, Strings({"p 0 0", "a 0 0", "b 0 4", "s 1 0", "c 1 0", "d 1 1", "e 2 0", "g 2 6"}));
}

// ---- What is left out, and what goes wrong.

TEST(ChapterImport, WritesNothingOfWhatIsNotShown) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>Before <span class='gone'> hidden <br/><img alt='x'/></span> after.</p>"
                          "<p class='gone'>Nothing</p><p hidden=''>Nor this</p><script>var x;</script>"
                          "<pre><span class='gone'>   </span>shown</pre>"),
                     ".gone { display: none }"),
            ImportStatus::kOk);
  // The title in the head is nothing either; nor is white space as
  // written, in what is nothing.
  EXPECT_EQ(f.Texts(), Strings({"Before after.", "{0/0/3/0/1000}shown"}));
  EXPECT_EQ(f.chapter.characters, 18U);
}

TEST(ChapterImport, LeavesOutWhatIsWrittenInsideARule) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page(
                "<p>Before</p><hr>Inside <b id='in'>a rule</b><br/><img alt='x'/> <pre>  y</pre></hr><p>After</p>")),
            ImportStatus::kOk);
  // The `pre` is a block of its own, though it is written inside.
  EXPECT_EQ(f.Texts(), Strings({"Before", "", "{0/0/3/0/1000}  y", "After"}));
  EXPECT_EQ(f.blocks.at(1).kind, SegmentBlockKind::kRule);
  EXPECT_EQ(f.blocks.at(1).characters, 1U);
  EXPECT_EQ(f.anchors, Strings({"in 1 0"}));
}

TEST(ChapterImport, ImportsADocumentWithNothingInItAsAWholeChapterOfNoBlocks) {
  ImportFixture f;
  ASSERT_EQ(f.Import(Page(" <div> <p></p> </div> ")), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kWhole);
  EXPECT_EQ(f.chapter.blocks, 0U);
  EXPECT_EQ(f.chapter.characters, 0U);
  EXPECT_TRUE(f.blocks.empty());
}

TEST(ChapterImport, UsesTheSheetsTheDocumentLinksAndItsOwnStyle) {
  ImportFixture f;
  f.book.Put("text/more.css", "p { text-align: right }");
  ASSERT_EQ(f.Import("<html><head><link rel='stylesheet' href='book.css'/><link rel='stylesheet' href='more.css'/>"
                     "<style>i { font-weight: bold }</style></head><body><p>One <i>two</i></p></body></html>",
                     "p { text-indent: 2em }"),
            ImportStatus::kOk);
  EXPECT_EQ(f.blocks.at(0).style.align, 1);
  EXPECT_TRUE(f.blocks.at(0).style.indent == (SegmentLength{.em = 128, .px = 0, .percent = 0}));
  EXPECT_EQ(f.Texts(), Strings({"One {3/1/0/0/1000}two"}));
  EXPECT_EQ(f.importer.Path(), kChapter);
}

TEST(ChapterImport, ImportsOneChapterAfterAnotherWithTheBooksSheetsReadOnce) {
  ImportFixture f;
  f.newBook = false;
  f.sheets.BeginBook();
  f.book.Put("text/second.xhtml", Page("<p>The second.</p>"));
  ASSERT_EQ(f.Import(Page("<p>The first.</p>"), "p { text-align: center }", 0), ImportStatus::kOk);
  const BundleChapter first = f.chapter;
  EXPECT_EQ(f.blocks.at(0).style.align, 2);
  const std::size_t opens = f.book.opens;
  ASSERT_EQ(f.ImportNamed("text/second.xhtml", 1), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"The second."}));
  EXPECT_EQ(f.blocks.at(0).style.align, 2);
  EXPECT_EQ(f.book.opens - opens, 2U);  // The document twice, and the sheet not again.
  EXPECT_GT(f.chapter.segmentOffset, first.segmentOffset);

  // Both are in the bundle.
  BundleChapter read;
  ASSERT_EQ(f.bundle.ReadChapter(0, read), BundleStatus::kOk);
  EXPECT_EQ(read.state, ChapterState::kWhole);
  EXPECT_EQ(read.segmentOffset, first.segmentOffset);
  ASSERT_EQ(f.bundle.ReadChapter(1, read), BundleStatus::kOk);
  EXPECT_EQ(read.blocks, 1U);
  ASSERT_EQ(f.bundle.ReadChapter(2, read), BundleStatus::kOk);
  EXPECT_EQ(read.state, ChapterState::kNotImported);
}

TEST(ChapterImport, CutsShortADocumentThatIsNotXmlFromSomewhereOn) {
  ImportFixture f;
  const std::string document = Page("<p id='a'>The first.</p><p>The second, <b>and  <<< no more");
  ASSERT_EQ(f.Import(document), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kCutShort);
  EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kNotXml));
  EXPECT_EQ(f.importer.Reason(), ImportReason::kNotXml);
  // What came before is kept, and the block that was open ends there.
  EXPECT_EQ(f.Texts(), Strings({"The first.", "The second, {1/0/0/0/1000}and"}));
  EXPECT_EQ(f.anchors, Strings({"a 0 0"}));
}

TEST(ChapterImport, CutsShortADocumentThatCannotBeReadToItsEnd) {
  ImportFixture f;
  const std::string document = Page("<p>The first.</p><p>The second.</p><p>The third.</p>");
  f.book.FailAfter(kChapter, document.find("<p>The third"));
  ASSERT_EQ(f.Import(document), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kCutShort);
  EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kUnreadable));
  EXPECT_EQ(f.Texts(), Strings({"The first.", "The second."}));
}

TEST(ChapterImport, RefusesADocumentOfWhichNothingWasRead) {
  {
    ImportFixture f;
    ASSERT_EQ(f.Import("This is not XML. <"), ImportStatus::kOk);
    EXPECT_EQ(f.chapter.state, ChapterState::kRefused);
    EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kNotXml));
    EXPECT_EQ(f.chapter.blocks, 0U);
    EXPECT_EQ(f.chapter.segmentBytes, 0U);
    BundleChapter read;
    ASSERT_EQ(f.bundle.ReadChapter(0, read), BundleStatus::kOk);
    EXPECT_EQ(read.state, ChapterState::kRefused);
  }
  {
    ImportFixture f;
    ASSERT_EQ(f.ImportNamed("text/no-such-file.xhtml"), ImportStatus::kOk);
    EXPECT_EQ(f.chapter.state, ChapterState::kRefused);
    EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kNoFile));
    EXPECT_EQ(f.importer.Reason(), ImportReason::kNoFile);
    // And a document after it is imported with no reason left over.
    ASSERT_EQ(f.Import(Page("<p>Text</p>")), ImportStatus::kOk);
    EXPECT_EQ(f.chapter.state, ChapterState::kWhole);
    EXPECT_EQ(f.chapter.reason, 0);
    EXPECT_EQ(f.importer.Reason(), ImportReason::kNone);
  }
  {
    ImportFixture f;
    const std::string document = Page("<p>Never reached.</p>");
    f.book.FailAfter(kChapter, document.find("<body>"));
    ASSERT_EQ(f.Import(document), ImportStatus::kOk);
    EXPECT_EQ(f.chapter.state, ChapterState::kRefused);
    EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kUnreadable));
  }
}

TEST(ChapterImport, SaysThatTheParserHadTooLittleMemoryForADocument) {
  ImportFixture f;
  // An element's name longer than the parser has room for.
  ASSERT_EQ(f.Import(Page("<p>Before.</p><" + std::string(40000, 'x') + "/><p>After.</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kCutShort);
  EXPECT_EQ(f.chapter.reason, static_cast<std::uint8_t>(ImportReason::kNoMemory));
  EXPECT_EQ(f.Texts(), Strings({"Before."}));
}

TEST(ChapterImport, CountsWhatWasLeftOutForBeingTooDeep) {
  ImportFixture f;
  std::string deep;
  for (std::size_t depth = 0; depth < ChapterImporter::kLevels + 3; ++depth) {
    deep += "<span>";
  }
  deep += "deep";
  for (std::size_t depth = 0; depth < ChapterImporter::kLevels + 3; ++depth) {
    deep += "</span>";
  }
  ASSERT_EQ(f.Import(Page("<p>" + deep + "</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"deep"}));
  EXPECT_EQ(f.importer.Blocks().Styles().LeftOut(bn::epub::StyleLeftOut::kTooDeep), 6U);
  EXPECT_EQ(f.importer.CascadeTooDeep(), 0U);
  EXPECT_EQ(f.chapter.leftOut, 6U);

  // With rules enough that ask something of the elements around, the
  // cascade keeps fewer open elements than there are, and what it does not
  // keep is counted too.
  std::string sheet;
  for (int rule = 0; rule < 250; ++rule) {
    sheet += ".a" + std::to_string(rule) + " .c" + std::to_string(rule) + " { margin: 0 } ";
  }
  ASSERT_EQ(f.Import(Page("<p>" + deep + "</p>"), sheet), ImportStatus::kOk);
  EXPECT_GT(f.importer.CascadeTooDeep(), 0U);
  EXPECT_EQ(f.chapter.leftOut, 6U + f.importer.CascadeTooDeep());
}

TEST(ChapterImport, CountsALanguageThereWasNoRoomFor) {
  ImportFixture f;
  std::string body;
  for (int language = 0; language < 20; ++language) {
    body += "<p xml:lang='x" + std::to_string(language) + "'>Text.</p>";
  }
  ASSERT_EQ(f.Import(Page(body, " lang='en'")), ImportStatus::kOk);
  // The book's own, and fourteen more.
  EXPECT_EQ(f.languages.size(), 14U);
  EXPECT_EQ(f.importer.Blocks().Styles().LeftOut(bn::epub::StyleLeftOut::kLanguage), 6U);
  EXPECT_EQ(f.chapter.leftOut, 6U);
}

TEST(ChapterImport, BeginsAfreshAfterAnImportThatFailedPartWay) {
  // A document that leaves the importer with something of everything in
  // hand wherever it stops: a space that waits, white space that is held,
  // text set in a way and a language of its own, a block that waits to
  // know whether it begins with a capital.
  std::string body;
  for (int i = 0; i < 300; ++i) {
    body += "<p xml:lang='fr' id='p" + std::to_string(i) +
            "'><b>gras</b> et <i>italique </i></p><pre>   <b>x  </b>  </pre>"
            "<p class='f'>&ldquo;<a id='a" +
            std::to_string(i) + "'/></p><p><span class='d'>T</span>exte &nosuch; </p>";
  }
  const std::string second = Page(
      "<p>Plain <i xml:lang='de'>kursiv</i> </p><pre>  as written</pre>"
      "<p class='f'>Capital.</p><p id='z'>End</p>");
  ImportFixture clean;
  ASSERT_EQ(clean.Import(second, kCapitals), ImportStatus::kOk);

  for (const int failing : {2, 3, 5, 7, 11, 13}) {
    ImportFixture f;
    f.book.Put(kChapter, Page(body));
    f.book.Put("text/book.css", kCapitals);
    FakeDocument document(f.book, kChapter);
    const std::size_t before = f.storage.written.size();
    ASSERT_EQ(f.importer.Begin(document, f.sheets, f.xml, f.file, f.bundle.DataEnd(), "en"), ImportStatus::kOk);
    for (bool more = true; more;) {
      if (f.storage.written.size() - before == static_cast<std::size_t>(failing)) {
        f.storage.failWrites = StorageStatus::kFailed;
      }
      more = f.importer.Step();
    }
    ASSERT_EQ(f.importer.Status(), ImportStatus::kStoreFailed) << failing;

    // The same importer, and storage that works again.
    f.storage.failWrites = StorageStatus::kOk;
    ASSERT_EQ(f.Import(second, kCapitals), ImportStatus::kOk) << failing;
    EXPECT_EQ(f.Texts(), clean.Texts()) << failing;
    EXPECT_EQ(f.anchors, clean.anchors) << failing;
    EXPECT_EQ(f.languages, clean.languages) << failing;
    EXPECT_EQ(f.chapter.leftOut, clean.chapter.leftOut) << failing;
    EXPECT_EQ(f.importer.Dropped(), clean.importer.Dropped()) << failing;
    EXPECT_EQ(f.importer.LeftOut(ImportLeftOut::kEntity), 0U) << failing;
    for (std::size_t block = 0; block < f.blocks.size() && block < clean.blocks.size(); ++block) {
      EXPECT_TRUE(f.blocks[block].style == clean.blocks[block].style) << failing << " " << block;
      EXPECT_EQ(f.blocks[block].source, clean.blocks[block].source) << failing << " " << block;
    }
  }
}

// Storage failing at each of a chapter's writes in turn.
void FailAtEveryWrite(const std::string& document, std::uint32_t blocks) {
  ImportFixture sound;
  ASSERT_EQ(sound.Import(document), ImportStatus::kOk);
  EXPECT_EQ(sound.chapter.blocks, blocks);

  // Storage failing at each of the chapter's writes in turn: its blocks,
  // and at the end its index, its ids and its directory.
  const int all = static_cast<int>(sound.importWrites);
  ASSERT_GT(all, 4);
  for (int failing = 0; failing < all; ++failing) {
    ImportFixture f;
    const std::size_t before = f.storage.written.size();
    f.book.Put(kChapter, document);
    f.book.Put("text/book.css", "");
    FakeDocument source(f.book, kChapter);
    ASSERT_EQ(f.importer.Begin(source, f.sheets, f.xml, f.file, f.bundle.DataEnd(), "en"), ImportStatus::kOk);
    bool more = true;
    while (more) {
      if (f.storage.written.size() - before == static_cast<std::size_t>(failing)) {
        f.storage.failWrites = StorageStatus::kFailed;
      }
      more = f.importer.Step();
    }
    EXPECT_EQ(f.importer.Status(), ImportStatus::kStoreFailed) << failing;
    EXPECT_FALSE(f.importer.Step());
  }
}

TEST(ChapterImport, SaysThatStorageFailed) {
  // A chapter of many pages of blocks.
  std::string paragraphs;
  for (int i = 0; i < 400; ++i) {
    paragraphs += "<p>A paragraph of the chapter, long enough that a few make a page.</p>";
  }
  FailAtEveryWrite(Page(paragraphs), 400);
  // And one whose blocks are less than its index and its ids, which are
  // written when the blocks have ended.
  std::string shortOnes;
  for (int i = 0; i < 255; ++i) {
    shortOnes += "<p id='a-paragraph-of-the-chapter-" + std::to_string(i) + "'>x</p>";
  }
  FailAtEveryWrite(Page(shortOnes), 255);
}

TEST(ChapterImport, RefusesADocumentThatOpensOnceAndNotAgain) {
  ImportFixture f;
  f.book.Put(kChapter, Page("<p>Text</p>"));
  f.book.Put("text/book.css", "");
  FakeDocument document(f.book, kChapter);
  document.failsAtOpen = 2;
  ASSERT_EQ(f.importer.Begin(document, f.sheets, f.xml, f.file, f.bundle.DataEnd(), "en"), ImportStatus::kOk);
  while (f.importer.Step()) {
  }
  EXPECT_EQ(f.importer.Status(), ImportStatus::kOk);
  EXPECT_EQ(f.importer.Chapter().state, ChapterState::kRefused);
  EXPECT_EQ(f.importer.Reason(), ImportReason::kNoFile);
  EXPECT_EQ(document.opens, 2);
}

TEST(ChapterImport, SaysItWasMisusedWhenGivenAPlaceThatIsNoSegments) {
  ImportFixture f;
  f.book.Put(kChapter, Page("<p>Text</p>"));
  f.book.Put("text/book.css", "");
  FakeDocument document(f.book, kChapter);
  // Not where a sector begins.
  ASSERT_EQ(f.importer.Begin(document, f.sheets, f.xml, f.file, f.bundle.DataEnd() + 7, "en"), ImportStatus::kOk);
  const std::size_t written = f.storage.written.size();
  while (f.importer.Step()) {
  }
  EXPECT_EQ(f.importer.Status(), ImportStatus::kMisused);
  EXPECT_EQ(f.storage.written.size(), written);
}

TEST(ChapterImport, RefusesToBeginWithMemoryOfTheWrongSize) {
  ImportFixture f;
  alignas(4) std::array<std::uint8_t, ChapterImporter::kWorkBytes - 4> small{};
  ChapterImporter importer(f.rules, small);
  f.book.Put(kChapter, Page("<p>Text</p>"));
  FakeDocument document(f.book, kChapter);
  EXPECT_EQ(importer.Begin(document, f.sheets, f.xml, f.file, f.bundle.DataEnd(), "en"), ImportStatus::kMisused);
  EXPECT_FALSE(importer.Step());
  EXPECT_EQ(importer.Status(), ImportStatus::kMisused);
  EXPECT_EQ(document.opens, 0);
}

TEST(ChapterImport, DoesNothingAtAStepBeforeItHasBegun) {
  ImportFixture f;
  const std::size_t written = f.storage.written.size();
  EXPECT_FALSE(f.importer.Step());
  EXPECT_EQ(f.importer.Status(), ImportStatus::kMisused);
  EXPECT_EQ(f.storage.written.size(), written);
  EXPECT_EQ(f.book.opens, 0U);
}

TEST(ChapterImport, WritesNoMoreAtAStepThanItSaysWhateverTheDocument) {
  // Blocks as close together as a document can have them, each with as
  // long a record as the book's sheet can give it.
  std::string body = "<div class='all'>";
  for (int i = 0; i < 3000; ++i) {
    body += "<hr/>a";
  }
  body += "</div>";
  ImportFixture f;
  ASSERT_EQ(f.Import(Page(body),
                     ".all { text-align: right; direction: rtl; font-size: 3em; line-height: 2em; text-indent: 1em;"
                     " margin: 1em 2em; padding: 0 1em; page-break-inside: avoid; float: left; width: 50%;"
                     " font-style: italic } hr { margin: 1em auto; width: 50%; page-break-before: always }"),
            ImportStatus::kOk);
  EXPECT_EQ(f.chapter.blocks, 6000U);
  // The fixture held every step to the most it may write, which is worked
  // out for the longest record a block may have. With the records that
  // importing writes, this document is as near it as one comes.
  EXPECT_GE(f.mostPages, 8U);
  EXPECT_LE(f.mostPages, ChapterImporter::kMostPagesPerStep);
  EXPECT_LE(f.mostRead, ChapterImporter::kMostBytesPerStep);
}

TEST(ChapterImport, WritesAParagraphLongerThanAnyBufferItHas) {
  std::string text;
  while (text.size() < 200000) {
    text += "words and more words \xE2\x80\x94 ";
  }
  ImportFixture f;
  ASSERT_EQ(f.Import(Page("<p>" + text + "</p><pre>" + text + "</pre>")), ImportStatus::kOk);
  ASSERT_EQ(f.blocks.size(), 2U);
  text.pop_back();  // The space at its end.
  EXPECT_EQ(f.blocks[0].items, text);
  EXPECT_EQ(f.blocks[1].items, "{0/0/3/0/1000}" + text + " ");
}

TEST(ChapterImport, CutsAPieceOfTextLongerThanTheWriterTakesWhereACharacterEnds) {
  // Text comes from the parser in pieces of a kilobyte or less, but for
  // what it had to keep back behind something long: here a comment. The
  // word that follows comes in one piece of several kilobytes, more than
  // the writer takes at once; and where the most it takes ends, the word
  // has its one character of two bytes.
  const std::string word = std::string(bn::epub::kSegmentMostText - 1, 'x') + "\xCE\xA9" + std::string(5000, 'x');
  ImportFixture f(131072);
  ASSERT_EQ(f.Import(Page("<!--" + std::string(9000, 'c') + "--><p>" + word + "</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kWhole);
  EXPECT_EQ(f.Texts(), Strings({word}));
  EXPECT_EQ(f.chapter.characters, word.size());
  // The writer counts a character only where it is given it whole: the
  // omega is of the fourth row of characters, and nothing else is.
  EXPECT_EQ(f.chapter.characterRows.at(0), 0x09);
}

TEST(ChapterImport, BeginsAfreshAfterAnImportThatFailedWithABlockInHand) {
  // One paragraph, longer than a page: storage fails while it is open.
  std::string text;
  while (text.size() < 20000) {
    text += "<b>bold</b> and <i>italic </i>";
  }
  ImportFixture f;
  f.book.Put(kChapter, Page("<p class='f'>" + text + "</p>"));
  f.book.Put("text/book.css", kCapitals);
  FakeDocument document(f.book, kChapter);
  const std::size_t before = f.storage.written.size();
  ASSERT_EQ(f.importer.Begin(document, f.sheets, f.xml, f.file, f.bundle.DataEnd(), "en"), ImportStatus::kOk);
  for (bool more = true; more;) {
    if (f.storage.written.size() - before == 2) {
      f.storage.failWrites = StorageStatus::kFailed;
    }
    more = f.importer.Step();
  }
  ASSERT_EQ(f.importer.Status(), ImportStatus::kStoreFailed);

  // The same importer, storage that works again, and a document that has
  // no block of its own for what was in hand to be taken for.
  f.storage.failWrites = StorageStatus::kOk;
  ASSERT_EQ(f.Import(Page(" <div id='x'> </div> ")), ImportStatus::kOk);
  EXPECT_EQ(f.chapter.state, ChapterState::kWhole);
  EXPECT_EQ(f.chapter.blocks, 0U);
  EXPECT_EQ(f.importer.Dropped(), 0U);
  // And one that would show a line or a block already begun.
  ASSERT_EQ(f.Import(Page("<pre>\n  as written</pre><p> text</p>")), ImportStatus::kOk);
  EXPECT_EQ(f.Texts(), Strings({"{0/0/3/0/1000}  as written", "text"}));
}

TEST(ChapterImport, NamesItsStatusesAndReasons) {
  EXPECT_STREQ(bn::epub::ImportStatusName(ImportStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::ImportStatusName(ImportStatus::kStoreFailed), "store failed");
  EXPECT_STREQ(bn::epub::ImportStatusName(ImportStatus::kTooBig), "too big");
  EXPECT_STREQ(bn::epub::ImportStatusName(ImportStatus::kMisused), "misused");
  EXPECT_STREQ(bn::epub::ImportReasonName(ImportReason::kNone), "none");
  EXPECT_STREQ(bn::epub::ImportReasonName(ImportReason::kNoFile), "no file");
  EXPECT_STREQ(bn::epub::ImportReasonName(ImportReason::kUnreadable), "unreadable");
  EXPECT_STREQ(bn::epub::ImportReasonName(ImportReason::kNotXml), "not XML");
  EXPECT_STREQ(bn::epub::ImportReasonName(ImportReason::kNoMemory), "no memory");
}

TEST(ChapterImport, SaysWhatBecomesOfADocumentThatIsNotImportedAtAll) {
  BundleFile file;
  BundleChapter chapter;
  chapter.blocks = 7;
  // The book has no such file.
  file.kind = static_cast<std::uint8_t>(bn::epub::MediaType::kXhtml);
  EXPECT_TRUE(bn::epub::ChapterWithoutDocument(file, chapter));
  EXPECT_EQ(chapter.state, ChapterState::kRefused);
  EXPECT_EQ(chapter.reason, static_cast<std::uint8_t>(ImportReason::kNoFile));
  EXPECT_EQ(chapter.blocks, 0U);
  // It is a picture.
  file.flags = bn::epub::kBundleFilePresent;
  file.kind = static_cast<std::uint8_t>(bn::epub::MediaType::kPng);
  EXPECT_TRUE(bn::epub::ChapterWithoutDocument(file, chapter));
  EXPECT_EQ(chapter.state, ChapterState::kNotDocument);
  EXPECT_EQ(chapter.reason, 0);
  // It is a document.
  file.kind = static_cast<std::uint8_t>(bn::epub::MediaType::kXhtml);
  EXPECT_FALSE(bn::epub::ChapterWithoutDocument(file, chapter));
}

// ---- HTML's names for characters.

TEST(HtmlEntities, KnowsTheNamesHtmlGivesCharacters) {
  EXPECT_EQ(HtmlEntity("nbsp"), 0xA0U);
  EXPECT_EQ(HtmlEntity("eacute"), 0xE9U);
  EXPECT_EQ(HtmlEntity("Eacute"), 0xC9U);
  EXPECT_EQ(HtmlEntity("mdash"), 0x2014U);
  EXPECT_EQ(HtmlEntity("hellip"), 0x2026U);
  EXPECT_EQ(HtmlEntity("euro"), 0x20ACU);
  EXPECT_EQ(HtmlEntity("AElig"), 0xC6U);   // The first in the order of bytes,
  EXPECT_EQ(HtmlEntity("zwnj"), 0x200CU);  // and the last.
  EXPECT_EQ(HtmlEntity("alefsym"), 0x2135U);
  EXPECT_EQ(HtmlEntity("diams"), 0x2666U);
  EXPECT_EQ(HtmlEntity("there4"), 0x2234U);
  EXPECT_EQ(HtmlEntity("sup2"), 0xB2U);
  EXPECT_EQ(HtmlEntity("Omega"), 0x3A9U);
  EXPECT_EQ(HtmlEntity("omega"), 0x3C9U);
  EXPECT_EQ(HtmlEntity("amp"), 0x26U);
  EXPECT_EQ(HtmlEntity("lt"), 0x3CU);
  EXPECT_EQ(HtmlEntity("apos"), 0x27U);
  EXPECT_EQ(bn::epub::kHtmlEntities, 253U);
}

TEST(HtmlEntities, KnowsNoOtherName) {
  EXPECT_EQ(HtmlEntity(""), 0U);
  EXPECT_EQ(HtmlEntity("NBSP"), 0U);  // A name is as it is written.
  EXPECT_EQ(HtmlEntity("Nbsp"), 0U);
  EXPECT_EQ(HtmlEntity("nbsp;"), 0U);
  EXPECT_EQ(HtmlEntity("&nbsp"), 0U);
  EXPECT_EQ(HtmlEntity("nbs"), 0U);
  EXPECT_EQ(HtmlEntity("nbspx"), 0U);
  EXPECT_EQ(HtmlEntity("A"), 0U);
  EXPECT_EQ(HtmlEntity("zzzz"), 0U);
  EXPECT_EQ(HtmlEntity(std::string(300, 'a')), 0U);
  EXPECT_EQ(HtmlEntity(std::string_view("nbsp\0", 5)), 0U);
}

}  // namespace
