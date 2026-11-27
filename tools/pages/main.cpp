// bn-pages: sets a chapter of a bundle in pages, with the reader of the
// format and the layout the firmware has, in the made-up face, and prints
// them as text: the page a chapter entered at some place is shown, the
// page after a line and the page before one, every page of a chapter or of
// a book with each way of coming to it tried against the others, and the
// page an entry of the table of contents leads to.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "epub/segment.h"
#include "layout/hyphenator.h"
#include "pager.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::epub::Bundle;
using bn::epub::BundleChapter;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::ChapterState;
using bn::epub::SegmentPlace;
using bn::epub::SegmentReader;
using bn::epub::SegmentStatus;
using bn::layout::Hyphenator;
using bn::tools::ChapterLine;
using bn::tools::LineRef;
using bn::tools::Page;
using bn::tools::PageGeometry;
using bn::tools::Pager;
using bn::tools::SetBlock;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s lines BUNDLE CHAPTER [FIRST [COUNT]] [OPTIONS]\n"
               "       %s page BUNDLE CHAPTER PLACE [OPTIONS]\n"
               "       %s forward BUNDLE CHAPTER BLOCK INTO [OPTIONS]\n"
               "       %s back BUNDLE CHAPTER BLOCK INTO [OPTIONS]\n"
               "       %s back BUNDLE CHAPTER end [OPTIONS]\n"
               "       %s walk BUNDLE [CHAPTER] [--pages] [OPTIONS]\n"
               "       %s contents BUNDLE ENTRY [OPTIONS]\n"
               "\n"
               "Sets a chapter of a bundle in pages, reading it as the firmware does and with\n"
               "the firmware's own setting of lines and choosing of pages, in the made-up face\n"
               "bn-lines sets text in. CHAPTER is a document of the book's spine, from 0.\n"
               "\n"
               "lines prints the lines of COUNT blocks from the block FIRST (all of them from\n"
               "the first, if neither is given): for each block a line that says which it is,\n"
               "its kind, its level and how many characters it is, and then its lines.\n"
               "\n"
               "page prints the page a chapter entered at PLACE is shown: the one that begins\n"
               "with the line the place is on, or a little above it where that line is kept\n"
               "with those before. PLACE is one of\n"
               "\n"
               "  start               the chapter's first line\n"
               "  block B [INTO]      so many characters into a block\n"
               "  characters N        the chapter's character of that number\n"
               "  source S [INTO]     so many characters into the last block that began at or\n"
               "                      before a place in the source document: what a saved\n"
               "                      position is\n"
               "  id NAME             an id of the document\n"
               "\n"
               "forward prints the page that begins with the line holding the character INTO\n"
               "of BLOCK, and back the page that ends with the line above it; back ... end,\n"
               "the chapter's last page as it is filled up from the chapter's end. Where the\n"
               "page above comes to a line that starts a page, it is that line's page filled\n"
               "down. Above a chapter's first line it is the last page of the chapter before.\n"
               "\n"
               "walk goes through every page of CHAPTER, or of every chapter that is imported,\n"
               "from its first line to its last, and then tries the other ways of coming to a\n"
               "page against those pages: the page above each, and the page each place a\n"
               "reader might enter at is shown. With --pages it prints where each page\n"
               "begins. It ends with 1 if any of them does not agree.\n"
               "\n"
               "contents prints the page the entry ENTRY of the table of contents, from 0,\n"
               "leads to.\n"
               "\n"
               "  --width N           how wide the page's text is, in quarters of a pixel\n"
               "                      (default: 1760)\n"
               "  --height N          how high it is, in pixels (default: 760)\n"
               "  --table TABLE       the hyphenation table to break words by: the output of\n"
               "                      bn-hyphen make (default: words are not broken)\n"
               "\n"
               "A page is printed as a line that says the chapter, where the page begins and\n"
               "ends (a block and a character of it, or \"end\"), how many lines it has, how\n"
               "high they stand, and whether it was filled down from its first line or up\n"
               "from its last. Then each of its lines: the block, the characters of the block\n"
               "that are on the line (from, and up to), how wide they are, how high the line\n"
               "is, the space above it, and letters for what it is (k kept with the line\n"
               "before, s starts a page, h a hyphen is shown at its end, l the last of its\n"
               "paragraph, or -); and after a bar its text. A picture is printed as \\x1A, and\n"
               "a backslash and any other byte below a space as \\xNN.\n",
               program, program, program, program, program, program, program);
}

