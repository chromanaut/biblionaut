#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// What bn-bundle does with a chapter of a bundle: prints one as text,
// writes one from the same text, and looks for a place in one.
//
// The text is a line for each thing a chapter is made of, in the order a
// document has them:
//
//   chapter STATE REASON SOURCE-BYTES     STATE is whole or cut-short
//   language TAG                          one for each the text names
//   size PICTURE WIDTH HEIGHT             one for each picture whose size is known
//   block KIND LEVEL SOURCE [FIELD...]    KIND is paragraph, heading, item,
//                                         picture, rule or cell, or a number
//   text TEXT
//   style BITS BOOK-BITS FAMILY LANGUAGE SIZE
//   link KIND CHAPTER [FRAGMENT]          KIND is inside, note or outside
//   endlink
//   picture NUMBER BITS W W W H H H [ALT] the three amounts of its width, and of its height
//   id NAME                               in a block, or between two
//   end                                   of a block
//
// A FIELD of a block's style is NAME=VALUES, with a star after it if the
// book's own style sheets gave it: align, direction, size, line, indent,
// left, right, before, after, breaking, depths, marker, table, width,
// height. Its values are numbers with commas between them, as the format
// has them. A star after a style's FAMILY says the book named the family.
//
// Text is as it is but for a byte under 0x20 and a backslash, which are
// written \xNN. A line that begins with # says something of the chapter
// that is not part of it, and an empty line nothing; neither is read.

namespace bn::tools {

// Text as it is printed: every byte that could confuse a reader of lines
// written out as its number.
std::string Printable(std::string_view text);

// Prints the chapter of that number of the bundle at `path`. Returns what
// the program is to return.
int PrintChapter(const std::string& path, std::size_t number);

// Writes the chapter that the text in the file at `textPath` describes
// into the bundle at `path`, as the chapter of that number, and commits
// it.
int WriteChapter(const std::string& path, std::size_t number, const std::string& textPath);

// Looks in the chapter of that number for a place: `what` is block,
// characters, source or id, and `value` the number or the name.
int FindInChapter(const std::string& path, std::size_t number, const std::string& what, const std::string& value);

}  // namespace bn::tools
