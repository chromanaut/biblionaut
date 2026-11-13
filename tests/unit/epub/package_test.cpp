#include "epub/package.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "core/metrics.h"
#include "storage_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::MediaType;
using bn::epub::Package;
using bn::epub::PackageStatus;
using bn::test::FakeStorage;
using bn::test::ZipBuilder;
using Bytes = std::vector<std::uint8_t>;

// The files of a book, in the order they are to go into the ZIP file:
// (path, contents).
using Files = std::vector<std::pair<std::string, std::string>>;

Bytes BytesOf(const std::string& text) { return {text.begin(), text.end()}; }

// container.xml, pointing at `packagePath`.
std::string Container(const std::string& packagePath) {
  return "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
         "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
         "<rootfiles><rootfile full-path=\"" +
         packagePath + "\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n";
}

// A package file with these three parts in it.
std::string Opf(const std::string& metadata, const std::string& manifest, const std::string& spine,
                const std::string& spineAttributes = " toc=\"ncx\"") {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">\n"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n" +
         metadata + "</metadata>\n<manifest>\n" + manifest + "</manifest>\n<spine" + spineAttributes + ">\n" + spine +
         "</spine>\n</package>\n";
}

const std::string kMetadata =
    "<dc:title>The Book</dc:title><dc:creator>A. Writer</dc:creator><dc:language>en</dc:language>\n";

std::uint32_t CounterNow(const std::string& name) {
  std::array<char, 2048> line{};
  bn::core::DefaultMetrics().FormatLine(line);
  const std::string text(line.data());
  const std::size_t at = text.find(name + "=");
  EXPECT_NE(at, std::string::npos) << text;
  return at == std::string::npos
             ? 0
             : static_cast<std::uint32_t>(std::strtoul(text.c_str() + at + name.size() + 1, nullptr, 10));
}

// Everything needed to read the package of a book built for the test.
struct PackageFixture {
  PackageFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Makes a ZIP file of `files`, every one compressed, and reads its
  // package with `memoryBytes` for the manifest and the spine.
  PackageStatus Read(const Files& files, std::size_t memoryBytes = 65'536) {
    ZipBuilder builder;
    for (const auto& [path, contents] : files) {
      builder.Add(path, BytesOf(contents), {.deflate = true});
    }
    return ReadZip(builder.Finish(), memoryBytes);
  }

