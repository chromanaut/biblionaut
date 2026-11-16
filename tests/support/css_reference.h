#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "epub/css.h"
#include "epub/css_rules.h"

// What a book's rules come to for an element, worked out the plain way, to
// set beside what epub::CssRules and epub::CssCascade say.
//
// It keeps every rule as it was told, and every element of the document
// in a tree. For an element it tries every selector of every rule against
// the tree, going back over every choice a selector leaves; then puts the
// declarations of those that hold in the order CSS gives them and takes
// the last for each property. It shares nothing with the table but the
// reader of CSS that tells both of the rules, and what the table's header
// says of its limits.
//
// The one thing it does that is not plain: it remembers, while it tries
// one selector on one element, what it found for each part of the
// selector at each element it looked at. Without that, a selector of
// eight parts on a document made to be awkward takes longer than a test
// can wait.

namespace bn::test {

// A document's elements, as they were opened: enough of each to try a
// selector against.
struct ReferenceElement {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::ptrdiff_t parent = -1;  // Places among the document's elements; -1 for none.
  std::ptrdiff_t before = -1;  // The element before it inside the same parent.
  std::size_t depth = 0;
  std::ptrdiff_t lastChild = -1;
};

class CssReference final : public epub::CssHandler {
 public:
  // ---- The rules: a sheet is told between BeginSheet() and EndSheet(),
  // ---- as epub::CssRules is.
  void BeginSheet() {
    ++sheets_;
    inSheet_ = true;
  }
  void EndSheet() { inSheet_ = false; }

  void OnRuleBegin() noexcept override {
    rules_.push_back({.sheet = sheets_ - 1, .selectors = {}, .declarations = {}});
  }
  void OnSelector(const epub::CssSelector& selector) noexcept override {
    Selector kept;
    kept.firstLetter = selector.firstLetter;
    kept.rank = selector.specificity;
    for (const epub::CssCompound& part : selector.compounds) {
      Compound compound;
      compound.combinator = part.combinator;
      compound.element = std::string(part.element);
      compound.id = std::string(part.id);
      for (const std::string_view name : part.classes) {
        compound.classes.emplace_back(name);
      }
      for (const epub::CssAttribute& attribute : part.attributes) {
        compound.attributes.push_back({.name = std::string(attribute.name),
                                       .value = std::string(attribute.value),
                                       .test = attribute.test,
                                       .anyPrefix = attribute.anyPrefix,
                                       .anyCase = attribute.anyCase});
      }
      kept.compounds.push_back(std::move(compound));
    }
    rules_.back().selectors.push_back(std::move(kept));
  }
  void OnDeclaration(const epub::CssDeclaration& declaration) noexcept override {
    rules_.back().declarations.push_back(Kept(declaration));
  }
  void OnRuleEnd() noexcept override {}
  void OnImport(std::string_view /*url*/) noexcept override {}
  void OnDropped(epub::CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}

  // ---- A document.

  // `sheets` as epub::CssCascade::Begin() is given them; `levels` how many
  // open elements the cascade it is set beside has room for.
  void Begin(const std::vector<std::uint8_t>& sheets, std::size_t levels) {
    elements_.clear();
    open_.clear();
    levels_ = levels;
    order_.assign(sheets_, 0);
    std::size_t place = 0;
    for (const std::uint8_t sheet : sheets) {
      if (sheet < sheets_) {
        order_[sheet] = ++place;
      }
    }
  }

  // An element opens: what the rules come to for it, and for its first
  // letter.
  void Open(std::string_view name, const std::vector<std::pair<std::string, std::string>>& attributes,
            epub::CssStyle& style, epub::CssStyle& firstLetter) {
    ReferenceElement element;
    element.name = std::string(name);
    element.attributes = attributes;
    element.depth = open_.size();
    if (!open_.empty()) {
      element.parent = open_.back();
      element.before = elements_[static_cast<std::size_t>(open_.back())].lastChild;
    }
    const auto place = static_cast<std::ptrdiff_t>(elements_.size());
    elements_.push_back(std::move(element));
    open_.push_back(place);

    // Every declaration of every selector that holds, with what decides
    // between them; then in that order, so that the last for a property is
    // the one that wins.
    std::vector<Setting> settings;
    std::vector<Setting> firstLetterSettings;
    for (std::size_t r = 0; r < rules_.size(); ++r) {
      const Rule& rule = rules_[r];
      if (order_[rule.sheet] == 0) {
        continue;
      }
      for (const Selector& selector : rule.selectors) {
        ++trying_;
        found_.resize(std::max(found_.size(), elements_.size() * kMostParts));
        if (selector.compounds.size() > kMostParts || !Holds(selector, selector.compounds.size() - 1, place)) {
          continue;
        }
        for (std::size_t d = 0; d < rule.declarations.size(); ++d) {
          const Declaration& declaration = rule.declarations[d];
          (selector.firstLetter ? firstLetterSettings : settings)
              .push_back({.order = {declaration.important, false, selector.rank, order_[rule.sheet], r, d},
                          .declaration = &declaration});
        }
      }
    }
    // Its own `style` attribute.
    own_.clear();
    for (const auto& [attribute, value] : elements_[static_cast<std::size_t>(place)].attributes) {
      if (attribute == "style") {
        Collector collector(own_);
        epub::ReadCssDeclarations(value, collector);
        break;  // The first of the name is the one a parser's Find() gives.
      }
    }
    for (std::size_t d = 0; d < own_.size(); ++d) {
      settings.push_back({.order = {own_[d].important, true, 0, 0, 0, d}, .declaration = &own_[d]});
    }
    Settle(settings, style);
    Settle(firstLetterSettings, firstLetter);
  }

