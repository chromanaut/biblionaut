#include "epub/css_rules.h"

#include <algorithm>
#include <cstring>

#include "core/metrics.h"
#include "names.h"

namespace bn::epub {

namespace {

// How things are kept in the block. A place is a count of bytes from the
// block's start, and 0 is no place. Numbers of two bytes have the lower
// first. Things are added at the top and never moved; the places of the
// names, in the order of the names, are kept at the block's end, growing
// down to meet them.
//
//   a name       [selectors 2] [length 2] [its bytes]
//                `selectors` is the place of the newest selector that is
//                looked for under this name.
//   a node       [older 2] [number 1] [before 1] [a compound]
//                `older` is the place of the node added before it;
//                `before` the number of the node that the element before
//                has to be, or kNoNode.
//   a rule       [declarations 2] [sheet 1]
//   a selector   [next 2] [rule 2] [rank 2] [shape 1] [before 1]
//                [a compound] [steps: kind 1, node 1]...
//                `next` is the place of the next older selector looked
//                for under the same name; the compound is what the
//                element it styles has to be; `before` as for a node;
//                the steps go outward from that element.
//   a compound   [what 1] [counts 1] [element 2]? [id 2]? [class 2]...
//                [attribute: name 2, value 2, how 1]...
//   declarations [how many 1] then for each [property 1] [form 1]
//                [value: 0, 1, 2 or 4 bytes]

constexpr std::size_t kFirstPlace = 2;
constexpr std::uint8_t kNoNode = 0xFF;

constexpr std::size_t kNameHead = 4;
constexpr std::size_t kNodeHead = 4;
constexpr std::size_t kRuleBytes = 3;
constexpr std::size_t kSelectorHead = 8;
constexpr std::size_t kStepBytes = 2;
constexpr std::size_t kAttributeBytes = 5;
constexpr std::size_t kMaxCompoundBytes =
    6 + (2 * CssReader::kMaxClasses) + (kAttributeBytes * CssReader::kMaxAttributes);

// A compound's first byte: which of an element's name and an id it has.
constexpr std::uint8_t kHasElement = 1;
constexpr std::uint8_t kHasId = 2;

// An attribute's last byte: the test, then these.
constexpr std::uint8_t kTestMask = 7;
constexpr std::uint8_t kAnyPrefix = 8;
constexpr std::uint8_t kAnyCase = 16;

// A selector's shape: whether it is for the first letter, then how many
// steps it has.
constexpr std::uint8_t kFirstLetter = 1;
constexpr unsigned kStepsShift = 1;

// A step's kind.
constexpr std::uint8_t kParent = 0;    // The node has to be the element straight outside.
constexpr std::uint8_t kAncestor = 1;  // The node has to be an element outside, at any depth.

// A declaration's form: what kind of value, whether it was important, the
// unit of a length, and how many bytes its number takes.
constexpr std::uint8_t kKindMask = 3;
constexpr std::uint8_t kImportant = 4;
constexpr unsigned kUnitShift = 3;
constexpr std::uint8_t kUnitMask = 7;
constexpr unsigned kSizeShift = 6;  // 0: the number is zero. 1: two bytes. 2: four.

// What decides between declarations, from what matters most: see the top
// of the header.
constexpr unsigned kRankImportant = 62;
constexpr unsigned kRankOwn = 61;
constexpr unsigned kRankSelector = 40;
constexpr unsigned kRankSheet = 32;

core::Counter cssRulesKept{"css_rules_kept"};
core::Counter cssRulesLeftOut{"css_rules_left_out"};

constexpr bool IsSpace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }

std::size_t CompoundBytes(std::uint8_t what, std::uint8_t counts) noexcept {
  return 2 + ((what & kHasElement) != 0 ? 2U : 0U) + ((what & kHasId) != 0 ? 2U : 0U) + (2U * (counts & 0x0FU)) +
         (kAttributeBytes * (counts >> 4U));
}

bool TestBit(std::span<const std::uint8_t> bits, std::size_t number) noexcept {
  return (number / 8) < bits.size() && ((static_cast<unsigned>(bits[number / 8]) >> (number % 8)) & 1U) != 0;
}

// `text` and `table` set against one another as names are ordered, where
// `text`'s capitals are taken as small letters if `lowered`.
int Compare(std::string_view text, bool lowered, std::string_view table) noexcept {
  const std::size_t shared = std::min(text.size(), table.size());
  for (std::size_t i = 0; i < shared; ++i) {
    const auto mine = static_cast<unsigned char>(lowered ? AsciiLower(text[i]) : text[i]);
    const auto theirs = static_cast<unsigned char>(table[i]);
    if (mine != theirs) {
      return mine < theirs ? -1 : 1;
    }
  }
  if (text.size() == table.size()) {
    return 0;
  }
  return text.size() < table.size() ? -1 : 1;
}

bool Same(std::string_view a, std::string_view b, bool anyCase) noexcept {
  return anyCase ? SameButForCase(a, b) : a == b;
}

// `count` bytes of `text` from `start`, which the caller knows are there.
std::string_view Piece(std::string_view text, std::size_t start, std::size_t count) noexcept {
  text.remove_prefix(start);
  text.remove_suffix(text.size() - count);
  return text;
}

// Whether what an attribute is, is what was asked of it.
bool Holds(CssAttributeTest test, std::string_view is, std::string_view asked, bool anyCase) noexcept {
  switch (test) {
    case CssAttributeTest::kPresent:
      return true;
    case CssAttributeTest::kEquals:
      return Same(is, asked, anyCase);
    case CssAttributeTest::kWord: {
      if (asked.empty() || std::any_of(asked.begin(), asked.end(), IsSpace)) {
        return false;
      }
      std::size_t start = 0;
      for (std::size_t i = 0; i <= is.size(); ++i) {
        if (i == is.size() || IsSpace(is[i])) {
          if (i - start == asked.size() && Same(Piece(is, start, i - start), asked, anyCase)) {
            return true;
          }
          start = i + 1;
        }
      }
      return false;
    }
    case CssAttributeTest::kDash:
      if (is.size() == asked.size()) {
        return Same(is, asked, anyCase);
      }
      return is.size() > asked.size() && is[asked.size()] == '-' && Same(Piece(is, 0, asked.size()), asked, anyCase);
    case CssAttributeTest::kBegins:
      return !asked.empty() && is.size() >= asked.size() && Same(Piece(is, 0, asked.size()), asked, anyCase);
    case CssAttributeTest::kEnds:
      return !asked.empty() && is.size() >= asked.size() &&
             Same(Piece(is, is.size() - asked.size(), asked.size()), asked, anyCase);
    case CssAttributeTest::kContains:
      if (asked.empty() || is.size() < asked.size()) {
        return false;
      }
      for (std::size_t at = 0; at + asked.size() <= is.size(); ++at) {
        if (Same(Piece(is, at, asked.size()), asked, anyCase)) {
          return true;
        }
      }
      return false;
  }
  return false;
}

}  // namespace