  PackageStatus ReadZip(const Bytes& zip, std::size_t memoryBytes = 65'536) {
    directory.Close();
    entries.Close();
    storage.Put("/book.epub", zip);
    EXPECT_EQ(directory.Open(storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(entries.Open(storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(archive.Open(directory), bn::epub::ZipStatus::kOk);
    memory.assign(memoryBytes, 0xEE);
    package = std::make_unique<Package>(memory);
    const PackageStatus status = package->Read(archive, entries, reader, xml);
    EXPECT_EQ(status, package->Status());
    return status;
  }

  // The path of an entry, read back from the file; "-" for none.
  std::string PathOf(const bn::epub::ZipEntry* entry) {
    if (entry == nullptr) {
      return "-";
    }
    std::array<char, 600> name{};
    std::size_t length = 0;
    EXPECT_EQ(bn::epub::ReadZipEntryName(entries, *entry, name, length), bn::epub::ZipStatus::kOk);
    return {name.data(), std::min(length, name.size())};
  }

  // The spine as bn-epub prints it: "+ xhtml path" for each document, '-'
  // for one that is set aside, '?' for a file that is not in the book.
  std::vector<std::string> Spine() {
    std::vector<std::string> lines;
    for (const bn::epub::SpineItem& item : package->Spine()) {
      lines.push_back(std::string(item.linear ? "+ " : "- ") + bn::epub::MediaTypeName(item.type) + " " +
                      (item.present ? PathOf(&item.entry) : "?"));
    }
    return lines;
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
  std::vector<std::uint8_t> memory;
  std::unique_ptr<Package> package;
};

// A book as EPUB 2 has it, with its package in a folder.
Files Epub2Book() {
  const std::string manifest =
      "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
      "<item id=\"css\" href=\"styles/main.css\" media-type=\"text/css\"/>\n"
      "<item id=\"cover-image\" href=\"images/cover.jpg\" media-type=\"image/jpeg\"/>\n"
      "<item id=\"cover\" href=\"text/cover.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"ch1\" href=\"text/chapter1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"ch2\" href=\"text/chapter2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"notes\" href=\"text/notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  const std::string spine =
      "<itemref idref=\"cover\"/><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/>"
      "<itemref idref=\"notes\" linear=\"no\"/>\n";
  const std::string metadata = kMetadata + "<meta name=\"cover\" content=\"cover-image\"/>\n";
  return {
      {"mimetype", "application/epub+zip"},
      {"META-INF/container.xml", Container("OEBPS/content.opf")},
      {"OEBPS/content.opf", Opf(metadata, manifest, spine)},
      {"OEBPS/toc.ncx", "<ncx/>"},
      {"OEBPS/styles/main.css", "p { margin: 0 }"},
      {"OEBPS/images/cover.jpg", "not really a picture"},
      {"OEBPS/text/cover.xhtml", "<html/>"},
      {"OEBPS/text/chapter1.xhtml", "<html>one</html>"},
      {"OEBPS/text/chapter2.xhtml", "<html>two</html>"},
      {"OEBPS/text/notes.xhtml", "<html>notes</html>"},
  };
}

TEST(Package, ReadsWhatABookSaysOfItself) {
  PackageFixture f;
  ASSERT_EQ(f.Read(Epub2Book()), PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "The Book");
  EXPECT_EQ(f.package->Author(), "A. Writer");
  EXPECT_EQ(f.package->Language(), "en");
  EXPECT_EQ(f.package->PackagePath(), "OEBPS/content.opf");
  EXPECT_EQ(f.package->ManifestItems(), 7u);
  EXPECT_EQ(f.Spine(),
            (std::vector<std::string>{"+ xhtml OEBPS/text/cover.xhtml", "+ xhtml OEBPS/text/chapter1.xhtml",
                                      "+ xhtml OEBPS/text/chapter2.xhtml", "- xhtml OEBPS/text/notes.xhtml"}));
  EXPECT_EQ(f.PathOf(f.package->Cover()), "OEBPS/images/cover.jpg");
  EXPECT_EQ(f.package->CoverType(), MediaType::kJpeg);
  EXPECT_EQ(f.PathOf(f.package->Ncx()), "OEBPS/toc.ncx");
  EXPECT_EQ(f.package->Nav(), nullptr);
  EXPECT_EQ(f.package->UnknownSpineItems(), 0u);
  EXPECT_FALSE(f.package->CutShort());

  // What the spine holds opens the document itself.
  const bn::epub::SpineItem& second = f.package->Spine()[1];
  ASSERT_EQ(f.reader.Open(f.entries, second.entry), bn::epub::ZipStatus::kOk);
  std::array<std::uint8_t, 64> text{};
  ASSERT_EQ(f.reader.Read(text), 16);
  EXPECT_EQ(std::string(text.begin(), text.begin() + 16), "<html>one</html>");
}

TEST(Package, ReadsABookLaidOutAsEpub3HasIt) {
  // The package at the top of the book, names with the opf: prefix, the
  // cover and the table of contents marked by their properties.
  const std::string opf =
      "<?xml version=\"1.0\"?>\n"
      "<opf:package xmlns:opf=\"http://www.idpf.org/2007/opf\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "version=\"3.0\">\n"
      "<opf:metadata><dc:title id=\"t\">Other &amp; Stranger Things</dc:title>\n"
      "<dc:creator opf:role=\"aut\">First Author</dc:creator><dc:creator>Second Author</dc:creator>\n"
      "<dc:language>pt-BR</dc:language><opf:meta property=\"dcterms:modified\">2026</opf:meta></opf:metadata>\n"
      "<opf:manifest>\n"
      "<opf:item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
      "<opf:item id=\"c\" href=\"cover.png\" media-type=\"image/png\" properties=\"other cover-image\"/>\n"
      "<opf:item id=\"p1\" href=\"p1.xhtml\" media-type=\"application/xhtml+xml\" properties=\"scripted svg\"/>\n"
      "</opf:manifest>\n<opf:spine><opf:itemref idref=\"p1\"/></opf:spine>\n</opf:package>\n";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"mimetype", "application/epub+zip"},
                    {"META-INF/container.xml", Container("package.opf")},
                    {"package.opf", opf},
                    {"nav.xhtml", "<html/>"},
                    {"cover.png", "png"},
                    {"p1.xhtml", "<html/>"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "Other & Stranger Things");
  EXPECT_EQ(f.package->Author(), "First Author") << "the first of them";
  EXPECT_EQ(f.package->Language(), "pt-BR");
  EXPECT_EQ(f.package->PackagePath(), "package.opf");
  EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml p1.xhtml"});
  EXPECT_EQ(f.PathOf(f.package->Cover()), "cover.png");
  EXPECT_EQ(f.package->CoverType(), MediaType::kPng);
  EXPECT_EQ(f.PathOf(f.package->Nav()), "nav.xhtml");
  EXPECT_EQ(f.package->Ncx(), nullptr);
}

TEST(Package, FindsItsFilesInOnePassWhateverOrderTheZipHasThemIn) {
  // The same book with its files in the ZIP file in three different
  // orders: as the package lists them, backwards, and shuffled.
  const Files book = Epub2Book();
  const Files backwards(book.rbegin(), book.rend());
  Files shuffled = book;
  for (std::size_t i = 0; i < shuffled.size(); ++i) {
    std::swap(shuffled[i], shuffled[(i * 7 + 3) % shuffled.size()]);
  }
  PackageFixture reference;
  ASSERT_EQ(reference.Read(book), PackageStatus::kOk);
  for (const Files& files : {book, backwards, shuffled}) {
    PackageFixture f;
    const std::uint32_t finds = CounterNow("zip_finds");
    const std::uint32_t records = CounterNow("zip_records");
    ASSERT_EQ(f.Read(files), PackageStatus::kOk);
    EXPECT_EQ(f.Spine(), reference.Spine());
    EXPECT_EQ(f.PathOf(f.package->Cover()), "OEBPS/images/cover.jpg");
    EXPECT_EQ(f.PathOf(f.package->Ncx()), "OEBPS/toc.ncx");
    // Two files are looked up by name, container.xml and the package, and
    // then the directory is read through once: never more than three
    // times over, however the files lie.
    EXPECT_EQ(CounterNow("zip_finds") - finds, 2u);
    EXPECT_LE(CounterNow("zip_records") - records, 3 * files.size());
  }
}

TEST(Package, StopsReadingTheDirectoryOnceItHasFoundEverything) {
  // A book whose wanted files come first, and two hundred pictures after.
  Files files = Epub2Book();
  for (int i = 0; i < 200; ++i) {
    files.emplace_back("OEBPS/images/picture-" + std::to_string(i) + ".jpg", "p");
  }
  PackageFixture f;
  const std::uint32_t records = CounterNow("zip_records");
  ASSERT_EQ(f.Read(files), PackageStatus::kOk);
  // container.xml and the package are the second and third entries; the
  // last file wanted is the tenth.
  EXPECT_LE(CounterNow("zip_records") - records, 2u + 3u + 10u);
  EXPECT_EQ(f.package->Spine().size(), 4u);
}

TEST(Package, WorksOutWhereEachFileIsFromTheWayThePackageRefersToIt) {
  const std::string manifest =
      "<item id=\"a\" href=\"../shared/a%20file.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"b\" href=\"./text/b.xhtml#start\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"c\" href=\"/top.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"d\" href=\"text/../d.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"e\" href=\"caf%C3%A9.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"web\" href=\"https://example.com/remote.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"gone\" href=\"text/not-in-the-book.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"nohref\" media-type=\"application/xhtml+xml\"/>\n";
  const std::string spine =
      "<itemref idref=\"a\"/><itemref idref=\"b\"/><itemref idref=\"c\"/><itemref idref=\"d\"/>"
      "<itemref idref=\"e\"/><itemref idref=\"web\"/><itemref idref=\"gone\"/>"
      "<itemref idref=\"nohref\"/><itemref idref=\"never-listed\"/><itemref/>";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("OEBPS/pkg/content.opf")},
                    {"OEBPS/pkg/content.opf", Opf(kMetadata, manifest, spine)},
                    {"OEBPS/shared/a file.xhtml", "a"},
                    {"OEBPS/pkg/text/b.xhtml", "b"},
                    {"top.xhtml", "c"},
                    {"OEBPS/pkg/d.xhtml", "d"},
                    {"OEBPS/pkg/caf\xC3\xA9.xhtml", "e"}}),
            PackageStatus::kOk);
  // A file that is somewhere else, or has no path, is not a file of the
  // book, and a document the spine names that is not one is left out. A
  // file the package lists that is not in the ZIP file keeps its place.
  EXPECT_EQ(f.package->ManifestItems(), 6u);
  EXPECT_EQ(f.Spine(), (std::vector<std::string>{"+ xhtml OEBPS/shared/a file.xhtml", "+ xhtml OEBPS/pkg/text/b.xhtml",
                                                 "+ xhtml top.xhtml", "+ xhtml OEBPS/pkg/d.xhtml",
                                                 "+ xhtml OEBPS/pkg/caf\xC3\xA9.xhtml", "+ xhtml ?"}));
  EXPECT_EQ(f.package->UnknownSpineItems(), 3u);
  const bn::epub::SpineItem& missing = f.package->Spine()[5];
  EXPECT_FALSE(missing.present);
  EXPECT_EQ(missing.entry.headerOffset, 0u);
  EXPECT_EQ(missing.entry.size, 0u);
}

