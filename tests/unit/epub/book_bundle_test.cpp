#include "epub/book_bundle.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/package.h"
#include "epub/package_digest.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "storage_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::BookBundler;
using bn::epub::BookBundleStatus;
using bn::epub::Bundle;
using bn::epub::BundleBook;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
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

std::uint32_t U16(const Bytes& bytes, std::size_t at) { return bytes.at(at) | (std::uint32_t{bytes.at(at + 1)} << 8U); }
std::uint32_t U32(const Bytes& bytes, std::size_t at) { return U16(bytes, at) | (U16(bytes, at + 2) << 16U); }

const std::string kContainer =
    "<?xml version=\"1.0\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "<rootfiles><rootfile full-path=\"OPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
    "</container>\n";

// A package file with these three parts in it, which names the identifier
// whose id is "id" as the book's own.
std::string Opf(const std::string& metadata, const std::string& manifest, const std::string& spine) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">\n"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n" +
         metadata + "</metadata>\n<manifest>\n" + manifest + "</manifest>\n<spine toc=\"ncx\">\n" + spine +
         "</spine>\n</package>\n";
}

// A table of contents as EPUB 2 has one, of these entries: (how far in,
// what it points at, what it is called).
struct Entry {
  int depth;
  std::string source;
  std::string label;
};
std::string Ncx(const std::vector<Entry>& entries) {
  std::string out = "<?xml version=\"1.0\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\"><navMap>\n";
  int open = 0;
  for (const Entry& entry : entries) {
    for (; open > entry.depth; --open) {
      out += "</navPoint>\n";
    }
    out += "<navPoint><navLabel><text>" + entry.label + "</text></navLabel><content src=\"" + entry.source + "\"/>";
    ++open;
  }
  for (; open > 0; --open) {
    out += "</navPoint>\n";
  }
  return out + "</navMap></ncx>\n";
}

const std::string kMetadata =
    "<dc:title>The Book</dc:title><dc:creator>A. Writer</dc:creator><dc:language>en-GB</dc:language>\n"
    "<dc:identifier id=\"id\">urn:uuid:0123</dc:identifier><meta name=\"cover\" content=\"cover-image\"/>\n";

// A small book: four documents, one of them not in the ZIP file and one
// set aside; three pictures, one of them the cover and one not in the ZIP
// file; and a table of contents of four entries.
Files SmallBook() {
  const std::string manifest =
      "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
      "<item id=\"ch1\" href=\"text/one.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"plate\" href=\"images/plate.png\" media-type=\"image/png\"/>\n"
      "<item id=\"gone\" href=\"text/gone.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"cover-image\" href=\"images/cover.jpg\" media-type=\"image/jpeg\"/>\n"
      "<item id=\"ch2\" href=\"text/two.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"lost\" href=\"images/lost.gif\" media-type=\"image/gif\"/>\n"
      "<item id=\"notes\" href=\"text/notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  const std::string spine =
      "<itemref idref=\"ch1\"/><itemref idref=\"gone\"/><itemref idref=\"ch2\"/>"
      "<itemref idref=\"notes\" linear=\"no\"/>\n";
  return {
      {"mimetype", "application/epub+zip"},
      {"OPS/images/cover.jpg", "the cover's bytes"},
      {"META-INF/container.xml", kContainer},
      {"OPS/text/two.xhtml", "<html>two</html>"},
      {"OPS/content.opf", Opf(kMetadata, manifest, spine)},
      {"OPS/toc.ncx", Ncx({{0, "text/one.xhtml", "One"},
                           {1, "text/one.xhtml#part-2", "A part of it"},
                           {0, "", "A heading with no link"},
                           {0, "text/two.xhtml#fin", "Two &amp; the end"}})},
      {"OPS/images/plate.png", "the plate's bytes"},
      {"OPS/text/one.xhtml", "<html>one</html>"},
      {"OPS/text/notes.xhtml", "<html>notes</html>"},
  };
}