// Text as it is printed: every byte that could confuse a reader of lines
// written out as its number.
std::string Printable(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == '\\') {
      std::array<char, 8> escaped{};
      std::snprintf(escaped.data(), escaped.size(), "\\x%02X", static_cast<unsigned>(byte));
      out += escaped.data();
    } else {
      out += ch;
    }
  }
  return out;
}

bool Number(const std::string& word, std::uint64_t most, std::uint32_t& value) {
  char* end = nullptr;
  const unsigned long long number = std::strtoull(word.c_str(), &end, 10);
  if (word.empty() || end == nullptr || *end != '\0' || number > most) {
    return false;
  }
  value = static_cast<std::uint32_t>(number);
  return true;
}

struct Options {
  PageGeometry geometry;
  std::string tablePath;
  bool pages = false;
  std::vector<std::string> words;  // What is not an option, after the command.
};

bool Parse(const std::vector<std::string>& arguments, Options& options) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    const bool hasValue = i + 1 < arguments.size();
    std::uint32_t number = 0;
    if (argument == "--pages") {
      options.pages = true;
    } else if (argument == "--width" && hasValue && Number(arguments[++i], 65535, number) && number >= 40) {
      options.geometry.width = static_cast<std::uint16_t>(number);
    } else if (argument == "--height" && hasValue && Number(arguments[++i], 65535, number) && number >= 1) {
      options.geometry.height = static_cast<std::uint16_t>(number);
    } else if (argument == "--table" && hasValue) {
      options.tablePath = arguments[++i];
    } else if (argument.starts_with("--")) {
      return false;
    } else {
      options.words.push_back(argument);
    }
  }
  return true;
}

// A bundle's file, open to be read, with the bundle it is.
class OpenBundle {
 public:
  explicit OpenBundle(const std::string& path) : storage_(path) {
    if (storage_.Start() != StorageStatus::kOk ||
        file.Open(storage_, "/bundle", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
      std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
      return;
    }
    if (const BundleStatus status = bundle.Open(file); status != BundleStatus::kOk) {
      std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::BundleStatusName(status));
      return;
    }
    ok = true;
  }

 private:
  bn::tools::OneFile storage_;
  std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(bn::epub::kBundlePageBytes);
  std::vector<std::uint8_t> page_ = std::vector<std::uint8_t>(bn::epub::kBundlePageBytes);

 public:
  bn::core::BlockFile file{buffer_};
  Bundle bundle{page_};
  bool ok = false;
};

// A chapter of a bundle, open to be set in pages.
class OpenChapter {
 public:
  OpenChapter(OpenBundle& opened, std::size_t number, const Options& options, Hyphenator* hyphenator)
      : pager(reader, options.geometry, hyphenator) {
    if (number >= opened.bundle.Book().documents || opened.bundle.ReadChapter(number, record) != BundleStatus::kOk) {
      std::fprintf(stderr, "error: the bundle has no chapter %zu\n", number);
      return;
    }
    if (record.state != ChapterState::kWhole && record.state != ChapterState::kCutShort) {
      std::fprintf(stderr, "error: chapter %zu is not imported\n", number);
      return;
    }
    if (const SegmentStatus status = reader.Open(opened.file, record); status != SegmentStatus::kOk) {
      std::fprintf(stderr, "error: chapter %zu: %s\n", number, bn::epub::SegmentStatusName(status));
      return;
    }
    ok = true;
  }