TEST(Package, ClosesUpTheSpineOverDocumentsItLeavesOut) {
  const std::string manifest =
      "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"b\" href=\"b.xhtml\" media-type=\"text/css\"/>\n"
      "<item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", Opf(kMetadata, manifest,
                                        "<itemref idref=\"unknown\" linear=\"no\"/><itemref idref=\"a\"/>"
                                        "<itemref idref=\"gone\"/><itemref idref=\"b\" linear=\"no\"/>"
                                        "<itemref idref=\"c\"/>")},
                    {"a.xhtml", "a"},
                    {"b.xhtml", "b"},
                    {"c.xhtml", "c"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.Spine(), (std::vector<std::string>{"+ xhtml a.xhtml", "- css b.xhtml", "+ xhtml c.xhtml"}));
  EXPECT_EQ(f.package->UnknownSpineItems(), 2u);
}

// Ids and paths are kept as numbers of 48 bits. These two ids, and these
// two paths, have numbers that are the same in their first 32 bits and
// differ only in the rest, so they are told apart only if all of the
// number is looked at.
TEST(Package, TellsApartNamesWhoseNumbersAreNearlyTheSame) {
  ASSERT_EQ(bn::epub::NameHash("id12514") & 0xFFFFFFFFU, bn::epub::NameHash("id93504") & 0xFFFFFFFFU);
  ASSERT_NE(bn::epub::NameHash("id12514"), bn::epub::NameHash("id93504"));
  ASSERT_EQ(bn::epub::NameHash("t/10123.xhtml") & 0xFFFFFFFFU, bn::epub::NameHash("t/22393.xhtml") & 0xFFFFFFFFU);
  ASSERT_NE(bn::epub::NameHash("t/10123.xhtml"), bn::epub::NameHash("t/22393.xhtml"));
  const std::string manifest =
      "<item id=\"id12514\" href=\"first.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"id93504\" href=\"second.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"x\" href=\"t/22393.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  PackageFixture f;
  // The ZIP file has both paths, the other one first.
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", Opf(kMetadata, manifest, "<itemref idref=\"id93504\"/><itemref idref=\"x\"/>")},
                    {"t/10123.xhtml", "the other one"},
                    {"first.xhtml", "1"},
                    {"second.xhtml", "2"},
                    {"t/22393.xhtml", "the one"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.Spine(), (std::vector<std::string>{"+ xhtml second.xhtml", "+ xhtml t/22393.xhtml"}));
}

TEST(Package, KeepsADocumentThatIsInTheSpineTwiceAndTheFirstOfTwoWithOneId) {
  const std::string manifest =
      "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"a\" href=\"other.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"b\" href=\"b.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"b-again\" href=\"b.xhtml\" media-type=\"text/css\"/>\n";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", Opf(kMetadata, manifest,
                                        "<itemref idref=\"a\"/><itemref idref=\"b\"/><itemref idref=\"a\"/>"
                                        "<itemref idref=\"b-again\"/>")},
                    {"a.xhtml", "a"},
                    {"b.xhtml", "b"},
                    {"other.xhtml", "o"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.Spine(),
            (std::vector<std::string>{"+ xhtml a.xhtml", "+ xhtml b.xhtml", "+ xhtml a.xhtml", "+ css b.xhtml"}));
}

TEST(Package, TakesTheFirstEntryWhenTheZipHasANameTwice) {
  ZipBuilder builder;
  builder.Add("META-INF/container.xml", BytesOf(Container("content.opf")));
  builder.Add("content.opf",
              BytesOf(Opf(kMetadata,
                          "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
                          "<item id=\"c\" href=\"c.png\" media-type=\"image/png\" properties=\"cover-image\"/>"
                          "<item id=\"z\" href=\"z.xhtml\" media-type=\"application/xhtml+xml\"/>",
                          "<itemref idref=\"a\"/><itemref idref=\"z\"/>")));
  builder.Add("a.xhtml", BytesOf("the first"));
  builder.Add("c.png", BytesOf("first"));
  builder.Add("a.xhtml", BytesOf("the second, longer one"));
  builder.Add("c.png", BytesOf("the second cover"));
  builder.Add("z.xhtml", BytesOf("z"));
  PackageFixture f;
  ASSERT_EQ(f.ReadZip(builder.Finish()), PackageStatus::kOk);
  ASSERT_EQ(f.package->Spine().size(), 2u);
  EXPECT_EQ(f.package->Spine()[0].entry.size, 9u);
  ASSERT_NE(f.package->Cover(), nullptr);
  EXPECT_EQ(f.package->Cover()->size, 5u);
  EXPECT_TRUE(f.package->Spine()[1].present);
}

TEST(Package, TidiesTheTitleAndTakesTheFirstOfEachThatSaysAnything) {
  const std::string metadata =
      "<dc:title>  </dc:title>\n<dc:title>\n   A  Tale\tof\n   Two&#160;Cities &amp; <i>More</i> Besides "
      "\n</dc:title>\n"
      "<dc:title>A Subtitle</dc:title>\n"
      "<dc:creator/><dc:creator>\n  Charles   Dickens\n</dc:creator><dc:creator>Someone Else</dc:creator>\n"
      "<dc:language> en-GB </dc:language><dc:language>fr</dc:language>\n";
  PackageFixture f;
  ASSERT_EQ(
      f.Read({{"META-INF/container.xml", Container("content.opf")},
              {"content.opf", Opf(metadata, "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                                  "<itemref idref=\"a\"/>")},
              {"a.xhtml", "a"}}),
      PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(),
            "A Tale of Two\xC2\xA0"
            "Cities & More Besides");
  EXPECT_EQ(f.package->Author(), "Charles Dickens");
  EXPECT_EQ(f.package->Language(), "en-GB");

  // The space that does not break, written the way HTML writes it, in a
  // package that names a DTD: it comes out as a space. Any other such name
  // is passed over.
  ASSERT_EQ(
      f.Read(
          {{"META-INF/container.xml", Container("content.opf")},
           {"content.opf",
            "<!DOCTYPE package PUBLIC \"x\" \"y\"><package><metadata><title>Tom&nbsp;Jones&mdash;A&nbsp;&nbsp;Foundling"
            "</title></metadata><manifest><item id=\"a\" href=\"a.xhtml\"/></manifest>"
            "<spine><itemref idref=\"a\"/></spine></package>"},
           {"a.xhtml", "a"}}),
      PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "Tom JonesA Foundling");

  // None of them given: none of them known, and the book is still a book.
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", Opf("", "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                                        "<itemref idref=\"a\"/>")},
                    {"a.xhtml", "a"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "");
  EXPECT_EQ(f.package->Author(), "");
  EXPECT_EQ(f.package->Language(), "");
}

