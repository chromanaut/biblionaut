// The fuzz target for finding the sheets a document asks for
// (epub/document_sheets.h).
//
// What it is given:
//
//   byte 0   bits 0 to 1: how much memory there is for the sheets' names
//            bits 2 to 3: how much for a document's `<style>` text
//            bits 4 to 5: how much the table of rules has
//            bits 6 to 7: how many bytes a file gives at one reading
//   byte 1   bits 0 to 2: which of the files fails part way, 7 for none
//            bits 3 to 7: after how many times eight bytes it fails
//   byte 2   how many steps into the first document it is given up, and
//            begun again; 0 for not at all
//   the rest up to eight files of a book, a byte of zero between one and
//            the next. A file is its path, a line end, and what it holds.
//            The documents are the files whose paths end in ".xhtml".
//
// The sheets of each document are found, in the order of the files, and
// then those of the first document again, with the plain working-out
// (the reference in tests/support) beside it. Every other document is
// opened here and handed over open, as a reader that knows where a book's
// documents are hands them over. What has to hold, whatever the bytes:
//
//   - every document ends with a list, in a number of steps that the
//     book's size bounds; but one whose path is longer than a book's path
//     may be (kMaxBookPath), which is no document;
//   - a step opens one file at the most, reads kMostBytesPerStep bytes of
//     the book at the most, and does not do both;
//   - a list has no sheet twice, every sheet in it is one the table has,
//     and each but the document's own has the path of a file of the book;
//   - the table has the book's sheets that were read, and one more while a
//     document's own rules are in it, and no others; and the names take
//     no more memory than was given for them;
//   - what is said to have been left out, kind by kind, is what the counts
//     come to;
//   - as long as nothing has been left out for want of room, the list and
//     the counts are those of the plain working-out; and so is the number
//     of rules the document's own `<style>` elements come to, where the
//     table had room for every rule it was given;
//   - a document found a second time has the same list, and by then
//     nothing is opened for it but the document;
//   - no file but a document is opened twice for the book, the sheet in
//     hand when a document was given up apart;
//   - the digest of what the documents were given (epub/sheets_digest.h)
//     counts what was found, and, as long as the plain working-out can be
//     set beside it and no document was given up, is the checksum of the
//     lines that working-out comes to.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_fakes.h"
#include "core/checksum.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/sheets_digest.h"
#include "epub/xml.h"
#include "fuzz_support.h"
#include "sheets_reference.h"

