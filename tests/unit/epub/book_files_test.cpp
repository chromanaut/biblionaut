#include "epub/book_files.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
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
  std::string Read(std::string_view path) {
    bn::core::ByteSource* const source = files.Open(path);
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