// ---- The rules ----

CssRules::CssRules(std::span<std::uint8_t> memory) noexcept
    : memory_(memory.data()), size_(std::min(memory.size(), kMaxBytes)) {
  Clear();
}

void CssRules::Clear() noexcept {
  top_ = kFirstPlace;
  names_ = 0;
  nodes_ = 0;
  newestNode_ = 0;
  keyless_ = 0;
  sheets_ = 0;
  rules_ = 0;
  selectors_ = 0;
  leftOutForRoom_ = 0;
  leftOutForNodes_ = 0;
  inSheet_ = false;
  inRule_ = false;
  leftOut_ = LeftOut::kNo;
}

std::size_t CssRules::Bytes() const noexcept { return top_ + (std::size_t{2} * names_); }

std::uint16_t CssRules::Load(std::size_t at) const noexcept {
  return static_cast<std::uint16_t>(static_cast<unsigned>(memory_[at]) |
                                    (static_cast<unsigned>(memory_[at + 1]) << 8U));
}

std::uint16_t CssRules::Older(std::uint16_t place) const noexcept {
  const std::uint16_t older = Load(place);
  return older < place ? older : std::uint16_t{0};
}

void CssRules::Store(std::size_t at, std::uint16_t value) noexcept {
  memory_[at] = static_cast<std::uint8_t>(value & 0xFFU);
  memory_[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}

bool CssRules::Fits(std::size_t bytes) const noexcept { return top_ + bytes + (std::size_t{2} * names_) <= size_; }

void CssRules::Push8(std::uint8_t value) noexcept { memory_[top_++] = value; }

void CssRules::Push16(std::uint16_t value) noexcept {
  Store(top_, value);
  top_ = static_cast<std::uint16_t>(top_ + 2);
}

std::size_t CssRules::IndexAt(std::size_t index) const noexcept {
  return size_ - (std::size_t{2} * names_) + (2 * index);
}

std::string_view CssRules::NameAt(std::uint16_t name) const noexcept {
  if (name == 0) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  return {reinterpret_cast<const char*>(memory_ + name + kNameHead), Load(name + 2U)};
}

std::uint16_t CssRules::Find(std::string_view text, bool lowered, std::size_t& index,
                             std::uint32_t* probes) const noexcept {
  std::size_t low = 0;
  std::size_t high = names_;
  while (low < high) {
    const std::size_t middle = low + ((high - low) / 2);
    const std::uint16_t name = Load(IndexAt(middle));
    if (probes != nullptr) {
      ++*probes;
    }
    const int order = Compare(text, lowered, NameAt(name));
    if (order == 0) {
      index = middle;
      return name;
    }
    if (order < 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  index = low;
  return 0;
}

std::uint16_t CssRules::Intern(std::string_view text) noexcept {
  std::size_t index = 0;
  if (const std::uint16_t name = Find(text, false, index, nullptr); name != 0) {
    return name;
  }
  if (!Fits(kNameHead + text.size() + 2)) {
    return 0;
  }
  const std::uint16_t name = top_;
  Push16(0);
  Push16(static_cast<std::uint16_t>(text.size()));
  std::copy(text.begin(), text.end(), memory_ + top_);
  top_ = static_cast<std::uint16_t>(top_ + text.size());
  // Its place goes among the others in order: those before it move down.
  const std::size_t start = IndexAt(0);
  std::memmove(memory_ + start - 2, memory_ + start, 2 * index);
  ++names_;
  Store(IndexAt(index), name);
  return name;
}

std::size_t CssRules::Encode(const CssCompound& compound, std::span<std::uint8_t> out) noexcept {
  std::size_t length = 2;
  const auto put = [&out, &length](std::uint16_t value) {
    out[length++] = static_cast<std::uint8_t>(value & 0xFFU);
    out[length++] = static_cast<std::uint8_t>(value >> 8U);
  };
  std::uint8_t what = 0;
  if (!compound.element.empty()) {
    const std::uint16_t name = Intern(compound.element);
    if (name == 0) {
      return 0;
    }
    what |= kHasElement;
    put(name);
  }
  if (!compound.id.empty()) {
    const std::uint16_t name = Intern(compound.id);
    if (name == 0) {
      return 0;
    }
    what |= kHasId;
    put(name);
  }
  const std::size_t classes = std::min(compound.classes.size(), CssReader::kMaxClasses);
  const std::size_t attributes = std::min(compound.attributes.size(), CssReader::kMaxAttributes);
  for (std::size_t i = 0; i < classes; ++i) {
    const std::uint16_t name = Intern(compound.classes[i]);
    if (name == 0) {
      return 0;
    }
    put(name);
  }
  for (std::size_t i = 0; i < attributes; ++i) {
    const CssAttribute& attribute = compound.attributes[i];
    const std::uint16_t name = Intern(attribute.name);
    // A value of nothing has no place, and needs none.
    const std::uint16_t value = attribute.value.empty() ? std::uint16_t{0} : Intern(attribute.value);
    if (name == 0 || (value == 0 && !attribute.value.empty())) {
      return 0;
    }
    put(name);
    put(value);
    out[length++] =
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(attribute.test) | (attribute.anyPrefix ? kAnyPrefix : 0U) |
                                  (attribute.anyCase ? kAnyCase : 0U));
  }
  out[0] = what;
  out[1] = static_cast<std::uint8_t>(classes | (attributes << 4U));
  return length;
}

bool CssRules::NodeFor(const CssCompound& compound, std::uint8_t prev, std::uint8_t& node) noexcept {
  std::array<std::uint8_t, kMaxCompoundBytes>
      encoded;  // NOLINT(cppcoreguidelines-pro-type-member-init): filled by Encode().
  const std::size_t length = Encode(compound, encoded);
  if (length == 0) {
    LeaveOut(LeftOut::kRoom);
    return false;
  }
  // The same thing asked before, after the same node, is the same node.
  for (std::uint16_t place = newestNode_; place != 0; place = Older(place)) {
    if (memory_[place + 3U] == prev && CompoundBytes(memory_[place + 4U], memory_[place + 5U]) == length &&
        std::equal(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(length),
                   memory_ + place + kNodeHead)) {
      node = memory_[place + 2U];
      return true;
    }
  }
  if (nodes_ == kMaxNodes) {
    LeaveOut(LeftOut::kNodes);
    return false;
  }
  if (!Fits(kNodeHead + length)) {
    LeaveOut(LeftOut::kRoom);
    return false;
  }
  const std::uint16_t place = top_;
  node = static_cast<std::uint8_t>(nodes_);
  Push16(newestNode_);
  Push8(node);
  Push8(prev);
  std::copy_n(encoded.begin(), length, memory_ + top_);
  top_ = static_cast<std::uint16_t>(top_ + length);
  newestNode_ = place;
  ++nodes_;
  return true;
}

void CssRules::LeaveOut(LeftOut why) noexcept {
  if (leftOut_ == LeftOut::kNo) {
    leftOut_ = why;
  }
}

bool CssRules::BeginSheet(std::uint8_t& sheet) noexcept {
  if (inSheet_ || sheets_ == kMaxSheets) {
    return false;
  }
  sheet = sheets_;
  sheet_ = sheets_;
  ++sheets_;
  inSheet_ = true;
  inRule_ = false;
  return true;
}

void CssRules::EndSheet() noexcept {
  if (inSheet_ && inRule_) {
    BackTo(ruleStart_);
  }
  inRule_ = false;
  inSheet_ = false;
}

CssRules::Place CssRules::Here() const noexcept {
  return {.top = top_, .names = names_, .nodes = nodes_, .sheets = sheets_, .rules = rules_, .selectors = selectors_};
}

void CssRules::BackTo(const Place& place) noexcept {
  if (place.top > top_ || place.top < kFirstPlace) {
    return;
  }
  // The names added since go, and the places of the rest close up at the
  // block's end, still in order. Of each name that stays, the selectors
  // added since are taken off the front of those looked for under it.
  std::size_t kept = 0;
  for (std::size_t i = names_; i > 0; --i) {
    const std::uint16_t name = Load(IndexAt(i - 1));
    if (name >= place.top) {
      continue;
    }
    std::uint16_t selector = Load(name);
    while (selector >= place.top) {
      selector = Older(selector);
    }
    Store(name, selector);
    ++kept;
    Store(size_ - (2 * kept), name);
  }
  names_ = static_cast<std::uint16_t>(kept);
  while (keyless_ >= place.top) {
    keyless_ = Older(keyless_);
  }
  while (newestNode_ >= place.top) {
    newestNode_ = Older(newestNode_);
  }
  top_ = place.top;
  nodes_ = place.nodes;
  sheets_ = place.sheets;
  rules_ = place.rules;
  selectors_ = place.selectors;
}

void CssRules::OnRuleBegin() noexcept {
  if (!inSheet_) {
    return;
  }
  inRule_ = true;
  leftOut_ = LeftOut::kNo;
  ruleStart_ = Here();
  ruleSelectors_ = 0;
  settings_ = {};
  if (!Fits(kRuleBytes)) {
    LeaveOut(LeftOut::kRoom);
    return;
  }
  rule_ = top_;
  Push16(0);
  Push8(sheet_);
}

void CssRules::OnSelector(const CssSelector& selector) noexcept {
  if (!inRule_ || leftOut_ != LeftOut::kNo || selector.compounds.empty() ||
      selector.compounds.size() > CssReader::kMaxCompounds) {
    return;
  }
  const std::span<const CssCompound> parts = selector.compounds;

  // The parts fall into runs joined by `+`, each run a row of elements
  // one after another inside the same element. A run to the left of the
  // last is one node, made of a node for each of its parts in turn. The
  // last run is the same but for its last part, which is the element that
  // is styled.
  std::array<std::uint8_t, CssReader::kMaxCompounds> kinds{};
  std::array<std::uint8_t, CssReader::kMaxCompounds> nodes{};
  std::size_t steps = 0;
  std::uint8_t before = kNoNode;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    std::uint8_t node = 0;
    if (!NodeFor(parts[i], before, node)) {
      return;
    }
    if (parts[i + 1].combinator == CssCombinator::kAdjacent) {
      before = node;
      continue;
    }
    // The run ends here: whatever is to its right is inside it.
    kinds[steps] = parts[i + 1].combinator == CssCombinator::kChild ? kParent : kAncestor;
    nodes[steps] = node;
    ++steps;
    before = kNoNode;
  }

  std::array<std::uint8_t, kMaxCompoundBytes>
      subject;  // NOLINT(cppcoreguidelines-pro-type-member-init): filled by Encode().
  const std::size_t length = Encode(parts.back(), subject);
  if (length == 0 || !Fits(kSelectorHead + length + (kStepBytes * steps))) {
    LeaveOut(LeftOut::kRoom);
    return;
  }

  // It is looked for under its id, or failing that its first class, or
  // failing that its element's name; and with none of them, among those
  // that every element is tried against.
  const std::uint8_t what = subject[0];
  const std::size_t classes = subject[1] & 0x0FU;
  std::size_t key = 0;
  if ((what & kHasId) != 0 || classes != 0) {
    key = (what & kHasElement) != 0 ? 4 : 2;  // The id, or the first class, comes after the element's name.
  } else if ((what & kHasElement) != 0) {
    key = 2;
  }
  const std::uint16_t name = key == 0 ? std::uint16_t{0}
                                      : static_cast<std::uint16_t>(static_cast<unsigned>(subject[key]) |
                                                                   (static_cast<unsigned>(subject[key + 1]) << 8U));

  const std::uint32_t rank = selector.specificity;
  const std::uint16_t place = top_;
  Push16(name == 0 ? keyless_ : Load(name));
  Push16(rule_);
  Push16(static_cast<std::uint16_t>((std::min<std::uint32_t>(rank >> 16U, 15) << 12U) |
                                    (std::min<std::uint32_t>((rank >> 8U) & 0xFFU, 255) << 4U) |
                                    std::min<std::uint32_t>(rank & 0xFFU, 15)));
  Push8(static_cast<std::uint8_t>((selector.firstLetter ? kFirstLetter : 0U) | (steps << kStepsShift)));
  Push8(before);
  std::copy_n(subject.begin(), length, memory_ + top_);
  top_ = static_cast<std::uint16_t>(top_ + length);
  // The steps go outward from the element: the last run found is the
  // first step.
  for (std::size_t i = steps; i > 0; --i) {
    Push8(kinds[i - 1]);
    Push8(nodes[i - 1]);
  }
  if (name == 0) {
    keyless_ = place;
  } else {
    Store(name, place);
  }
  ++ruleSelectors_;
  ++selectors_;
}

void CssRules::OnDeclaration(const CssDeclaration& declaration) noexcept {
  const auto property = static_cast<std::size_t>(declaration.property);
  if (!inRule_ || leftOut_ != LeftOut::kNo || property >= kCssProperties) {
    return;
  }
  Setting& setting = settings_[property];
  if (setting.set && setting.important && !declaration.important) {
    return;  // What is important stays, whatever is written after it.
  }
  std::uint16_t families = 0;
  if (declaration.value.kind == CssValue::Kind::kFamilies && !declaration.value.families.empty()) {
    families = Intern(declaration.value.families);
    if (families == 0) {
      LeaveOut(LeftOut::kRoom);
      return;
    }
  }
  setting = {.set = true,
             .important = declaration.important,
             .kind = declaration.value.kind,
             .keyword = declaration.value.keyword,
             .unit = declaration.value.unit,
             .families = families,
             .number = declaration.value.number};
}

void CssRules::OnRuleEnd() noexcept {
  if (!inRule_) {
    return;
  }
  inRule_ = false;
  // How many properties it sets, and what keeping them takes: for each
  // its property, its form and its value.
  std::size_t set = 0;
  std::size_t bytes = 1;
  for (const Setting& setting : settings_) {
    if (!setting.set) {
      continue;
    }
    ++set;
    bytes += 2;
    if (setting.kind == CssValue::Kind::kKeyword) {
      bytes += 1;
    } else if (setting.kind == CssValue::Kind::kFamilies) {
      bytes += 2;
    } else if (setting.number != 0) {
      bytes += setting.number >= -32768 && setting.number <= 32767 ? 2 : 4;
    }
  }
  if (leftOut_ == LeftOut::kNo && ruleSelectors_ != 0 && set != 0 && !Fits(bytes)) {
    LeaveOut(LeftOut::kRoom);
  }
  if (leftOut_ != LeftOut::kNo || ruleSelectors_ == 0 || set == 0) {
    // A rule with no selector that could be kept, or that sets nothing,
    // is not kept, and is not counted as left out either.
    if (leftOut_ == LeftOut::kRoom) {
      ++leftOutForRoom_;
      cssRulesLeftOut.Add();
    } else if (leftOut_ == LeftOut::kNodes) {
      ++leftOutForNodes_;
      cssRulesLeftOut.Add();
    }
    BackTo(ruleStart_);
    return;
  }
  Store(rule_, top_);
  Push8(static_cast<std::uint8_t>(set));
  for (std::size_t property = 0; property < kCssProperties; ++property) {
    const Setting& setting = settings_[property];
    if (!setting.set) {
      continue;
    }
    Push8(static_cast<std::uint8_t>(property));
    const std::uint8_t form =
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(setting.kind) | (setting.important ? kImportant : 0U));
    switch (setting.kind) {
      case CssValue::Kind::kKeyword:
        Push8(form);
        Push8(static_cast<std::uint8_t>(setting.keyword));
        break;
      case CssValue::Kind::kFamilies:
        Push8(form);
        Push16(setting.families);
        break;
      case CssValue::Kind::kLength: {
        const std::uint8_t unit = static_cast<std::uint8_t>(static_cast<std::uint8_t>(setting.unit) << kUnitShift);
        const auto bits = static_cast<std::uint32_t>(setting.number);
        if (setting.number == 0) {
          Push8(form | unit);
        } else if (setting.number >= -32768 && setting.number <= 32767) {
          Push8(static_cast<std::uint8_t>(static_cast<unsigned>(form) | unit | (1U << kSizeShift)));
          Push16(static_cast<std::uint16_t>(bits & 0xFFFFU));
        } else {
          Push8(static_cast<std::uint8_t>(static_cast<unsigned>(form) | unit | (2U << kSizeShift)));
          Push16(static_cast<std::uint16_t>(bits & 0xFFFFU));
          Push16(static_cast<std::uint16_t>(bits >> 16U));
        }
        break;
      }
    }
  }
  ++rules_;
  cssRulesKept.Add();
}

