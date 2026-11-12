#include <cstdlib>

#include "desktop/folder_storage.h"
#include "hw/platform.h"

namespace bn::hw {

core::Storage& SystemStorage() noexcept {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, the first time storage is asked for.
  const char* folder = std::getenv("BN_STORAGE");
  static FolderStorage instance(folder != nullptr ? folder : "");
  return instance;
}

}  // namespace bn::hw
