#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "epub/css.h"
#include "epub/xml.h"

// A book's rules, kept; and what they come to for an element.
//
// CssRules keeps the rules that a CssReader tells of, from as many sheets
// as a book has, in one block of memory that the caller gives it. It is
// what a CssReader is handed as its handler.
//
// CssCascade goes through a document beside its parser. Told of each
// element as it opens and closes, it says what the rules set for that
// element: for each property, the value of the declaration that wins by
// CSS's own ordering. A document is given the sheets it asks for, in the
// order it asks for them, and the rules of every other sheet in the table
// are passed over.
//
// What wins, for one property of one element, the first of these that
// differs deciding:
//
//   1. a declaration marked `!important` over one that is not;
//   2. the element's own `style` attribute over any rule;
//   3. the selector that ranks higher (CssSelector::specificity);
//   4. the sheet that comes later in the document's list;
//   5. what was written later in the sheet.
//
// What is set is what the sheets say, and no more is made of it here: a
// value of `inherit`, an em, a size that is "larger" are given as they
// were written. What an element takes from the one it is inside is for
// whoever keeps the styles of the open elements.
//
// Nothing is kept of a document but, for each element that is open, a few
// bytes that say which of the rules' selectors it and the element before
// it could be part of (see CssCascade). So a selector may ask anything of
// an element's ancestors, and of the elements before it, however many:
// "hgroup > h2 + p + p".

namespace bn::epub {

// What the rules set for one element: for each property that something
// set, the value that won.
struct CssStyle {
  // In CssProperty's order. An entry means something only if Has() says
  // the property was set.
  std::array<CssValue, kCssProperties> values{};
  // A bit for each property that was set, the first property's the lowest.
  std::uint32_t set = 0;

  bool Has(CssProperty property) const noexcept { return ((set >> static_cast<unsigned>(property)) & 1U) != 0; }
  const CssValue& operator[](CssProperty property) const noexcept { return values[static_cast<std::size_t>(property)]; }
};

// The rules of a book's sheets.
//
// A sheet is added by BeginSheet(), then by telling this of its rules,
// which is what CssReader::Read(source, rules) does, then by EndSheet().
// What it is told of an `@import`, or of something passed over, it does
// nothing with.
//
// Everything is kept in the caller's block, each name once: about 12 bytes
// for a selector, 4 for a declaration, and a name's own length and 6 for
// each different name. Of a rule that sets one property more than once,
// only the declaration that would win is kept. In 520 books that comes to
// about 48 bytes a rule, 4 KB for the rules of a book at the median and
// 24 KB for the book with most.
//
// A rule there is no room for is left out whole, and counted; the rules
// before it are as they were. So is a rule that would take the table past
// kMaxNodes (below).
//
// Used by one task at a time.
class CssRules final : public CssHandler {
 public:
  // The most sheets a table holds. A book of 520 had 18.
  static constexpr std::size_t kMaxSheets = 64;
  // The most different things that selectors can ask of the elements
  // around the one they style: of an ancestor, or of an element before
  // it. "div.note" in "div.note > p" and in "div.note p + p" is one such
  // thing, and "p" in the second another. Each costs a CssCascade a bit
  // for every open element, twice. A book of 520 had 141.
  static constexpr std::size_t kMaxNodes = 255;
  // The most of the block that is used: places in it are kept in 16 bits.
  static constexpr std::size_t kMaxBytes = 65535;

  // `memory` must stay alive, and be left alone, as long as this is.
  explicit CssRules(std::span<std::uint8_t> memory) noexcept;

  CssRules(const CssRules&) = delete;
  CssRules& operator=(const CssRules&) = delete;
  ~CssRules() = default;

  // Forgets every sheet.
  void Clear() noexcept;

  // A sheet's rules follow. `sheet` is given the number it is to be known
  // by to CssCascade::Begin(). False, with nothing begun, if the table has
  // kMaxSheets already or a sheet has been begun and not ended.
  [[nodiscard]] bool BeginSheet(std::uint8_t& sheet) noexcept;
  // The sheet's rules have all been told. A rule that was still being told
  // is left out.
  void EndSheet() noexcept;

