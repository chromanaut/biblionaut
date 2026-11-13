#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Finding a file of a book from the way another file of the book refers
// to it.

namespace bn::epub {

// The longest path a file in a book may have and still be found. Longer
// ones exist in principle; no book has been seen with one of 100 bytes.
inline constexpr std::size_t kMaxBookPath = 512;

// Works out which file of the book `reference` means: an `href` or `src`
// as written in a document of the book, such as "../images/a%20b.jpg#top".
// `base` is the directory that document is in, as a path from the top of
// the book ending in '/', or empty for the top itself.
//
// The answer, written to `path`, is the file's name as the ZIP file has
// it: from the top of the book, with no '/' in front. Getting there means
// dropping what follows a '#' or a '?', turning %XX into the byte it
// stands for, starting again from the top for a reference that begins
// with '/', and working out every "." and ".." (one that would climb out
// of the book stays at the top).
//
// Returns how long the path is, or 0 if the reference is not to a file of
// the book: it is empty, it is to somewhere else ("https://..."), or the
// path would be longer than `path` has room for.
std::size_t ResolveBookPath(std::string_view base, std::string_view reference, std::span<char> path) noexcept;

// The directory a file of the book is in, in the form ResolveBookPath()
// wants its `base`: "OEBPS/text/" for "OEBPS/text/ch1.xhtml", and nothing
// for a file at the top.
std::string_view BookDirectory(std::string_view path) noexcept;

// A number that stands for a path, or for any other name: the same for
// the same bytes, and in practice different for different ones. 48 bits
// of it are used, so that among the few thousand names a book can have
// there is about one chance in a hundred million of two sharing a number.
std::uint64_t NameHash(std::string_view name) noexcept;

}  // namespace bn::epub
