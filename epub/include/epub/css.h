#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/byte_source.h"

// Reading CSS: the style sheets of a book, the `<style>` elements of its
// documents and the `style` attributes of their elements.
//
// A sheet is read from a ByteSource a piece at a time and never held
// whole. What is found in it is told, as it is found, to a CssHandler: a
// rule's selectors, then each thing the rule sets.
//
// Only so much of CSS is read, and the rest is passed over and counted,
// by name, so that what a collection of books loses to that can be seen:
//
//   - Selectors by element, class, id and what an attribute is
//     ("[hidden]", "[epub|type~='chapter']"), joined by descent ("div p"),
//     as child ("div > p") or as the element straight after ("h2 + p"),
//     with `*` for any element and `:link` taken as always so.
//     `::first-letter` is read, for drop caps. A selector with anything
//     else in it (any other pseudo-class or pseudo-element, `~`, a
//     namespace before an element's name) is dropped, and a rule all of
//     whose selectors were dropped is dropped with them.
//   - The properties of CssProperty. `margin`, `padding`, `font`,
//     `page-break-*` and `break-*` are taken apart into those. Any other
//     property is dropped, as is a value that is not understood.
//   - `@media`: the rules in a block are read if the block is for `all`
//     or `screen`, or for "not" some other kind of device, and has no
//     condition in brackets. A reader's screen is not asked its size: what
//     is made of a book must not depend on which device made it.
//   - `@import` is told to the handler, which may go and read that sheet.
//     Every other at-rule is passed over, with its block if it has one.
//
// The sheet is not trusted, and nothing in it is a mistake that stops the
// reading: whatever its bytes are, reading goes on to the end as CSS's own
// rules for mistakes have it, dropping what cannot be made sense of. A
// selector list or a declaration longer than there is room for
// (kMaxPrelude, kMaxDeclaration) is dropped. Text is taken as UTF-8.
//
// It uses no memory but its own, about 2 KB.

namespace bn::epub {

// What a rule can set.
enum class CssProperty : std::uint8_t {
  kFontStyle,       // normal, italic, oblique
  kFontWeight,      // normal, bold, bolder, lighter, or a number from 1 to 1000
  kFontVariant,     // normal, small-caps
  kFontFamily,      // a list of names: see CssNextFamily()
  kFontSize,        // a length, or one of the sizes by name
  kLineHeight,      // normal, a length, or a plain number
  kTextAlign,       // left, right, center, justify, start, end
  kTextIndent,      // a length
  kTextDecoration,  // none, underline, line-through, overline
  kVerticalAlign,   // baseline, sub, super, top, bottom, middle, text-top, text-bottom, or a length
  kMarginTop,       // a length, or auto
  kMarginRight,
  kMarginBottom,
  kMarginLeft,
  kPaddingTop,  // a length
  kPaddingRight,
  kPaddingBottom,
  kPaddingLeft,
  kDisplay,      // block, inline, inline-block, list-item, none
  kFloat,        // left, right, none
  kBreakBefore,  // auto, always, avoid
  kBreakAfter,
  kBreakInside,  // auto, avoid
  kWidth,        // a length, or auto
  kHeight,
};

// The name of a property as CSS has it: "font-style", "margin-top", ...
const char* CssPropertyName(CssProperty property) noexcept;

// The words a value can be.
enum class CssKeyword : std::uint8_t {
  kNone = 0,
  kInherit,  // Also for `unset`, which for every property here that inherits is the same.
  kInitial,
  kNormal,
  kAuto,
  kItalic,
  kOblique,
  kBold,
  kBolder,
  kLighter,
  kSmallCaps,
  kLeft,
  kRight,
  kCenter,
  kJustify,
  kStart,
  kEnd,
  kUnderline,
  kLineThrough,
  kOverline,
  kBaseline,
  kSub,
  kSuper,
  kTop,
  kBottom,
  kMiddle,
  kTextTop,
  kTextBottom,
  kBlock,
  kInline,
  kInlineBlock,
  kListItem,
  kAlways,
  kAvoid,
  kXxSmall,
  kXSmall,
  kSmall,
  kMedium,
  kLarge,
  kXLarge,
  kXxLarge,
  kSmaller,
  kLarger,
};

// The word as CSS has it: "inherit", "small-caps", ... ("none" for kNone).
const char* CssKeywordName(CssKeyword keyword) noexcept;

// What a number in a value is a number of.
enum class CssUnit : std::uint8_t {
  kNumber,   // Nothing: a line height of 1.5, a weight of 700.
  kPx,       // CSS pixels. The absolute units (pt, in, cm, mm, pc, Q) are turned into these.
  kEm,       // The font's size. `ex` and `ch` are taken as half of one.
  kRem,      // The size of the document's own font.
  kPercent,  // Of whatever the property says a percentage is of.
};

// "", "px", "em", "rem" or "%".
const char* CssUnitName(CssUnit unit) noexcept;

// What a declaration sets a property to: a word, a length, or (for
// font-family) the list of names as it was written.
struct CssValue {
  enum class Kind : std::uint8_t { kKeyword, kLength, kFamilies };