void CssRules::OnImport(std::string_view /*url*/) noexcept {}

void CssRules::OnDropped(CssDropped /*dropped*/, std::string_view /*text*/) noexcept {}

// ---- What they come to ----

CssCascade::CssCascade(const CssRules& rules, std::span<std::uint8_t> memory) noexcept
    : rules_(rules), memory_(memory) {}

void CssCascade::Begin(std::span<const std::uint8_t> sheets) noexcept {
  order_ = {};
  std::uint8_t place = 0;
  for (const std::uint8_t sheet : sheets) {
    // A sheet given twice counts where it was given last, as it would if
    // it had been read twice. There are no more places than sheets.
    if (sheet < rules_.Sheets() && place < CssRules::kMaxSheets) {
      order_[sheet] = ++place;
    }
  }
  top_ = rules_.top_;
  nodes_ = rules_.nodes_;
  maskBytes_ = (nodes_ + 7) / 8;
  levels_ = maskBytes_ == 0 ? SIZE_MAX : memory_.size() / (2 * maskBytes_);
  depth_ = 0;
  tooDeep_ = 0;
  tooManyClasses_ = 0;
  work_ = 0;
  mostWork_ = 0;
}

std::span<std::uint8_t> CssCascade::Self(std::size_t level) noexcept {
  return memory_.subspan(level * 2 * maskBytes_, maskBytes_);
}

