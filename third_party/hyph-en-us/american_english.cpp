#include "hyphenation/american_english.h"

#include <cstdint>

namespace bn::hyphenation {
namespace {

// The table bn-hyphen made, as the build wrote it out (bytes.cmake).
constexpr std::uint8_t kTable[] = {
#include "american_english_bytes.inc"
};

// From the header of hyph-en-us.tex, which the patterns come from. Both are
// in what AmericanEnglish() gives, so that a program with the table has the
// notice too.
constexpr char kNotice[] =
    "Hyphenation patterns for American English. Copyright (C) 1990, 2004, 2005 Gerard D.C. Kuiken. "
    "Copying and distribution of this file, with or without modification, are permitted in any medium without "
    "royalty provided the copyright notice and this notice are preserved. "
    "They include the patterns and exceptions of Donald E. Knuth's hyphen.tex.";

}  // namespace

Patterns AmericanEnglish() noexcept { return Patterns{.table = kTable, .notice = kNotice}; }

}  // namespace bn::hyphenation
