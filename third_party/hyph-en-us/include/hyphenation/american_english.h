#pragma once

#include <cstdint>
#include <span>
#include <string_view>

// TeX's hyphenation patterns for American English, as a table that
// layout/hyphenator.h reads where it lies.
//
// The table is made as part of the build, by bn-hyphen, from the patterns
// and the words written out with their breaks that the build downloads
// (third_party/hyph-en-us/CMakeLists.txt). It is compiled in as constant
// data: on the device it stays in flash and is read there, and nothing is
// copied into memory. It is about 37 KB, and is only in a program that
// asks for it.

namespace bn::hyphenation {

struct Patterns {
  // As layout::Hyphenator::Open() takes it. No break before two letters,
  // nor after three.
  std::span<const std::uint8_t> table;
  // The patterns' copyright line and notice, which go with every copy of
  // them, the table included.
  std::string_view notice;
};

Patterns AmericanEnglish() noexcept;

}  // namespace bn::hyphenation
