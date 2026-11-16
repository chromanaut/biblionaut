// bn-lines: sets paragraphs of text in lines, by the code the firmware sets
// them with, and says what is on each line or what the setting took.
//
// There are no fonts here to measure text with. A made-up face stands in
// for one (layout/made_up_face.h): every character has a width that depends
// on its first byte and on nothing else, so that whoever reads the output
// can add a line up. The device check for layout sets text in the same
// face, and its figures are these.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/line_digest.h"
#include "layout/made_up_face.h"
#include "layout/paragraph_setter.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;
using bn::layout::LineBreaker;
using bn::layout::LineDigest;
using bn::layout::MadeUpFace;
using bn::layout::ParagraphSetter;
using bn::layout::ParagraphSettings;
using bn::layout::ParagraphStatus;
using bn::layout::TextLine;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [FILE] [--width N] [--first N,N,...] [--penalty N] [--table TABLE]\n"
               "                [--nodes N] [--records N] [--figures]\n"
               "\n"
               "Sets text in lines, as the firmware does.\n"
               "\n"
               "The text is read from FILE or, without one, from what it is given to read.\n"
               "It is UTF-8. An empty line ends a paragraph; a line end inside a paragraph is\n"
               "one the text asks for. What is between two asterisks is in a second style,\n"
               "and the asterisks are not part of the text.\n"
               "\n"
               "For each line of each paragraph it prints, with a tab between them:\n"
               "\n"
               "  where the line begins and ends, in bytes of the paragraph's text\n"
               "  how wide it is\n"
               "  how many places between words it has\n"
               "  h if a hyphen is shown at its end, l if it ends the paragraph or ends where\n"
               "    the text asks, w if it is too wide for the line, or - for none of these\n"
               "  its text, with a hyphen after it if one is shown\n"
               "\n"
               "and an empty line after each paragraph.\n"
               "\n"
               "  --width N        how wide a line may be (default: 1760)\n"
               "  --first N,N,...  how wide each of the paragraph's first lines may be, where\n"
               "                   that is not the same\n"
               "  --penalty N      what ending a line inside a word with a hyphen costs, as the\n"
               "                   square of the gap at a line's end that is as bad (default:\n"
               "                   12100, a gap of 110)\n"
               "  --table TABLE    a language's hyphenation table, as bn-hyphen makes them.\n"
               "                   Without one no word is broken but where the text says\n"
               "  --nodes N        how many places a line could end are held at once\n"
               "                   (default: 256)\n"
               "  --records N      how many items of a paragraph are held at once (default:\n"
               "                   768)\n"
               "  --figures        print no lines, only what setting them took\n"
               "\n"
               "Widths are in quarters of a pixel, and are those of a made-up face: a small\n"
               "letter is 36 wide; i, l, j, t, f and . , ; : ' ! | are 18; m, w, M and W are 54;\n"
               "any other capital is 44; a hyphen-minus is 22; a character of more than one\n"
               "byte is 40; a space is 18 and the hyphen shown at a line's end 22. In the\n"
               "second style each of these is 4 wider. A mark that is set over the letter\n"
               "before it (U+0300 to U+036F) has no width.\n"
               "\n"
               "Ends with 0 if all went well, and 1 if a file cannot be read, the table is not\n"
               "one, or the widths or the memory asked for cannot be worked with.\n",
               program);
}

// Some text in one style.
struct Run {
  std::string text;
  std::uint16_t style = 0;
};

// A paragraph as it is written in the file, taken apart at its asterisks.
std::vector<Run> RunsOf(const std::string& written) {
  std::vector<Run> runs(1);
  for (const char ch : written) {
    if (ch == '*') {
      runs.push_back({.text = "", .style = static_cast<std::uint16_t>(runs.back().style == 0 ? 1 : 0)});
    } else {
      runs.back().text += ch;
    }
  }
  return runs;
}

// What setting the paragraphs took, added up.
struct Figures {
  std::uint64_t paragraphs = 0;
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t hyphens = 0;
  std::uint64_t tooWide = 0;
  std::uint64_t lastResortCuts = 0;
  std::uint64_t wordsAsked = 0;
  std::uint64_t calls = 0;
  std::uint64_t settledEarly = 0;
  std::uint64_t paragraphsSettledEarly = 0;
  std::uint64_t linesCutShort = 0;
  std::size_t mostNodes = 0;
  std::uint32_t mostBreakerWork = 0;
  std::uint32_t mostWork = 0;
  std::uint32_t mostLines = 0;
};

