#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The characters that HTML's names stand for.
//
// A document of a book may write a character by a name HTML gives it:
// `&nbsp;`, `&mdash;`, `&eacute;`. XML knows five of them. The parser tells
// of the others by their names (xml.h), and this says which character each
// is.

namespace bn::epub {

// How many names are known: the 252 of HTML 4.01, and `apos`.
inline constexpr std::size_t kHtmlEntities = 253;

// The character the name stands for, as HTML has it: `nbsp` is U+00A0. The
// name is without its `&` and its `;`, and as it is written: `Aacute` and
// `aacute` are two characters. 0 for a name that is none of them.
std::uint32_t HtmlEntity(std::string_view name) noexcept;

}  // namespace bn::epub