  // A place to go back to: BackTo() forgets every sheet and rule added
  // since Here() gave it. It is how a document's own rules, from its
  // `<style>` elements, are kept only as long as the document is being
  // read. Not between BeginSheet() and EndSheet(); and a place is no good
  // once the table has been taken back past it, or cleared: only a place
  // that Here() gave, for the table as it still stands below it, is to be
  // gone back to.
  struct Place {
    std::uint16_t top = 0;
    std::uint16_t names = 0;
    std::uint16_t nodes = 0;
    std::uint8_t sheets = 0;
    std::uint32_t rules = 0;
    std::uint32_t selectors = 0;
  };
  Place Here() const noexcept;
  void BackTo(const Place& place) noexcept;

  // What the table holds.
  std::size_t Sheets() const noexcept { return sheets_; }
  std::uint32_t Rules() const noexcept { return rules_; }
  std::uint32_t Selectors() const noexcept { return selectors_; }
  std::size_t Names() const noexcept { return names_; }
  std::size_t Nodes() const noexcept { return nodes_; }
  // How much of the block is in use, and how much there is.
  std::size_t Bytes() const noexcept;
  std::size_t Capacity() const noexcept { return size_; }
  // Rules left out since the table was made or cleared: for want of room,
  // and for want of nodes.
  std::uint32_t LeftOutForRoom() const noexcept { return leftOutForRoom_; }
  std::uint32_t LeftOutForNodes() const noexcept { return leftOutForNodes_; }

  // What a CssReader tells.
  void OnRuleBegin() noexcept override;
  void OnSelector(const CssSelector& selector) noexcept override;
  void OnDeclaration(const CssDeclaration& declaration) noexcept override;
  void OnRuleEnd() noexcept override;
  void OnImport(std::string_view url) noexcept override;
  void OnDropped(CssDropped dropped, std::string_view text) noexcept override;

 private:
  friend class CssCascade;

  // Why the rule being told is to be left out, if it is.
  enum class LeftOut : std::uint8_t { kNo, kRoom, kNodes };

  std::uint16_t Load(std::size_t at) const noexcept;
  // The place kept at `place`, which is of something added before what is
  // there, and so lower; 0 if it is not lower. Going from each thing to
  // the one before it this way always comes to an end.
  std::uint16_t Older(std::uint16_t place) const noexcept;
  void Store(std::size_t at, std::uint16_t value) noexcept;
  // Whether there is room for `bytes` more.
  bool Fits(std::size_t bytes) const noexcept;
  void Push8(std::uint8_t value) noexcept;
  void Push16(std::uint16_t value) noexcept;

  // The place of the `index`-th name in order, and where that place is
  // kept.
  std::size_t IndexAt(std::size_t index) const noexcept;
  std::string_view NameAt(std::uint16_t name) const noexcept;
  // The name's place, or 0 if the table does not have it. With `lowered`,
  // capitals in `text` are taken as the small letters the table has.
  std::uint16_t Find(std::string_view text, bool lowered, std::size_t& index, std::uint32_t* probes) const noexcept;
  // The name's place, adding it if it is new. 0 if there is no room.
  std::uint16_t Intern(std::string_view text) noexcept;

  // Writes `compound` at `out`, which has room for kMaxCompoundBytes, as
  // it is kept, and returns its length; 0 if there is no room for one of
  // its names.
  std::size_t Encode(const CssCompound& compound, std::span<std::uint8_t> out) noexcept;
  // The number of the node for `compound` after the node `prev` (kNoNode
  // for none), adding it if it is new. False if there is no room, or no
  // number left: `leftOut_` says which.
  bool NodeFor(const CssCompound& compound, std::uint8_t prev, std::uint8_t& node) noexcept;
  void LeaveOut(LeftOut why) noexcept;