std::span<std::uint8_t> CssCascade::LastChild(std::size_t level) noexcept {
  return memory_.subspan((level * 2 * maskBytes_) + maskBytes_, maskBytes_);
}

bool CssCascade::Asks(std::size_t at, const XmlAttributes& attributes) noexcept {
  const std::string_view name = rules_.NameAt(rules_.Load(at));
  const std::string_view asked = rules_.NameAt(rules_.Load(at + 2));
  const std::uint8_t how = rules_.memory_[at + 4];
  const auto test = static_cast<CssAttributeTest>(how & kTestMask);
  const bool anyPrefix = (how & kAnyPrefix) != 0;
  for (std::size_t i = 0; i < attributes.Count(); ++i) {
    ++work_;
    const std::string_view written = attributes.Name(i);
    if ((SameButForCase(written, name) || (anyPrefix && SameButForCase(XmlLocalName(written), name))) &&
        Holds(test, attributes.Value(i), asked, (how & kAnyCase) != 0)) {
      return true;
    }
  }
  return false;
}

bool CssCascade::Matches(std::size_t at, const Element& element, std::size_t& length) noexcept {
  const std::uint8_t what = rules_.memory_[at];
  const std::uint8_t counts = rules_.memory_[at + 1];
  length = CompoundBytes(what, counts);
  std::size_t next = at + 2;
  if ((what & kHasElement) != 0) {
    if (rules_.Load(next) != element.name) {
      return false;
    }
    next += 2;
  }
  if ((what & kHasId) != 0) {
    if (rules_.Load(next) != element.id) {
      return false;
    }
    next += 2;
  }
  const std::span<const std::uint16_t> has(element.classes.data(), element.classCount);
  for (std::size_t i = 0; i < (counts & 0x0FU); ++i, next += 2) {
    if (std::find(has.begin(), has.end(), rules_.Load(next)) == has.end()) {
      return false;
    }
  }
  for (std::size_t i = 0; i < (counts >> 4U); ++i, next += kAttributeBytes) {
    if (!Asks(next, *element.attributes)) {
      return false;
    }
  }
  return true;
}

