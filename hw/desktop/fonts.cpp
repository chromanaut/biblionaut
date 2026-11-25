#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/byte_map.h"
#include "desktop/device_profile.h"
#include "hw/platform.h"

namespace bn::hw {

namespace {

// The file BN_TEXT names, or nothing if it names none or none that can be
// read.
std::vector<std::uint8_t> ReadImage() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, the first time the fonts are asked for.
  const char* path = std::getenv("BN_TEXT");
  if (path == nullptr) {
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

core::ByteMap& SystemFonts() noexcept {
  static const std::vector<std::uint8_t> kImage = ReadImage();
  // As much of it mapped at once as the device simulated has room for.
  static core::MemoryByteMap instance(kImage, kFontPageBytes, ChosenDevice().fontPages);
  return instance;
}

}  // namespace bn::hw