// An entry of a bundle's contents, as the bytes have it.
struct Kept {
  unsigned depth = 0;
  unsigned chapter = 0;
  std::string fragment;
  std::string label;

  bool operator==(const Kept& other) const = default;
};

// A book in storage, read as the device reads one, and the bundle made of
// it.
struct BookFixture {
  BookFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Puts a ZIP file of `files` in storage, each one compressed, and reads
  // its package.
  PackageStatus ReadBook(const Files& files, std::size_t packageBytes = 65'536) {
    ZipBuilder builder;
    for (const auto& [path, contents] : files) {
      builder.Add(path, BytesOf(contents), {.deflate = true});
    }
    return ReadZip(builder.Finish(), packageBytes);
  }

  PackageStatus ReadZip(const Bytes& bytes, std::size_t packageBytes = 65'536) {
    bundle.Close();
    bundleFile.Close();
    directory.Close();
    entries.Close();
    zip = bytes;
    storage.Put("/book.epub", zip);
    EXPECT_EQ(directory.Open(storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(entries.Open(storage, "/book.epub", OpenMode::kRead), StorageStatus::kOk);
    EXPECT_EQ(archive.Open(directory), bn::epub::ZipStatus::kOk);
    memory.assign(packageBytes, 0xEE);
    package = std::make_unique<Package>(memory);
    return package->Read(archive, entries, reader, xml);
  }

  static BundleBook About() {
    BundleBook about;
    about.textRules = 3;
    about.importer = 7;
    about.epubModified = 0x5B7A1234;
    const std::string madeBy = "tests";
    std::copy(madeBy.begin(), madeBy.end(), about.madeBy.begin());
    return about;
  }

  // Makes the bundle of the book that was read, a step at a time, holding
  // every step to what one may read and write. Leaves the bundle's file
  // closed.
  BookBundleStatus Make() {
    bundle.Close();
    bundleFile.Close();
    EXPECT_EQ(bundleFile.Open(storage, "/book.bundle", OpenMode::kCreate), StorageStatus::kOk);
    steps = 0;
    bool more = bundler.Begin(*package, archive, entries, reader, xml, bundleFile, About()) == BookBundleStatus::kOk;
    while (more) {
      const std::size_t reads = storage.reads.size();
      const std::size_t writes = storage.written.size();
      const int syncs = storage.syncs;
      more = bundler.Step();
      ++steps;
      std::size_t readBytes = 0;
      for (std::size_t i = reads; i < storage.reads.size(); ++i) {
        readBytes += storage.reads[i].size;
      }
      EXPECT_LE(readBytes, 5u * 4096u) << "step " << steps << " read more of the book than a step may";
      EXPECT_LE(storage.written.size(), writes + 2) << "step " << steps << " wrote more than two pages";
      for (std::size_t i = writes; i < storage.written.size(); ++i) {
        EXPECT_EQ(storage.written[i].size, 4096u) << "step " << steps;
        EXPECT_EQ(storage.written[i].offset % 4096, 0u) << "step " << steps;
      }
      EXPECT_LE(storage.syncs, syncs + 1) << "step " << steps;
    }
    bundleFile.Close();
    return bundler.Status();
  }

  BundleStatus Open() {
    bundle.Close();
    if (bundleFile.Open(storage, "/book.bundle", OpenMode::kRead) != StorageStatus::kOk) {
      return BundleStatus::kStoreFailed;
    }
    return bundle.Open(bundleFile);
  }

  // The whole of one of the bundle's tables.
  Bytes Table(BundleTable table) {
    Bytes bytes(bundle.Table(table).bytes);
    EXPECT_EQ(bundle.ReadTable(table, 0, bytes), BundleStatus::kOk);
    return bytes;
  }

  std::vector<std::string> Strings() {
    const Bytes bytes = Table(BundleTable::kStrings);
    std::vector<std::string> strings;
    for (std::size_t at = 0; at + 2 <= bytes.size();) {
      const std::size_t length = U16(bytes, at);
      strings.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at + 2),
                           bytes.begin() + static_cast<std::ptrdiff_t>(at + 2 + length));
      at += 2 + length;
    }
    return strings;
  }