bool CssCascade::StepsMatch(std::size_t at, std::size_t count) noexcept {
  // The steps go outward from the element that is opening, which is not
  // yet among the open ones: the innermost of those is its parent.
  //
  // A step that asks for the element straight outside leaves no choice.
  // One that asks for an element at any depth is given the nearest that
  // will do, for itself and for the steps after it that leave no choice:
  // nearer leaves more above it for whatever comes after, so if the
  // nearest will not do, none will.
  std::ptrdiff_t level = static_cast<std::ptrdiff_t>(depth_) - 1;
  std::size_t step = 0;
  while (step < count) {
    std::size_t end = step + 1;
    while (end < count && rules_.memory_[at + (kStepBytes * end)] == kParent) {
      ++end;
    }
    const auto run = static_cast<std::ptrdiff_t>(end - step);
    const auto holdsAt = [this, at, step, end](std::ptrdiff_t innermost) {
      for (std::size_t i = step; i < end; ++i) {
        ++work_;
        const std::ptrdiff_t here = innermost - static_cast<std::ptrdiff_t>(i - step);
        if (!TestBit(Self(static_cast<std::size_t>(here)), rules_.memory_[at + (kStepBytes * i) + 1])) {
          return false;
        }
      }
      return true;
    };
    if (rules_.memory_[at + (kStepBytes * step)] == kParent) {
      if (level + 1 < run || !holdsAt(level)) {
        return false;
      }
    } else {
      while (level + 1 >= run && !holdsAt(level)) {
        --level;
      }
      if (level + 1 < run) {
        return false;
      }
    }
    level -= run;
    step = end;
  }
  return true;
}

