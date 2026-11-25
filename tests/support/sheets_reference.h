#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "book_fakes.h"
#include "core/byte_source.h"
#include "epub/book_path.h"
#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/document_sheets.h"
#include "epub/xml.h"

// The style sheets a document asks for, worked out the plain way, to set
// beside what epub::DocumentSheets says.
//
// It reads the whole of the document's head into lists, then goes down
// from each link by calling itself, reading a sheet again for its imports
// every time it comes to it, and keeps every name as a string. It shares
// with DocumentSheets the parser of XML and the reader of CSS that tell
// both what is in the files, the working-out of a path, and the rule for
// a medium; and nothing of how the links are judged, how the list is
// built, or what is kept from one document to the next. It reads a file
// in the pieces the book gives it in, since what the parser has told of a
// file by the time the file fails goes by how its bytes came. To say how many
// rules a document's own `<style>` elements come to, it puts the text of
// each, kept whole as a string, through a table of its own.
//
// What it does not work out is what DocumentSheets leaves out for want of
// room (a sheet or an import past what it keeps): its book is as large as
// it likes. Whoever sets the two side by side looks only at books where
// DocumentSheets left nothing out for that.

namespace bn::test {

// What a document should be given.
struct SheetsExpected {
  bool document = false;  // The book has the document.
  // The paths of its sheets, in the list's order; kOwnStyle for the sheet
  // of its own `<style>` rules.
  std::vector<std::string> sheets;
  // What was left out for this document, by kind.
  std::array<std::uint32_t, epub::kSheetLeftOutKinds> leftOut{};
  // The text of each `<style>` that its own sheet is made of, in order.
  std::vector<std::string> ownStyles;
};

class SheetsReference {
 public:
  static constexpr std::string_view kOwnStyle = "<style>";

  // `styleBytes` is how much room the document's `<style>` text has.
  SheetsReference(const FakeBook& book, std::size_t styleBytes) : book_(book), styleBytes_(styleBytes) {}

  // A book begins: no sheet has been read.
  void BeginBook() { read_.clear(); }

  SheetsExpected For(std::string_view document) {
    expected_ = {};
    if (document.empty() || document.size() > epub::kMaxBookPath || !book_.Has(document)) {
      return expected_;
    }
    expected_.document = true;
    Head head;
    ReadHead(document, head);

    // The links that count, in their order: the first so many are followed.
    std::vector<std::string> followed;
    for (const Link& link : head.links) {
      if (!link.hasHref || !HasWord(link.rel, "stylesheet") || HasWord(link.rel, "alternate") || !IsCss(link.type)) {
        continue;
      }
      if (!epub::CssMediaApplies(link.media)) {
        Count(epub::SheetLeftOut::kLinkMedia);
        continue;
      }
      const std::string path = Resolved(epub::BookDirectory(document), link.href);
      if (path.empty()) {
        Count(epub::SheetLeftOut::kLinkNotInBook);
      } else if (followed.size() == epub::DocumentSheets::kMaxLinks) {
        Count(epub::SheetLeftOut::kLinkTooMany);
      } else {
        followed.push_back(path);
      }
    }
    for (const std::string& path : followed) {
      Give(path, false, false, {});
    }

    // Its own rules: each `<style>` that fits in what is left of the room.
    std::size_t used = 0;
    bool own = false;
    for (const Style& style : head.styles) {
      if (!IsCss(style.type)) {
        continue;
      }
      // One for another medium is said as it opens, closed or not.
      if (!epub::CssMediaApplies(style.media)) {
        Count(epub::SheetLeftOut::kStyleMedia);
        continue;
      }
      // Only a `<style>` that closes is one to read.
      if (!style.closed || style.text.empty()) {
        continue;
      }
      if (used + style.text.size() + 1 > styleBytes_) {
        Count(epub::SheetLeftOut::kStyleTooLong);
        continue;
      }
      used += style.text.size() + 1;
      own = true;
      expected_.ownStyles.push_back(style.text);
      for (std::size_t i = ImportsOf(style.text, FakeBook::kNever, FakeBook::kNever).size(); i > 0; --i) {
        Count(epub::SheetLeftOut::kStyleImport);
      }
    }
    if (own) {
      expected_.sheets.emplace_back(kOwnStyle);
    }
    return expected_;
  }