class Printer final : public bn::layout::TextLineSink {
 public:
  Printer(bool print, Figures& figures) noexcept : print_(print), figures_(figures) {}

  // The text the lines that follow are lines of.
  void Of(const std::string* text) noexcept { text_ = text; }

  const LineDigest& Digest() const noexcept { return digest_; }

  void OnLine(const TextLine& line) noexcept override {
    digest_.Add(line);
    ++figures_.lines;
    figures_.hyphens += line.hyphen ? 1U : 0U;
    figures_.tooWide += line.overfull ? 1U : 0U;
    if (!print_) {
      return;
    }
    std::string flags;
    flags += line.hyphen ? "h" : "";
    flags += line.last ? "l" : "";
    flags += line.overfull ? "w" : "";
    const std::string_view on = line.begin <= line.end && line.end <= text_->size()
                                    ? std::string_view(*text_).substr(line.begin, line.end - line.begin)
                                    : std::string_view("(not in the text)");
    std::printf("%u\t%u\t%u\t%u\t%s\t%.*s%s\n", line.begin, line.end, line.width, static_cast<unsigned>(line.spaces),
                flags.empty() ? "-" : flags.c_str(), static_cast<int>(on.size()), on.data(), line.hyphen ? "-" : "");
  }

 private:
  bool print_;
  Figures& figures_;
  const std::string* text_ = nullptr;
  LineDigest digest_;
};

bool ReadFile(const std::string& path, std::string& contents) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "bn-lines: cannot read %s\n", path.c_str());
    return false;
  }
  contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// The number an option is given, from 0 to `most`. False if it is not one.
bool Number(const std::string& text, unsigned long most, unsigned long& number) {
  char* end = nullptr;
  number = std::strtoul(text.c_str(), &end, 10);
  return !text.empty() && text[0] >= '0' && text[0] <= '9' && *end == '\0' && number <= most;
}

struct Options {
  std::string path;
  std::string tablePath;
  std::uint16_t width = 1760;
  std::vector<std::uint16_t> first;
  std::uint32_t penalty = 12100;
  std::size_t nodes = LineBreaker::kUsualNodes;
  std::size_t records = ParagraphSetter::kUsualRecords;
  bool figures = false;
};

bool Parse(const std::vector<std::string>& arguments, Options& options) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    const bool hasValue = i + 1 < arguments.size();
    unsigned long number = 0;
    if (argument == "--figures") {
      options.figures = true;
    } else if (argument == "--table" && hasValue) {
      options.tablePath = arguments[++i];
    } else if (argument == "--width" && hasValue && Number(arguments[++i], 0xFFFF, number)) {
      options.width = static_cast<std::uint16_t>(number);
    } else if (argument == "--penalty" && hasValue && Number(arguments[++i], 0xFFFFFFFFUL, number)) {
      options.penalty = static_cast<std::uint32_t>(number);
    } else if (argument == "--nodes" && hasValue && Number(arguments[++i], 65535, number)) {
      options.nodes = number;
    } else if (argument == "--records" && hasValue && Number(arguments[++i], 1UL << 24U, number)) {
      options.records = number;
    } else if (argument == "--first" && hasValue) {
      std::string rest = arguments[++i];
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        if (!Number(rest.substr(0, comma), 0xFFFF, number)) {
          return false;
        }
        options.first.push_back(static_cast<std::uint16_t>(number));
        rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
      }
    } else if (!argument.starts_with("--") && options.path.empty()) {
      options.path = argument;
    } else {
      return false;
    }
  }
  return true;
}

// The paragraphs of a file: what is between its empty lines.
std::vector<std::string> ParagraphsOf(const std::string& contents) {
  std::vector<std::string> paragraphs;
  std::string paragraph;
  std::size_t from = 0;
  while (from <= contents.size()) {
    std::size_t end = contents.find('\n', from);
    end = end == std::string::npos ? contents.size() : end;
    std::string line = contents.substr(from, end - from);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (!paragraph.empty()) {
        paragraphs.push_back(paragraph);
      }
      paragraph.clear();
    } else {
      paragraph += (paragraph.empty() ? "" : "\n") + line;
    }
    from = end + 1;
  }
  if (!paragraph.empty()) {
    paragraphs.push_back(paragraph);
  }
  return paragraphs;
}