  void Close() {
    const std::ptrdiff_t closed = open_.back();
    open_.pop_back();
    if (!open_.empty()) {
      elements_[static_cast<std::size_t>(open_.back())].lastChild = closed;
    }
  }

  std::size_t Rules() const { return rules_.size(); }

  // Whether two styles are the same in everything that was set.
  static bool Same(const epub::CssStyle& a, const epub::CssStyle& b) {
    if (a.set != b.set) {
      return false;
    }
    for (std::size_t property = 0; property < epub::kCssProperties; ++property) {
      if (((a.set >> property) & 1U) == 0) {
        continue;
      }
      const epub::CssValue& mine = a.values[property];
      const epub::CssValue& theirs = b.values[property];
      if (mine.kind != theirs.kind) {
        return false;
      }
      if (mine.kind == epub::CssValue::Kind::kKeyword && mine.keyword != theirs.keyword) {
        return false;
      }
      if (mine.kind == epub::CssValue::Kind::kLength && (mine.number != theirs.number || mine.unit != theirs.unit)) {
        return false;
      }
      if (mine.kind == epub::CssValue::Kind::kFamilies && mine.families != theirs.families) {
        return false;
      }
    }
    return true;
  }

 private:
  struct Attribute {
    std::string name;
    std::string value;
    epub::CssAttributeTest test = epub::CssAttributeTest::kPresent;
    bool anyPrefix = false;
    bool anyCase = false;
  };
  struct Compound {
    epub::CssCombinator combinator = epub::CssCombinator::kFirst;
    std::string element;
    std::string id;
    std::vector<std::string> classes;
    std::vector<Attribute> attributes;
  };
  struct Selector {
    std::vector<Compound> compounds;
    bool firstLetter = false;
    std::uint32_t rank = 0;
  };
  struct Declaration {
    epub::CssProperty property = epub::CssProperty::kFontStyle;
    epub::CssValue value;
    std::string families;  // The list `value.families` is made to look at.
    bool important = false;
  };
  struct Rule {
    std::size_t sheet = 0;
    std::vector<Selector> selectors;
    std::vector<Declaration> declarations;
  };
  struct Setting {
    // Important or not; the element's own or a rule's; the selector's
    // rank; where the sheet comes; which rule; which declaration of it.
    std::tuple<bool, bool, std::uint32_t, std::size_t, std::size_t, std::size_t> order;
    const Declaration* declaration = nullptr;
  };

  static Declaration Kept(const epub::CssDeclaration& declaration) {
    Declaration kept;
    kept.property = declaration.property;
    kept.value = declaration.value;
    kept.families = std::string(declaration.value.families);
    kept.value.families = {};
    kept.important = declaration.important;
    return kept;
  }

  class Collector final : public epub::CssHandler {
   public:
    explicit Collector(std::vector<Declaration>& into) : into_(into) {}
    void OnRuleBegin() noexcept override {}
    void OnSelector(const epub::CssSelector& /*selector*/) noexcept override {}
    void OnDeclaration(const epub::CssDeclaration& declaration) noexcept override {
      into_.push_back(Kept(declaration));
    }
    void OnRuleEnd() noexcept override {}
    void OnImport(std::string_view /*url*/) noexcept override {}
    void OnDropped(epub::CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}

   private:
    std::vector<Declaration>& into_;
  };

