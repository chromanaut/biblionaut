#pragma once

#include <cstdint>

#include "epub/package.h"

// For checking the package reader against itself somewhere else: on a
// device against the desktop, or against another reading altogether.

namespace bn::epub {

// Everything `package` holds of the book it last read, boiled down to a
// checksum: the CRC-32 of these lines, each with its line end.
//
//   title: A Tale of Two Cities      the title, the author and the language,
//   author: Charles Dickens          or "-" for one the book does not give
//   language: en
//   package: OEBPS/content.opf       the path of the package file
//   manifest: 44                     how many files the manifest lists
//   cover: jpeg 1024 30000 9a0b1c2d  its kind and its entry, or "cover: -"
//   nav: 2048 512 00c0ffee           its entry, or "nav: -"
//   ncx: 4096 900 1234abcd           its entry, or "ncx: -"
//   spine: 40                        how many documents, then a line for each:
//   + xhtml 8192 6000 89abcdef       '+' if it is in the run of the book and
//   - xhtml ?                        '-' if set aside, its kind, and its
//                                    entry, or '?' if it is not in the book
//   unknown: 0                       documents the spine named that the
//                                    manifest has not got
//   cut short: 0                     1 if the package file went wrong after
//                                    its spine
//
// An entry is written as three numbers: where its header is in the ZIP
// file and how many bytes it holds, in decimal, and the checksum of those
// bytes as eight digits of hexadecimal. They say which file it is as well
// as its name would, and getting them costs no reading of the ZIP file.
//
// These are the lines `bn-epub info` prints, with entries in place of
// names. Two readings of a book that come to the same checksum found the
// same things in its package.
std::uint32_t PackageChecksum(const Package& package) noexcept;

}  // namespace bn::epub