int SetFile(const Options& options) {
  std::string contents;
  if (options.path.empty()) {
    contents.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  } else if (!ReadFile(options.path, contents)) {
    return 1;
  }
  std::string tableBytes;
  Hyphenator hyphenator;
  if (!options.tablePath.empty()) {
    if (!ReadFile(options.tablePath, tableBytes)) {
      return 1;
    }
    const std::span<const std::uint8_t> table(
        reinterpret_cast<const std::uint8_t*>(
            tableBytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        tableBytes.size());
    const HyphenStatus status = hyphenator.Open(table);
    if (status != HyphenStatus::kOk) {
      std::fprintf(stderr, "bn-lines: %s: %s\n", options.tablePath.c_str(), bn::layout::HyphenStatusName(status));
      return 1;
    }
  }

  std::vector<LineBreaker::Node> nodes(options.nodes);
  std::vector<ParagraphSetter::Record> records(options.records);
  ParagraphSetter setter(nodes, records);
  setter.UseHyphenator(options.tablePath.empty() ? nullptr : &hyphenator);
  MadeUpFace face;
  Figures figures;
  Printer printer(!options.figures, figures);
  const ParagraphSettings settings{.widths = {.first = options.first, .rest = options.width},
                                   .hyphenPenalty = options.penalty};

  for (const std::string& written : ParagraphsOf(contents)) {
    const std::vector<Run> runs = RunsOf(written);
    std::string text;
    for (const Run& run : runs) {
      text += run.text;
    }
    printer.Of(&text);
    const ParagraphStatus begun = setter.Begin(settings, face, printer);
    if (begun != ParagraphStatus::kOk) {
      std::fprintf(stderr, "bn-lines: %s\n", bn::layout::ParagraphStatusName(begun));
      return 1;
    }
    const std::uint64_t linesBefore = figures.lines;
    for (const Run& run : runs) {
      std::string_view rest = run.text;
      while (!rest.empty()) {
        rest.remove_prefix(setter.AddText(rest, run.style));
        ++figures.calls;
      }
    }
    ++figures.calls;
    while (!setter.Finish()) {
      ++figures.calls;
    }
    ++figures.paragraphs;
    figures.bytes += text.size();
    figures.lastResortCuts += setter.LastResortCuts();
    figures.wordsAsked += setter.WordsAsked();
    figures.linesCutShort += setter.LinesCutShort();
    figures.settledEarly += setter.Breaker().SettledEarly();
    figures.paragraphsSettledEarly += setter.Breaker().SettledEarly() != 0 ? 1U : 0U;
    figures.mostNodes = std::max(figures.mostNodes, setter.Breaker().MostNodes());
    figures.mostBreakerWork = std::max(figures.mostBreakerWork, setter.Breaker().MostWork());
    figures.mostWork = std::max(figures.mostWork, setter.MostWork());
    figures.mostLines = std::max(figures.mostLines, static_cast<std::uint32_t>(figures.lines - linesBefore));
    if (!options.figures) {
      std::printf("\n");
    }
  }

  if (options.figures) {
    const auto count = [](std::uint64_t number) { return static_cast<unsigned long long>(number); };
    std::printf("paragraphs %llu, bytes of text %llu, lines %llu, the most in one paragraph %u\n",
                count(figures.paragraphs), count(figures.bytes), count(figures.lines), figures.mostLines);
    std::printf("lines with a hyphen shown %llu, too wide %llu\n", count(figures.hyphens), count(figures.tooWide));
    std::printf("words the hyphenator was asked about %llu, places made as a last resort %llu\n",
                count(figures.wordsAsked), count(figures.lastResortCuts));
    std::printf("lines given out early %llu, in %llu paragraphs; ended for want of records %llu\n",
                count(figures.settledEarly), count(figures.paragraphsSettledEarly), count(figures.linesCutShort));
    std::printf("nodes in use at the most %zu of %zu\n", figures.mostNodes, options.nodes);
    std::printf("calls %llu; the most work in one, by the setter %u of %u, by the breaker %u of %u\n",
                count(figures.calls), figures.mostWork, ParagraphSetter::MostWorkFor(options.records),
                figures.mostBreakerWork, LineBreaker::MostWorkFor(options.nodes));
    std::printf("bytes measured %llu; checksum of the lines %08x\n", count(face.BytesMeasured()),
                static_cast<unsigned>(printer.Digest().Checksum()));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> all(argv, argv + argc);
  Options options;
  if (!Parse({all.begin() + (all.empty() ? 0 : 1), all.end()}, options)) {
    PrintUsage(all.empty() ? "bn-lines" : all[0].c_str());
    return 1;
  }
  return SetFile(options);
}