  static char Lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }
  static std::string Lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), Lower);
    return out;
  }
  static bool IsSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }
  static std::vector<std::string> Words(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    for (const char ch : text) {
      if (!IsSpace(ch)) {
        word += ch;
      } else if (!word.empty()) {
        words.push_back(std::move(word));
        word.clear();
      }
    }
    if (!word.empty()) {
      words.push_back(std::move(word));
    }
    return words;
  }
  static const std::string* Find(const ReferenceElement& element, std::string_view name) {
    for (const auto& [attribute, value] : element.attributes) {
      if (attribute == name) {
        return &value;
      }
    }
    return nullptr;
  }

  // Whether an attribute that is `is` is what `asked` asks.
  static bool Answers(std::string is, const Attribute& asked) {
    std::string value = asked.value;
    if (asked.anyCase) {
      is = Lowered(is);
      value = Lowered(value);
    }
    switch (asked.test) {
      case epub::CssAttributeTest::kPresent:
        return true;
      case epub::CssAttributeTest::kEquals:
        return is == value;
      case epub::CssAttributeTest::kWord: {
        const std::vector<std::string> words = Words(is);
        return !value.empty() && Words(value).size() == 1 && Words(value).front() == value &&
               std::find(words.begin(), words.end(), value) != words.end();
      }
      case epub::CssAttributeTest::kDash:
        return is == value || is.starts_with(value + "-");
      case epub::CssAttributeTest::kBegins:
        return !value.empty() && is.starts_with(value);
      case epub::CssAttributeTest::kEnds:
        return !value.empty() && is.ends_with(value);
      case epub::CssAttributeTest::kContains:
        return !value.empty() && is.find(value) != std::string::npos;
    }
    return false;
  }

  bool Is(const Compound& compound, const ReferenceElement& element) const {
    if (!compound.element.empty() && compound.element != Lowered(element.name)) {
      return false;
    }
    if (!compound.id.empty()) {
      const std::string* id = Find(element, "id");
      if (id == nullptr || *id != compound.id) {
        return false;
      }
    }
    if (!compound.classes.empty()) {
      const std::string* written = Find(element, "class");
      std::vector<std::string> has = written == nullptr ? std::vector<std::string>() : Words(*written);
      // Only so many of an element's classes are looked at.
      has.resize(std::min(has.size(), epub::CssCascade::kMaxClasses));
      for (const std::string& wanted : compound.classes) {
        if (std::find(has.begin(), has.end(), wanted) == has.end()) {
          return false;
        }
      }
    }
    for (const Attribute& asked : compound.attributes) {
      bool found = false;
      for (const auto& [name, value] : element.attributes) {
        const std::string lowered = Lowered(name);
        const std::size_t colon = lowered.rfind(':');  // As epub::XmlLocalName() has it.
        const std::string local = colon == std::string::npos ? lowered : lowered.substr(colon + 1);
        if ((lowered == asked.name || (asked.anyPrefix && local == asked.name)) && Answers(value, asked)) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  }

  // Whether the selector's parts up to `part` hold, with `part` being the
  // element at `place`. It calls itself for each choice a selector leaves,
  // which is the plain way and why this is here.
  // NOLINTNEXTLINE(misc-no-recursion)
  bool Holds(const Selector& selector, std::size_t part, std::ptrdiff_t place) {
    // What was found for this part at this element, if it was looked at
    // before for the selector and the element in hand.
    Found& found = found_[(static_cast<std::size_t>(place) * kMostParts) + part];
    if (found.trying != trying_) {
      found = {.trying = trying_, .holds = HoldsAfresh(selector, part, place)};
    }
    return found.holds;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  bool HoldsAfresh(const Selector& selector, std::size_t part, std::ptrdiff_t place) {
    const ReferenceElement& element = elements_[static_cast<std::size_t>(place)];
    const Compound& compound = selector.compounds[part];
    if (!Is(compound, element)) {
      return false;
    }
    if (part == 0) {
      return true;
    }
    switch (compound.combinator) {
      case epub::CssCombinator::kFirst:
        return false;
      case epub::CssCombinator::kChild:
        return Kept(element.parent) && Holds(selector, part - 1, element.parent);
      case epub::CssCombinator::kDescendant:
        for (std::ptrdiff_t outer = element.parent; Kept(outer);
             outer = elements_[static_cast<std::size_t>(outer)].parent) {
          if (Holds(selector, part - 1, outer)) {
            return true;
          }
        }
        return false;
      case epub::CssCombinator::kAdjacent:
        return Kept(element.before) && Holds(selector, part - 1, element.before);
    }
    return false;
  }

  // Whether there is such an element and the cascade had room to keep it:
  // what is asked of one it had no room for is not so.
  bool Kept(std::ptrdiff_t place) const {
    return place >= 0 && elements_[static_cast<std::size_t>(place)].depth < levels_;
  }

  static void Settle(std::vector<Setting>& settings, epub::CssStyle& style) {
    std::stable_sort(settings.begin(), settings.end(),
                     [](const Setting& a, const Setting& b) { return a.order < b.order; });
    style.set = 0;
    for (const Setting& setting : settings) {
      const auto property = static_cast<std::size_t>(setting.declaration->property);
      style.values[property] = setting.declaration->value;
      style.values[property].families = setting.declaration->families;
      style.set |= 1U << property;
    }
  }

  std::vector<Rule> rules_;
  std::size_t sheets_ = 0;
  bool inSheet_ = false;
  std::vector<std::size_t> order_;
  std::vector<ReferenceElement> elements_;
  std::vector<std::ptrdiff_t> open_;
  std::vector<Declaration> own_;
  std::size_t levels_ = SIZE_MAX;

  // The most parts a selector has, and what was found for each at each
  // element: good only for the trying it was found in.
  static constexpr std::size_t kMostParts = epub::CssReader::kMaxCompounds;
  struct Found {
    std::uint64_t trying = 0;
    bool holds = false;
  };
  std::vector<Found> found_;
  std::uint64_t trying_ = 0;
};

}  // namespace bn::test