namespace {

using bn::epub::CssRules;
using bn::epub::DocumentSheets;
using bn::epub::SheetLeftOut;
using bn::test::FakeBook;

constexpr std::array<std::size_t, 4> kNamesBytes = {2048, 96, 24, 0};
constexpr std::array<std::size_t, 4> kStyleBytes = {4096, 64, 8, 0};
constexpr std::array<std::size_t, 4> kTableBytes = {65535, 2048, 200, 40};
constexpr std::array<std::size_t, 4> kPieceBytes = {FakeBook::kNever, 300, 7, 1};
constexpr std::size_t kMostFiles = 8;
// More steps than any book of eight files of this size can take.
constexpr std::size_t kMostSteps = 400000;

using Counts = std::array<std::uint32_t, bn::epub::kSheetLeftOutKinds>;

// Counts what the finder says it left out.
struct Told final : public bn::epub::SheetsListener {
  void OnSheetRead(std::string_view path, std::uint8_t /*sheet*/,
                   const bn::epub::CssReader& /*reader*/) noexcept override {
    (void)bn::fuzz::Touch(path);
    ++read;
  }
  void OnLeftOut(SheetLeftOut what, std::string_view text) noexcept override {
    BN_FUZZ_CHECK(static_cast<std::size_t>(what) < bn::epub::kSheetLeftOutKinds,
                  "a kind of thing left out that there is not");
    (void)bn::fuzz::Touch(text);
    ++leftOut[static_cast<std::size_t>(what)];
  }
  Counts leftOut{};
  std::size_t read = 0;
};

Counts CountsOf(const DocumentSheets& sheets) {
  Counts counts{};
  for (std::size_t kind = 0; kind < counts.size(); ++kind) {
    counts[kind] = sheets.LeftOut(static_cast<SheetLeftOut>(kind));
  }
  return counts;
}

// Takes the steps of finding a document's sheets, holding each to what a
// step may do.
void Run(DocumentSheets& sheets, FakeBook& book, std::size_t most) {
  for (std::size_t steps = 0; steps < most; ++steps) {
    const std::size_t opens = book.opens;
    const std::uint64_t bytes = book.bytesRead;
    const bool more = sheets.Step();
    const std::size_t opened = book.opens - opens;
    const std::uint64_t read = book.bytesRead - bytes;
    BN_FUZZ_CHECK(opened <= 1, "a step opened more than one file");
    BN_FUZZ_CHECK(read <= DocumentSheets::kMostBytesPerStep, "a step read more of the book than a step may");
    BN_FUZZ_CHECK(opened == 0 || read == 0, "a step opened a file and read from one");
    if (!more) {
      return;
    }
  }
  BN_FUZZ_CHECK(most != kMostSteps, "a document's sheets were not found in any number of steps");
}

// The document's list, as paths, checked for what any list has to be.
std::vector<std::string> Listed(const DocumentSheets& sheets, const CssRules& rules, const FakeBook& book) {
  std::vector<std::string> paths;
  const std::span<const std::uint8_t> list = sheets.Sheets();
  BN_FUZZ_CHECK(list.size() <= CssRules::kMaxSheets, "a list longer than the table has sheets");
  for (std::size_t i = 0; i < list.size(); ++i) {
    BN_FUZZ_CHECK(list[i] < rules.Sheets(), "a sheet in the list that the table has not");
    BN_FUZZ_CHECK(std::count(list.begin(), list.end(), list[i]) == 1, "a sheet twice in a list");
    const std::string_view path = sheets.PathOf(list[i]);
    const bool own = sheets.HasOwnStyle() && i + 1 == list.size();
    BN_FUZZ_CHECK(path.empty() == own, "a sheet with no path that is not the document's own, or the other way");
    BN_FUZZ_CHECK(own || book.Has(path), "a sheet in the list that is no file of the book");
    paths.emplace_back(own ? bn::test::SheetsReference::kOwnStyle : path);
  }
  return paths;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const unsigned failing = input.Byte();
  const std::size_t givenUpAfter = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  std::string_view text(reinterpret_cast<const char*>(rest.data()), rest.size());

  // The files: a path, a line end, and the rest.
  FakeBook book;
  std::vector<std::string> documents;
  std::vector<std::string> paths;
  while (!text.empty() && paths.size() < kMostFiles) {
    const std::size_t zero = std::min(text.find('\0'), text.size());
    std::string_view file = text;
    file.remove_suffix(text.size() - zero);
    text.remove_prefix(std::min(zero + 1, text.size()));
    const std::size_t line = std::min(file.find('\n'), file.size());
    std::string_view path = file;
    path.remove_suffix(file.size() - line);
    file.remove_prefix(std::min(line + 1, file.size()));
    if (path.empty() || book.Has(path)) {
      continue;
    }
    book.Put(path, file);
    paths.emplace_back(path);
    if (path.ends_with(".xhtml")) {
      documents.emplace_back(path);
    }
  }
  // Seven is for none, in a book of eight files too.
  if ((failing & 7U) != 7 && (failing & 7U) < paths.size()) {
    book.FailAfter(paths[failing & 7U], std::size_t{failing >> 3U} * 8);
  }
  book.pieceBytes = kPieceBytes[(choices >> 6U) & 3U];

  // Far more than a head of this size needs, and little enough to clear
  // for every input.
  std::vector<std::uint8_t> parserMemory(65536);
  bn::epub::XmlParser parser(parserMemory);
  bn::epub::CssReader reader;
  const std::size_t tableBytes = kTableBytes[(choices >> 4U) & 3U];
  std::vector<std::uint8_t> tableMemory(tableBytes);
  CssRules rules(tableMemory);
  std::vector<char> names(kNamesBytes[choices & 3U]);
  const std::size_t styleBytes = kStyleBytes[(choices >> 2U) & 3U];
  std::vector<std::uint8_t> style(styleBytes);
  DocumentSheets sheets(book, parser, reader, rules, names, style);
  Told told;
  sheets.SetListener(&told);
  sheets.BeginBook();
  bn::test::SheetsReference reference(book, styleBytes);

  // A document the book has not: no list, and nothing counted. (If the
  // bytes have made a file of that name, it is a document like any other.)
  // Then the book is begun again, which forgets it and whatever it asked
  // for: what follows is the book from its start.
  BN_FUZZ_CHECK(sheets.Find("no such document") == bn::epub::SheetsStatus::kNoDocument || book.Has("no such document"),
                "a document that is not there was found");
  sheets.BeginBook();
  BN_FUZZ_CHECK(
      rules.Sheets() == 0 && sheets.SheetsRead() == 0 && sheets.NamesBytes() == 0 && CountsOf(sheets) == Counts{},
      "a book begun again was not begun from nothing");
  told.leftOut = {};
  told.read = 0;
  book.opens = 0;
  book.opened.clear();

  // Whether what the finder gives can still be set beside the plain
  // working-out: not once it has had no room for something, and not what
  // it says of a damaged sheet once a document was given up part way,
  // since it may have said that then.
  bool comparable = true;
  bool gaveUp = false;
  // What the documents were given, boiled down; and the lines it should
  // be the checksum of, from the plain working-out.
  bn::epub::SheetsDigest digest;
  digest.Reset();
  std::string lines;
  std::uint64_t given = 0;
  std::vector<std::string> firstList;
  std::uint32_t firstRules = 0;

  std::vector<std::string> order = documents;
  if (!documents.empty()) {
    order.push_back(documents.front());
  }
  for (std::size_t turn = 0; turn < order.size(); ++turn) {
    const std::string& document = order[turn];
    if (turn == 0 && givenUpAfter != 0) {
      sheets.Begin(document);
      Run(sheets, book, givenUpAfter);
      gaveUp = true;
    }
    const Counts before = CountsOf(sheets);
    const std::size_t opens = book.opens;
    // Every other document is handed over open, from the book's one source
    // for whatever is open: the same list has to come of it.
    bn::core::ByteSource* const handed =
        turn % 2 == 1 && document.size() <= bn::epub::kMaxBookPath ? book.Open(document) : nullptr;
    if (handed != nullptr) {
      sheets.Begin(document, *handed);
    } else {
      sheets.Begin(document);
    }
    Run(sheets, book, kMostSteps);
    digest.Add(document, sheets);
    lines += "D " + document + "\n";
    if (document.size() > bn::epub::kMaxBookPath) {
      BN_FUZZ_CHECK(sheets.Status() == bn::epub::SheetsStatus::kNoDocument && sheets.Sheets().empty(),
                    "a document with a path too long to be one was given a list");
      BN_FUZZ_CHECK(book.opens == opens, "a file was opened for a path too long to be a document's");
      lines += "N\n";
      continue;
    }
    BN_FUZZ_CHECK(sheets.Status() == bn::epub::SheetsStatus::kOk && sheets.Finished(),
                  "a document of the book did not end with a list");
    const std::vector<std::string> listed = Listed(sheets, rules, book);
    given += listed.size();
    BN_FUZZ_CHECK(rules.Sheets() == sheets.SheetsRead() + (sheets.HasOwnStyle() ? 1U : 0U),
                  "the table has other sheets than those read and the document's own");
    BN_FUZZ_CHECK(sheets.SheetsRead() == told.read, "a sheet was read that was not told of, or the other way");
    BN_FUZZ_CHECK(sheets.NamesBytes() <= names.size(), "the names take more memory than there is for them");
    BN_FUZZ_CHECK(rules.Bytes() <= std::max<std::size_t>(tableBytes, 2), "the table holds more than it has room for");
    const Counts after = CountsOf(sheets);
    BN_FUZZ_CHECK(after == told.leftOut, "what was said to be left out is not what the counts come to");

    comparable = comparable && sheets.LeftOut(SheetLeftOut::kSheetTooMany) == 0 &&
                 sheets.LeftOut(SheetLeftOut::kImportTooMany) == 0;
    std::optional<std::uint32_t> ownRules;
    if (comparable) {
      const bn::test::SheetsExpected expected = reference.For(document);
      ownRules = bn::test::SheetsReference::OwnRules(expected);
      BN_FUZZ_CHECK(expected.document, "the plain working-out has no such document");
      BN_FUZZ_CHECK(listed == expected.sheets, "the list is not the one the plain working-out gives");
      for (const std::string& sheet : expected.sheets) {
        lines += "S " + sheet + "\n";
      }
      for (std::size_t kind = 0; kind < after.size(); ++kind) {
        if (expected.leftOut[kind] != 0) {
          lines += std::string("X ") + bn::epub::SheetLeftOutName(static_cast<SheetLeftOut>(kind)) + " " +
                   std::to_string(expected.leftOut[kind]) + "\n";
        }
        if (gaveUp && static_cast<SheetLeftOut>(kind) == SheetLeftOut::kSheetNotRead) {
          continue;
        }
        BN_FUZZ_CHECK(after[kind] - before[kind] == expected.leftOut[kind],
                      "what was left out is not what the plain working-out leaves out");
      }
    }
    if (turn == 0) {
      firstList = listed;
      firstRules = rules.Rules();
    } else if (turn + 1 == order.size() && comparable && order.front().size() <= bn::epub::kMaxBookPath) {
      // The first document again, every sheet of it known by now.
      BN_FUZZ_CHECK(listed == firstList, "a document found again has another list");
      BN_FUZZ_CHECK(book.opens - opens == 1, "a document found again had more opened for it than itself");
      BN_FUZZ_CHECK(order.size() != 2 || rules.Rules() == firstRules,
                    "a document found again straight away left the table with other rules");
    }
    // The document's own rules, which beginning a document takes out of
    // the table again: as many as its `<style>` texts come to, each read
    // to its own end. Only where the table kept every rule it was given.
    if (ownRules && rules.LeftOutForRoom() == 0 && rules.LeftOutForNodes() == 0) {
      const std::uint32_t with = rules.Rules();
      sheets.Begin(document);
      BN_FUZZ_CHECK(with - rules.Rules() == *ownRules,
                    "a document's own rules are not those of its styles, each read to its end");
    }
  }

  // The digest counted what there was, and is the checksum of the lines.
  BN_FUZZ_CHECK(digest.Documents() == order.size(), "the digest has not as many documents as were found");
  BN_FUZZ_CHECK(digest.Given() == given, "the digest has not as many sheets as the lists had");
  BN_FUZZ_CHECK(digest.Unopened() <= digest.Documents(), "the digest has more documents unopened than there were");
  if (comparable && !gaveUp) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
    const std::uint32_t expected = bn::core::Crc32({reinterpret_cast<const std::uint8_t*>(lines.data()), lines.size()});
    BN_FUZZ_CHECK(digest.Checksum() == expected, "the digest is not the checksum of the lines it is said to be");
  }

  // A sheet is read once for the whole book, and one the book has not is
  // looked for once; but the sheet in hand when a document was given up is
  // begun again.
  std::map<std::string, std::size_t> times;
  for (const std::string& name : book.opened) {
    ++times[name];
  }
  std::size_t again = 0;
  for (const auto& [name, count] : times) {
    const bool isDocument = std::find(documents.begin(), documents.end(), name) != documents.end();
    if (!isDocument) {
      again += count - 1;
    }
  }
  BN_FUZZ_CHECK(again <= (gaveUp ? 1U : 0U), "a file that is no document was opened more than once for the book");
  return 0;
}