TEST(Package, ReadsTheOldWayOfWritingMetadata) {
  // As books made before EPUB wrote it: the names with capitals, inside
  // another element.
  const std::string metadata =
      "<dc-metadata><dc:Title>Old Style</dc:Title><dc:Creator>Old Hand</dc:Creator>"
      "<dc:Language>de</dc:Language></dc-metadata><x-metadata><meta name=\"cover\" content=\"c\"/>"
      "</x-metadata>\n";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", Opf(metadata,
                                        "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
                                        "<item id=\"c\" href=\"c.gif\" media-type=\"image/gif\"/>",
                                        "<itemref idref=\"a\"/>")},
                    {"a.xhtml", "a"},
                    {"c.gif", "gif"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "Old Style");
  EXPECT_EQ(f.package->Author(), "Old Hand");
  EXPECT_EQ(f.package->Language(), "de");
  EXPECT_EQ(f.PathOf(f.package->Cover()), "c.gif");
  EXPECT_EQ(f.package->CoverType(), MediaType::kGif);
}

TEST(Package, CutsATitleThatIsTooLongAtACharacter) {
  // 100 characters of three bytes each: 300 bytes, where 256 are kept.
  std::string longTitle;
  for (int i = 0; i < 100; ++i) {
    longTitle += "\xE6\x96\x87";
  }
  PackageFixture f;
  ASSERT_EQ(
      f.Read({{"META-INF/container.xml", Container("content.opf")},
              {"content.opf",
               Opf("<dc:title>" + longTitle + "</dc:title><dc:creator>" + std::string(200, 'n') + "</dc:creator>",
                   "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>", "<itemref idref=\"a\"/>")},
              {"a.xhtml", "a"}}),
      PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), longTitle.substr(0, 255)) << "85 whole characters";
  EXPECT_EQ(f.package->Author(), std::string(128, 'n'));
}

TEST(Package, FindsTheCoverTheWayTheBookNamesIt) {
  const auto coverOf = [](const std::string& metadata, const std::string& extraItems) {
    PackageFixture f;
    const std::string manifest =
        "<item id=\"page\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "<item id=\"pic\" href=\"cover.jpg\" media-type=\"image/jpg\"/>\n"
        "<item id=\"other\" href=\"other.png\" media-type=\"image/png\"/>\n" +
        extraItems;
    EXPECT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                      {"content.opf", Opf(kMetadata + metadata, manifest, "<itemref idref=\"page\"/>")},
                      {"cover.xhtml", "page"},
                      {"cover.jpg", "jpeg"},
                      {"other.png", "png"}}),
              PackageStatus::kOk);
    return f.PathOf(f.package->Cover()) + " " + bn::epub::MediaTypeName(f.package->CoverType());
  };
  // EPUB 2's way: a meta element that gives the picture's id.
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"pic\"/>", ""), "cover.jpg jpeg");
  EXPECT_EQ(coverOf("<meta content=\"other\" name=\"cover\"/>", ""), "other.png png");
  EXPECT_EQ(coverOf("<meta name=\"calibre:series\" content=\"other\"/><meta name=\"cover\" content=\"pic\"/>"
                    "<meta name=\"cover\" content=\"other\"/><meta name=\"last\" content=\"other\"/>",
                    ""),
            "cover.jpg jpeg");
  // Some books give the id of the cover's page there. That is no picture.
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"page\"/>", ""), "- other");
  // Or an id nothing has, or no cover at all.
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"nothing\"/>", ""), "- other");
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"\"/>", ""), "- other");
  // A meta element that names nothing is not the one that names the cover,
  // and does not name a file that was given no id.
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"\"/><meta name=\"cover\" content=\"pic\"/>", ""), "cover.jpg jpeg");
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"\"/>", "<item href=\"other.png\" media-type=\"image/png\"/>"),
            "- other");
  EXPECT_EQ(coverOf("", ""), "- other");
  // EPUB 3's way wins where a book has both.
  EXPECT_EQ(coverOf("<meta name=\"cover\" content=\"pic\"/>",
                    "<item id=\"x\" href=\"other.png\" media-type=\"image/png\" properties=\"cover-image\"/>"),
            "other.png png");
  // Two marked as the cover: the first.
  EXPECT_EQ(coverOf("",
                    "<item id=\"x\" href=\"other.png\" media-type=\"image/png\" properties=\"cover-image\"/>"
                    "<item id=\"y\" href=\"cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>"),
            "other.png png");
  // A picture the book names but does not have.
  EXPECT_EQ(coverOf("", "<item id=\"x\" href=\"lost.png\" media-type=\"image/png\" properties=\"cover-image\"/>"),
            "- png");
}