  // How many rules a document's own `<style>` texts come to: each read to
  // its own end, into one sheet of a table with room for all of them.
  static std::uint32_t OwnRules(const SheetsExpected& expected) {
    std::vector<std::uint8_t> memory(65535);
    epub::CssRules rules(memory);
    std::uint8_t number = 0;
    if (expected.ownStyles.empty() || !rules.BeginSheet(number)) {
      return 0;
    }
    for (const std::string& text : expected.ownStyles) {
      Failing source(text, FakeBook::kNever, FakeBook::kNever);
      epub::CssReader reader;
      static_cast<void>(reader.Read(source, rules));
    }
    rules.EndSheet();
    return rules.Rules();
  }

 private:
  struct Link {
    bool hasHref = false;
    std::string href;
    std::string rel;
    std::string type;
    std::string media;
  };
  struct Style {
    std::string type;
    std::string media;
    std::string text;
    bool closed = false;
  };
  struct Head {
    std::vector<Link> links;
    std::vector<Style> styles;
  };

  // Bytes that fail after so many, and come so many at a time at the most,
  // as a file of a FakeBook does.
  class Failing final : public core::ByteSource {
   public:
    Failing(std::string_view bytes, std::size_t failAfter, std::size_t pieceBytes)
        : bytes_(bytes), failAfter_(failAfter), pieceBytes_(pieceBytes) {}
    [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
      const std::size_t end = std::min(bytes_.size(), failAfter_);
      if (at_ == end && failAfter_ <= bytes_.size()) {
        return kError;
      }
      const std::size_t count = std::min({dst.size(), end - at_, pieceBytes_});
      std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at_), count, dst.begin());
      at_ += count;
      return static_cast<std::ptrdiff_t>(count);
    }