 private:
  std::vector<std::uint8_t> page_ = std::vector<std::uint8_t>(bn::epub::kBundlePageBytes);

 public:
  BundleChapter record;
  SegmentReader reader{page_};
  Pager pager;
  bool ok = false;
};

// Whether a chapter has a segment to set: one that was imported.
bool IsImported(Bundle& bundle, std::size_t number) {
  BundleChapter record;
  return bundle.ReadChapter(number, record) == BundleStatus::kOk &&
         (record.state == ChapterState::kWhole || record.state == ChapterState::kCutShort);
}

std::string Where(const Pager& pager, LineRef line, const ChapterLine* at) {
  if (line == pager.End() || at == nullptr) {
    return "end";
  }
  return std::to_string(line.block) + "+" + std::to_string(at->begin);
}

// The line a reference is to. Null for the chapter's end, and where the
// chapter could not be read.
const ChapterLine* LineAt(Pager& pager, LineRef line) {
  const SetBlock* set = nullptr;
  if (line == pager.End() || !pager.Block(line.block, set) || line.line >= set->lines.size()) {
    return nullptr;
  }
  return &set->lines[line.line];
}

void PrintLine(std::uint32_t block, const ChapterLine& line) {
  std::string flags;
  flags += line.keep ? "k" : "";
  flags += line.starts ? "s" : "";
  flags += line.hyphen ? "h" : "";
  flags += line.last ? "l" : "";
  std::printf("%u %u %u %u %u %u %s |%s\n", static_cast<unsigned>(block), static_cast<unsigned>(line.begin),
              static_cast<unsigned>(line.end), static_cast<unsigned>(line.width), static_cast<unsigned>(line.height),
              static_cast<unsigned>(line.gap), flags.empty() ? "-" : flags.c_str(), Printable(line.text).c_str());
}

int Failed(const Pager& pager) {
  std::fprintf(stderr, "error: %s\n", pager.Error().c_str());
  return 1;
}

int PrintPage(std::size_t chapter, Pager& pager, const Page& page) {
  if (!page.found) {
    std::printf("no page\n");
    return 0;
  }
  const ChapterLine* first = LineAt(pager, page.first);
  const ChapterLine* next = LineAt(pager, page.next);
  if (first == nullptr) {
    return Failed(pager);
  }
  std::printf("page chapter %zu from %s to %s lines %zu height %u %s%s\n", chapter,
              Where(pager, page.first, first).c_str(), Where(pager, page.next, next).c_str(), page.lines.size(),
              static_cast<unsigned>(page.height), page.filledDown ? "down" : "up", page.cutShort ? " cut-short" : "");
  for (const LineRef line : page.lines) {
    const ChapterLine* at = LineAt(pager, line);
    if (at == nullptr) {
      return Failed(pager);
    }
    PrintLine(line.block, *at);
  }
  return 0;
}

int Lines(OpenChapter& chapter, std::uint32_t first, std::uint32_t count) {
  const std::uint32_t blocks = chapter.reader.Blocks();
  for (std::uint32_t block = first; block < blocks && block - first < count; ++block) {
    const SetBlock* set = nullptr;
    if (!chapter.pager.Block(block, set)) {
      return Failed(chapter.pager);
    }
    std::printf("block %u kind %u level %u characters %u before %u lines %zu\n", static_cast<unsigned>(block),
                static_cast<unsigned>(set->kind), static_cast<unsigned>(set->level),
                static_cast<unsigned>(set->characters), static_cast<unsigned>(set->charactersBefore),
                set->lines.size());
    for (const ChapterLine& line : set->lines) {
      PrintLine(block, line);
    }
  }
  return 0;
}