TEST(Package, FindsTheTablesOfContents) {
  const auto tocsOf = [](const std::string& manifest, const std::string& spineAttributes) {
    PackageFixture f;
    EXPECT_EQ(
        f.Read({{"META-INF/container.xml", Container("content.opf")},
                {"content.opf",
                 Opf(kMetadata, "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>\n" + manifest,
                     "<itemref idref=\"a\"/>", spineAttributes)},
                {"a.xhtml", "a"},
                {"toc.ncx", "ncx"},
                {"second.ncx", "ncx"},
                {"nav.xhtml", "nav"}}),
        PackageStatus::kOk);
    return "nav " + f.PathOf(f.package->Nav()) + ", ncx " + f.PathOf(f.package->Ncx());
  };
  const std::string ncx = "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
  const std::string second = "<item id=\"ncx2\" href=\"second.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
  const std::string nav =
      "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n";
  EXPECT_EQ(tocsOf(ncx, " toc=\"ncx\""), "nav -, ncx toc.ncx");
  EXPECT_EQ(tocsOf(ncx + nav, " toc=\"ncx\""), "nav nav.xhtml, ncx toc.ncx");
  EXPECT_EQ(tocsOf(nav, ""), "nav nav.xhtml, ncx -");
  EXPECT_EQ(tocsOf("", ""), "nav -, ncx -");
  // The spine says which, where there are two; with nothing said, or an id
  // nothing has, it is the first file that is one.
  EXPECT_EQ(tocsOf(ncx + second, " toc=\"ncx2\""), "nav -, ncx second.ncx");
  EXPECT_EQ(tocsOf(second + ncx, ""), "nav -, ncx second.ncx");
  EXPECT_EQ(tocsOf(ncx, " toc=\"no-such-id\""), "nav -, ncx toc.ncx");
  // What the spine names is taken at its word, whatever kind it says it is.
  EXPECT_EQ(tocsOf("<item id=\"t\" href=\"toc.ncx\" media-type=\"text/xml\"/>\n", " toc=\"t\""), "nav -, ncx toc.ncx");
  // Two marked as the nav: the first.
  EXPECT_EQ(
      tocsOf(nav + "<item id=\"n2\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n", ""),
      "nav nav.xhtml, ncx -");
  // A property that only has "nav" inside it is another property.
  EXPECT_EQ(
      tocsOf("<item id=\"n\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"navigation\"/>\n",
             ""),
      "nav -, ncx -");
}

TEST(Package, FindsThePackageFileTheContainerNames) {
  const Files rest = {
      {"OEBPS/content.opf", Opf(kMetadata, "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                                "<itemref idref=\"a\"/>")},
      {"OEBPS/a.xhtml", "a"},
      {"my book/content.opf",
       Opf("<dc:title>Spaced</dc:title>", "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
           "<itemref idref=\"a\"/>")},
      {"my book/a.xhtml", "a"}};
  const auto with = [&rest](const std::string& container) {
    Files files = rest;
    files.insert(files.begin(), {"META-INF/container.xml", container});
    return files;
  };
  PackageFixture f;
  // Two are named: the one that says it is a package is the one, wherever
  // it comes.
  ASSERT_EQ(f.Read(with("<container><rootfiles><rootfile full-path=\"other.pdf\" media-type=\"application/pdf\"/>"
                        "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"Application/OEBPS-Package+XML\"/>"
                        "<rootfile full-path=\"my book/content.opf\" media-type=\"application/oebps-package+xml\"/>"
                        "</rootfiles></container>")),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->PackagePath(), "OEBPS/content.opf");
  // None says what it is: the first.
  ASSERT_EQ(f.Read(with("<container><rootfile full-path=\"my book/content.opf\"/>"
                        "<rootfile full-path=\"OEBPS/content.opf\"/></container>")),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->PackagePath(), "my book/content.opf");
  EXPECT_EQ(f.package->Title(), "Spaced");
  EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml my book/a.xhtml"});
  // A path written the way a link is, when no file has it as written.
  ASSERT_EQ(f.Read(with(Container("my%20book/./content.opf"))), PackageStatus::kOk);
  EXPECT_EQ(f.package->PackagePath(), "my book/content.opf");
  EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml my book/a.xhtml"});
  // A container that goes wrong after it has named the package has said enough.
  ASSERT_EQ(f.Read(with("<container><rootfile full-path=\"OEBPS/content.opf\"/><oops></container>")),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->PackagePath(), "OEBPS/content.opf");
}

TEST(Package, SaysWhatIsMissingWhenABookHasNoPackage) {
  PackageFixture f;
  const Files rest = {
      {"content.opf", Opf(kMetadata, "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                          "<itemref idref=\"a\"/>")},
      {"a.xhtml", "a"}};
  const auto with = [&rest](const std::string& name, const std::string& container) {
    Files files = rest;
    files.insert(files.begin(), {name, container});
    return files;
  };
  EXPECT_EQ(f.Read(rest), PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("meta-inf/container.xml", Container("content.opf"))), PackageStatus::kNoContainer)
      << "names are matched exactly";
  EXPECT_EQ(f.Read(with("META-INF/container.xml", "<container><rootfiles/></container>")), PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", "<container><rootfile full-path=\"\"/></container>")),
            PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", "not XML at all")), PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", "")), PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", Container(std::string(600, 'p') + ".opf"))),
            PackageStatus::kNoContainer);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", Container("OEBPS/content.opf"))), PackageStatus::kNoPackage);
  EXPECT_EQ(f.Read(with("META-INF/container.xml", Container("https://example.com/content.opf"))),
            PackageStatus::kNoPackage);
  // Whatever went wrong, nothing half read is left to be taken for an answer.
  EXPECT_TRUE(f.package->Spine().empty());
  EXPECT_EQ(f.package->Cover(), nullptr);
  EXPECT_EQ(f.package->Status(), PackageStatus::kNoPackage);
}

TEST(Package, RefusesAPackageFileThatListsNothingToRead) {
  PackageFixture f;
  const std::string item = "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>";
  const auto read = [&f](const std::string& opf) {
    return f.Read({{"META-INF/container.xml", Container("content.opf")}, {"content.opf", opf}, {"a.xhtml", "a"}});
  };
  EXPECT_EQ(read("this is not XML"), PackageStatus::kNotPackage);
  EXPECT_EQ(read(""), PackageStatus::kNotPackage);
  EXPECT_EQ(read("<html><body>some other XML</body></html>"), PackageStatus::kNotPackage);
  EXPECT_EQ(read(Opf(kMetadata, item, "")), PackageStatus::kNotPackage) << "an empty spine";
  EXPECT_EQ(read(Opf(kMetadata, "", "<itemref idref=\"a\"/>")), PackageStatus::kNotPackage)
      << "a spine that names nothing the manifest has";
  // It goes wrong before the spine: there is nothing to go on.
  const std::string whole = Opf(kMetadata, item, "<itemref idref=\"a\"/>");
  EXPECT_EQ(read(whole.substr(0, whole.find("<spine"))), PackageStatus::kNotPackage);
  // The parts have to be where a package has them: an item is an item of
  // the manifest, not anything of that name anywhere.
  EXPECT_EQ(read("<package><metadata>" + item + "<itemref idref=\"a\"/></metadata></package>"),
            PackageStatus::kNotPackage);
  EXPECT_EQ(
      read("<package><manifest><group>" + item + "</group></manifest><spine><itemref idref=\"a\"/></spine></package>"),
      PackageStatus::kNotPackage);
  EXPECT_EQ(
      read("<package><manifest>" + item + "</manifest><spine><group><itemref idref=\"a\"/></group></spine></package>"),
      PackageStatus::kNotPackage);
}