void CssCascade::Apply(std::uint16_t declarations, Rank rank, CssStyle& style, Ranks& ranks) const noexcept {
  const std::uint8_t* memory = rules_.memory_;
  std::size_t at = declarations;
  const std::size_t count = memory[at++];
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t property = memory[at++];
    const std::uint8_t form = memory[at++];
    CssValue value;
    value.kind = static_cast<CssValue::Kind>(form & kKindMask);
    switch (value.kind) {
      case CssValue::Kind::kKeyword:
        value.keyword = static_cast<CssKeyword>(memory[at++]);
        break;
      case CssValue::Kind::kFamilies:
        value.families = rules_.NameAt(rules_.Load(at));
        at += 2;
        break;
      case CssValue::Kind::kLength: {
        value.unit = static_cast<CssUnit>((static_cast<unsigned>(form) >> kUnitShift) & kUnitMask);
        const unsigned size = form >> kSizeShift;
        if (size == 1) {
          value.number = static_cast<std::int16_t>(rules_.Load(at));
          at += 2;
        } else if (size == 2) {
          value.number =
              static_cast<std::int32_t>(rules_.Load(at) | (static_cast<std::uint32_t>(rules_.Load(at + 2)) << 16U));
          at += 4;
        }
        break;
      }
    }
    if (property >= kCssProperties) {
      continue;
    }
    const Rank mine = rank | ((form & kImportant) != 0 ? Rank{1} << kRankImportant : Rank{0});
    if (!style.Has(static_cast<CssProperty>(property)) || mine >= ranks.of[property]) {
      style.values[property] = value;
      style.set |= 1U << property;
      ranks.of[property] = mine;
    }
  }
}

