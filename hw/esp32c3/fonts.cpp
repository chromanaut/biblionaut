#include <esp_err.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/byte_map.h"
#include "core/metrics.h"
#include "hw/platform.h"

// The fonts image where the firmware has it: the data partition of the
// flash called `fonts`, a stretch of which is read by being mapped into the
// processor's address space.
//
// The processor maps flash in pages of 64 KB, 128 of them at the most, which
// the firmware's own code and constants share: so the whole of a partition
// of 12 MB cannot be mapped at once, and whoever reads it maps the stretch
// that is wanted and gives it up afterwards. A stretch takes every page it
// touches.

namespace bn::hw {

namespace {

constexpr const char* kLabel = "fonts";
// The size of a page (CONFIG_MMU_PAGE_SIZE on this chip).
constexpr std::size_t kPageBytes = 65536;
// The most stretches mapped at once.
constexpr std::size_t kMostStretches = 32;

// Stretches mapped since start, those refused for want of address space or
// of a place in the list, and the most room ever taken at once.
core::Counter fontMaps{"font_maps"};
core::Counter fontMapsRefused{"font_maps_refused"};
core::Gauge fontMappedKb{"font_mapped_kb"};

class FontsPartition final : public core::ByteMap {
 public:
  FontsPartition() noexcept
      : partition_(esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLabel)) {}

  std::size_t Size() const noexcept override { return partition_ != nullptr ? partition_->size : 0; }

  [[nodiscard]] std::span<const std::uint8_t> Map(std::size_t at, std::size_t length) noexcept override {
    if (partition_ == nullptr || length == 0 || at > partition_->size || length > partition_->size - at) {
      return {};
    }
    Stretch* free = nullptr;
    for (Stretch& stretch : mapped_) {
      if (stretch.data == nullptr) {
        free = &stretch;
        break;
      }
    }
    const void* address = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (free == nullptr ||
        esp_partition_mmap(partition_, at, length, ESP_PARTITION_MMAP_DATA, &address, &handle) != ESP_OK) {
      fontMapsRefused.Add();
      return {};
    }
    // The mapping begins at the page the stretch's first byte is in.
    const std::size_t pages = (((partition_->address + at) % kPageBytes) + length + kPageBytes - 1) / kPageBytes;
    *free =
        Stretch{.data = static_cast<const std::uint8_t*>(address), .length = length, .handle = handle, .pages = pages};
    mappedPages_ += pages;
    fontMaps.Add();
    fontMappedKb.Set(static_cast<std::uint32_t>(mappedPages_ * (kPageBytes / 1024)));
    return {free->data, length};
  }

  void Unmap(std::span<const std::uint8_t> stretch) noexcept override {
    if (stretch.empty()) {
      return;
    }
    for (Stretch& mapped : mapped_) {
      if (mapped.data == stretch.data() && mapped.length == stretch.size()) {
        esp_partition_munmap(mapped.handle);
        mappedPages_ -= mapped.pages;
        mapped = Stretch{};
        fontMappedKb.Set(static_cast<std::uint32_t>(mappedPages_ * (kPageBytes / 1024)));
        return;
      }
    }
  }

  std::size_t MappedBytes() const noexcept override { return mappedPages_ * kPageBytes; }

 private:
  struct Stretch {
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;
    esp_partition_mmap_handle_t handle = 0;
    std::size_t pages = 0;
  };

  const esp_partition_t* partition_;
  std::array<Stretch, kMostStretches> mapped_{};
  std::size_t mappedPages_ = 0;
};

}  // namespace

core::ByteMap& SystemFonts() noexcept {
  static FontsPartition instance;
  return instance;
}

}  // namespace bn::hw
