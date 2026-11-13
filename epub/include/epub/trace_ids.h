#pragma once

#include "core/trace.h"

// Trace ids of book parsing, listed once as X(index, constant, name,
// argument), in the same form as every other part's list:
//
//   index     the id's number within this part. It is part of the trace
//             format: add new ids at the end, and never reuse or renumber one.
//   constant  what code passes to the trace macros.
//   name      what tools show for it.
//   argument  what the record's number means, or "" if it carries none.
//
// The names are only compiled into the trace tool, never into the firmware.
#define BN_EPUB_TRACE_IDS(X)                                                \
  /* Reading a book's package (epub/package.h), and how that ended, a    */ \
  /* PackageStatus: 0 is read.                                           */ \
  X(0, kSpanPackageRead, "package_read", "status")                          \
  /* The four parts of that, in the order they come. Finding and reading */ \
  /* container.xml, which names the package file.                        */ \
  X(1, kSpanPackageContainer, "package_container", "")                      \
  /* Finding the package file and parsing it.                            */ \
  X(2, kSpanPackageFile, "package_file", "")                                \
  /* Matching what the spine and the metadata name to the files the      */ \
  /* manifest lists: the argument is how many documents the spine named. */ \
  X(3, kSpanPackageResolve, "package_resolve", "documents")                 \
  /* Going through the ZIP file's directory for the files that are       */ \
  /* wanted: the argument is how many files the manifest lists.          */ \
  X(4, kSpanPackageFindFiles, "package_find_files", "files")

namespace bn::epub {

#define BN_EPUB_TRACE_ID_CONSTANT(index, constant, name, argument) \
  inline constexpr core::SpanId constant = core::MakeSpanId(core::TracePart::kEpub, (index));
BN_EPUB_TRACE_IDS(BN_EPUB_TRACE_ID_CONSTANT)
#undef BN_EPUB_TRACE_ID_CONSTANT

}  // namespace bn::epub