  std::vector<Kept> Contents() {
    const Bytes bytes = Table(BundleTable::kContents);
    std::vector<Kept> all;
    for (std::size_t at = 0; at + 6 <= bytes.size();) {
      Kept kept;
      kept.depth = bytes[at];
      const std::size_t fragment = bytes[at + 1];
      kept.chapter = U16(bytes, at + 2);
      const std::size_t label = U16(bytes, at + 4);
      const auto from = bytes.begin() + static_cast<std::ptrdiff_t>(at + 6);
      kept.fragment.assign(from, from + static_cast<std::ptrdiff_t>(fragment));
      kept.label.assign(from + static_cast<std::ptrdiff_t>(fragment),
                        from + static_cast<std::ptrdiff_t>(fragment + label));
      all.push_back(kept);
      at += 6 + fragment + label;
    }
    return all;
  }

  // What the file a record says is there holds, read out of the EPUB by
  // the record alone.
  std::string Holds(const BundleFile& file) {
    if (reader.Open(entries, file.entry) != bn::epub::ZipStatus::kOk) {
      return "(cannot be opened)";
    }
    std::string out;
    std::array<std::uint8_t, 256> piece{};
    for (;;) {
      const std::ptrdiff_t got = reader.Read(piece);
      if (got <= 0) {
        return got == 0 ? out : "(cannot be read)";
      }
      out.append(piece.begin(), piece.begin() + got);
    }
  }

  // The checksum of the ZIP file's directory, from where its end record
  // says the directory is.
  std::uint32_t DirectoryChecksum() const {
    const std::size_t end = zip.size() - 22;
    const std::size_t bytes = U32(zip, end + 12);
    const std::size_t offset = U32(zip, end + 16);
    return bn::core::Crc32(std::span(zip).subspan(offset, bytes));
  }

  FakeStorage storage;
  Bytes zip;
  alignas(4) std::array<std::uint8_t, 4096> directoryBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> entryBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> bundleBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  alignas(4) std::array<std::uint8_t, 4096> records{};
  alignas(4) std::array<std::uint8_t, 4096> openPage{};
  BlockFile directory{directoryBuffer};
  BlockFile entries{entryBuffer};
  BlockFile bundleFile{bundleBuffer};
  bn::epub::ZipArchive archive;
  std::vector<std::uint8_t> window = std::vector<std::uint8_t>(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader{window};
  std::vector<std::uint8_t> xmlMemory = std::vector<std::uint8_t>(65'536);
  bn::epub::XmlParser xml{xmlMemory};
  std::vector<std::uint8_t> memory;
  std::unique_ptr<Package> package;
  BookBundler bundler{page, records};
  Bundle bundle{openPage};
  int steps = 0;
};

TEST(BookBundler, MakesABundleOfWhatTheBookSaysOfItself) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(SmallBook()), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);

  // What the header page says.
  const BundleBook& book = f.bundle.Book();
  EXPECT_EQ(book, f.bundler.Book());
  EXPECT_EQ(book.textRules, 3u);
  EXPECT_EQ(book.importer, 7u);
  EXPECT_EQ(book.epubBytes, f.zip.size());
  EXPECT_EQ(book.epubModified, 0x5B7A1234u);
  EXPECT_EQ(book.epubDirectoryChecksum, f.DirectoryChecksum());
  EXPECT_EQ(book.packageChecksum, bn::epub::PackageChecksum(*f.package));
  EXPECT_EQ(book.documents, 4u);
  EXPECT_EQ(book.pictures, 3u);
  EXPECT_EQ(book.contents, 4u);
  EXPECT_EQ(book.amiss, 0u);
  EXPECT_EQ(std::string(book.madeBy.data()), "tests");
  EXPECT_TRUE(f.bundle.IsOf(static_cast<std::uint32_t>(f.zip.size()), 0x5B7A1234, f.DirectoryChecksum()));