  Kind kind = Kind::kKeyword;
  CssKeyword keyword = CssKeyword::kNone;  // For kKeyword.
  // For kLength: so many thousandths of `unit`. 1.5em is 1500 of kEm.
  // Never more than kCssMaxNumber either way.
  std::int32_t number = 0;
  CssUnit unit = CssUnit::kNumber;
  // For kFamilies: the names, as written, commas and quotes and all. Only
  // good while the handler that was given it is running.
  std::string_view families;
};

// The most a number in a value can be, in thousandths: a number further
// from zero than this is held to it.
inline constexpr std::int32_t kCssMaxNumber = 100'000'000;

// One thing a rule sets.
struct CssDeclaration {
  CssProperty property = CssProperty::kFontStyle;
  CssValue value;
  bool important = false;  // It was marked `!important`.
};

// How one part of a selector stands to the part before it.
enum class CssCombinator : std::uint8_t {
  kFirst,       // It is the first part.
  kDescendant,  // "a b": inside it, at any depth.
  kChild,       // "a > b": directly inside it.
  kAdjacent,    // "a + b": the element straight after it, inside the same one.
};

// What is asked of an attribute's value.
enum class CssAttributeTest : std::uint8_t {
  kPresent,   // "[a]": nothing; the element has the attribute.
  kEquals,    // "[a=v]": it is v.
  kWord,      // "[a~=v]": one of its words, parted by spaces, is v.
  kDash,      // "[a|=v]": it is v, or begins with v and a `-`.
  kBegins,    // "[a^=v]": it begins with v.
  kEnds,      // "[a$=v]": it ends with v.
  kContains,  // "[a*=v]": v is somewhere in it.
};

// "", "=", "~=", "|=", "^=", "$=" or "*=".
const char* CssAttributeTestName(CssAttributeTest test) noexcept;

// One thing a part of a selector asks of an attribute.
struct CssAttribute {
  // The attribute's name as a document writes it, in small letters. A
  // namespace is gone by its prefix, as XmlParser has it: `epub|type` in a
  // sheet is "epub:type" here. What the sheet's `@namespace` says the
  // prefix stands for is not looked at.
  std::string_view name;
  // What the value is held against, without its quotes and backslashes.
  // Empty for kPresent; and it may be empty for the others, where by CSS's
  // rules kWord, kBegins, kEnds and kContains then match nothing.
  std::string_view value;
  CssAttributeTest test = CssAttributeTest::kPresent;
  // "[*|type]": the name may have any prefix in a document, or none.
  bool anyPrefix = false;
  // "[a=v i]": capitals and small letters of English are taken as the same.
  bool anyCase = false;
};

// One part of a selector: what an element has to be to match it.
struct CssCompound {
  CssCombinator combinator = CssCombinator::kFirst;
  std::string_view element;                   // The element's name, in small letters; empty for any.
  std::string_view id;                        // Its id; empty for any.
  std::span<const std::string_view> classes;  // Classes it must have, all of them.
  std::span<const CssAttribute> attributes;   // What its attributes must be, all of them.
};

// One selector: its parts from the outermost to the one that is styled.
// Its text is only good while the handler that was given it is running.
struct CssSelector {
  std::span<const CssCompound> compounds;
  // It ends in `::first-letter`: what the rule sets is for the first
  // letter of the element, not the element.
  bool firstLetter = false;
  // How CSS ranks it against other selectors, greater winning: the count
  // of ids, then of classes (and attributes, and `:link`), then of element
  // names (and `::first-letter`), eight bits each.
  std::uint32_t specificity = 0;
};

// The kinds of thing that are passed over, for the count.
enum class CssDropped : std::uint8_t {
  kSelectorAttribute,      // A selector whose brackets are not a test of an attribute. The text is the selector.
  kSelectorPseudoClass,    // One with a pseudo-class that is not read. The text is the pseudo-class.
  kSelectorPseudoElement,  // One with a pseudo-element that is not read. The text is the pseudo-element.
  kSelectorCombinator,     // One with `~`. The text is the selector.
  kSelectorOther,          // Not a selector, or one with more parts, classes or attributes than there is room for.
  kRule,                   // A rule none of whose selectors could be read. No text.
  kProperty,               // A declaration of a property that is not read. The text is the property's name.
  kValue,                  // A value that was not understood. The text is the declaration.
  kDeclaration,            // Something where a declaration should be that is not one. The text is what it was.
  kAtRule,                 // An at-rule that is passed over. The text is its name, without the @.
  kMedia,                  // An `@media` block that is not for this kind of device. The text is what it is for.
  kTooLong,                // A selector list or a declaration longer than there is room for. The text is its start.
};
inline constexpr std::size_t kCssDroppedKinds = 12;

// "attribute selector", "property", ...
const char* CssDroppedName(CssDropped dropped) noexcept;

// What is told of a sheet as it is read.
class CssHandler {
 public:
  // A rule begins. Its selectors follow, each one that could be read, then
  // its declarations, then OnRuleEnd(). A rule with no selector that could
  // be read is not told of at all.
  virtual void OnRuleBegin() noexcept = 0;
  virtual void OnSelector(const CssSelector& selector) noexcept = 0;
  // One property set. A declaration of several at once (`margin`, `font`)
  // comes as one call for each. For a `style` attribute there is no rule:
  // only these.
  virtual void OnDeclaration(const CssDeclaration& declaration) noexcept = 0;
  virtual void OnRuleEnd() noexcept = 0;

