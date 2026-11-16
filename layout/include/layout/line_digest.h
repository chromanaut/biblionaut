#pragma once

#include <cstdint>

#include "layout/paragraph_setter.h"

// A checksum of lines of text, as the setter gives them out: so that the
// lines a device set can be shown to be the desktop's without either
// carrying them all. `bn-lines --figures` prints it, and so does the
// device check for layout.
//
// Each line adds 19 bytes to a CRC-32 (core/checksum.h), little-endian:
// where it begins and ends, its width and its number in the paragraph
// (4 bytes each), how many places between words it has (2), and a byte of
// flags: 1 if a hyphen is shown, 2 if it is a last line, 4 if it is too
// wide. A paragraph's lines begin again at number 0, so where one
// paragraph ends and the next begins is in the checksum too.

namespace bn::layout {

class LineDigest final {
 public:
  void Add(const TextLine& line) noexcept;

  std::uint32_t Checksum() const noexcept { return crc_; }
  std::uint64_t Lines() const noexcept { return lines_; }

 private:
  std::uint32_t crc_ = 0;
  std::uint64_t lines_ = 0;
};

}  // namespace bn::layout