  std::uint8_t* memory_ = nullptr;
  std::size_t size_ = 0;     // Of the block, as much as is used.
  std::uint16_t top_ = 0;    // Where the next thing kept goes.
  std::uint16_t names_ = 0;  // How many names, whose places are kept in order at the block's end.
  std::uint16_t nodes_ = 0;
  std::uint16_t newestNode_ = 0;  // The place of the node added last; each has the place of the one before.
  std::uint16_t keyless_ = 0;     // The newest selector that styles an element whatever its name, id and classes.
  std::uint8_t sheets_ = 0;
  std::uint32_t rules_ = 0;
  std::uint32_t selectors_ = 0;
  std::uint32_t leftOutForRoom_ = 0;
  std::uint32_t leftOutForNodes_ = 0;

  // The sheet and the rule that are being told.
  bool inSheet_ = false;
  bool inRule_ = false;
  LeftOut leftOut_ = LeftOut::kNo;
  Place ruleStart_;
  std::uint8_t sheet_ = 0;
  std::uint16_t rule_ = 0;  // The place of the rule's own record.
  std::uint16_t ruleSelectors_ = 0;
  // What the rule has set so far: for each property, the declaration that
  // would win of those told.
  struct Setting {
    bool set = false;
    bool important = false;
    CssValue::Kind kind = CssValue::Kind::kKeyword;
    CssKeyword keyword = CssKeyword::kNone;
    CssUnit unit = CssUnit::kNumber;
    CssFamilyClass familyClass = CssFamilyClass::kUnset;
    std::int32_t number = 0;
  };
  std::array<Setting, kCssProperties> settings_{};
};

// What the rules come to for the elements of one document.
//
// Begin() once for a document, then Open() and Close() for each of its
// elements as its parser tells of them, in a document's own order.
//
// What it keeps of an open element is a bit for each of the table's nodes,
// twice: which of them the element is, and which the last element to
// close inside it was. That is LevelBytes() for each element that is
// open, out of the memory this was given. An element opened deeper than
// there is room for is still given the rules that ask nothing of the
// elements around it, which are most rules, and is counted.
//
// The rules must not be taken back past where they stood at Begin(), nor
// cleared, while the document is being gone through. Rules added since
// are not seen until the next Begin().
//
// Used by one task at a time.
class CssCascade final {
 public:
  // The most classes of one element that are looked at: those written
  // after them are as if they were not. An element of 520 books had 10.
  static constexpr std::size_t kMaxClasses = 16;

  // The most work one Open() can do, counted as Work() counts it: in a
  // table of so many names, nodes and selectors, for an element with so
  // many attributes, with so many elements open around it.
  //
  // The names of the element, its id and its classes are each looked for
  // among the table's, which halves what is left at each look. Every node
  // is then tried on the element, and every selector that could style it:
  // each asks at the most 8 things of the element's attributes, a
  // selector looks at each open element once for each of its parts, and
  // one that is true of the element sets each property once at the most.
  //
  // Reading the element's own `style` attribute is not counted: it takes
  // as long as the attribute is.
  static constexpr std::uint64_t MostWorkFor(std::size_t names, std::size_t nodes, std::size_t selectors,
                                             std::size_t attributes, std::size_t open) noexcept {
    std::uint64_t looks = 1;
    for (std::size_t left = names; left > 0; left /= 2) {
      ++looks;
    }
    const std::uint64_t asked = 1 + (CssReader::kMaxAttributes * static_cast<std::uint64_t>(attributes));
    return ((kMaxClasses + 2) * looks) + (nodes * asked) +
           (selectors * (asked + (CssReader::kMaxCompounds * static_cast<std::uint64_t>(open)) + kCssProperties));
  }

  // `memory` is for what is kept of the open elements, and must stay
  // alive, and be left alone, as long as this is. So must `rules`.
  CssCascade(const CssRules& rules, std::span<std::uint8_t> memory) noexcept;

