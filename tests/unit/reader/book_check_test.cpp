#include "reader/book_check.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "core/metrics.h"
#include "epub/xml.h"
#include "epub/xml_digest.h"
#include "epub/zip.h"
#include "reader/trace_ids.h"
#include "storage_fakes.h"
#include "stream_fakes.h"
#include "trace_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::StorageStatus;
using bn::reader::BookCheck;
using bn::test::FakeStorage;
using bn::test::ZipBuilder;
using Bytes = std::vector<std::uint8_t>;
using State = bn::reader::BookCheck::State;

Bytes BytesOf(const std::string& text) { return {text.begin(), text.end()}; }

// Text that compresses well, `size` bytes of it.
Bytes Prose(std::size_t size, unsigned seed = 1) {
  static const std::array<std::string, 6> kWords = {"the ", "reader ", "turned ", "a ", "page ", "quietly.\n"};
  Bytes out;
  unsigned state = seed;
  while (out.size() < size) {
    state = (state * 1103515245U) + 12345U;
    const std::string& word = kWords.at((state >> 16U) % kWords.size());
    out.insert(out.end(), word.begin(), word.end());
  }
  out.resize(size);
  return out;
}

// A chapter of XHTML exactly `size` bytes long.
Bytes Xhtml(std::size_t size, unsigned seed = 1) {
  const std::string open = "<html><body><p class=\"text\">";
  const std::string close = "</p></body></html>";
  Bytes out(open.begin(), open.end());
  const Bytes words = Prose(size - open.size() - close.size(), seed);
  out.insert(out.end(), words.begin(), words.end());
  out.insert(out.end(), close.begin(), close.end());
  return out;
}

