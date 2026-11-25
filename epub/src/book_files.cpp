#include "epub/book_files.h"

namespace bn::epub {

core::ByteSource* ZipBookFiles::Open(std::string_view path) noexcept {
  ZipEntry entry;
  status_ = archive_.Find(path, entry);
  if (status_ == ZipStatus::kOk) {
    status_ = reader_.Open(entries_, entry);
  }
  return status_ == ZipStatus::kOk ? &reader_ : nullptr;
}

}  // namespace bn::epub