  // `@import`: the sheet names another whose rules come before its own.
  // `url` is as written, to be worked out from where this sheet is.
  virtual void OnImport(std::string_view url) noexcept = 0;

  // Something was passed over. `text` is what, as the kind says, and may
  // be cut short.
  virtual void OnDropped(CssDropped dropped, std::string_view text) noexcept = 0;

 protected:
  ~CssHandler() = default;  // Never destroyed through the interface.
};

// How reading a sheet stands.
enum class CssStatus : std::uint8_t {
  kOk = 0,        // Nothing wrong so far; at the end, the sheet was read.
  kNotStarted,    // Begin() has not been called.
  kSourceFailed,  // The source it was being read from failed.
};

// "ok", "not started" or "source failed".
const char* CssStatusName(CssStatus status) noexcept;

// Reads one sheet at a time. It can be used for one after another.
//
// A step reads kChunkBytes from the source at the most, and tells the
// handler of what those bytes completed.
//
// Used by one task at a time.
class CssReader {
 public:
  // How much of the sheet is read at each Step().
  static constexpr std::size_t kChunkBytes = 1024;
  // The longest list of selectors in front of a rule, and the longest
  // declaration, that are read, in bytes. The longest seen in 520 books
  // are 649 and 179.
  static constexpr std::size_t kMaxPrelude = 1024;
  static constexpr std::size_t kMaxDeclaration = 512;
  // The most parts a selector can have, and the most classes and the most
  // tests of attributes among them. In 520 books five selectors have more
  // parts, each styling a table of contents; no part has more than 6
  // classes, and no selector asks more than 3 things of attributes.
  static constexpr std::size_t kMaxCompounds = 8;
  static constexpr std::size_t kMaxClasses = 12;
  static constexpr std::size_t kMaxAttributes = 8;

  CssReader() noexcept = default;

  CssReader(const CssReader&) = delete;
  CssReader& operator=(const CssReader&) = delete;
  ~CssReader() = default;

  // Gets ready to read a sheet from `source` and tell `handler` of it.
  // Both must stay alive until the sheet is done with. Reads nothing yet.
  void Begin(core::ByteSource& source, CssHandler& handler) noexcept;