// What the parser finds in `documents`, taken one after another the way
// the check takes the XML of a book: (how many events, their checksum).
std::pair<std::uint32_t, std::uint32_t> EventsOf(const std::vector<Bytes>& documents) {
  std::vector<std::uint8_t> memory(65'536);
  bn::epub::XmlParser parser(memory);
  bn::epub::XmlEventDigest digest;
  for (const Bytes& document : documents) {
    bn::core::MemoryByteSource source(document);
    (void)parser.Parse(source, digest);
    digest.EndDocument();
  }
  return {digest.Events(), digest.Checksum()};
}

std::string Hex8(std::uint32_t value) {
  std::array<char, 9> text{};
  std::snprintf(text.data(), text.size(), "%08x", static_cast<unsigned>(value));
  return text.data();
}

// A clock that moves on a little every time it is read, so that whatever
// is timed takes some time.
class SteppingClock final : public bn::core::Clock {
 public:
  std::uint64_t NowUs() noexcept override {
    now += step;
    return now;
  }
  std::uint64_t now = 0;
  std::uint64_t step = 50;
};

// A small book: 5 entries, of which 3 are compressed, and those three XML.
struct Book {
  explicit Book(unsigned seed = 1) : xml{Xhtml(300, seed), Xhtml(30'000, seed + 1), Xhtml(9'000, seed + 2)} {
    ZipBuilder builder;
    builder.Add("mimetype", BytesOf("application/epub+zip"));
    builder.Add("META-INF/container.xml", xml[0], {.deflate = true});
    builder.Add("OEBPS/chapter1.xhtml", xml[1], {.deflate = true});
    builder.Add("OEBPS/chapter2.xhtml", xml[2], {.deflate = true, .descriptor = true});
    builder.Add("OEBPS/cover.jpg", Prose(5'000, seed + 3));
    file = builder.Finish();
    chapterData = builder.At(2).data;
    inflatedIn = builder.At(1).dataSize + builder.At(2).dataSize + builder.At(3).dataSize;
  }
  // What its entries take up in the file: the compressed ones, and the two kept as they are.
  std::uint64_t BytesIn() const { return inflatedIn + 20 + 5'000; }
  static constexpr std::uint32_t kEntries = 5;
  static constexpr std::uint64_t kBytesOut = 20 + 300 + 30'000 + 9'000 + 5'000;
  static constexpr std::uint64_t kInflatedOut = 300 + 30'000 + 9'000;
  static constexpr std::uint32_t kXmlDocuments = 3;
  static constexpr std::uint64_t kXmlBytes = 300 + 30'000 + 9'000;
  std::vector<Bytes> xml;  // Its XML documents, in the order they are in the file.
  Bytes file;
  std::size_t chapterData = 0;  // Where the first chapter's compressed bytes are.
  std::uint64_t inflatedIn = 0;
};

struct CheckFixture {
  CheckFixture() { EXPECT_EQ(storage.Start(), StorageStatus::kOk); }

  // Turns until the check is no longer running. Returns how many it took.
  // NOLINTNEXTLINE(readability-make-member-function-const): it changes the check, which is only pointed to.
  int RunToTheEnd() {
    int turns = 0;
    while (check->CurrentState() == State::kRunning && turns < 100'000) {
      check->Tick();
      ++turns;
    }
    return turns;
  }

  // Every line of text sent so far.
  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    for (const bn::test::StreamItem& item : bn::test::ParseStream(sink.received)) {
      if (item.isFrame && item.type == bn::core::FrameType::kText) {
        lines.push_back(item.Text());
      }
    }
    return lines;
  }
  // The first line that has `part` in it, or "".
  std::string LineWith(const std::string& part) const {
    for (const std::string& line : Lines()) {
      if (line.find(part) != std::string::npos) {
        return line;
      }
    }
    return {};
  }
  std::string AllLines() const {
    std::string all;
    for (const std::string& line : Lines()) {
      all += line + "\n";
    }
    return all;
  }

  // What the trace holds under `id`, in order.
  std::vector<std::uint32_t> Recorded(bn::core::SpanId id) {
    std::array<bn::core::TraceRecord, 64> records{};
    const std::size_t count = trace.tracer.Drain(records);
    std::vector<std::uint32_t> numbers;
    for (std::size_t i = 0; i < count; ++i) {
      if (records[i].id == id) {
        numbers.push_back(records[i].arg);
      }
    }
    return numbers;
  }

  bn::test::TraceFixture<64> trace;
  bn::test::FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};
  FakeStorage storage;
  SteppingClock clock;
  // On the heap, where the application keeps it.
  std::unique_ptr<BookCheck> check = std::make_unique<BookCheck>(storage, clock, stream);
};

// What reading the ZIP file at `path` straight through costs in reads of
// storage, done the way `bn-zip check` does it: each entry read out 4 KB
// at a time and then looked up by name, through two files with 4 KB
// buffers.
std::size_t ReadsOfAStraightCheck(FakeStorage& storage, const std::string& path) {
  const std::size_t before = storage.reads.size();
  std::array<std::uint8_t, 4096> directoryBuffer{};
  std::array<std::uint8_t, 4096> entriesBuffer{};
  bn::core::BlockFile directory(directoryBuffer);
  bn::core::BlockFile entries(entriesBuffer);
  EXPECT_EQ(directory.Open(storage, path, bn::core::OpenMode::kRead), StorageStatus::kOk);
  EXPECT_EQ(entries.Open(storage, path, bn::core::OpenMode::kRead), StorageStatus::kOk);
  bn::epub::ZipArchive archive;
  EXPECT_EQ(archive.Open(directory), bn::epub::ZipStatus::kOk);
  std::vector<std::uint8_t> window(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader reader(window);
  bn::epub::ZipArchive::Cursor cursor;
  std::array<char, 256> name{};
  std::array<std::uint8_t, 4096> part{};
  for (;;) {
    bn::epub::ZipEntry entry;
    if (archive.Next(cursor, entry, name) != bn::epub::ZipStatus::kOk) {
      break;
    }
    EXPECT_EQ(reader.Open(entries, entry), bn::epub::ZipStatus::kOk);
    while (reader.Read(part) > 0) {
    }
    bn::epub::ZipEntry found;
    EXPECT_EQ(archive.Find(std::string_view(name.data(), entry.nameLength), found), bn::epub::ZipStatus::kOk);
  }
  return storage.reads.size() - before;
}

TEST(BookCheck, DoesNothingUntilItIsBegun) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  EXPECT_EQ(fixture.check->CurrentState(), State::kIdle);
  fixture.check->Tick();
  fixture.check->Tick();
  EXPECT_EQ(fixture.check->CurrentState(), State::kIdle);
  EXPECT_EQ(fixture.storage.opens, 0);
  EXPECT_EQ(fixture.storage.directoryOpens, 0);
  EXPECT_TRUE(fixture.Lines().empty());
}

TEST(BookCheck, ReadsEveryBookAndSaysWhatItFound) {
  CheckFixture fixture;
  const Book first(1);
  const Book second(50);
  fixture.storage.Put("/books/Alpha.epub", first.file);
  fixture.storage.Put("/books/Beta, Volume 2.EPUB", second.file);
  // Things in /books that are not books.
  fixture.storage.Put("/books/notes.txt", BytesOf("not a book"));
  fixture.storage.Put("/books/._Alpha.epub", BytesOf("what a computer left beside the real one"));
  fixture.storage.Put("/books/.epub", BytesOf("a name that is all ending"));
  fixture.storage.Put("/books/folder.epub/inside.txt", BytesOf("a directory, whatever it is called"));
  // And a book that is not in /books.
  fixture.storage.Put("/elsewhere.epub", first.file);

  fixture.check->Begin();
  EXPECT_EQ(fixture.check->CurrentState(), State::kRunning);
  EXPECT_EQ(fixture.storage.opens, 0) << "nothing happens until a turn";
  fixture.RunToTheEnd();

  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_EQ(fixture.check->Books(), 2u);
  EXPECT_EQ(fixture.check->FailedBooks(), 0u);
  const BookCheck::Figures& totals = fixture.check->Totals();
  EXPECT_EQ(totals.entries, 2 * Book::kEntries);
  EXPECT_EQ(totals.failedEntries, 0u);
  EXPECT_EQ(totals.bytesOut, 2 * Book::kBytesOut);
  EXPECT_EQ(totals.bytesIn, first.BytesIn() + second.BytesIn());
  EXPECT_EQ(totals.inflatedOut, 2 * Book::kInflatedOut);
  EXPECT_EQ(totals.inflatedIn, first.inflatedIn + second.inflatedIn);

  // It changed nothing and left nothing open.
  EXPECT_EQ(fixture.storage.changes, 0);
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
  EXPECT_EQ(fixture.storage.ListedCount(), 0);

  // A line as each book is begun, two as it ends, and two for them all.
  const std::string all = fixture.AllLines();
  EXPECT_NE(all.find("book check: the checksum alone, 262144 bytes in "), std::string::npos) << all;
  EXPECT_NE(all.find("book 1: " + std::to_string(first.file.size()) + " bytes, /books/Alpha.epub\n"), std::string::npos)
      << all;
  EXPECT_NE(all.find("book 2: " + std::to_string(second.file.size()) + " bytes, /books/Beta, Volume 2.EPUB\n"),
            std::string::npos)
      << all;
  const std::string sizes =
      "5 entries, " + std::to_string(Book::kBytesOut) + " bytes out of " + std::to_string(first.BytesIn()) + ".";
  EXPECT_NE(all.find("book 1: ok. " + sizes + " Directory found in 1 read, "), std::string::npos) << all;
  EXPECT_NE(fixture.LineWith("book 1: card ").find(" bytes in ("), std::string::npos) << all;
  EXPECT_NE(all.find("book check: passed. 2 books, 10 entries, " + std::to_string(2 * Book::kBytesOut) +
                     " bytes out of " + std::to_string(first.BytesIn() + second.BytesIn()) + ".\n"),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("book check: card "), std::string::npos) << all;
  EXPECT_EQ(all.find("FAILED"), std::string::npos) << all;
  EXPECT_EQ(all.find("elsewhere"), std::string::npos) << all;

  // And the trace has each book and the whole run.
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanBookCheckBook), (std::vector<std::uint32_t>{1, 1}));
  std::array<char, 2048> metrics{};
  bn::core::DefaultMetrics().FormatLine(metrics);
  const std::string line(metrics.data());
  EXPECT_NE(line.find("book_check=1["), std::string::npos) << line;
  EXPECT_NE(line.find("book_check_books=2["), std::string::npos) << line;
}

