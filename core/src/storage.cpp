#include "core/storage.h"

namespace bn::core {

const char* StorageStatusName(StorageStatus status) noexcept {
  switch (status) {
    case StorageStatus::kOk:
      return "ok";
    case StorageStatus::kAbsent:
      return "absent";
    case StorageStatus::kNotFound:
      return "not found";
    case StorageStatus::kExists:
      return "exists";
    case StorageStatus::kBadPath:
      return "bad path";
    case StorageStatus::kTooManyOpen:
      return "too many open";
    case StorageStatus::kTooBig:
      return "too big";
    case StorageStatus::kNoSpace:
      return "no space";
    case StorageStatus::kFailed:
      return "failed";
  }
  return "unknown";
}

bool IsStoragePath(std::string_view path) noexcept {
  if (path.empty() || path.size() > Storage::kMaxPath || path.front() != '/') {
    return false;
  }
  if (path.size() == 1) {
    return true;  // The top directory.
  }
  constexpr std::string_view kNotInNames = "\\:*?\"<>|";
  std::size_t partLength = 0;
  for (std::size_t i = 1; i < path.size(); ++i) {
    const char ch = path[i];
    if (ch == '/') {
      // What came before must be a name: not empty, and not ending in a
      // dot or a space.
      if (partLength == 0 || path[i - 1] == '.' || path[i - 1] == ' ') {
        return false;
      }
      partLength = 0;
      continue;
    }
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F || kNotInNames.find(ch) != std::string_view::npos) {
      return false;
    }
    ++partLength;
  }
  // And so must the last part: a path does not end in '/'.
  return partLength > 0 && path.back() != '.' && path.back() != ' ';
}

}  // namespace bn::core