// An element with the name of one of a package's parts, inside another of
// them, is not a part.
TEST(Package, KnowsThePartsOfAPackageOnlyWhereAPackageHasThem) {
  const std::string opf =
      "<package><metadata><spine/><manifest/><dc:title>Nested</dc:title>"
      "<meta name=\"cover\" content=\"c\"/></metadata>"
      "<manifest><metadata/><spine/><item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"c\" href=\"c.png\" media-type=\"image/png\"/></manifest>"
      "<spine><manifest/><metadata/><itemref idref=\"a\"/></spine></package>";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", opf},
                    {"a.xhtml", "a"},
                    {"c.png", "c"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "Nested");
  EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml a.xhtml"});
  EXPECT_EQ(f.PathOf(f.package->Cover()), "c.png");
}

TEST(Package, KeepsWhatAPackageSaidBeforeItWentWrong) {
  PackageFixture f;
  const std::string item = "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>";
  const std::string whole = Opf(kMetadata, item, "<itemref idref=\"a\"/>");
  // Cut off after its spine has begun, and with rubbish after its end.
  for (const std::string& damaged :
       {whole.substr(0, whole.find("</spine>")), whole + "<trailing>", whole.substr(0, whole.size() - 12)}) {
    ASSERT_EQ(
        f.Read({{"META-INF/container.xml", Container("content.opf")}, {"content.opf", damaged}, {"a.xhtml", "a"}}),
        PackageStatus::kOk);
    EXPECT_TRUE(f.package->CutShort());
    EXPECT_EQ(f.package->Title(), "The Book");
    EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml a.xhtml"});
  }
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")}, {"content.opf", whole}, {"a.xhtml", "a"}}),
            PackageStatus::kOk);
  EXPECT_FALSE(f.package->CutShort());
}

TEST(Package, TakesTheSpineWhereverInThePackageItComes) {
  // Before the manifest, which a package should not do and nothing stops.
  const std::string opf =
      "<package><spine toc=\"n\"><itemref idref=\"b\"/><itemref idref=\"a\"/></spine>"
      "<manifest><item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"b\" href=\"b.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"n\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest>"
      "<metadata><title>Backwards</title><meta name=\"cover\" content=\"missing\"/></metadata></package>";
  PackageFixture f;
  ASSERT_EQ(f.Read({{"META-INF/container.xml", Container("content.opf")},
                    {"content.opf", opf},
                    {"a.xhtml", "a"},
                    {"b.xhtml", "b"},
                    {"toc.ncx", "n"}}),
            PackageStatus::kOk);
  EXPECT_EQ(f.package->Title(), "Backwards");
  EXPECT_EQ(f.Spine(), (std::vector<std::string>{"+ xhtml b.xhtml", "+ xhtml a.xhtml"}));
  EXPECT_EQ(f.PathOf(f.package->Ncx()), "toc.ncx");

  // Its memory is counted the same, and when there is not enough for the
  // files, which now come last, that is what is said.
  const Files book = {{"META-INF/container.xml", Container("content.opf")},
                      {"content.opf", opf},
                      {"a.xhtml", "a"},
                      {"b.xhtml", "b"},
                      {"toc.ncx", "n"}};
  const std::size_t needed = (2 * 28) + (3 * 16);
  ASSERT_EQ(f.Read(book, needed), PackageStatus::kOk);
  EXPECT_EQ(f.package->MemoryUsed(), needed);
  EXPECT_EQ(f.PathOf(f.package->Ncx()), "toc.ncx");
  EXPECT_EQ(f.Read(book, needed - 16), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, needed - 4), PackageStatus::kNoMemory);
}

// A book of `items` files, of which the first `documents` are its spine.
Files BigBook(int items, int documents) {
  std::string manifest;
  std::string spine;
  Files files = {{"META-INF/container.xml", Container("OEBPS/content.opf")}, {"OEBPS/content.opf", ""}};
  for (int i = 0; i < items; ++i) {
    const std::string name = "file-" + std::to_string(i) + ".xhtml";
    manifest +=
        "<item id=\"id" + std::to_string(i) + "\" href=\"text/" + name + "\" media-type=\"application/xhtml+xml\"/>\n";
    if (i < documents) {
      spine += "<itemref idref=\"id" + std::to_string(i) + "\"/>";
    }
    files.emplace_back("OEBPS/text/" + name, std::to_string(i));
  }
  files[1].second = Opf(kMetadata, manifest, spine);
  return files;
}

TEST(Package, NeedsSixteenBytesAFileAndTwentyEightADocument) {
  ASSERT_EQ(sizeof(bn::epub::SpineItem), 28u);
  const Files book = BigBook(100, 40);
  const std::size_t forFiles = 1'600;           // 100 files at 16 bytes,
  const std::size_t needed = forFiles + 1'120;  // and 40 documents at 28.
  PackageFixture f;
  ASSERT_EQ(f.Read(book, needed), PackageStatus::kOk);
  EXPECT_EQ(f.package->MemoryUsed(), needed);
  EXPECT_EQ(f.package->ManifestItems(), 100u);
  ASSERT_EQ(f.package->Spine().size(), 40u);
  EXPECT_EQ(f.PathOf(&f.package->Spine()[39].entry), "OEBPS/text/file-39.xhtml");
  for (const bn::epub::SpineItem& item : f.package->Spine()) {
    EXPECT_TRUE(item.present);
  }
  // With room to spare it uses no more.
  ASSERT_EQ(f.Read(book, 4 * needed), PackageStatus::kOk);
  EXPECT_EQ(f.package->MemoryUsed(), needed);

  // One file's worth short, or one document's, and the book is refused,
  // with nothing half listed.
  EXPECT_EQ(f.Read(book, needed - 4), PackageStatus::kNoMemory);
  EXPECT_TRUE(f.package->Spine().empty());
  EXPECT_EQ(f.Read(book, forFiles + 12), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, forFiles), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, forFiles - 4), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, forFiles - 16), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, 0), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read(book, 3), PackageStatus::kNoMemory);
  EXPECT_EQ(f.Read({{"mimetype", "not a book at all"}}, 0), PackageStatus::kNoMemory) << "said before anything is read";
}

