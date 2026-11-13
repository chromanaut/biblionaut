#pragma once

#include <cstdint>

#include "epub/toc.h"

// For checking the table of contents reader against itself somewhere
// else: on a device against the desktop, or against another reading
// altogether.

namespace bn::epub {

// What a reader made of a table of contents, boiled down to a count and a
// checksum. Give it to a TocReader as its handler.
//
// Every entry is taken as a line of four parts with a tab between each:
//
//   how far in it is, from 0
//   the document it points into, by its place in the spine from 0, or "-"
//     for none
//   the place within the document, or "-" for its start
//   its label
//
// The checksum is the CRC-32 of those lines, each with its line end. They
// are the lines `bn-epub toc` prints for the entries, but for a byte in a
// label or a place that it writes out as a number. Nothing is kept of
// them.
//
// Two readings of a table of contents that come to the same count and
// checksum found the same entries, in the same order.
class TocDigest final : public TocHandler {
 public:
  TocDigest() noexcept = default;

  // Never asks for no more.
  bool OnEntry(const TocEntry& entry) noexcept override;

  // Starts the count and the checksum again from nothing.
  void Reset() noexcept;

  std::uint32_t Checksum() const noexcept { return checksum_; }
  std::uint32_t Entries() const noexcept { return entries_; }
  // How many levels the entries go to: 0 with no entries, 1 if none is
  // under another.
  std::uint32_t Levels() const noexcept { return levels_; }
  // How many of the entries point into no document.
  std::uint32_t WithoutDocument() const noexcept { return withoutDocument_; }

 private:
  std::uint32_t checksum_ = 0;
  std::uint32_t entries_ = 0;
  std::uint32_t levels_ = 0;
  std::uint32_t withoutDocument_ = 0;
};

}  // namespace bn::epub
