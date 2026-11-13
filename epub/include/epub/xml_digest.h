#pragma once

#include <cstdint>
#include <string_view>

#include "epub/xml.h"

// For checking the XML parser against itself somewhere else: on a device
// against the desktop, or against another parser altogether.

namespace bn::epub {

// What a parser made of a document, or of several one after another,
// boiled down to a count and a checksum. Give it to an XmlParser as its
// handler.
//
// Every event is taken as the line that `bn-xml events` prints for it:
//
//   (name          an element begins
//   Aname value    one of its attributes, in the order they were written
//   )name          an element ends
//   -text          text, all that comes between two other events as one line
//   &name          a reference to an entity that was not expanded
//
// with a backslash, a line end, a tab and a carriage return in text and
// values written as \\, \n, \t and \r. The checksum is the CRC-32 of those
// lines, each with its line end, and the count is the number of lines.
// Nothing is kept of them, so a document of any size costs no memory.
//
// Two parsers that give the same count and checksum for the same documents
// found the same things in them, in the same order.
class XmlEventDigest final : public XmlHandler {
 public:
  XmlEventDigest() noexcept = default;

  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override;
  void OnEndElement(std::string_view name) noexcept override;
  void OnText(std::string_view text) noexcept override;
  void OnEntity(std::string_view name) noexcept override;

  // Call when a document is over, however it ended: text may be waiting
  // for the end of its line. The count and the checksum then go on into
  // the next document, unless Reset() is called.
  void EndDocument() noexcept;

  // Starts the count and the checksum again from nothing.
  void Reset() noexcept;

  std::uint32_t Checksum() const noexcept { return checksum_; }
  std::uint32_t Events() const noexcept { return events_; }
  // How many bytes the lines came to: what has been checksummed.
  std::uint64_t Bytes() const noexcept { return bytes_; }

 private:
  void Add(std::string_view bytes) noexcept;
  void AddEscaped(std::string_view raw) noexcept;
  // Ends the line of text that is under way, if one is.
  void EndText() noexcept;

  std::uint32_t checksum_ = 0;
  std::uint32_t events_ = 0;
  std::uint64_t bytes_ = 0;
  bool inText_ = false;
};

// Whether a file of this name in an EPUB is taken for XML by the tools
// that check the parser: one ending in .xhtml, .html, .htm, .xml, .opf or
// .ncx, in capitals or not. (An importer goes by what the book's package
// file says each file is, not by its name.)
bool HasXmlFileEnding(std::string_view name) noexcept;

}  // namespace bn::epub