TEST(Package, ReadsABookOfManyFilesInLittleMemory) {
  // Larger than the largest book that has been seen: 800 files, 400 of
  // them documents.
  const Files book = BigBook(800, 400);
  PackageFixture f;
  ASSERT_EQ(f.Read(book, 24'000), PackageStatus::kOk);
  EXPECT_EQ(f.package->MemoryUsed(), 24'000u) << "800 files at 16 bytes and 400 documents at 28";
  ASSERT_EQ(f.package->Spine().size(), 400u);
  EXPECT_EQ(f.PathOf(&f.package->Spine()[0].entry), "OEBPS/text/file-0.xhtml");
  EXPECT_EQ(f.PathOf(&f.package->Spine()[399].entry), "OEBPS/text/file-399.xhtml");
  EXPECT_EQ(f.package->Spine()[217].entry.size, 3u) << "the file that holds \"217\"";
}

TEST(Package, ReadsNoMoreOfAPackageFileOnceItHasRunOutOfMemory) {
  // A package file of over 70 KB, stored as it is, and room for ten of its
  // 800 files. Read to its end it would be eighteen reads of storage.
  const Files book = BigBook(800, 400);
  ASSERT_GT(book[1].second.size(), 70'000u);
  ZipBuilder builder;
  for (const auto& [path, contents] : book) {
    builder.Add(path, BytesOf(contents));
  }
  PackageFixture f;
  const std::size_t before = f.storage.reads.size();
  ASSERT_EQ(f.ReadZip(builder.Finish(), 160), PackageStatus::kNoMemory);
  EXPECT_LE(f.storage.reads.size() - before, 8u);
}

TEST(Package, MakesDoWithMemoryThatStartsAnywhere) {
  const Files book = BigBook(20, 10);
  for (std::size_t offset = 1; offset < 8; ++offset) {
    PackageFixture f;
    ZipBuilder builder;
    for (const auto& [path, contents] : book) {
      builder.Add(path, BytesOf(contents));
    }
    f.storage.Put("/book.epub", builder.Finish());
    ASSERT_EQ(f.directory.Open(f.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    ASSERT_EQ(f.entries.Open(f.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
    ASSERT_EQ(f.archive.Open(f.directory), bn::epub::ZipStatus::kOk);
    alignas(8) std::array<std::uint8_t, 2048> memory{};
    Package package{std::span<std::uint8_t>(memory).subspan(offset, 1024)};
    ASSERT_EQ(package.Read(f.archive, f.entries, f.reader, f.xml), PackageStatus::kOk) << offset;
    ASSERT_EQ(package.Spine().size(), 10u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(package.Spine().data()) % alignof(bn::epub::SpineItem), 0u);
    EXPECT_GE(reinterpret_cast<const std::uint8_t*>(package.Spine().data()), memory.data() + offset);
  }
}

TEST(Package, ReportsABookItCannotRead) {
  // Storage that fails.
  PackageFixture failing;
  ASSERT_EQ(failing.Read(Epub2Book()), PackageStatus::kOk);
  failing.storage.failReads = true;
  failing.directory.Close();
  ASSERT_EQ(failing.directory.Open(failing.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(failing.package->Read(failing.archive, failing.entries, failing.reader, failing.xml),
            PackageStatus::kUnreadable);
  EXPECT_TRUE(failing.package->Spine().empty());

  // A package file whose bytes are not what its checksum says: one
  // letter of its title is another letter, so it is as good XML as it was.
  ZipBuilder builder;
  const Files book = Epub2Book();
  for (const auto& [path, contents] : book) {
    builder.Add(path, BytesOf(contents));
  }
  Bytes zip = builder.Finish();
  zip.at(builder.At(2).data + book[2].second.find("The Book")) ^= 0x01U;
  PackageFixture damaged;
  EXPECT_EQ(damaged.ReadZip(zip), PackageStatus::kUnreadable);

  // A parser with too little memory to parse anything.
  PackageFixture small;
  std::vector<std::uint8_t> tiny(256);
  bn::epub::XmlParser smallParser(tiny);
  ZipBuilder plain;
  for (const auto& [path, contents] : book) {
    plain.Add(path, BytesOf(contents));
  }
  small.storage.Put("/book.epub", plain.Finish());
  ASSERT_EQ(small.directory.Open(small.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(small.entries.Open(small.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(small.archive.Open(small.directory), bn::epub::ZipStatus::kOk);
  std::vector<std::uint8_t> memory(4096);
  Package package(memory);
  EXPECT_EQ(package.Read(small.archive, small.entries, small.reader, smallParser), PackageStatus::kNoMemory);
}

TEST(Package, ReportsADirectoryItCannotFollowToTheFilesItWants) {
  ZipBuilder builder;
  const Files book = Epub2Book();
  for (const auto& [path, contents] : book) {
    builder.Add(path, BytesOf(contents));
  }
  Bytes zip = builder.Finish();
  // The record of the last file but one no longer begins as a record
  // does. container.xml and the package come before it and are found;
  // the last document of the spine comes after it and cannot be.
  zip.at(builder.At(book.size() - 2).record) ^= 0xFFU;
  PackageFixture f;
  EXPECT_EQ(f.ReadZip(zip), PackageStatus::kUnreadable);
  EXPECT_TRUE(f.package->Spine().empty());
  // The cover and the NCX come before it too, and were found. They are
  // not given out either.
  EXPECT_EQ(f.package->Cover(), nullptr);
  EXPECT_EQ(f.package->Ncx(), nullptr);
}

TEST(Package, PassesOverAFileWithANameLongerThanAnyItKeeps) {
  Files files = Epub2Book();
  files.insert(files.begin() + 3, {"OEBPS/" + std::string(700, 'n') + ".xhtml", "long"});
  PackageFixture f;
  ASSERT_EQ(f.Read(files), PackageStatus::kOk);
  EXPECT_EQ(f.package->Spine().size(), 4u);
  for (const bn::epub::SpineItem& item : f.package->Spine()) {
    EXPECT_TRUE(item.present);
  }
}

TEST(Package, CanReadOneBookAfterAnother) {
  PackageFixture f;
  ASSERT_EQ(f.Read(Epub2Book()), PackageStatus::kOk);
  Package& package = *f.package;
  // The same Package, another book: nothing of the first is left.
  ZipBuilder builder;
  builder.Add("META-INF/container.xml", BytesOf(Container("p.opf")));
  builder.Add("p.opf", BytesOf(Opf("<dc:title>Second</dc:title>",
                                   "<item id=\"x\" href=\"x.xhtml\" media-type=\"application/xhtml+xml\"/>",
                                   "<itemref idref=\"x\"/>", "")));
  builder.Add("x.xhtml", BytesOf("x"));
  f.directory.Close();
  f.entries.Close();
  f.storage.Put("/second.epub", builder.Finish());
  ASSERT_EQ(f.directory.Open(f.storage, "/second.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(f.entries.Open(f.storage, "/second.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(f.archive.Open(f.directory), bn::epub::ZipStatus::kOk);
  ASSERT_EQ(package.Read(f.archive, f.entries, f.reader, f.xml), PackageStatus::kOk);
  EXPECT_EQ(package.Title(), "Second");
  EXPECT_EQ(package.Author(), "");
  EXPECT_EQ(package.Language(), "");
  EXPECT_EQ(package.PackagePath(), "p.opf");
  EXPECT_EQ(package.ManifestItems(), 1u);
  EXPECT_EQ(f.Spine(), std::vector<std::string>{"+ xhtml x.xhtml"});
  EXPECT_EQ(package.Cover(), nullptr);
  EXPECT_EQ(package.Ncx(), nullptr);
  EXPECT_EQ(package.MemoryUsed(), 16u + 28);

  // And the first again: what it names is found as it was the first time.
  f.directory.Close();
  f.entries.Close();
  ASSERT_EQ(f.directory.Open(f.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(f.entries.Open(f.storage, "/book.epub", bn::core::OpenMode::kRead), StorageStatus::kOk);
  ASSERT_EQ(f.archive.Open(f.directory), bn::epub::ZipStatus::kOk);
  ASSERT_EQ(package.Read(f.archive, f.entries, f.reader, f.xml), PackageStatus::kOk);
  EXPECT_EQ(package.Title(), "The Book");
  EXPECT_EQ(f.PathOf(package.Cover()), "OEBPS/images/cover.jpg");
  EXPECT_EQ(f.PathOf(package.Ncx()), "OEBPS/toc.ncx");
  EXPECT_EQ(package.Spine().size(), 4u);
}

TEST(Package, CountsThePackagesItReads) {
  PackageFixture f;
  const std::uint32_t read = CounterNow("packages_read");
  const std::uint32_t failed = CounterNow("packages_failed");
  ASSERT_EQ(f.Read(Epub2Book()), PackageStatus::kOk);
  EXPECT_EQ(f.Read({{"mimetype", "x"}}), PackageStatus::kNoContainer);
  EXPECT_EQ(CounterNow("packages_read") - read, 2u);
  EXPECT_EQ(CounterNow("packages_failed") - failed, 1u);
}

TEST(MediaType, IsKnownByTheNamesBooksUse) {
  using bn::epub::MediaTypeOf;
  EXPECT_EQ(MediaTypeOf("application/xhtml+xml"), MediaType::kXhtml);
  EXPECT_EQ(MediaTypeOf("Application/XHTML+XML"), MediaType::kXhtml);
  EXPECT_EQ(MediaTypeOf("text/html"), MediaType::kXhtml);
  EXPECT_EQ(MediaTypeOf("text/css"), MediaType::kCss);
  EXPECT_EQ(MediaTypeOf("application/x-dtbncx+xml"), MediaType::kNcx);
  EXPECT_EQ(MediaTypeOf("image/jpeg"), MediaType::kJpeg);
  EXPECT_EQ(MediaTypeOf("image/jpg"), MediaType::kJpeg);
  EXPECT_EQ(MediaTypeOf("image/png"), MediaType::kPng);
  EXPECT_EQ(MediaTypeOf("image/gif"), MediaType::kGif);
  EXPECT_EQ(MediaTypeOf("image/svg+xml"), MediaType::kSvg);
  for (const char* font :
       {"font/otf", "font/ttf", "application/vnd.ms-opentype", "application/x-font-otf", "application/x-font-ttf",
        "application/font-sfnt", "application/x-font-truetype", "font/opentype", "FONT/WOFF2"}) {
    EXPECT_EQ(MediaTypeOf(font), MediaType::kFont) << font;
  }
  for (const char* other : {"", "text/plain", "application/xml", "application/vnd.adobe-page-template+xml",
                            "image/webp", "application/xhtml+xml ", "text/htm", "image/jpeg2", "fon"}) {
    EXPECT_EQ(MediaTypeOf(other), MediaType::kOther) << other;
  }
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kOther), "other");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kXhtml), "xhtml");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kCss), "css");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kNcx), "ncx");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kJpeg), "jpeg");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kPng), "png");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kGif), "gif");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kSvg), "svg");
  EXPECT_STREQ(bn::epub::MediaTypeName(MediaType::kFont), "font");
}

TEST(PackageStatus, EveryStatusHasAName) {
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kOk), "ok");
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kNoContainer), "no container");
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kNoPackage), "no package");
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kNotPackage), "not a package");
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kNoMemory), "no memory");
  EXPECT_STREQ(bn::epub::PackageStatusName(PackageStatus::kUnreadable), "unreadable");
}