void CssCascade::Try(std::uint16_t chain, const Element& element, CssStyle& style, Ranks& ranks, CssStyle& firstLetter,
                     Ranks& firstLetterRanks) noexcept {
  const std::uint8_t* memory = rules_.memory_;
  const bool parentKept = depth_ > 0 && depth_ - 1 < levels_;
  for (std::uint16_t place = chain; place != 0; place = rules_.Older(place)) {
    if (place >= top_) {
      continue;  // Added since the document began.
    }
    ++work_;
    const std::uint16_t rule = rules_.Load(place + 2U);
    const std::uint16_t declarations = rules_.Load(rule);
    const std::uint8_t sheet = memory[rule + 2U];
    if (declarations == 0 || sheet >= order_.size() || order_[sheet] == 0) {
      continue;
    }
    std::size_t length = 0;
    if (!Matches(place + kSelectorHead, element, length)) {
      continue;
    }
    const std::uint8_t shape = memory[place + 6U];
    const std::uint8_t before = memory[place + 7U];
    const std::size_t steps = shape >> kStepsShift;
    if (before != kNoNode && !(parentKept && TestBit(LastChild(depth_ - 1), before))) {
      continue;
    }
    if (steps != 0 && !(parentKept && StepsMatch(place + kSelectorHead + length, steps))) {
      continue;
    }
    const Rank rank = (Rank{rules_.Load(place + 4U)} << kRankSelector) | (Rank{order_[sheet]} << kRankSheet) | rule;
    work_ += memory[declarations];
    if ((shape & kFirstLetter) != 0) {
      Apply(declarations, rank, firstLetter, firstLetterRanks);
    } else {
      Apply(declarations, rank, style, ranks);
    }
  }
}