  // Reads the next piece of the sheet. Returns true if there is more to
  // do, false once the sheet has ended or the source has failed: Status()
  // says which.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  CssStatus Read(core::ByteSource& source, CssHandler& handler) noexcept;

  CssStatus Status() const noexcept { return status_; }
  // The sheet was read to its end.
  bool Finished() const noexcept { return finished_; }

  // How many rules the handler was told of, and how many declarations in
  // them.
  std::uint32_t Rules() const noexcept { return rules_; }
  std::uint32_t Declarations() const noexcept { return declarations_; }
  // How many things of each kind were passed over.
  std::uint32_t Dropped(CssDropped dropped) const noexcept { return dropped_[static_cast<std::size_t>(dropped)]; }

 private:
  // Where in a sheet the reader is.
  enum class Place : std::uint8_t {
    kPrelude,       // Before a rule's `{`, or in an at-rule up to its `;` or `{`.
    kDeclarations,  // Inside a rule's block.
    kSkippedBlock,  // Inside a block that is being passed over.
  };

  void Take(char ch) noexcept;
  // A byte that is not part of a comment.
  void TakeText(char ch) noexcept;
  void TakePrelude(char ch) noexcept;
  void TakeDeclarations(char ch) noexcept;
  void TakeSkipped(char ch) noexcept;
  // The prelude has ended at a `{` (`block`), or at a `;` or the end of
  // the sheet.
  void EndPrelude(bool block) noexcept;
  // Whether the prelude gathered so far is an at-rule's.
  bool IsAtRule() noexcept;
  void EndAtRule(std::span<char> text, bool block) noexcept;
  void BeginRule(std::span<char> selectors) noexcept;
  void EndDeclaration() noexcept;
  void EndRule() noexcept;
  void SkipBlock() noexcept;
  void Drop(CssDropped dropped, std::string_view text) noexcept;
  // Adds a byte to the text being gathered, if there is room for it.
  void Keep(char ch) noexcept;
  void ClearText() noexcept;
  // The sheet has ended: whatever was open is closed.
  void End() noexcept;

  core::ByteSource* source_ = nullptr;
  CssHandler* handler_ = nullptr;
  CssStatus status_ = CssStatus::kNotStarted;
  bool finished_ = false;

  Place place_ = Place::kPrelude;
  bool inComment_ = false;
  bool slash_ = false;            // The last byte was a `/` that may be opening a comment: it has not been taken yet.
  bool star_ = false;             // In a comment, the last byte was a `*`.
  char quote_ = 0;                // The quote a string was opened with, while inside one.
  bool escaped_ = false;          // The last byte was a backslash that the next belongs to.
  bool ruleTold_ = false;         // The rule whose block is being read was told to the handler.
  std::uint32_t skipDepth_ = 0;   // How many blocks deep inside the block being passed over.
  std::uint32_t mediaDepth_ = 0;  // How many `@media` blocks that are being read are open.
  std::uint32_t parens_ = 0;      // How many round and square brackets are open in the text being gathered,
  std::uint32_t braces_ = 0;      // and, in a declaration, how many curly ones.

  std::uint32_t rules_ = 0;
  std::uint32_t declarations_ = 0;
  std::array<std::uint32_t, kCssDroppedKinds> dropped_{};

  // The text being gathered: a prelude, or one declaration.
  std::size_t length_ = 0;
  bool tooLong_ = false;
  std::array<char, kMaxPrelude> text_{};
};

// Reads the declarations of a `style` attribute, telling `handler` of
// each (OnDeclaration and OnDropped only).
void ReadCssDeclarations(std::string_view text, CssHandler& handler) noexcept;

// Whether an `@media` block, or an `@import`, that says it is for `query`
// is for a reader of books: see the top of this file. An empty query is.
bool CssMediaApplies(std::string_view query) noexcept;

// Takes the next name off the front of a font-family value: `rest` is
// moved past it, and the name is put in `name` without its quotes, in
// small letters, with each run of spaces made one. Returns the name; an
// empty one when there are no more. A name longer than `name` is cut.
std::string_view CssNextFamily(std::string_view& rest, std::span<char> name) noexcept;

}  // namespace bn::epub