  EXPECT_EQ(f.Strings(), (std::vector<std::string>{"The Book", "A. Writer", "en-GB", "urn:uuid:0123"}));

  // The spine: each document's record opens that document.
  const std::array<std::string, 4> documents = {"<html>one</html>", "", "<html>two</html>", "<html>notes</html>"};
  for (std::size_t i = 0; i < 4; ++i) {
    BundleFile file;
    ASSERT_EQ(f.bundle.ReadSpine(i, file), BundleStatus::kOk);
    EXPECT_EQ(file.kind, static_cast<std::uint8_t>(MediaType::kXhtml)) << i;
    const bool present = i != 1;
    const bool inRun = i != 3;
    EXPECT_EQ(file.flags, (inRun ? bn::epub::kBundleFileInRun : 0U) | (present ? bn::epub::kBundleFilePresent : 0U))
        << i;
    if (present) {
      EXPECT_EQ(f.Holds(file), documents[i]) << i;
    } else {
      EXPECT_EQ(file.entry.headerOffset, 0u);
      EXPECT_EQ(file.entry.size, 0u);
    }
  }

  // The pictures, in the manifest's order, which is not the ZIP file's.
  BundleFile picture;
  ASSERT_EQ(f.bundle.ReadPicture(0, picture), BundleStatus::kOk);
  EXPECT_EQ(picture.kind, static_cast<std::uint8_t>(MediaType::kPng));
  EXPECT_EQ(picture.flags, bn::epub::kBundleFilePresent);
  EXPECT_EQ(f.Holds(picture), "the plate's bytes");
  ASSERT_EQ(f.bundle.ReadPicture(1, picture), BundleStatus::kOk);
  EXPECT_EQ(picture.kind, static_cast<std::uint8_t>(MediaType::kJpeg));
  EXPECT_EQ(picture.flags, bn::epub::kBundleFilePresent | bn::epub::kBundleFileCover);
  EXPECT_EQ(f.Holds(picture), "the cover's bytes");
  ASSERT_EQ(f.bundle.ReadPicture(2, picture), BundleStatus::kOk);
  EXPECT_EQ(picture.flags, 0u) << "a picture the EPUB does not have";
  EXPECT_EQ(picture.kind, 0u);
  EXPECT_EQ(picture.entry.size, 0u);
  EXPECT_EQ(f.bundler.PicturesPresent(), 2u);
  EXPECT_EQ(f.bundler.Passes(), 1u);

  EXPECT_EQ(f.Contents(), (std::vector<Kept>{{0, 0, "", "One"},
                                             {1, 0, "part-2", "A part of it"},
                                             {0, bn::epub::kBundleNoChapter, "", "A heading with no link"},
                                             {0, 2, "fin", "Two & the end"}}));

  // Nothing is imported yet; and every table is what its checksum says.
  EXPECT_EQ(f.bundle.Totals().notImported, 4u);
  for (std::size_t chapter = 0; chapter < 4; ++chapter) {
    BundleChapter record;
    ASSERT_EQ(f.bundle.ReadChapter(chapter, record), BundleStatus::kOk);
    EXPECT_EQ(record, BundleChapter{});
  }
  for (const BundleTable table :
       {BundleTable::kStrings, BundleTable::kSpine, BundleTable::kPictures, BundleTable::kContents}) {
    EXPECT_EQ(f.bundle.Check(table), BundleStatus::kOk);
  }
}

