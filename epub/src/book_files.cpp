#include "epub/book_files.h"

#include <algorithm>

namespace bn::epub {

core::ByteSource* ZipBookFiles::Open(std::string_view path) noexcept {
  ZipEntry entry;
  status_ = archive_.Find(path, entry);
  if (status_ == ZipStatus::kOk) {
    status_ = reader_.Open(entries_, entry);
  }
  return status_ == ZipStatus::kOk ? &reader_ : nullptr;
}

core::ByteSource* ZipBookFiles::OpenAt(const ZipEntry& entry, std::span<char> path, std::size_t& length) noexcept {
  std::size_t whole = 0;
  status_ = ReadZipEntryName(entries_, entry, path, whole);
  length = status_ == ZipStatus::kOk ? std::min(whole, path.size()) : 0;
  if (status_ == ZipStatus::kOk && whole > path.size()) {
    status_ = ZipStatus::kUnsupported;
  }
  if (status_ == ZipStatus::kOk) {
    status_ = reader_.Open(entries_, entry);
  }
  return status_ == ZipStatus::kOk ? &reader_ : nullptr;
}

}  // namespace bn::epub
