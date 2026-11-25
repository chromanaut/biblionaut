#pragma once

#include <string_view>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "epub/zip.h"

// The files of a book, reached by their paths.
//
// What reads a book's documents and the files they name goes through this,
// and never sees that a book is a ZIP file: a test can hand it files held
// in memory.

namespace bn::epub {

// A book's files, one open at a time.
class BookFiles {
 public:
  // Opens the file at `path`, to be read from its start: a path from the
  // top of the book with no '/' in front, as ResolveBookPath() gives it and
  // as the ZIP file names its entries. Whatever was open before is done
  // with. Null if the book has no such file, or it cannot be opened.
  //
  // The source is good until the next Open(). A file that is damaged ends
  // in the source's kError, at the latest when its last byte has been
  // read, so a file has only been read whole once the source has given
  // its end.
  [[nodiscard]] virtual core::ByteSource* Open(std::string_view path) noexcept = 0;

 protected:
  ~BookFiles() = default;  // Never destroyed through the interface.
};

// The entries of a ZIP file as a book's files.
//
// It finds each with ZipArchive::Find(), whose cost that header gives: next
// to nothing for entries asked for in the file's order, and a read of the
// whole directory for one that is not there. It reads through the reader
// it is given, one entry at a time.
class ZipBookFiles final : public BookFiles {
 public:
  // `archive` must be open, `entries` a BlockFile open on the same ZIP file
  // for the entries' own bytes (see ZipArchive for why it should not be the
  // directory's), and all three must stay alive, and be left alone while a
  // file is being read, as long as this is used.
  ZipBookFiles(ZipArchive& archive, core::BlockFile& entries, ZipEntryReader& reader) noexcept
      : archive_(archive), entries_(entries), reader_(reader) {}

  [[nodiscard]] core::ByteSource* Open(std::string_view path) noexcept override;

  // How finding or opening the last file asked for ended: kOk if it was
  // opened, kNotFound if the book has none of that name, or what was wrong.
  ZipStatus Status() const noexcept { return status_; }

 private:
  ZipArchive& archive_;
  core::BlockFile& entries_;
  ZipEntryReader& reader_;
  ZipStatus status_ = ZipStatus::kNotOpen;
};

}  // namespace bn::epub