TEST(BookCheck, RecordsTheWholeRunInTheTrace) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanBookCheck), std::vector<std::uint32_t>{1});
}

TEST(BookCheck, MakesTheSameReadsAsACheckOnTheDesktopAndCountsEveryOne) {
  CheckFixture fixture;
  // Big enough that its directory and its entries are blocks apart.
  ZipBuilder builder;
  builder.Add("mimetype", BytesOf("application/epub+zip"));
  for (int chapter = 0; chapter < 40; ++chapter) {
    // Every other one is XML and is parsed to its end; the rest are not,
    // and the parser stops at their first byte. Either way the entry is
    // read out just as it would be without a parser.
    const std::size_t size = 3'000 + (static_cast<std::size_t>(chapter) * 700);
    const auto seed = static_cast<unsigned>(chapter);
    builder.Add("OEBPS/text/chapter-" + std::to_string(chapter) + ".xhtml",
                chapter % 2 == 0 ? Xhtml(size, seed) : Prose(size, seed), {.deflate = chapter % 5 != 0});
  }
  fixture.storage.Put("/books/long.epub", builder.Finish());

  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const BookCheck::Figures totals = fixture.check->Totals();
  const std::size_t readsOfTheCheck = fixture.storage.reads.size();
  EXPECT_EQ(totals.reads, readsOfTheCheck) << "every read of storage is counted to the book";
  EXPECT_GT(totals.reads, 20u);
  std::uint64_t bytes = 0;
  for (const FakeStorage::ReadCall& call : fixture.storage.reads) {
    bytes += std::min<std::uint64_t>(call.size, fixture.storage.Contents("/books/long.epub").size() - call.offset);
  }
  EXPECT_EQ(totals.readBytes, bytes);

  EXPECT_EQ(ReadsOfAStraightCheck(fixture.storage, "/books/long.epub"), readsOfTheCheck);
}

