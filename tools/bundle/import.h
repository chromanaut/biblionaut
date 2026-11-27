#pragma once

#include <string>

// What bn-bundle does to import: the chapters of a book made into segments
// of its bundle.

namespace bn::tools {

// Imports the chapters of the EPUB at `bookPath` into the bundle at
// `bundlePath`, which was made of it: the one whose number `which` is, or
// every one if `which` is "all", each committed as it is done. Prints a
// line for each, and one for the memory importing was given. Returns what
// the program is to return.
int ImportChapters(const std::string& bookPath, const std::string& bundlePath, const std::string& which);

}  // namespace bn::tools
