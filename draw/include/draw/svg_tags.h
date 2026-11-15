#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bn::draw {

// How reading the tags of a file ended.
enum class SvgTagStatus : std::uint8_t {
  kOk = 0,
  kMalformed,   // What follows a `<`, or an attribute's name, is not what XML has there.
  kUnbalanced,  // An element is closed that was never opened.
  kTruncated,   // The bytes ended inside a tag, or with elements still open.
};

// The name of a status, for diagnostics: "ok", "malformed", "unbalanced",
// "truncated".
const char* SvgTagStatusName(SvgTagStatus status) noexcept;

// One thing a file's tags say.
struct SvgTagToken {
  enum class Kind : std::uint8_t {
    kNone,       // Nothing yet: the bytes ran out, or reading has failed.
    kOpen,       // An element begins. `text` is its name.
    kAttribute,  // An attribute of it begins. `text` is its name.
    kValue,      // A piece of that attribute's value.
    kOpened,     // The element's attributes are over.
    kClose,      // An element ends. `text` is its name.
    kText,       // A piece of the text between tags.
  };

  Kind kind = Kind::kNone;
  // A name is whole, and is the reader's own copy. A piece of a value or
  // of text is as much as was to hand, and may be any length from one
  // byte up. Either is good until the reader is next called.
  std::string_view text;
  // With kOpened: the element is over as well, having been written `<a/>`.
  bool empty = false;
  // With kValue and kText: the piece is the last bytes of those just
  // taken, as they were written. Otherwise it is something they stood for,
  // such as the `&` of `&amp;`.
  bool ofInput = false;
};

// Reads the tags of an SVG file: as much of XML as a drawing needs, and
// no more memory than a name.
//
// expat, which reads a book's other XML, has to hold a whole tag at once.
// An SVG file can have a tag of megabytes: the outline of an engraving is
// one attribute, and so is a picture inside a cover. So this reader gives
// an attribute's value out in pieces as it arrives and keeps none of it.
// The bytes are given in pieces of any size, cut anywhere.
//
// It gives out one thing at a call: an element's name, an attribute's
// name, a piece of a value, the end of an element's attributes, an
// element's end, a piece of text. So whoever calls it decides how much is
// done at a time, and each byte is looked at once.
//
// What it reads of XML: elements and attributes, with either kind of
// quote; `&lt;`, `&gt;`, `&amp;`, `&quot;`, `&apos;` and characters by
// number, in values and in text; text marked as CDATA. Comments,
// processing instructions and a document type are passed over. What it
// does not do:
//
// - It does not check that an element is closed by its own name, only that
//   as many are closed as were opened.
// - A name longer than kLongestName is given as its first kLongestName
//   bytes.
// - An entity it does not know, which a document type can declare, stands
//   for nothing, and UnknownEntities() counts them. An `&` that begins no
//   entity is taken as itself.
// - A value is given as written: its line ends and tabs are not made into
//   spaces.
// - The bytes are taken to be UTF-8, or at least to have ASCII's tags.
//
// The bytes are not trusted. Whatever they are, reading ends with a
// status. The object is 76 bytes on the device.
class SvgTagReader {
 public:
  static constexpr std::size_t kLongestName = 40;

  // Reads from the front of `bytes` as far as the next thing there is to
  // tell, puts that in `token`, and returns how many bytes it took: the
  // rest are for the next call. If the bytes run out first it has taken
  // them all and `token` is kNone. Once reading has failed it takes
  // whatever it is given and tells nothing.
  [[nodiscard]] std::size_t Next(std::string_view bytes, SvgTagToken& token) noexcept;

  // Hands back the last `count` bytes of a piece just given out with
  // `ofInput` set, to be given again: for when only the front of the piece
  // could be used. They are then to be the first bytes of the next call.
  void GiveBack(std::size_t count) noexcept { offset_ -= static_cast<std::uint32_t>(count); }

  // Says that the bytes have ended.
  SvgTagStatus Finish() noexcept;

  SvgTagStatus Status() const noexcept { return status_; }
  // How many bytes had been read when reading failed.
  std::uint32_t ErrorOffset() const noexcept { return errorOffset_; }
  // How many elements are open.
  std::uint32_t Depth() const noexcept { return depth_; }
  std::uint32_t UnknownEntities() const noexcept { return unknownEntities_; }

 private:
  enum class State : std::uint8_t {
    kContent,             // Between tags.
    kLess,                // After a `<`.
    kOpenName,            // In the name of an element that begins.
    kInTag,               // Between an element's attributes.
    kEmptySlash,          // After the `/` of `<a/>`.
    kAttributeName,       //
    kAfterAttributeName,  // Before the `=`.
    kBeforeValue,         // After it, before the quote.
    kValue,               // Within the quotes.
    kEntity,              // After an `&`.
    kCloseName,           // In the name of an element that ends.
    kAfterCloseName,      // Before its `>`.
    kInstruction,         // In `<? ... ?>`.
    kBang,                // After `<!`, before it is known what follows.
    kComment,             // In `<!-- ... -->`.
    kCData,               // In `<![CDATA[ ... ]]>`.
    kDeclaration,         // In `<!DOCTYPE ... >`.
  };

  std::size_t Give(SvgTagToken::Kind kind, std::string_view text, bool ofInput, std::size_t taken,
                   SvgTagToken& token) noexcept;
  std::size_t Fail(SvgTagStatus status, std::size_t taken, std::size_t size) noexcept;
  void AddToName(char c) noexcept;
  std::string_view Name() const noexcept { return {name_.data(), nameSize_}; }
  // What the entity that has been read stands for, as bytes in `entity_`:
  // none if it is not one that is known.
  std::size_t Decoded() noexcept;

  State state_ = State::kContent;
  State back_ = State::kContent;  // Where an entity was met: in text or in a value.
  SvgTagStatus status_ = SvgTagStatus::kOk;
  char quote_ = 0;            // The quote a value began with, or the one open in a document type.
  std::uint8_t matched_ = 0;  // How much of what ends a comment, an instruction or CDATA has been seen.
  std::uint8_t nameSize_ = 0;
  std::uint8_t entitySize_ = 0;
  std::uint8_t nesting_ = 0;  // Square brackets open in a document type.
  std::array<char, kLongestName> name_{};
  std::array<char, 12> entity_{};  // An `&` and what follows it, up to the `;`.
  std::uint32_t depth_ = 0;
  std::uint32_t offset_ = 0;
  std::uint32_t errorOffset_ = 0;
  std::uint32_t unknownEntities_ = 0;
};

}  // namespace bn::draw
