#include "import.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "chapter.h"
#include "core/block_file.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "epub/book_bundle.h"
#include "epub/book_files.h"
#include "epub/bundle.h"
#include "epub/chapter_import.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "support/one_file.h"

// bn-bundle import: the chapters of a book made into segments of its
// bundle, with the code the firmware imports with.

namespace bn::tools {

namespace {

using core::StorageStatus;
using epub::BundleChapter;
using epub::BundleStatus;
using epub::ChapterImporter;
using epub::ChapterState;
using epub::ImportStatus;

// What a device gives the parser and the table of a book's rules.
constexpr std::size_t kXmlBytes = 24576;
constexpr std::size_t kRulesBytes = 24576;

// A reason as one word: "not-XML".
std::string ReasonWord(std::uint8_t reason) {
  std::string word = epub::ImportReasonName(static_cast<epub::ImportReason>(reason));
  std::replace(word.begin(), word.end(), ' ', '-');
  return word;
}

const char* StateName(ChapterState state) {
  switch (state) {
    case ChapterState::kNotImported:
      return "not-imported";
    case ChapterState::kWhole:
      return "whole";
    case ChapterState::kCutShort:
      return "cut-short";
    case ChapterState::kNotDocument:
      return "not-a-document";
    case ChapterState::kRefused:
      return "refused";
  }
  return "?";
}

}  // namespace

int ImportChapters(const std::string& bookPath, const std::string& bundlePath, const std::string& which) {
  // The book, as the firmware has it: the ZIP file open twice, for its
  // directory and for its entries.
  OneFile book(bookPath);
  alignas(4) static std::array<std::uint8_t, 4096> directoryBuffer{};
  alignas(4) static std::array<std::uint8_t, 4096> entryBuffer{};
  core::BlockFile directory(directoryBuffer);
  core::BlockFile entries(entryBuffer);
  if (book.Start() != StorageStatus::kOk ||
      directory.Open(book, "/book", core::OpenMode::kRead) != StorageStatus::kOk ||
      entries.Open(book, "/book", core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", bookPath.c_str());
    return 1;
  }
  epub::ZipArchive archive;
  if (const epub::ZipStatus status = archive.Open(directory); status != epub::ZipStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", bookPath.c_str(), epub::ZipStatusName(status));
    return 1;
  }
  std::vector<std::uint8_t> window(core::Inflater::kWindowBytes);
  epub::ZipEntryReader reader(window);
  epub::ZipBookFiles files(archive, entries, reader);

  // What finds a document's sheets, and the table they go into.
  std::vector<std::uint8_t> xmlMemory(kXmlBytes);
  epub::XmlParser xml(xmlMemory);
  std::vector<std::uint8_t> rulesMemory(kRulesBytes);
  epub::CssRules rules(rulesMemory);
  epub::CssReader css;
  std::vector<char> names(epub::DocumentSheets::kNamesBytes);
  std::vector<std::uint8_t> style(epub::DocumentSheets::kStyleBytes);
  epub::DocumentSheets sheets(files, xml, css, rules, names, style);
  sheets.BeginBook();

  // The bundle, to be added to.
  OneNewFile changed(bundlePath);
  alignas(4) static std::array<std::uint8_t, 4096> bundleBuffer{};
  alignas(4) static std::array<std::uint8_t, 4096> page{};
  core::BlockFile file(bundleBuffer);
  epub::Bundle bundle(page);
  if (changed.Start() != StorageStatus::kOk ||
      file.Open(changed, "/bundle", core::OpenMode::kUpdate) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot change %s\n", bundlePath.c_str());
    return 1;
  }
  if (const BundleStatus status = bundle.Open(file); status != BundleStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", bundlePath.c_str(), epub::BundleStatusName(status));
    return 1;
  }

  const std::size_t documents = bundle.Book().documents;
  std::size_t first = 0;
  std::size_t last = documents;
  if (which != "all") {
    char* end = nullptr;
    const unsigned long long number = std::strtoull(which.c_str(), &end, 10);
    if (which.empty() || end == nullptr || *end != '\0' || number >= documents) {
      std::fprintf(stderr, "error: the bundle has no chapter %s\n", which.c_str());
      return 1;
    }
    first = static_cast<std::size_t>(number);
    last = first + 1;
  }

  // The book's own language: the third of the bundle's strings, each a
  // length in two bytes and then its bytes.
  std::vector<std::uint8_t> strings(bundle.Table(epub::BundleTable::kStrings).bytes);
  if (bundle.ReadTable(epub::BundleTable::kStrings, 0, strings) != BundleStatus::kOk) {
    std::fprintf(stderr, "error: %s: a table could not be read\n", bundlePath.c_str());
    return 1;
  }
  std::string language;
  std::size_t at = 0;
  for (std::size_t i = 0; i < 3 && at + 2 <= strings.size(); ++i) {
    const std::size_t length = strings[at] | (static_cast<std::size_t>(strings[at + 1]) << 8U);
    at += 2;
    if (i == 2 && at + length <= strings.size()) {
      language.assign(strings.begin() + static_cast<std::ptrdiff_t>(at),
                      strings.begin() + static_cast<std::ptrdiff_t>(at + length));
    }
    at += length;
  }

  alignas(4) static std::array<std::uint8_t, ChapterImporter::kWorkBytes> work{};
  ChapterImporter importer(rules, work);
  std::size_t mostParser = 0;
  for (std::size_t number = first; number < last; ++number) {
    epub::BundleFile spine;
    if (const BundleStatus status = bundle.ReadSpine(number, spine); status != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: %s\n", bundlePath.c_str(), epub::BundleStatusName(status));
      return 1;
    }
    BundleChapter record;
    std::size_t steps = 0;
    std::string path = "-";
    if (!epub::ChapterWithoutDocument(spine, record)) {
      epub::ZipChapterDocument document(files, spine.entry);
      ImportStatus status = importer.Begin(document, sheets, xml, file, bundle.DataEnd(), language);
      while (status == ImportStatus::kOk && importer.Step()) {
        ++steps;
      }
      status = importer.Status();
      if (status != ImportStatus::kOk) {
        std::fprintf(stderr, "error: chapter %zu: %s\n", number, epub::ImportStatusName(status));
        return 1;
      }
      record = importer.Chapter();
      path = Printable(importer.Path());
      mostParser = std::max(mostParser, xml.Memory().HighWater());
    }
    if (const BundleStatus status = bundle.Commit(number, record); status != BundleStatus::kOk) {
      std::fprintf(stderr, "error: chapter %zu: %s\n", number, epub::BundleStatusName(status));
      return 1;
    }
    const bool imported = record.state == ChapterState::kWhole || record.state == ChapterState::kCutShort;
    std::printf(
        "chapter %zu %s reason=%s blocks=%u characters=%u bytes=%u left_out=%u dropped=%u entities_left_out=%u "
        "held_left_out=%u too_deep=%u cascade_too_deep=%u languages=%zu steps=%zu path=%s\n",
        number, StateName(record.state), ReasonWord(record.reason).c_str(), static_cast<unsigned>(record.blocks),
        static_cast<unsigned>(record.characters), static_cast<unsigned>(record.segmentBytes),
        static_cast<unsigned>(record.leftOut), static_cast<unsigned>(imported ? importer.Dropped() : 0),
        static_cast<unsigned>(imported ? importer.LeftOut(epub::ImportLeftOut::kEntity) : 0),
        static_cast<unsigned>(imported ? importer.LeftOut(epub::ImportLeftOut::kHeld) : 0),
        static_cast<unsigned>(imported ? importer.Blocks().Styles().LeftOut(epub::StyleLeftOut::kTooDeep) : 0),
        static_cast<unsigned>(imported ? importer.CascadeTooDeep() : 0),
        imported ? importer.Blocks().Styles().Languages() : 0, steps, path.c_str());
  }

  // What importing was given to work in, and how much of the two that
  // depend on the book it used.
  const std::size_t buffers = directoryBuffer.size() + entryBuffer.size() + bundleBuffer.size();
  const std::size_t finder = names.size() + style.size();
  std::printf(
      "memory work=%zu parser=%zu parser_used=%zu rules=%zu rules_used=%zu window=%zu buffers=%zu sheets=%zu "
      "page=%zu all=%zu\n",
      work.size(), xmlMemory.size(), mostParser, rulesMemory.size(), rules.Bytes(), window.size(), buffers, finder,
      page.size(),
      work.size() + xmlMemory.size() + rulesMemory.size() + window.size() + buffers + finder + page.size());
  return 0;
}

}  // namespace bn::tools
