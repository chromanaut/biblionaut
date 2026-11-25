#include "epub/book_files.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "epub/book_path.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/xml.h"
#include "storage_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::ZipStatus;
using Bytes = std::vector<std::uint8_t>;

Bytes BytesOf(std::string_view text) { return {text.begin(), text.end()}; }

// A ZIP file on a started storage, open twice, as a book's files.
struct BookFixture {
  explicit BookFixture(const Bytes& zip) {
    storage.Put("/book.epub", zip);
    EXPECT_EQ(storage.Start(), StorageStatus::kOk);
    EXPECT_EQ(directory.Open(storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(entries.Open(storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(archive.Open(directory), ZipStatus::kOk);
  }

  // Everything the file at `path` holds; "(none)" if it cannot be opened,
  // and "(failed)" after what was read if it fails part way.
  std::string Read(std::string_view path) { return ReadFrom(files.Open(path)); }

  // The entry of that name, as the directory has it.
  bn::epub::ZipEntry EntryOf(std::string_view name) {
    bn::epub::ZipEntry entry;
    EXPECT_EQ(archive.Find(name, entry), ZipStatus::kOk) << name;
    return entry;
  }

  static std::string ReadFrom(bn::core::ByteSource* source) {
    if (source == nullptr) {
      return "(none)";
    }
    std::string out;
    Bytes piece(300);
    for (;;) {
      const std::ptrdiff_t read = source->Read(piece);
      if (read < 0) {
        return out + "(failed)";
      }
      if (read == 0) {
        return out;
      }
      out.append(piece.begin(), piece.begin() + read);
    }
  }

  bn::test::FakeStorage storage;
  Bytes directoryBuffer = Bytes(4096);
  Bytes entryBuffer = Bytes(4096);
  bn::core::BlockFile directory{directoryBuffer};
  bn::core::BlockFile entries{entryBuffer};
  bn::epub::ZipArchive archive;
  Bytes window = Bytes(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader{window};
  bn::epub::ZipBookFiles files{archive, entries, reader};
};

TEST(ZipBookFiles, OpensTheEntriesOfAZipFileByTheirPaths) {
  const std::string chapter = "<html><body>" + std::string(5000, 'x') + "</body></html>";
  bn::test::ZipBuilder zip;
  zip.Add("mimetype", BytesOf("application/epub+zip"));
  zip.Add("OEBPS/css/book.css", BytesOf("p { margin: 0 }"), {.deflate = true});
  zip.Add("OEBPS/text/one.xhtml", BytesOf(chapter), {.deflate = true});
  BookFixture book(zip.Finish());

  EXPECT_EQ(book.Read("OEBPS/text/one.xhtml"), chapter);
  EXPECT_EQ(book.files.Status(), ZipStatus::kOk);
  // In any order, one after another through the one reader.
  EXPECT_EQ(book.Read("mimetype"), "application/epub+zip");
  EXPECT_EQ(book.Read("OEBPS/css/book.css"), "p { margin: 0 }");
  EXPECT_EQ(book.Read("OEBPS/text/one.xhtml"), chapter);
}

TEST(ZipBookFiles, SaysWhenTheBookHasNoSuchFile) {
  bn::test::ZipBuilder zip;
  zip.Add("a.css", BytesOf("p { margin: 0 }"));
  BookFixture book(zip.Finish());
  EXPECT_EQ(book.Read("b.css"), "(none)");
  EXPECT_EQ(book.files.Status(), ZipStatus::kNotFound);
  // A name is the entry's, byte for byte.
  EXPECT_EQ(book.Read("A.css"), "(none)");
  EXPECT_EQ(book.Read("/a.css"), "(none)");
  EXPECT_EQ(book.Read("a.css"), "p { margin: 0 }");
  EXPECT_EQ(book.files.Status(), ZipStatus::kOk);
}

TEST(ZipBookFiles, ADamagedEntryEndsInTheSourcesError) {
  bn::test::ZipBuilder zip;
  zip.Add("a.css", BytesOf("p { margin: 0 } h1 { margin: 0 }"));
  Bytes bytes = zip.Finish();
  // One byte of the entry's data changed: its checksum is no longer right.
  const std::string text = "h1 { margin";
  const auto at = std::search(bytes.begin(), bytes.end(), text.begin(), text.end());
  ASSERT_NE(at, bytes.end());
  *at = 'H';
  BookFixture book(bytes);
  EXPECT_EQ(book.Read("a.css"), "p { margin: 0 } H1 { margin: 0 }(failed)");
}

TEST(ZipBookFiles, OpensAFileFromItsPlaceAndSaysWhatItIsCalled) {
  const std::string chapter = "<html><body>" + std::string(5000, 'x') + "</body></html>";
  bn::test::ZipBuilder zip;
  zip.Add("mimetype", BytesOf("application/epub+zip"));
  zip.Add("OEBPS/css/book.css", BytesOf("p { margin: 0 }"), {.deflate = true});
  zip.Add("OEBPS/text/one.xhtml", BytesOf(chapter), {.deflate = true});
  BookFixture book(zip.Finish());
  const bn::epub::ZipEntry one = book.EntryOf("OEBPS/text/one.xhtml");
  const bn::epub::ZipEntry sheet = book.EntryOf("OEBPS/css/book.css");

  std::array<char, bn::epub::kMaxBookPath> path{};
  std::size_t length = 0;
  bn::core::ByteSource* source = book.files.OpenAt(one, path, length);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(std::string_view(path.data(), length), "OEBPS/text/one.xhtml");
  EXPECT_EQ(BookFixture::ReadFrom(source), chapter);
  EXPECT_EQ(book.files.Status(), ZipStatus::kOk);
  // In any order, and with files found by their names in between.
  source = book.files.OpenAt(sheet, path, length);
  EXPECT_EQ(std::string_view(path.data(), length), "OEBPS/css/book.css");
  EXPECT_EQ(BookFixture::ReadFrom(source), "p { margin: 0 }");
  EXPECT_EQ(book.Read("mimetype"), "application/epub+zip");
  EXPECT_EQ(BookFixture::ReadFrom(book.files.OpenAt(one, path, length)), chapter);
}

TEST(ZipBookFiles, OpeningAFileFromItsPlaceDoesNotReadTheDirectory) {
  // A directory of several blocks, and the last entry of it: found by its
  // name from a search that starts at the first, the whole directory is
  // read; opened from its place, none of it is.
  bn::test::ZipBuilder zip;
  for (int number = 0; number < 300; ++number) {
    zip.Add("OEBPS/text/a-chapter-with-quite-a-long-name-" + std::to_string(number) + ".xhtml", BytesOf("<html/>"));
  }
  const std::string last = "OEBPS/text/a-chapter-with-quite-a-long-name-299.xhtml";
  BookFixture book(zip.Finish());
  const bn::epub::ZipEntry entry = book.EntryOf(last);
  // The search is back at the start after going all the way round for a
  // name that is not there.
  bn::epub::ZipEntry none;
  EXPECT_EQ(book.archive.Find("no such file", none), ZipStatus::kNotFound);

  const std::size_t before = book.storage.reads.size();
  EXPECT_EQ(book.Read(last), "<html/>");
  const std::size_t byName = book.storage.reads.size() - before;
  EXPECT_EQ(book.archive.Find("no such file", none), ZipStatus::kNotFound);
  // What reading the entry itself left in the entries' buffer is put out
  // of it, so that opening it again has to read it again.
  EXPECT_EQ(book.Read("OEBPS/text/a-chapter-with-quite-a-long-name-0.xhtml"), "<html/>");
  EXPECT_EQ(book.archive.Find("no such file", none), ZipStatus::kNotFound);
  const std::size_t between = book.storage.reads.size();
  std::array<char, bn::epub::kMaxBookPath> path{};
  std::size_t length = 0;
  EXPECT_EQ(BookFixture::ReadFrom(book.files.OpenAt(entry, path, length)), "<html/>");
  const std::size_t byPlace = book.storage.reads.size() - between;
  EXPECT_EQ(std::string_view(path.data(), length), last);
  EXPECT_LE(byPlace, 2U);
  EXPECT_GT(byName, byPlace + 3);
}

TEST(ZipBookFiles, SaysWhenAFileCannotBeOpenedFromItsPlaceAndWhichItWas) {
  bn::test::ZipBuilder zip;
  zip.Add("a-name-of-twenty-two.x", BytesOf("p { margin: 0 }"));
  zip.Add("locked.xhtml", BytesOf("<html/>"), {.flags = 1});
  BookFixture book(zip.Finish());

  // A path longer than there is room for: as much as fits, and no file.
  std::array<char, 10> little{};
  std::size_t length = 99;
  EXPECT_EQ(book.files.OpenAt(book.EntryOf("a-name-of-twenty-two.x"), little, length), nullptr);
  EXPECT_EQ(std::string_view(little.data(), length), "a-name-of-");
  EXPECT_EQ(book.files.Status(), ZipStatus::kUnsupported);
  // Room for it exactly is room.
  std::array<char, 22> exact{};
  EXPECT_NE(book.files.OpenAt(book.EntryOf("a-name-of-twenty-two.x"), exact, length), nullptr);
  EXPECT_EQ(length, 22U);

  // An entry that is marked as encrypted cannot be opened; its path is
  // read all the same.
  std::array<char, bn::epub::kMaxBookPath> path{};
  EXPECT_EQ(book.files.OpenAt(book.EntryOf("locked.xhtml"), path, length), nullptr);
  EXPECT_EQ(std::string_view(path.data(), length), "locked.xhtml");
  EXPECT_EQ(book.files.Status(), ZipStatus::kUnsupported);

  // A place that is no entry's: nothing of a path, and no file.
  bn::epub::ZipEntry nowhere = book.EntryOf("locked.xhtml");
  nowhere.headerOffset += 3;
  length = 99;
  EXPECT_EQ(book.files.OpenAt(nowhere, path, length), nullptr);
  EXPECT_EQ(length, 0U);
  EXPECT_EQ(book.files.Status(), ZipStatus::kDamaged);
  nowhere.headerOffset = 0x7FFFFFFF;
  EXPECT_EQ(book.files.OpenAt(nowhere, path, length), nullptr);
  EXPECT_EQ(length, 0U);
  // And the files are as good as before.
  EXPECT_EQ(book.Read("a-name-of-twenty-two.x"), "p { margin: 0 }");
}

TEST(ZipBookFiles, ADocumentOpenedFromItsPlaceIsHandedOverForItsSheets) {
  bn::test::ZipBuilder zip;
  zip.Add("OEBPS/css/fonts.css", BytesOf("body { font-family: serif }"));
  zip.Add("OEBPS/text/one.xhtml",
          BytesOf("<html><head><link rel='stylesheet' href='../css/book.css'/></head><body/></html>"),
          {.deflate = true});
  zip.Add("OEBPS/css/book.css", BytesOf("@import 'fonts.css'; p { text-indent: 1em }"), {.deflate = true});
  BookFixture book(zip.Finish());
  const bn::epub::ZipEntry one = book.EntryOf("OEBPS/text/one.xhtml");

  Bytes parserMemory(262144);
  bn::epub::XmlParser parser(parserMemory);
  bn::epub::CssReader reader;
  Bytes tableMemory(4096);
  bn::epub::CssRules rules(tableMemory);
  std::vector<char> names(bn::epub::DocumentSheets::kNamesBytes);
  Bytes style(bn::epub::DocumentSheets::kStyleBytes);
  bn::epub::DocumentSheets sheets(book.files, parser, reader, rules, names, style);
  sheets.BeginBook();
  // The document comes from the one reader its sheets are then opened
  // with: its head has been read by the time the first of them is.
  std::array<char, bn::epub::kMaxBookPath> path{};
  std::size_t length = 0;
  bn::core::ByteSource* const document = book.files.OpenAt(one, path, length);
  ASSERT_NE(document, nullptr);
  ASSERT_EQ(sheets.Find(std::string_view(path.data(), length), *document), bn::epub::SheetsStatus::kOk);
  const std::span<const std::uint8_t> list = sheets.Sheets();
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(sheets.PathOf(list[0]), "OEBPS/css/fonts.css");
  EXPECT_EQ(sheets.PathOf(list[1]), "OEBPS/css/book.css");
  EXPECT_EQ(rules.Rules(), 2U);
}

TEST(ZipBookFiles, ADocumentsSheetsAreFoundInAZipFile) {
  bn::test::ZipBuilder zip;
  zip.Add("OEBPS/css/fonts.css", BytesOf("body { font-family: serif }"));
  zip.Add("OEBPS/text/one.xhtml",
          BytesOf("<html><head><link rel='stylesheet' href='../css/book.css'/><link rel='stylesheet' "
                  "href='gone.css'/><style>p.first { text-indent: 0 }</style></head><body/></html>"),
          {.deflate = true});
  zip.Add("OEBPS/css/book.css", BytesOf("@import 'fonts.css'; p { text-indent: 1em }"), {.deflate = true});
  BookFixture book(zip.Finish());

  Bytes parserMemory(262144);
  bn::epub::XmlParser parser(parserMemory);
  bn::epub::CssReader reader;
  Bytes tableMemory(4096);
  bn::epub::CssRules rules(tableMemory);
  std::vector<char> names(bn::epub::DocumentSheets::kNamesBytes);
  Bytes style(bn::epub::DocumentSheets::kStyleBytes);
  bn::epub::DocumentSheets sheets(book.files, parser, reader, rules, names, style);
  sheets.BeginBook();
  ASSERT_EQ(sheets.Find("OEBPS/text/one.xhtml"), bn::epub::SheetsStatus::kOk);
  const std::span<const std::uint8_t> list = sheets.Sheets();
  ASSERT_EQ(list.size(), 3U);
  EXPECT_EQ(sheets.PathOf(list[0]), "OEBPS/css/fonts.css");
  EXPECT_EQ(sheets.PathOf(list[1]), "OEBPS/css/book.css");
  EXPECT_TRUE(sheets.HasOwnStyle());
  EXPECT_EQ(rules.Rules(), 3U);
  EXPECT_EQ(sheets.LeftOut(bn::epub::SheetLeftOut::kLinkNotInBook), 1U);
}

}  // namespace