void CssCascade::Open(std::string_view name, const XmlAttributes& attributes, CssStyle& style,
                      CssStyle& firstLetter) noexcept {
  style.set = 0;
  firstLetter.set = 0;
  work_ = 0;

  // The element's name, its id and its classes, as the places of those
  // names in the table: one the table has not got can be in no rule.
  Element element;
  element.attributes = &attributes;
  std::uint32_t probes = 0;
  std::size_t index = 0;
  element.name = rules_.Find(name, true, index, &probes);
  std::string_view value;
  if (attributes.Find("id", value)) {
    element.id = rules_.Find(value, false, index, &probes);
  }
  if (attributes.Find("class", value)) {
    std::size_t words = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= value.size(); ++i) {
      if (i != value.size() && !IsSpace(value[i])) {
        continue;
      }
      if (i != start) {
        if (words == kMaxClasses) {
          ++tooManyClasses_;
          break;
        }
        ++words;
        const std::uint16_t place = rules_.Find(Piece(value, start, i - start), false, index, &probes);
        if (place != 0) {
          element.classes[element.classCount++] = place;
        }
      }
      start = i + 1;
    }
  }
  work_ += probes;

  // Which nodes it is: each one whose compound is true of it, after the
  // node that the element before it had to be.
  const bool kept = depth_ < levels_;
  const bool parentKept = depth_ > 0 && depth_ - 1 < levels_;
  if (kept && maskBytes_ != 0) {
    const std::span<std::uint8_t> self = Self(depth_);
    std::fill(self.begin(), self.end(), std::uint8_t{0});
    const std::span<std::uint8_t> last = LastChild(depth_);
    std::fill(last.begin(), last.end(), std::uint8_t{0});
    for (std::uint16_t place = rules_.newestNode_; place != 0; place = rules_.Older(place)) {
      const std::size_t number = rules_.memory_[place + 2U];
      if (place >= top_ || number >= nodes_) {
        continue;  // Added since the document began.
      }
      ++work_;
      const std::uint8_t before = rules_.memory_[place + 3U];
      std::size_t length = 0;
      if (Matches(place + kNodeHead, element, length) &&
          (before == kNoNode || (parentKept && TestBit(LastChild(depth_ - 1), before)))) {
        self[number / 8] = static_cast<std::uint8_t>(self[number / 8] | (1U << (number % 8)));
      }
    }
  } else if (!kept) {
    ++tooDeep_;
  }

  // The selectors that could be for it: those kept under its id, each of
  // its classes and its name, each of those names once, and those kept
  // under none.
  Ranks ranks;
  Ranks firstLetterRanks;
  std::array<std::uint16_t, kMaxClasses + 2> under{};
  std::size_t underCount = 0;
  const auto add = [&under, &underCount](std::uint16_t place) {
    const std::span<const std::uint16_t> seen(under.data(), underCount);
    if (place != 0 && std::find(seen.begin(), seen.end(), place) == seen.end()) {
      under[underCount++] = place;
    }
  };
  add(element.id);
  for (std::size_t i = 0; i < element.classCount; ++i) {
    add(element.classes[i]);
  }
  add(element.name);
  for (std::size_t i = 0; i < underCount; ++i) {
    Try(rules_.Load(under[i]), element, style, ranks, firstLetter, firstLetterRanks);
  }
  Try(rules_.keyless_, element, style, ranks, firstLetter, firstLetterRanks);

  // Its own `style` attribute, over all of those but what is important.
  if (attributes.Find("style", value)) {
    class Own final : public CssHandler {
     public:
      Own(CssStyle& style, Ranks& ranks) noexcept : style_(style), ranks_(ranks) {}
      void OnRuleBegin() noexcept override {}
      void OnSelector(const CssSelector& /*selector*/) noexcept override {}
      void OnDeclaration(const CssDeclaration& declaration) noexcept override {
        const auto property = static_cast<std::size_t>(declaration.property);
        if (property >= kCssProperties) {
          return;
        }
        const Rank mine = (declaration.important ? Rank{1} << kRankImportant : Rank{0}) | (Rank{1} << kRankOwn);
        if (!style_.Has(declaration.property) || mine >= ranks_.of[property]) {
          style_.values[property] = declaration.value;
          style_.set |= 1U << property;
          ranks_.of[property] = mine;
        }
      }
      void OnRuleEnd() noexcept override {}
      void OnImport(std::string_view /*url*/) noexcept override {}
      void OnDropped(CssDropped /*dropped*/, std::string_view /*text*/) noexcept override {}

     private:
      CssStyle& style_;
      Ranks& ranks_;
    };
    Own own(style, ranks);
    ReadCssDeclarations(value, own);
  }

  ++depth_;
  mostWork_ = std::max(mostWork_, work_);
}

void CssCascade::Close() noexcept {
  if (depth_ == 0) {
    return;
  }
  --depth_;
  if (maskBytes_ == 0 || depth_ == 0 || depth_ >= levels_) {
    return;
  }
  // The element that closed is now the one before whatever opens next
  // inside its parent. One too deep to be kept leaves nothing there, and
  // nothing is what is there: no element at its depth was kept before it,
  // and its parent began with nothing.
  const std::span<std::uint8_t> self = Self(depth_);
  std::copy(self.begin(), self.end(), LastChild(depth_ - 1).begin());
}

}  // namespace bn::epub