// Finds the place the words after a command name, as a block and how far
// into it. `atEnd` if it is the chapter's end.
int FindPlace(OpenChapter& chapter, const std::vector<std::string>& words, std::size_t from, std::uint32_t& block,
              std::uint32_t& into, bool& atEnd) {
  SegmentReader& reader = chapter.reader;
  const std::string what = from < words.size() ? words[from] : "";
  const std::size_t given = words.size() - std::min(words.size(), from + 1);
  std::uint32_t number = 0;
  std::uint32_t further = 0;
  SegmentStatus status = SegmentStatus::kMisused;
  into = 0;
  atEnd = false;
  if (what == "start" && given == 0) {
    block = 0;
    atEnd = reader.Blocks() == 0;
    return 0;
  }
  if (what == "id" && given == 1) {
    status = reader.FindAnchor(words[from + 1]);
  } else if ((given == 1 || (given == 2 && what != "characters")) && Number(words[from + 1], 0xFFFFFFFFULL, number) &&
             (given == 1 || Number(words[from + 2], 0xFFFFFFFFULL, further))) {
    status = what == "block"        ? reader.FindBlock(number)
             : what == "characters" ? reader.FindCharacters(number)
             : what == "source"     ? reader.FindSource(number)
                                    : SegmentStatus::kMisused;
  }
  if (status == SegmentStatus::kMisused) {
    std::fprintf(stderr, "error: a place is start, block B [INTO], characters N, source S [INTO] or id NAME\n");
    return 2;
  }
  if (status != SegmentStatus::kOk) {
    std::fprintf(stderr, "error: looking for the place: %s\n", bn::epub::SegmentStatusName(status));
    return 1;
  }
  const SegmentPlace& place = reader.Place();
  if (!place.found) {
    std::fprintf(stderr, "error: the chapter has no such place\n");
    return 1;
  }
  block = place.block;
  into = place.into + further;
  atEnd = place.atEnd;
  return 0;
}

// The page a chapter entered at a place is shown; at its end, its last
// page, filled up.
int Enter(std::size_t number, OpenChapter& chapter, std::uint32_t block, std::uint32_t into, bool atEnd) {
  Page page;
  if (atEnd ? !chapter.pager.Back(chapter.pager.End(), page) : !chapter.pager.Entering(block, into, page)) {
    return Failed(chapter.pager);
  }
  return PrintPage(number, chapter.pager, page);
}

Hyphenator* OpenTable(const Options& options, std::string& bytes, Hyphenator& hyphenator, bool& failed) {
  failed = false;
  if (options.tablePath.empty()) {
    return nullptr;
  }
  std::ifstream in(options.tablePath, std::ios::binary);
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  const std::span<const std::uint8_t> table(
      reinterpret_cast<const std::uint8_t*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      bytes.size());
  const bn::layout::HyphenStatus status = in ? hyphenator.Open(table) : bn::layout::HyphenStatus::kBadTable;
  if (status != bn::layout::HyphenStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", options.tablePath.c_str(), bn::layout::HyphenStatusName(status));
    failed = true;
    return nullptr;
  }
  return &hyphenator;
}

// What a walk of a chapter came to.
struct Walked {
  std::uint64_t pages = 0;
  std::uint64_t lines = 0;
  std::uint64_t blocks = 0;
  std::uint64_t characters = 0;
  std::uint64_t backs = 0;
  std::uint64_t entered = 0;
  std::uint32_t checksum = 0;
};