TEST(BookBundler, WritesTheStringsAndTheContentsAsTheirLengthsAndTheirBytes) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(SmallBook()), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  const std::string strings = std::string("\x08\x00The Book", 10) +
                              std::string(
                                  "\x09\x00"
                                  "A. Writer",
                                  11) +
                              std::string(
                                  "\x05\x00"
                                  "en-GB",
                                  7) +
                              std::string("\x0d\x00urn:uuid:0123", 15);
  EXPECT_EQ(f.Table(BundleTable::kStrings), BytesOf(strings));
  // How far in, the fragment's length, the document, the label's length,
  // the fragment, the label.
  const std::string contents = std::string("\x00\x00\x00\x00\x03\x00One", 9) +
                               std::string("\x01\x06\x00\x00\x0c\x00part-2A part of it", 24) +
                               std::string(
                                   "\x00\x00\xff\xff\x16\x00"
                                   "A heading with no link",
                                   28) +
                               std::string(
                                   "\x00\x03\x02\x00\x0d\x00"
                                   "finTwo & the end",
                                   22);
  EXPECT_EQ(f.Table(BundleTable::kContents), BytesOf(contents));
}

// A book of so many documents, pictures and entries of its table of
// contents, its pictures put into the ZIP file last one first.
Files BigBook(int documents, int pictures, int entries) {
  std::string manifest = "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
  std::string spine;
  Files files = {{"META-INF/container.xml", kContainer}};
  for (int i = 0; i < documents; ++i) {
    const std::string n = std::to_string(i);
    manifest.append("<item id=\"d").append(n).append("\" href=\"d").append(n);
    manifest.append(".xhtml\" media-type=\"application/xhtml+xml\"/>\n");
    spine += "<itemref idref=\"d" + n + "\"/>";
    files.emplace_back("OPS/d" + n + ".xhtml", "document " + n);
  }
  for (int i = 0; i < pictures; ++i) {
    const std::string n = std::to_string(i);
    manifest.append("<item id=\"p").append(n).append("\" href=\"img/p").append(n);
    manifest.append(".jpg\" media-type=\"image/jpeg\"/>\n");
  }
  for (int i = pictures - 1; i >= 0; --i) {
    if (i % 7 != 3) {  // One in seven is not in the ZIP file.
      files.emplace_back("OPS/img/p" + std::to_string(i) + ".jpg", "picture " + std::to_string(i));
    }
  }
  std::vector<Entry> toc;
  toc.reserve(static_cast<std::size_t>(entries));
  for (int i = 0; i < entries; ++i) {
    toc.push_back({i % 3 == 2 ? 1 : 0, "d" + std::to_string(i % documents) + ".xhtml#at-" + std::to_string(i),
                   "Entry number " + std::to_string(i)});
  }
  files.emplace_back("OPS/toc.ncx", Ncx(toc));
  files.emplace_back("OPS/content.opf", Opf(kMetadata, manifest, spine));
  return files;
}