   private:
    std::string_view bytes_;
    std::size_t failAfter_;
    std::size_t pieceBytes_;
    std::size_t at_ = 0;
  };

  // Writes down the links and the `<style>` elements of a head, as far as
  // `<body>`.
  class HeadReader final : public epub::XmlHandler {
   public:
    HeadReader(epub::XmlParser& parser, Head& head) : parser_(parser), head_(head) {}
    void OnStartElement(std::string_view name, const epub::XmlAttributes& attributes) noexcept override {
      if (inStyle_ > 0) {
        ++inStyle_;
        return;
      }
      const std::string local = Lowered(epub::XmlLocalName(name));
      if (!rootSeen_) {
        rootSeen_ = true;
        if (local != "html") {
          parser_.Stop();
        }
        return;
      }
      if (local == "body") {
        parser_.Stop();
      } else if (local == "link") {
        Link link;
        std::string_view value;
        link.hasHref = attributes.Find("href", value);
        link.href = link.hasHref ? std::string(value) : std::string();
        link.rel = attributes.Find("rel", value) ? std::string(value) : std::string();
        link.type = attributes.Find("type", value) ? std::string(value) : std::string();
        link.media = attributes.Find("media", value) ? std::string(value) : std::string();
        head_.links.push_back(std::move(link));
      } else if (local == "style") {
        inStyle_ = 1;
        std::string_view value;
        Style style;
        style.type = attributes.Find("type", value) ? std::string(value) : std::string();
        style.media = attributes.Find("media", value) ? std::string(value) : std::string();
        head_.styles.push_back(std::move(style));
      }
    }
    void OnEndElement(std::string_view /*name*/) noexcept override {
      if (inStyle_ == 0) {
        return;
      }
      --inStyle_;
      if (inStyle_ == 0) {
        head_.styles.back().closed = true;
      }
    }
    void OnText(std::string_view text) noexcept override {
      if (inStyle_ > 0) {
        head_.styles.back().text += text;
      }
    }
    void OnEntity(std::string_view /*name*/) noexcept override {}

   private:
    epub::XmlParser& parser_;
    Head& head_;
    bool rootSeen_ = false;
    std::size_t inStyle_ = 0;
  };

  // Writes down the imports a sheet names.
  class ImportReader final : public epub::CssHandler {
   public:
    void OnRuleBegin() noexcept override {}
    void OnSelector(const epub::CssSelector& /*selector*/) noexcept override {}
    void OnDeclaration(const epub::CssDeclaration& /*declaration*/) noexcept override {}
    void OnRuleEnd() noexcept override {}
    void OnImport(std::string_view url) noexcept override { imports.emplace_back(url); }
    void OnDropped(epub::CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}
    std::vector<std::string> imports;
  };

  static std::string Lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
      c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
  }

  static std::vector<std::string> Words(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
      if (std::string_view(" \t\n\r\f").find(c) != std::string_view::npos) {
        if (!word.empty()) {
          words.push_back(std::exchange(word, {}));
        }
      } else {
        word += c;
      }
    }
    if (!word.empty()) {
      words.push_back(word);
    }
    return words;
  }

  static bool HasWord(std::string_view list, std::string_view word) {
    const std::vector<std::string> words = Words(Lowered(list));
    return std::find(words.begin(), words.end(), word) != words.end();
  }

  static bool IsCss(std::string_view type) {
    const std::vector<std::string> words = Words(Lowered(type));
    // Nothing but spaces, or the one word; "text/css x" is neither.
    const bool spacesInside = words.size() > 1;
    return words.empty() || (!spacesInside && words.front() == "text/css");
  }

  static std::string Resolved(std::string_view base, std::string_view reference) {
    std::array<char, epub::kMaxBookPath> path{};
    const std::size_t length = epub::ResolveBookPath(base, reference, path);
    return {path.data(), length};
  }

  void Count(epub::SheetLeftOut what) { ++expected_.leftOut[static_cast<std::size_t>(what)]; }

  void ReadHead(std::string_view document, Head& head) {
    epub::XmlParser parser(memory_);
    HeadReader reader(parser, head);
    Failing source(book_.Contents(document), book_.FailsAfter(document), book_.pieceBytes);
    const epub::XmlStatus status = parser.Parse(source, reader);
    if (status != epub::XmlStatus::kOk && status != epub::XmlStatus::kStopped) {
      Count(epub::SheetLeftOut::kHeadNotRead);
    }
  }

  static std::vector<std::string> ImportsOf(std::string_view sheet, std::size_t failAfter, std::size_t pieceBytes) {
    Failing source(sheet, failAfter, pieceBytes);
    ImportReader imports;
    epub::CssReader reader;
    static_cast<void>(reader.Read(source, imports));
    return imports.imports;
  }

  // A sheet named by a link or an import: those it imports, and then
  // itself. `within` is the sheets on the way down to it.
  // NOLINTNEXTLINE(misc-no-recursion): the plain way, which goes no deeper than DocumentSheets::kMaxDepth.
  void Give(const std::string& name, bool nowhere, bool isImport, const std::vector<std::string>& within) {
    if (isImport && within.size() > epub::DocumentSheets::kMaxDepth) {
      Count(epub::SheetLeftOut::kImportTooDeep);
      return;
    }
    if (isImport && !nowhere && std::find(within.begin(), within.end(), name) != within.end()) {
      Count(epub::SheetLeftOut::kImportLoop);
      return;
    }
    if (nowhere || !book_.Has(name)) {
      Count(isImport ? epub::SheetLeftOut::kImportNotInBook : epub::SheetLeftOut::kLinkNotInBook);
      return;
    }
    const std::string_view contents = book_.Contents(name);
    const std::size_t failAfter = book_.FailsAfter(name);
    // A sheet whose file fails is said once for the book: when it is read.
    if (read_.insert(name).second && failAfter <= contents.size()) {
      Count(epub::SheetLeftOut::kSheetNotRead);
    }
    std::vector<std::string> inside = within;
    inside.push_back(name);
    for (const std::string& url : ImportsOf(contents, failAfter, book_.pieceBytes)) {
      const std::string path = Resolved(epub::BookDirectory(name), url);
      Give(path.empty() ? url : path, path.empty(), true, inside);
    }
    // Named before, it counts where it is named last.
    std::erase(expected_.sheets, name);
    expected_.sheets.push_back(name);
  }

  const FakeBook& book_;
  std::size_t styleBytes_;
  // The parser's memory: far more than a head needs.
  std::vector<std::uint8_t> memory_ = std::vector<std::uint8_t>(262144);
  std::set<std::string> read_;
  SheetsExpected expected_;
};

}  // namespace bn::test