  CssCascade(const CssCascade&) = delete;
  CssCascade& operator=(const CssCascade&) = delete;
  ~CssCascade() = default;

  // A document begins, with no element open. `sheets` are the sheets it
  // is given, by the numbers CssRules::BeginSheet() gave them: the rules
  // of the first apply first, and so lose to those of the later ones. A
  // number that is no sheet's is passed over.
  void Begin(std::span<const std::uint8_t> sheets) noexcept;

  // An element opens. `style` is made what the rules and the element's
  // own `style` attribute set for it, and `firstLetter` what the rules
  // set for its first letter ("p::first-letter"). Of a list of font
  // families the kind of face it comes to is given, and not the names.
  void Open(std::string_view name, const XmlAttributes& attributes, CssStyle& style, CssStyle& firstLetter) noexcept;
  // The element opened last, and not yet closed, closes.
  void Close() noexcept;

  // How many elements are open.
  std::size_t Depth() const noexcept { return depth_; }
  // What is kept of each open element, in bytes, with the rules as they
  // were at Begin(); and how many open elements there is room for.
  std::size_t LevelBytes() const noexcept { return 2 * maskBytes_; }
  std::size_t Levels() const noexcept { return levels_; }
  // Elements of this document that were opened deeper than that.
  std::uint32_t TooDeep() const noexcept { return tooDeep_; }
  // Elements of this document that had more classes than are looked at.
  std::uint32_t TooManyClasses() const noexcept { return tooManyClasses_; }

  // The work the last Open() did, and the most any has done for this
  // document: never more than MostWorkFor() says.
  std::uint64_t Work() const noexcept { return work_; }
  std::uint64_t MostWork() const noexcept { return mostWork_; }

 private:
  // What is known of the element that is opening.
  struct Element {
    std::uint16_t name =
        0;  // The places of its name and its id among the table's names; 0 if the table has not got them.
    std::uint16_t id = 0;
    std::array<std::uint16_t, kMaxClasses> classes{};
    std::size_t classCount = 0;
    const XmlAttributes* attributes = nullptr;
  };
  // What decides between two declarations of one property: the greater
  // wins.
  using Rank = std::uint64_t;
  struct Ranks {
    std::array<Rank, kCssProperties> of{};
  };

  std::span<std::uint8_t> Self(std::size_t level) noexcept;
  std::span<std::uint8_t> LastChild(std::size_t level) noexcept;
  // Whether the compound kept at `at` is true of the element; `length` is
  // given how many bytes it takes.
  bool Matches(std::size_t at, const Element& element, std::size_t& length) noexcept;
  bool Asks(std::size_t at, const XmlAttributes& attributes) noexcept;
  // Whether the parts of a selector that are about the elements around the
  // one it styles are true: `count` steps kept at `at`.
  bool StepsMatch(std::size_t at, std::size_t count) noexcept;
  void Try(std::uint16_t chain, const Element& element, CssStyle& style, Ranks& ranks, CssStyle& firstLetter,
           Ranks& firstLetterRanks) noexcept;
  void Apply(std::uint16_t declarations, Rank rank, CssStyle& style, Ranks& ranks) const noexcept;

  const CssRules& rules_;
  std::span<std::uint8_t> memory_;
  std::array<std::uint8_t, CssRules::kMaxSheets>
      order_{};                // Where each sheet comes in this document, from 1; 0 for a sheet it was not given.
  std::uint16_t top_ = 0;      // Where the rules stood at Begin(): nothing kept after it is looked at.
  std::size_t nodes_ = 0;      // How many nodes there were then.
  std::size_t maskBytes_ = 0;  // A bit for each of them, in so many bytes.
  std::size_t levels_ = 0;
  std::size_t depth_ = 0;
  std::uint32_t tooDeep_ = 0;
  std::uint32_t tooManyClasses_ = 0;
  std::uint64_t work_ = 0;
  std::uint64_t mostWork_ = 0;
};

}  // namespace bn::epub