TEST(BookCheck, ParsesTheXmlOfEveryBookAndSaysWhatItFound) {
  CheckFixture fixture;
  const Book first(1);
  const Book second(50);
  fixture.storage.Put("/books/a.epub", first.file);
  fixture.storage.Put("/books/b.epub", second.file);
  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();

  const BookCheck::Figures& totals = fixture.check->Totals();
  EXPECT_EQ(totals.xmlDocuments, 2 * Book::kXmlDocuments);
  EXPECT_EQ(totals.xmlBytes, 2 * Book::kXmlBytes);
  EXPECT_EQ(totals.xmlNotXml, 0u);
  EXPECT_EQ(totals.xmlRefused, 0u);
  // What the parser found is what it finds in the same documents on
  // their own, in the same order.
  const auto [firstEvents, firstChecksum] = EventsOf(first.xml);
  const auto [secondEvents, secondChecksum] = EventsOf(second.xml);
  EXPECT_EQ(totals.xmlEvents, firstEvents + secondEvents);
  EXPECT_EQ(fixture.check->XmlChecksum(), secondChecksum) << "of the book last read";
  EXPECT_GT(firstEvents, 10u);
  EXPECT_GT(totals.xmlEventBytes, 2 * Book::kXmlBytes - 400) << "nearly all of it is text, and the text is checksummed";
  EXPECT_GT(totals.xmlMostMemory, 2'000u);
  EXPECT_LT(totals.xmlMostMemory, BookCheck::kXmlMemoryBytes);

  const std::string all = fixture.AllLines();
  EXPECT_NE(all.find("book 1: XML: 3 documents, 39300 bytes, " + std::to_string(firstEvents) + " events, checksum " +
                     Hex8(firstChecksum) + "; parsing "),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("book 2: XML: 3 documents, 39300 bytes, " + std::to_string(secondEvents) + " events, checksum " +
                     Hex8(secondChecksum) + "; parsing "),
            std::string::npos)
      << all;
  const std::string line = fixture.LineWith("book 2: XML: ");
  EXPECT_NE(line.find(" KB/s); most memory "), std::string::npos) << line;
  EXPECT_TRUE(line.ends_with('.'));
  EXPECT_EQ(line.find("Not XML"), std::string::npos) << line;
  const std::string last = fixture.LineWith("book check: XML: ");
  EXPECT_NE(last.find("book check: XML: 6 documents, 78600 bytes, " + std::to_string(firstEvents + secondEvents) +
                      " events; parsing "),
            std::string::npos)
      << last;
  EXPECT_TRUE(last.ends_with(" KB/s); " + std::to_string(totals.xmlEventBytes) + " bytes of events checksummed."))
      << last;
  EXPECT_EQ(fixture.LineWith("book check: XML memory: "),
            "book check: XML memory: the most a document used " + std::to_string(totals.xmlMostMemory) +
                " bytes; documents over 8 KB: " + std::to_string(totals.xmlOver8K) + ", over 12 KB: " +
                std::to_string(totals.xmlOver12K) + ", over 16 KB: " + std::to_string(totals.xmlOver16K) + ".");

  std::array<char, 2048> metrics{};
  bn::core::DefaultMetrics().FormatLine(metrics);
  EXPECT_NE(std::string(metrics.data()).find("xml_kb_per_s="), std::string::npos) << metrics.data();
}

TEST(BookCheck, TakesAFileForXmlByTheEndOfItsName) {
  CheckFixture fixture;
  ZipBuilder builder;
  const Bytes document = Xhtml(500);
  for (const char* name : {"a.xhtml", "b.XHTML", "c.html", "d.htm", "e.xml", "f.opf", "g.ncx", "toc.NCX"}) {
    builder.Add(name, document, {.deflate = true});
  }
  for (const char* name : {"h.css", "i.jpg", "xhtml", "j.xhtml.bak", "mimetype"}) {
    builder.Add(name, document);
  }
  fixture.storage.Put("/books/a.epub", builder.Finish());
  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_EQ(fixture.check->Totals().entries, 13u);
  EXPECT_EQ(fixture.check->Totals().xmlDocuments, 8u);
  EXPECT_EQ(fixture.check->Totals().xmlBytes, 8u * 500);
}

TEST(BookCheck, CountsWhatIsNotXmlAndReadsItToTheEndAllTheSame) {
  CheckFixture fixture;
  // Two documents the parser stops in, one near its start and one near
  // its end, and a good one between them.
  Bytes mismatched = BytesOf("<html><body><p>one</b>");
  const Bytes rest = Prose(20'000);
  mismatched.insert(mismatched.end(), rest.begin(), rest.end());
  Bytes unfinished = Xhtml(15'000);
  unfinished.resize(unfinished.size() - 10);
  const Bytes good = Xhtml(4'000, 7);
  const Bytes otherEncoding = BytesOf("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a>caf\xE9</a>");
  ZipBuilder builder;
  builder.Add("mismatched.xhtml", mismatched, {.deflate = true});
  builder.Add("good.xhtml", good, {.deflate = true});
  builder.Add("unfinished.xhtml", unfinished);
  builder.Add("windows.xml", otherEncoding);
  fixture.storage.Put("/books/a.epub", builder.Finish());

  fixture.check->Begin();
  fixture.RunToTheEnd();
  // A book is none the worse, as a file, for holding what is not XML.
  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const BookCheck::Figures& totals = fixture.check->Totals();
  EXPECT_EQ(totals.entries, 4u);
  EXPECT_EQ(totals.failedEntries, 0u);
  EXPECT_EQ(totals.bytesOut, mismatched.size() + good.size() + unfinished.size() + otherEncoding.size())
      << "every byte was read out";
  EXPECT_EQ(totals.xmlDocuments, 4u);
  EXPECT_EQ(totals.xmlNotXml, 3u);
  EXPECT_EQ(totals.xmlRefused, 0u);
  // The parser was given no more of the first than it took to find the
  // mistake, and all of the others.
  EXPECT_LE(totals.xmlBytes, 4'096u + good.size() + unfinished.size() + otherEncoding.size());
  EXPECT_GE(totals.xmlBytes, good.size() + unfinished.size() + otherEncoding.size());
  // What it found before each mistake counts, as it does for bn-xml.
  const auto [events, checksum] = EventsOf({mismatched, good, unfinished, otherEncoding});
  EXPECT_EQ(totals.xmlEvents, events);
  EXPECT_EQ(fixture.check->XmlChecksum(), checksum);
  EXPECT_TRUE(fixture.LineWith("book 1: XML: 4 documents, ").ends_with(". Not XML: 3.")) << fixture.AllLines();
  EXPECT_NE(fixture.LineWith("book check: XML: 4 documents, ").find(" Not XML: 3."), std::string::npos)
      << fixture.AllLines();
}

TEST(BookCheck, CountsADocumentThatNeedsMoreMemoryThanTheParserHas) {
  CheckFixture fixture;
  // One tag longer than all the parser's memory, and a good deal after it.
  std::string huge = "<html><img src=\"data:" + std::string(2 * BookCheck::kXmlMemoryBytes, 'A') + "\"/>";
  huge += std::string(10'000, 'x') + "</html>";
  ZipBuilder builder;
  builder.Add("huge.xhtml", BytesOf(huge), {.deflate = true});
  builder.Add("good.xhtml", Xhtml(4'000), {.deflate = true});
  fixture.storage.Put("/books/a.epub", builder.Finish());

  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  const BookCheck::Figures& totals = fixture.check->Totals();
  EXPECT_EQ(totals.failedEntries, 0u);
  EXPECT_EQ(totals.bytesOut, huge.size() + 4'000);
  EXPECT_EQ(totals.xmlDocuments, 2u);
  EXPECT_EQ(totals.xmlRefused, 1u);
  EXPECT_EQ(totals.xmlNotXml, 0u);
  // The long one took all it could get; the good one takes its few
  // thousand bytes, how many depending on the computer.
  EXPECT_GT(totals.xmlMostMemory, 16u * 1024);
  EXPECT_EQ(totals.xmlOver16K, 1u);
  EXPECT_GE(totals.xmlOver12K, 1u);
  EXPECT_GE(totals.xmlOver8K, totals.xmlOver12K);
  EXPECT_TRUE(fixture.LineWith("book 1: XML: 2 documents, ").ends_with(". Refused for memory: 1."))
      << fixture.AllLines();
  // The good one after it was parsed as if nothing had happened.
  const std::uint32_t events = EventsOf({Xhtml(4'000)}).first;
  EXPECT_EQ(totals.xmlEvents, events + 1) << "and the one event the long tag came after";
}

TEST(BookCheck, CountsItsTimeInThreeParts) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed);
  const BookCheck::Figures& totals = fixture.check->Totals();
  // The clock moves 50 us at each reading of it, and storage's meter reads
  // it twice around each call it passes on.
  EXPECT_GT(totals.storageUs, 0u);
  EXPECT_EQ(totals.storageUs % 50, 0u);
  EXPECT_GE(totals.storageUs, std::uint64_t{50} * totals.reads);
  EXPECT_GT(totals.inflateUs, 0u);
  EXPECT_GT(totals.xmlUs, 0u);
  EXPECT_EQ(totals.xmlUs % 50, 0u);
  EXPECT_GT(totals.workUs, totals.storageUs + totals.inflateUs + totals.xmlUs)
      << "there is the directory to read as well";
  EXPECT_GT(fixture.check->ChecksumUs(), 0u);

  // A book with nothing compressed in it has no inflating to time.
  CheckFixture plain;
  ZipBuilder builder;
  builder.Add("mimetype", BytesOf("application/epub+zip"));
  builder.Add("OEBPS/cover.jpg", Prose(20'000));
  plain.storage.Put("/books/plain.epub", builder.Finish());
  plain.check->Begin();
  plain.RunToTheEnd();
  ASSERT_EQ(plain.check->CurrentState(), State::kPassed);
  EXPECT_EQ(plain.check->Totals().inflateUs, 0u);
  EXPECT_EQ(plain.check->Totals().inflatedOut, 0u);
  EXPECT_EQ(plain.check->Totals().inflatedIn, 0u);
  EXPECT_EQ(plain.check->Totals().bytesOut, 20'020u);
  // Nor any XML to parse, and so no line about it.
  EXPECT_EQ(plain.check->Totals().xmlDocuments, 0u);
  EXPECT_EQ(plain.check->Totals().xmlUs, 0u);
  EXPECT_EQ(plain.AllLines().find("XML"), std::string::npos) << plain.AllLines();
}

TEST(BookCheck, WorksForAShortWhileEachTurn) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  fixture.storage.Put("/books/b.epub", Book(9).file);
  // Each reading of the clock is a millisecond: a turn is over after a
  // few steps.
  fixture.clock.step = 1'000;
  fixture.check->Begin();
  int turns = 0;
  while (fixture.check->CurrentState() == State::kRunning && turns < 10'000) {
    const std::uint64_t before = fixture.clock.now;
    fixture.check->Tick();
    const std::uint64_t took = fixture.clock.now - before;
    // A turn goes on until the slice is used up, and no more than one
    // step past that. No step here reads the clock more than forty times.
    EXPECT_LT(took, BookCheck::kSliceUs + 40'000) << turns;
    if (fixture.check->CurrentState() == State::kRunning) {
      EXPECT_GE(took, BookCheck::kSliceUs) << turns;
    }
    ++turns;
  }
  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed);
  EXPECT_GT(turns, 3);
  EXPECT_EQ(fixture.check->Totals().entries, 2 * Book::kEntries);

  // With a clock that stands still there is nothing to stop a turn, and
  // one is enough.
  CheckFixture still;
  still.storage.Put("/books/a.epub", Book().file);
  still.clock.step = 0;
  still.check->Begin();
  still.check->Tick();
  EXPECT_EQ(still.check->CurrentState(), State::kPassed);
  EXPECT_EQ(still.check->Totals().workUs, 0u);
  EXPECT_NE(still.LineWith("book 1: card ").find("(0 KB/s)"), std::string::npos) << still.AllLines();
}

TEST(BookCheck, LooksInTheTopDirectoryWhenBooksHasNoBook) {
  // No /books at all.
  CheckFixture none;
  none.storage.Put("/top.epub", Book().file);
  none.storage.Put("/readme.txt", BytesOf("x"));
  none.check->Begin();
  none.RunToTheEnd();
  EXPECT_EQ(none.check->CurrentState(), State::kPassed) << none.AllLines();
  EXPECT_EQ(none.check->Books(), 1u);
  EXPECT_NE(none.AllLines().find("book check: there is no /books. Looking in the top directory.\n"), std::string::npos);
  EXPECT_NE(none.AllLines().find(" bytes, /top.epub\n"), std::string::npos) << none.AllLines();

  // A /books with no book in it.
  CheckFixture empty;
  empty.storage.Put("/books/readme.txt", BytesOf("x"));
  empty.storage.Put("/top.epub", Book().file);
  empty.check->Begin();
  empty.RunToTheEnd();
  EXPECT_EQ(empty.check->CurrentState(), State::kPassed) << empty.AllLines();
  EXPECT_NE(empty.AllLines().find("book check: no book in /books. Looking in the top directory.\n"), std::string::npos);
  EXPECT_EQ(empty.check->Books(), 1u);

  // Books in /books: the top directory is left alone.
  CheckFixture both;
  both.storage.Put("/books/a.epub", Book().file);
  both.storage.Put("/top.epub", BytesOf("not even a ZIP file"));
  both.check->Begin();
  both.RunToTheEnd();
  EXPECT_EQ(both.check->CurrentState(), State::kPassed) << both.AllLines();
  EXPECT_EQ(both.check->Books(), 1u);
  EXPECT_EQ(both.storage.directoryOpens, 1);
}

TEST(BookCheck, LooksInFoldersOneLevelDownAndNoFurther) {
  CheckFixture fixture;
  fixture.storage.Put("/books/Solo.epub", Book(1).file);
  fixture.storage.Put("/books/Series One/1 First.epub", Book(2).file);
  fixture.storage.Put("/books/Series One/2 Second.epub", Book(3).file);
  fixture.storage.Put("/books/Series One/cover.jpg", BytesOf("not a book"));
  fixture.storage.Put("/books/Series Two/Only.epub", Book(4).file);
  // Not looked in: a folder inside a folder, a folder whose name begins
  // with a dot, and one whose name leaves no room for a book's.
  fixture.storage.Put("/books/Series One/extras/Deeper.epub", Book(5).file);
  fixture.storage.Put("/books/.cache/Hidden.epub", Book(6).file);
  fixture.storage.Put("/books/" + std::string(243, 'f') + "/a.epub", Book(7).file);

  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_EQ(fixture.check->Books(), 4u);
  EXPECT_EQ(fixture.check->Totals().entries, 4 * Book::kEntries);
  EXPECT_EQ(fixture.storage.directoryOpens, 3) << "/books and the two series";
  const std::string all = fixture.AllLines();
  for (const char* path : {"/books/Solo.epub", "/books/Series One/1 First.epub", "/books/Series One/2 Second.epub",
                           "/books/Series Two/Only.epub"}) {
    EXPECT_NE(all.find(std::string(" bytes, ") + path + "\n"), std::string::npos) << path << "\n" << all;
  }
  EXPECT_EQ(all.find("Deeper"), std::string::npos) << all;
  EXPECT_EQ(all.find("Hidden"), std::string::npos) << all;
  EXPECT_NE(all.find("book check: 1 folder inside a folder not looked in: only one level down is.\n"),
            std::string::npos)
      << all;
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
  EXPECT_EQ(fixture.storage.ListedCount(), 0);

  // The same from the top directory, when there is no /books.
  CheckFixture top;
  top.storage.Put("/Top.epub", Book(1).file);
  top.storage.Put("/Series/1.epub", Book(2).file);
  top.storage.Put("/Series/more/2.epub", Book(3).file);
  top.check->Begin();
  top.RunToTheEnd();
  EXPECT_EQ(top.check->CurrentState(), State::kPassed) << top.AllLines();
  EXPECT_EQ(top.check->Books(), 2u);
  EXPECT_NE(top.AllLines().find(" bytes, /Series/1.epub\n"), std::string::npos) << top.AllLines();
  EXPECT_NE(top.AllLines().find(" bytes, /Top.epub\n"), std::string::npos) << top.AllLines();
}

TEST(BookCheck, SaysWhenAFolderCannotBeListed) {
  CheckFixture fixture;
  fixture.storage.Put("/books/Series/1.epub", Book().file);
  fixture.storage.Put("/other", BytesOf("x"));
  // Something else has three files open: with /books being listed, there
  // is no room left to list the folder in it.
  std::array<bn::core::FileHandle, 3> held{};
  for (bn::core::FileHandle& file : held) {
    ASSERT_EQ(fixture.storage.Open("/other", bn::core::OpenMode::kRead, file), StorageStatus::kOk);
  }
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_NE(fixture.AllLines().find("book check: FAILED. /books/Series cannot be listed: too many open"),
            std::string::npos)
      << fixture.AllLines();
  EXPECT_EQ(fixture.storage.ListedCount(), 0);
  EXPECT_EQ(fixture.storage.OpenCount(), 3) << "only what was held is still open";
}

TEST(BookCheck, FailsWhenThereIsNoBookAnywhere) {
  CheckFixture fixture;
  fixture.storage.Put("/books/readme.txt", BytesOf("x"));
  fixture.storage.Put("/notes.txt", BytesOf("x"));
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_EQ(fixture.check->Books(), 0u);
  EXPECT_NE(fixture.AllLines().find("book check: FAILED. No book found"), std::string::npos) << fixture.AllLines();
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanBookCheck), std::vector<std::uint32_t>{0});
  EXPECT_EQ(fixture.storage.ListedCount(), 0);
}

TEST(BookCheck, LeavesOutABookWhoseNameWouldMakeTooLongAPath) {
  CheckFixture fixture;
  // "/books/" and 249 bytes are 256, one more than a path may be.
  const std::string tooLong = std::string(244, 'n') + ".epub";
  const std::string longest = std::string(243, 'n') + ".epub";
  fixture.storage.Put("/books/" + tooLong, Book().file);
  fixture.storage.Put("/books/" + longest, Book().file);
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed) << fixture.AllLines();
  EXPECT_EQ(fixture.check->Books(), 1u);
  // The line that names it is cut where a line must be.
  EXPECT_EQ(fixture.LineWith("book 1: " + std::to_string(Book().file.size()) + " bytes, /books/nnn").size(),
            bn::core::kMaxTextLength);
}

TEST(BookCheck, SaysWhichBookCannotBeReadAndGoesOnToTheNext) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a-not-a-zip.epub", Prose(5'000));
  fixture.storage.Put("/books/b-empty.epub", {});
  Bytes cut = Book().file;
  cut.resize(cut.size() - 30);
  fixture.storage.Put("/books/c-cut-short.epub", cut);
  fixture.storage.Put("/books/d-good.epub", Book().file);

  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_EQ(fixture.check->Books(), 4u);
  EXPECT_EQ(fixture.check->FailedBooks(), 3u);
  EXPECT_EQ(fixture.check->Totals().entries, Book::kEntries) << "the good one was still read";
  const std::string all = fixture.AllLines();
  EXPECT_NE(all.find("book 1: FAILED. It cannot be read: not a zip file\n"), std::string::npos) << all;
  EXPECT_NE(all.find("book 2: FAILED. It cannot be read: not a zip file\n"), std::string::npos) << all;
  EXPECT_NE(all.find("book 3: FAILED. It cannot be read: "), std::string::npos) << all;
  EXPECT_NE(all.find("book 4: ok. 5 entries"), std::string::npos) << all;
  EXPECT_NE(all.find("book check: FAILED. 3 of 4 books, 5 entries, "), std::string::npos) << all;
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanBookCheckBook), (std::vector<std::uint32_t>{0, 0, 0, 1}));
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
  EXPECT_EQ(fixture.storage.ListedCount(), 0);

  std::array<char, 2048> metrics{};
  bn::core::DefaultMetrics().FormatLine(metrics);
  EXPECT_NE(std::string(metrics.data()).find("book_check=0["), std::string::npos) << metrics.data();
}

TEST(BookCheck, SaysWhichEntryIsNotSoundAndReadsTheRest) {
  CheckFixture fixture;
  const Book book;
  Bytes damaged = book.file;
  damaged.at(book.chapterData + 2'000) ^= 0x10U;
  fixture.storage.Put("/books/damaged.epub", damaged);

  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_EQ(fixture.check->Books(), 1u);
  EXPECT_EQ(fixture.check->FailedBooks(), 1u);
  EXPECT_EQ(fixture.check->Totals().entries, Book::kEntries);
  EXPECT_EQ(fixture.check->Totals().failedEntries, 1u);
  const std::string line = fixture.LineWith("book 1: FAILED. ");
  EXPECT_NE(line.find("1 of 5 entries, "), std::string::npos) << line;
  // Which of the two it is depends on what the changed bit did to the stream.
  EXPECT_TRUE(line.ends_with(" First to fail: entry 3, bad checksum") ||
              line.ends_with(" First to fail: entry 3, damaged"))
      << line;
  EXPECT_NE(fixture.AllLines().find("book check: FAILED. 1 of 1 book, 1 of 5 entries, "), std::string::npos)
      << fixture.AllLines();
  EXPECT_EQ(fixture.Recorded(bn::reader::kSpanBookCheckBook), std::vector<std::uint32_t>{0});
}

TEST(BookCheck, SaysWhenADirectoryStopsPartWay) {
  CheckFixture fixture;
  ZipBuilder builder;
  builder.Add("first.txt", Prose(100));
  builder.Add("second.txt", Prose(100));
  builder.Add("third.txt", Prose(100));
  Bytes file = builder.Finish();
  // The third record no longer begins as a record does.
  file.at(builder.At(2).record) ^= 0xFFU;
  fixture.storage.Put("/books/broken-directory.epub", file);

  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.check->CurrentState(), State::kFailed);
  EXPECT_EQ(fixture.check->Totals().entries, 2u);
  EXPECT_EQ(fixture.check->Totals().failedEntries, 1u);
  const std::string line = fixture.LineWith("book 1: FAILED. ");
  EXPECT_NE(line.find("1 of 2 entries, "), std::string::npos) << line;
  EXPECT_TRUE(line.ends_with(" First to fail: entry 3, damaged")) << line;
}

TEST(BookCheck, ReportsStorageThatFails) {
  // No directory can be listed.
  CheckFixture unlistable;
  unlistable.storage.Put("/books/a.epub", Book().file);
  unlistable.storage.failListing = true;
  unlistable.check->Begin();
  unlistable.RunToTheEnd();
  EXPECT_EQ(unlistable.check->CurrentState(), State::kFailed);
  EXPECT_NE(unlistable.AllLines().find("book check: FAILED. /books cannot be listed: failed"), std::string::npos)
      << unlistable.AllLines();

  // A listing that stops after one thing.
  CheckFixture stops;
  stops.storage.Put("/books/a.epub", Book().file);
  stops.storage.Put("/books/b.epub", Book().file);
  stops.storage.failListingAfter = 1;
  stops.check->Begin();
  stops.RunToTheEnd();
  EXPECT_EQ(stops.check->CurrentState(), State::kFailed);
  EXPECT_EQ(stops.check->Books(), 1u);
  EXPECT_NE(stops.AllLines().find("book check: FAILED. The listing stopped: failed"), std::string::npos)
      << stops.AllLines();
  EXPECT_EQ(stops.storage.ListedCount(), 0);
  EXPECT_EQ(stops.storage.OpenCount(), 0);

  // Reads that fail: the book cannot be read, and the check says so.
  CheckFixture unreadable;
  unreadable.storage.Put("/books/a.epub", Book().file);
  unreadable.storage.failReads = true;
  unreadable.check->Begin();
  unreadable.RunToTheEnd();
  EXPECT_EQ(unreadable.check->CurrentState(), State::kFailed);
  EXPECT_NE(unreadable.AllLines().find("book 1: FAILED. It cannot be read: storage failed"), std::string::npos)
      << unreadable.AllLines();

  // No card at all.
  CheckFixture absent;
  absent.storage.present = false;
  EXPECT_EQ(absent.storage.Start(), StorageStatus::kAbsent);
  absent.check->Begin();
  absent.RunToTheEnd();
  EXPECT_EQ(absent.check->CurrentState(), State::kFailed);
  EXPECT_NE(absent.AllLines().find("book check: FAILED. /books cannot be listed: absent"), std::string::npos)
      << absent.AllLines();
}

TEST(BookCheck, CanBeRunAgainAndStartsItsCountsAfresh) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  fixture.check->Begin();
  fixture.RunToTheEnd();
  ASSERT_EQ(fixture.check->CurrentState(), State::kPassed);
  const BookCheck::Figures first = fixture.check->Totals();

  // Beginning while it runs does not start it over. (The clock is made to
  // move fast enough that a turn is only a few steps.)
  fixture.clock.step = 2'000;
  fixture.check->Begin();
  for (int turn = 0; turn < 3; ++turn) {
    fixture.check->Tick();
  }
  ASSERT_EQ(fixture.check->CurrentState(), State::kRunning);
  const int opensSoFar = fixture.storage.directoryOpens;
  EXPECT_EQ(opensSoFar, 2) << "it has got as far as listing /books again";
  fixture.check->Begin();
  fixture.RunToTheEnd();
  EXPECT_EQ(fixture.storage.directoryOpens, opensSoFar);

  EXPECT_EQ(fixture.check->CurrentState(), State::kPassed);
  EXPECT_EQ(fixture.check->Books(), 1u);
  EXPECT_EQ(fixture.check->Totals().entries, first.entries);
  EXPECT_EQ(fixture.check->Totals().bytesOut, first.bytesOut);
  EXPECT_EQ(fixture.check->Totals().reads, first.reads);
}

TEST(BookCheck, LetsGoOfTheCardWhenItIsDestroyedPartWay) {
  CheckFixture fixture;
  fixture.storage.Put("/books/a.epub", Book().file);
  fixture.clock.step = 5'000;  // A step or two a turn.
  fixture.check->Begin();
  for (int turn = 0; turn < 4; ++turn) {
    fixture.check->Tick();
  }
  ASSERT_EQ(fixture.check->CurrentState(), State::kRunning);
  EXPECT_GT(fixture.storage.OpenCount() + fixture.storage.ListedCount(), 0);
  fixture.check.reset();
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
  EXPECT_EQ(fixture.storage.ListedCount(), 0);
}

}  // namespace
