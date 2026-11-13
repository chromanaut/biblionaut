#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/byte_source.h"
#include "core/region_heap.h"

// The parser itself is expat's (third_party/expat), and stays behind this
// header: nothing of expat's shows through.
struct XML_ParserStruct;

// Reading XML: the files an EPUB describes itself with, and its chapters.
//
// A document is read from a ByteSource a piece at a time and never held
// whole. What is found in it is reported, as it is found, to an XmlHandler.
//
// The document is not trusted. Whatever bytes it holds, parsing ends with
// an answer; what the parser may take in memory is fixed beforehand, and
// a document that needs more is refused.
//
// What is understood: XML 1.0 as expat reads it, in UTF-8, UTF-16,
// ISO-8859-1 or US-ASCII, with everything given out as UTF-8. Names are
// given as they are written, prefix and all ("dc:title"); namespaces are
// not worked out, which also means that a book which uses a prefix it
// never declared can still be read.
//
// What is not: entities are not expanded. The five that every XML document
// has (&amp; &lt; &gt; &quot; &apos;) and numbered characters (&#160;) come
// out as the text they stand for. Of the rest:
//
//   - one the document does not declare, such as &nbsp;, is reported by
//     name for the handler to make of what it can, if the document names a
//     DTD outside itself (as XHTML's `<!DOCTYPE html PUBLIC ...>` does). In
//     a document that names none, XML calls such a reference a mistake,
//     and parsing stops there;
//   - one the document declares for itself comes out as text, as the
//     reference was written: "&me;". Since nothing is ever expanded, a
//     document cannot be made that expands without end.

namespace bn::epub {

// How parsing a document ended, or stands.
enum class XmlStatus : std::uint8_t {
  kOk = 0,               // Nothing wrong so far; at the end, a whole document was read.
  kNotStarted,           // Begin() has not been called.
  kNotWellFormed,        // The document is not XML: ErrorCode() and Line() say what and where.
  kUnsupportedEncoding,  // It is in an encoding other than the four above.
  kNoMemory,             // It needs more memory than the parser was given.
  kSourceFailed,         // The source it was being read from failed.
  kStopped,              // Stop() was called.
};

// The name of a status, for diagnostics: "ok", "not well-formed", ...
const char* XmlStatusName(XmlStatus status) noexcept;

// A name without its prefix: "title" for "dc:title", and "title" for
// "title".
std::string_view XmlLocalName(std::string_view name) noexcept;

// The attributes of one start tag, in the order they were written. Only
// good while the handler that was given them is running.
class XmlAttributes {
 public:
  // `pairs`: name, value, name, value, ..., ended by a null. Null for none.
  explicit XmlAttributes(const char* const* pairs) noexcept;

  std::size_t Count() const noexcept { return count_; }
  // The name and the value of the `index`-th. `index` must be below Count().
  std::string_view Name(std::size_t index) const noexcept;
  std::string_view Value(std::size_t index) const noexcept;

  // The value of the attribute written with exactly this name. False,
  // leaving `value` alone, if there is none.
  bool Find(std::string_view name, std::string_view& value) const noexcept;

 private:
  const char* const* pairs_;
  std::size_t count_ = 0;
};

// What is told of a document as it is read. Text is UTF-8, and only good
// while the call is running.
class XmlHandler {
 public:
  virtual void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept = 0;
  virtual void OnEndElement(std::string_view name) noexcept = 0;

  // Text between tags. A run of text may come in any number of pieces,
  // cut anywhere but inside a character.
  virtual void OnText(std::string_view text) noexcept = 0;

  // A reference to an entity that was not expanded, such as &nbsp;: its
  // name, without the & and the ;. See the top of this file for when.
  virtual void OnEntity(std::string_view name) noexcept = 0;

 protected:
  ~XmlHandler() = default;  // Never destroyed through the interface.
};

// Parses one document at a time. It can be used for one after another.
//
// Everything it needs while parsing comes out of the memory it is given
// when it is made: nothing comes from the general heap. How much a
// document needs depends on its longest tag and on how deep its elements
// go. Memory().HighWaterSinceReset() says how much the document in hand
// has used, and Memory().HighWater() the most any has.
//
// Used by one task at a time.
class XmlParser {
 public:
  // How much of the document is read, and parsed, at each Step().
  static constexpr std::size_t kChunkBytes = 1024;

  // `memory` must stay alive, and be left alone, as long as the parser is.
  explicit XmlParser(std::span<std::uint8_t> memory) noexcept;

  XmlParser(const XmlParser&) = delete;
  XmlParser& operator=(const XmlParser&) = delete;
  ~XmlParser();

  // The salt for the parser's tables of names. With a salt that a
  // document's author cannot guess, a document cannot be written to make
  // those tables slow. Until one is set, a fixed one is used. Takes effect
  // at the next Begin().
  void SetSalt(const std::array<std::uint8_t, 16>& salt) noexcept { salt_ = salt; }

  // Gets ready to read a document from `source` and tell `handler` of it,
  // dropping whatever document was being read. Both must stay alive until
  // the document is done with. Reads nothing yet.
  [[nodiscard]] XmlStatus Begin(core::ByteSource& source, XmlHandler& handler) noexcept;

  // Reads the next piece of the document and tells the handler what is in
  // it. Returns true if there is more to do, false once the document has
  // ended or parsing has failed: Status() says which.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  [[nodiscard]] XmlStatus Parse(core::ByteSource& source, XmlHandler& handler) noexcept;

  // Ends the parsing, with kStopped. May be called from inside a handler,
  // which is then the last to be called.
  void Stop() noexcept;

  XmlStatus Status() const noexcept { return status_; }
  // The document was read to its end and is XML.
  bool Finished() const noexcept { return finished_; }

  // Where the parser has got to: for a document that failed, where. Lines
  // are counted from 1; the offset is in bytes from the document's start.
  std::uint32_t Line() const noexcept;
  std::uint32_t ByteOffset() const noexcept;
  // For kNotWellFormed and kUnsupportedEncoding, expat's number for what
  // was wrong (its XML_Error); 0 otherwise.
  int ErrorCode() const noexcept { return errorCode_; }

  // The parser's memory: how much it has used, and whether it ran out.
  const core::RegionHeap& Memory() const noexcept { return heap_; }

 private:
  void End() noexcept;
  bool Fail(XmlStatus status) noexcept;

  // What expat calls. `self` is the XmlParser.
  static void HandleStart(void* self, const char* name, const char** attributes) noexcept;
  static void HandleEnd(void* self, const char* name) noexcept;
  static void HandleText(void* self, const char* text, int length) noexcept;
  static void HandleEntity(void* self, const char* name, int isParameterEntity) noexcept;

  core::RegionHeap heap_;
  XML_ParserStruct* parser_ = nullptr;
  core::ByteSource* source_ = nullptr;
  XmlHandler* handler_ = nullptr;
  XmlStatus status_ = XmlStatus::kNotStarted;
  bool finished_ = false;
  bool inHandler_ = false;
  int errorCode_ = 0;
  std::uint32_t line_ = 0;
  std::uint32_t byteOffset_ = 0;
  std::array<std::uint8_t, 16> salt_;
};

}  // namespace bn::epub