// The book is not trusted: whatever is done to its package file, reading
// it ends with an answer, and what it gives as the spine is all there.
TEST(Package, SurvivesDamageAnywhereInAPackageFile) {
  const Files book = Epub2Book();
  const std::string good = book[2].second;
  PackageFixture f;
  int read = 0;
  int refused = 0;
  const auto tryIt = [&](const std::string& opf) {
    ZipBuilder builder;
    builder.Add("META-INF/container.xml", BytesOf(book[1].second));
    builder.Add("OEBPS/content.opf", BytesOf(opf));
    for (std::size_t i = 3; i < book.size(); ++i) {
      builder.Add(book[i].first, BytesOf(book[i].second));
    }
    const PackageStatus status = f.ReadZip(builder.Finish(), 1024);
    if (status == PackageStatus::kOk) {
      ++read;
      EXPECT_FALSE(f.package->Spine().empty());
      for (const bn::epub::SpineItem& item : f.package->Spine()) {
        if (item.present) {
          EXPECT_LT(item.entry.headerOffset, f.entries.Size());
        } else {
          EXPECT_EQ(item.entry.size, 0u);
        }
      }
    } else {
      ++refused;
      EXPECT_TRUE(f.package->Spine().empty());
      EXPECT_TRUE(status == PackageStatus::kNotPackage || status == PackageStatus::kNoMemory)
          << bn::epub::PackageStatusName(status);
    }
  };
  // Every bit turned over, one at a time.
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::string damaged = good;
      damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ (1U << bit));
      tryIt(damaged);
    }
  }
  // Cut short at every length.
  for (std::size_t length = 0; length < good.size(); ++length) {
    tryIt(good.substr(0, length));
  }
  EXPECT_GT(read, 1'000);
  EXPECT_GT(refused, 1'000);
}

}  // namespace