TEST(BookBundler, FillsThePicturesInTheManifestsOrderWhateverOrderTheZipHasThemIn) {
  BookFixture f;
  // 400 pictures: three passes over the directory, of 170, 170 and 60.
  Files files = BigBook(3, 400, 5);
  // The ZIP file has one picture's name twice: the first is the one. And
  // the manifest has one path twice: the first item is the picture, and
  // the second has a place and no file.
  files.emplace_back("OPS/img/p11.jpg", "another of that name");
  std::string& opf = files.back().first == "OPS/content.opf" ? files.back().second : files[files.size() - 2].second;
  const std::string before = "</manifest>";
  opf.insert(opf.find(before), "<item id=\"again\" href=\"img/p20.jpg\" media-type=\"image/jpeg\"/>\n");
  ASSERT_EQ(f.ReadBook(files, 131'072), PackageStatus::kOk);
  ASSERT_EQ(f.package->Pictures(), 401u);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  EXPECT_EQ(f.bundler.Passes(), 3u);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  ASSERT_EQ(f.bundle.Book().pictures, 401u);
  std::uint32_t present = 0;
  for (std::size_t i = 0; i < 400; ++i) {
    BundleFile file;
    ASSERT_EQ(f.bundle.ReadPicture(i, file), BundleStatus::kOk);
    if (i % 7 == 3) {
      ASSERT_EQ(file.flags, 0u) << i;
      continue;
    }
    ++present;
    ASSERT_EQ(file.flags, bn::epub::kBundleFilePresent) << i;
    ASSERT_EQ(file.kind, static_cast<std::uint8_t>(MediaType::kJpeg)) << i;
    ASSERT_EQ(f.Holds(file), "picture " + std::to_string(i)) << i;
  }
  BundleFile again;
  ASSERT_EQ(f.bundle.ReadPicture(400, again), BundleStatus::kOk);
  EXPECT_EQ(again.flags, 0u);
  EXPECT_EQ(f.bundler.PicturesPresent(), present);
  EXPECT_EQ(f.bundle.Check(BundleTable::kPictures), BundleStatus::kOk);

  // A book with none goes through the directory for none.
  ASSERT_EQ(f.ReadBook(BigBook(2, 0, 1)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  EXPECT_EQ(f.bundler.Passes(), 0u);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().pictures, 0u);
  EXPECT_EQ(f.bundle.Table(BundleTable::kPictures).bytes, 0u);
  // And one with exactly a pass's worth, once.
  ASSERT_EQ(f.ReadBook(BigBook(2, 170, 1)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  EXPECT_EQ(f.bundler.Passes(), 1u);
  ASSERT_EQ(f.ReadBook(BigBook(2, 171, 1)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  EXPECT_EQ(f.bundler.Passes(), 2u);
}

TEST(BookBundler, DoesALittleAStepHoweverLargeTheBook) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(BigBook(600, 400, 900), 131'072), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk) << "each step is held to its bounds as the bundle is made";
  EXPECT_GT(f.steps, 100);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().documents, 600u);
  EXPECT_EQ(f.bundle.Book().contents, 900u);
  const std::vector<Kept> contents = f.Contents();
  ASSERT_EQ(contents.size(), 900u);
  EXPECT_EQ(contents[899], (Kept{1, 899 % 600, "at-899", "Entry number 899"}));
  BundleFile file;
  ASSERT_EQ(f.bundle.ReadSpine(599, file), BundleStatus::kOk);
  EXPECT_EQ(f.Holds(file), "document 599");
  for (const BundleTable table :
       {BundleTable::kStrings, BundleTable::kSpine, BundleTable::kPictures, BundleTable::kContents}) {
    EXPECT_EQ(f.bundle.Check(table), BundleStatus::kOk);
  }
}

TEST(BookBundler, MakesABundleOfABookWhoseContentsAreMissingOrSpoiled) {
  const auto with = [](const std::string& ncx, bool listed = true) {
    Files files = SmallBook();
    for (auto& [path, contents] : files) {
      if (path == "OPS/toc.ncx") {
        contents = ncx;
      }
      if (path == "OPS/content.opf" && !listed) {
        const std::size_t item = contents.find("<item id=\"ncx\"");
        contents.erase(item, contents.find("/>\n", item) + 3 - item);
      }
    }
    return files;
  };
  BookFixture f;
  // No table of contents at all: no contents, and nothing amiss.
  ASSERT_EQ(f.ReadBook(with("", false)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().contents, 0u);
  EXPECT_EQ(f.bundle.Book().amiss, 0u);
  EXPECT_EQ(f.bundle.Table(BundleTable::kContents).bytes, 0u);

  // One that is not a table of contents: none, and the header says so.
  ASSERT_EQ(f.ReadBook(with("this is not XML")), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().contents, 0u);
  EXPECT_EQ(f.bundle.Book().amiss, bn::epub::kBundleContentsNotWhole);

  // One that goes wrong part way: the entries that came before.
  std::string cut = Ncx({{0, "text/one.xhtml", "One"}, {0, "text/two.xhtml", "Two"}, {0, "text/two.xhtml", "Three"}});
  cut.insert(cut.find("<navPoint><navLabel><text>Three"), "<<<");
  ASSERT_EQ(f.ReadBook(with(cut)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().amiss, bn::epub::kBundleContentsNotWhole);
  EXPECT_EQ(f.Contents(), (std::vector<Kept>{{0, 0, "", "One"}, {0, 2, "", "Two"}}));
  EXPECT_EQ(f.bundle.Check(BundleTable::kContents), BundleStatus::kOk);

  // A package that goes wrong after its spine: the book is still a book.
  Files files = SmallBook();
  for (auto& [path, contents] : files) {
    if (path == "OPS/content.opf") {
      contents.insert(contents.find("</package>"), "<guide><<<");
    }
  }
  ASSERT_EQ(f.ReadBook(files), PackageStatus::kOk);
  ASSERT_TRUE(f.package->CutShort());
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().amiss, bn::epub::kBundlePackageCutShort);
  EXPECT_EQ(f.bundle.Book().contents, 4u);
}

TEST(BookBundler, KeepsNoMoreEntriesOfTheContentsThanABundleHasRoomFor) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(BigBook(2, 0, 65'540)), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  ASSERT_EQ(f.Open(), BundleStatus::kOk);
  EXPECT_EQ(f.bundle.Book().contents, 65'535u);
  EXPECT_EQ(f.bundle.Book().amiss, bn::epub::kBundleContentsNotWhole);
  EXPECT_EQ(f.bundle.Check(BundleTable::kContents), BundleStatus::kOk);
}

TEST(BookBundler, RefusesWhatItCannotMakeABundleOf) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(SmallBook()), PackageStatus::kOk);
  // A file for the bundle that already has something in it.
  f.storage.Put("/book.bundle", BytesOf("something"));
  ASSERT_EQ(f.bundleFile.Open(f.storage, "/book.bundle", OpenMode::kUpdate), StorageStatus::kOk);
  EXPECT_EQ(f.bundler.Begin(*f.package, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
            BookBundleStatus::kMisused);
  EXPECT_FALSE(f.bundler.Step());
  EXPECT_EQ(f.storage.Contents("/book.bundle"), BytesOf("something"));
  f.bundleFile.Close();

  // Memory that is not a page.
  std::array<std::uint8_t, 4092> small{};
  BookBundler cramped(f.page, small);
  ASSERT_EQ(f.bundleFile.Open(f.storage, "/book.bundle", OpenMode::kCreate), StorageStatus::kOk);
  EXPECT_EQ(cramped.Make(*f.package, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
            BookBundleStatus::kMisused);
  BookBundler crampedMaker(small, f.records);
  EXPECT_EQ(crampedMaker.Make(*f.package, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
            BookBundleStatus::kMisused);

  // A package that was not read, or could not be.
  Package unread(f.memory);
  EXPECT_EQ(f.bundler.Make(unread, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
            BookBundleStatus::kNoBook);
  ASSERT_NE(f.ReadBook({{"META-INF/container.xml", kContainer}, {"OPS/content.opf", "not a package"}}),
            PackageStatus::kOk);
  ASSERT_EQ(f.bundleFile.Open(f.storage, "/book.bundle", OpenMode::kCreate), StorageStatus::kOk);
  EXPECT_EQ(f.bundler.Make(*f.package, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
            BookBundleStatus::kNoBook);
  EXPECT_EQ(f.bundleFile.Size(), 0u) << "and nothing was written";
}

TEST(BookBundler, EndsInAStatusWhenStorageFailsAndLeavesNoBundle) {
  // How many steps the making of a book of some size takes; then storage
  // failing, to read and to write, before each of them. The table of
  // contents is the first file of the ZIP file, blocks away from the
  // package and from the directory, and its entries are called what
  // does not compress, so that it is blocks long itself: reading it is
  // then reads of storage in steps of its own, and not of a block the
  // file already has.
  Files book = BigBook(40, 200, 0);
  std::rotate(book.begin(), book.end() - 2, book.end() - 1);
  ASSERT_EQ(book.front().first, "OPS/toc.ncx");
  std::vector<Entry> toc;
  toc.reserve(400);
  std::uint32_t random = 12345;
  for (int i = 0; i < 400; ++i) {
    std::string label;
    for (int letter = 0; letter < 24; ++letter) {
      random = (random * 1664525U) + 1013904223U;
      label += static_cast<char>('a' + ((random >> 24U) % 26U));
    }
    toc.push_back({0, "d" + std::to_string(i % 40) + ".xhtml", label});
  }
  book.front().second = Ncx(toc);
  int all = 0;
  Bytes whole;
  {
    BookFixture f;
    ASSERT_EQ(f.ReadBook(book, 131072), PackageStatus::kOk);
    ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
    all = f.steps;
    whole = f.storage.Contents("/book.bundle");
  }
  int failedReading = 0;
  int failedWriting = 0;
  for (const int fault : {0, 1, 2}) {
    for (int before = 0; before < all; ++before) {
      BookFixture f;
      ASSERT_EQ(f.ReadBook(book, 131072), PackageStatus::kOk);
      ASSERT_EQ(f.bundleFile.Open(f.storage, "/book.bundle", OpenMode::kCreate), StorageStatus::kOk);
      ASSERT_EQ(f.bundler.Begin(*f.package, f.archive, f.entries, f.reader, f.xml, f.bundleFile, BookFixture::About()),
                BookBundleStatus::kOk);
      bool more = true;
      for (int step = 0; step < before && more; ++step) {
        more = f.bundler.Step();
      }
      ASSERT_TRUE(more);
      f.storage.failReads = fault == 0;
      f.storage.failWrites = fault == 1 ? StorageStatus::kNoSpace : StorageStatus::kOk;
      f.storage.failSyncs = fault == 2;
      while (f.bundler.Step()) {
      }
      f.storage.failReads = false;
      f.storage.failWrites = StorageStatus::kOk;
      f.storage.failSyncs = false;
      f.bundleFile.Close();
      const BookBundleStatus status = f.bundler.Status();
      if (status == BookBundleStatus::kOk) {
        // Nothing that was left of it asked that of storage: the bundle
        // is the one made when nothing fails, and not one with less in
        // it for what could not be read.
        EXPECT_EQ(f.Open(), BundleStatus::kOk) << fault << " " << before;
        EXPECT_TRUE(f.storage.Contents("/book.bundle") == whole)
            << "storage failed and a different bundle was made: " << fault << " before step " << before;
        continue;
      }
      EXPECT_EQ(status, fault == 0 ? BookBundleStatus::kUnreadable : BookBundleStatus::kStoreFailed)
          << fault << " before step " << before;
      failedReading += fault == 0 ? 1 : 0;
      failedWriting += fault != 0 ? 1 : 0;
      if (fault != 2) {
        // (A sync that fails leaves whatever the card has, which here is
        // all of it: it is the status that says to make it again.)
        EXPECT_NE(f.Open(), BundleStatus::kOk) << "a making that failed left a bundle: " << fault << " " << before;
      }
    }
  }
  EXPECT_GT(failedReading, 2);
  EXPECT_GT(failedWriting, 4);
}

TEST(BookBundler, MakesTheSameBundleOfTheSameBookEveryTime) {
  BookFixture f;
  ASSERT_EQ(f.ReadBook(BigBook(60, 200, 80), 131'072), PackageStatus::kOk);
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  const Bytes first = f.storage.Contents("/book.bundle");
  ASSERT_EQ(f.Make(), BookBundleStatus::kOk);
  EXPECT_EQ(f.storage.Contents("/book.bundle"), first);
}

}  // namespace
