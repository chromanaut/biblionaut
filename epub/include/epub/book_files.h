#pragma once

#include <cstddef>
#include <span>
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

  // Opens the file whose place in the ZIP file its caller already has, as
  // the directory or a book's package gave it, without finding it by its
  // name. The file's path is read from the entry's own header into `path`:
  // the way back from a place to the name its links are from. `length` is
  // how much of `path` that is.
  //
  // Null if the entry cannot be opened, or its path cannot be read, or the
  // path is longer than `path` has room for (kUnsupported): as much of the
  // path as there was room for is in `path` all the same, to say which
  // file it was, and nothing of it if it could not be read.
  //
  // This is how a document of a book's spine is opened to be handed to
  // DocumentSheets::Begin(): the package has its place (SpineItem::entry),
  // and a search of the directory for its name is saved.
  [[nodiscard]] core::ByteSource* OpenAt(const ZipEntry& entry, std::span<char> path, std::size_t& length) noexcept;

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