// Goes through every page of a chapter, and tries the other ways of coming
// to a page against them. False, with what was wrong said, if any does not
// agree or the chapter cannot be read.
bool Walk(std::size_t number, OpenChapter& chapter, bool printPages, Walked& walked) {
  Pager& pager = chapter.pager;
  const auto wrong = [number](const std::string& what) {
    std::fprintf(stderr, "error: chapter %zu: %s\n", number, what.c_str());
    return false;
  };
  const auto name = [&pager](LineRef line) { return Where(pager, line, LineAt(pager, line)); };

  // Down from the first line: each page begins where the one before ends.
  std::vector<Page> pages;
  for (LineRef first{}; first != pager.End();) {
    Page page;
    if (!pager.Forward(first, page)) {
      return wrong(pager.Error());
    }
    if (!page.found || page.lines.empty() || page.lines.front() != first || !(page.next > first)) {
      return wrong("the page from " + name(first) + " does not begin there, or holds nothing");
    }
    first = page.next;
    pages.push_back(page);
  }
  std::uint64_t lines = 0;
  std::set<LineRef> firsts;
  for (const Page& page : pages) {
    lines += page.lines.size();
    firsts.insert(page.first);
    const std::string where = name(page.first) + "\n";
    walked.checksum = bn::core::Crc32(
        {reinterpret_cast<const std::uint8_t*>(where.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
         where.size()},
        walked.checksum);
    if (printPages) {
      std::printf("page chapter %zu from %s lines %zu height %u\n", number, name(page.first).c_str(), page.lines.size(),
                  static_cast<unsigned>(page.height));
    }
  }
  std::uint64_t all = 0;
  for (std::uint32_t block = 0; block < chapter.reader.Blocks(); ++block) {
    const SetBlock* set = nullptr;
    if (!pager.Block(block, set)) {
      return wrong(pager.Error());
    }
    all += set->lines.size();
  }
  if (lines != all) {
    return wrong("its pages hold " + std::to_string(lines) + " lines, and it has " + std::to_string(all));
  }

  // The page above each: it ends where that page begins, and holds the
  // lines of the page before, unless that one began with a line that is
  // kept with the one above it; or it is the page of a line that starts
  // one, filled down.
  Page above;
  if (!pager.Back(LineRef{}, above)) {
    return wrong(pager.Error());
  }
  if (above.found) {
    return wrong("there is a page above its first line");
  }
  for (std::size_t i = 1; i <= pages.size(); ++i) {
    const LineRef before = i < pages.size() ? pages[i].first : pager.End();
    if (!pager.Back(before, above)) {
      return wrong(pager.Error());
    }
    ++walked.backs;
    const ChapterLine* began = LineAt(pager, pages[i - 1].first);
    if (!above.found || began == nullptr) {
      return wrong("there is no page above " + name(before));
    }
    if (above.filledDown) {
      const ChapterLine* first = LineAt(pager, above.first);
      if (first == nullptr || !first->starts || !firsts.contains(above.first)) {
        return wrong("the page above " + name(before) + " was filled down from " + name(above.first) +
                     ", which starts no page");
      }
      continue;
    }
    if (above.next != before || above.lines.empty() || above.lines.front() != above.first) {
      return wrong("the page above " + name(before) + " does not end there");
    }
    const bool mayBegin = began->starts || !began->keep;
    if (mayBegin ? above.first > pages[i - 1].first : !(above.first < before)) {
      return wrong("the page above " + name(before) + " begins at " + name(above.first) + ", below " +
                   name(pages[i - 1].first) + " where the page before began");
    }
  }

  // The page each place a reader might enter at is shown: it holds the
  // line the place is on, and begins with it if a page may.
  for (std::uint32_t block = 0; block < chapter.reader.Blocks(); ++block) {
    const SetBlock* set = nullptr;
    if (!pager.Block(block, set)) {
      return wrong(pager.Error());
    }
    for (std::uint32_t line = 0; line < set->lines.size(); line += 1 + (block % 3)) {
      const LineRef asked{.block = block, .line = line};
      const ChapterLine at = set->lines[line];
      Page shown;
      if (!pager.Entering(block, at.begin, shown)) {
        return wrong(pager.Error());
      }
      ++walked.entered;
      const bool holds = std::find(shown.lines.begin(), shown.lines.end(), asked) != shown.lines.end();
      if (!shown.found || !holds || shown.first > asked || ((at.starts || !at.keep) && shown.first != asked)) {
        return wrong("entered at " + name(asked) + ", the page shown begins at " + name(shown.first));
      }
    }
  }
  walked.pages += pages.size();
  walked.lines += lines;
  walked.blocks += chapter.reader.Blocks();
  walked.characters += chapter.reader.Characters();
  return true;
}

// Reads an entry of the table of contents: the chapter it leads to, and
// the name of the place there.
int ReadContents(Bundle& bundle, std::uint32_t entry, std::uint32_t& chapter, std::string& fragment) {
  if (entry >= bundle.Book().contents) {
    std::fprintf(stderr, "error: the table of contents has no entry %u\n", static_cast<unsigned>(entry));
    return 1;
  }
  const std::uint32_t bytes = bundle.Table(BundleTable::kContents).bytes;
  std::vector<std::uint8_t> table(bytes);
  if (bundle.ReadTable(BundleTable::kContents, 0, table) != BundleStatus::kOk) {
    std::fprintf(stderr, "error: the table of contents could not be read\n");
    return 1;
  }
  std::size_t at = 0;
  for (std::uint32_t i = 0;; ++i) {
    if (at + 6 > table.size()) {
      std::fprintf(stderr, "error: the table of contents is cut off\n");
      return 1;
    }
    const std::size_t fragmentBytes = table[at + 1];
    const std::size_t labelBytes = table[at + 4] | (std::size_t{table[at + 5]} << 8U);
    if (at + 6 + fragmentBytes + labelBytes > table.size()) {
      std::fprintf(stderr, "error: the table of contents is cut off\n");
      return 1;
    }
    if (i == entry) {
      chapter = table[at + 2] | (std::uint32_t{table[at + 3]} << 8U);
      fragment.assign(table.begin() + static_cast<std::ptrdiff_t>(at + 6),
                      table.begin() + static_cast<std::ptrdiff_t>(at + 6 + fragmentBytes));
      return 0;
    }
    at += 6 + fragmentBytes + labelBytes;
  }
}

int Run(const std::string& command, const Options& options) {
  const std::vector<std::string>& words = options.words;
  if (words.empty()) {
    return 2;
  }
  std::string tableBytes;
  Hyphenator table;
  bool failed = false;
  Hyphenator* hyphenator = OpenTable(options, tableBytes, table, failed);
  if (failed) {
    return 1;
  }
  OpenBundle opened(words[0]);
  if (!opened.ok) {
    return 1;
  }
  std::uint32_t number = 0;

  if (command == "walk" && words.size() <= 2) {
    if (words.size() == 2 && !Number(words[1], 65535, number)) {
      return 2;
    }
    Walked walked;
    std::size_t chapters = 0;
    for (std::size_t chapter = 0; chapter < opened.bundle.Book().documents; ++chapter) {
      if (words.size() == 2 ? chapter != number : !IsImported(opened.bundle, chapter)) {
        continue;
      }
      OpenChapter open(opened, chapter, options, hyphenator);
      if (!open.ok || !Walk(chapter, open, options.pages, walked)) {
        return 1;
      }
      ++chapters;
    }
    if (words.size() == 2 && chapters == 0) {
      std::fprintf(stderr, "error: the bundle has no chapter %u\n", static_cast<unsigned>(number));
      return 1;
    }
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::printf("chapters %zu, pages %llu, lines %llu, blocks %llu, characters %llu\n", chapters, count(walked.pages),
                count(walked.lines), count(walked.blocks), count(walked.characters));
    std::printf("pages above a page %llu, and places entered at %llu: all agree with the pages\n", count(walked.backs),
                count(walked.entered));
    std::printf("checksum of where the pages begin %08x\n", static_cast<unsigned>(walked.checksum));
    return 0;
  }

  if (command == "contents" && words.size() == 2) {
    std::uint32_t chapterNumber = 0;
    std::string fragment;
    if (!Number(words[1], 65535, number)) {
      return 2;
    }
    if (const int read = ReadContents(opened.bundle, number, chapterNumber, fragment); read != 0) {
      return read;
    }
    if (chapterNumber == 0xFFFF) {
      std::fprintf(stderr, "error: the entry leads to no document of the book\n");
      return 1;
    }
    OpenChapter chapter(opened, chapterNumber, options, hyphenator);
    if (!chapter.ok) {
      return 1;
    }
    // The place it names; or the chapter's start, if it names none or the
    // chapter has none of that name.
    std::uint32_t block = 0;
    std::uint32_t into = 0;
    bool atEnd = chapter.reader.Blocks() == 0;
    if (!fragment.empty()) {
      if (const SegmentStatus status = chapter.reader.FindAnchor(fragment); status != SegmentStatus::kOk) {
        std::fprintf(stderr, "error: looking for the place: %s\n", bn::epub::SegmentStatusName(status));
        return 1;
      }
      if (chapter.reader.Place().found) {
        block = chapter.reader.Place().block;
        into = chapter.reader.Place().into;
        atEnd = chapter.reader.Place().atEnd;
      } else {
        std::printf("# the chapter has no place of that name: its start\n");
      }
    }
    return Enter(chapterNumber, chapter, block, into, atEnd);
  }

  if (words.size() < 2 || !Number(words[1], 65535, number)) {
    return 2;
  }
  OpenChapter chapter(opened, number, options, hyphenator);
  if (!chapter.ok) {
    return 1;
  }
  if (command == "lines" && words.size() <= 4) {
    std::uint32_t first = 0;
    std::uint32_t count = 0xFFFFFFFFU;
    if ((words.size() >= 3 && !Number(words[2], 0xFFFFFFFFULL, first)) ||
        (words.size() == 4 && !Number(words[3], 0xFFFFFFFFULL, count))) {
      return 2;
    }
    return Lines(chapter, first, count);
  }
  if (command == "page") {
    std::uint32_t block = 0;
    std::uint32_t into = 0;
    bool atEnd = false;
    if (const int found = FindPlace(chapter, words, 2, block, into, atEnd); found != 0) {
      return found;
    }
    return Enter(number, chapter, block, into, atEnd);
  }
  if (command == "forward" || command == "back") {
    LineRef line = chapter.pager.End();
    std::uint32_t into = 0;
    const bool end = command == "back" && words.size() == 3 && words[2] == "end";
    if (!end &&
        (words.size() != 4 || !Number(words[2], 0xFFFFFFFFULL, line.block) || !Number(words[3], 0xFFFFFFFFULL, into))) {
      return 2;
    }
    if (!end && !chapter.pager.LineHolding(line.block, into, line)) {
      return Failed(chapter.pager);
    }
    Page page;
    if (command == "forward" ? !chapter.pager.Forward(line, page) : !chapter.pager.Back(line, page)) {
      return Failed(chapter.pager);
    }
    if (page.found || command == "forward") {
      return PrintPage(number, chapter.pager, page);
    }
    // Above a chapter's first line: the last page of the chapter before
    // that has one.
    for (std::size_t before = number; before-- > 0;) {
      if (!IsImported(opened.bundle, before)) {
        continue;
      }
      OpenChapter earlier(opened, before, options, hyphenator);
      if (!earlier.ok) {
        return 1;
      }
      if (earlier.reader.Blocks() == 0) {
        continue;
      }
      if (!earlier.pager.Back(earlier.pager.End(), page)) {
        return Failed(earlier.pager);
      }
      return PrintPage(before, earlier.pager, page);
    }
    return PrintPage(number, chapter.pager, page);
  }
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> all(argv, argv + argc);
  Options options;
  if (all.size() < 2 || !Parse({all.begin() + 2, all.end()}, options)) {
    PrintUsage(all.empty() ? "bn-pages" : all[0].c_str());
    return 2;
  }
  const int result = Run(all[1], options);
  if (result == 2) {
    PrintUsage(all[0].c_str());
  }
  return result;
}
