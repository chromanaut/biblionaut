#include "partition_check.h"

#include <esp_err.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <spi_flash_mmap.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Only compiled into a build with BN_PARTITION_CHECK.

namespace bn::firmware {

namespace {

constexpr const char* kLabel = "fonts";
// The processor maps flash in pages of this size.
constexpr std::size_t kPage = 65536;
// What the whole partition is read through, a window at a time.
constexpr std::size_t kWindow = 1048576;
// How many times a window is mapped and unmapped, to time it.
constexpr int kMapTimes = 100;
// How many reads of kSmallRead bytes are timed, and over how much of the
// partition when they are scattered (the cache seldom holds them), and when
// they are not (it nearly always does).
constexpr int kSmallReads = 20000;
constexpr std::size_t kSmallRead = 64;
constexpr std::size_t kScatteredOver = 4194304;
constexpr std::size_t kGatheredIn = 8192;

// What the fonts partition is filled with, a word at a time, from where the
// word is: partition_pattern.py works out the same numbers.
std::uint32_t WordAt(std::uint32_t offset) noexcept {
  std::uint32_t x = offset ^ 0x6A09E667U;
  x ^= x >> 16U;
  x *= 0x7FEB352DU;
  x ^= x >> 15U;
  x *= 0x846CA68BU;
  x ^= x >> 16U;
  return x;
}

std::uint64_t NowUs() noexcept { return static_cast<std::uint64_t>(esp_timer_get_time()); }

// Lets the idle task run. The watchdog watches it, and the check is done
// before anything else in the firmware gives it a turn.
void LetIdleRun() noexcept { vTaskDelay(1); }

// What the reads add up to, kept so that the compiler cannot leave them out.
volatile std::uint32_t readSum = 0;

void Say(const char* text) noexcept { esp_rom_printf("partition check: %s\n", text); }

class Line {
 public:
  template <typename... Args>
  explicit Line(const char* format, Args... args) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): formatting into our own buffer.
    std::snprintf(text_, sizeof(text_), format, args...);
    Say(text_);
  }

 private:
  char text_[160] = {};
};

void SayFlash() noexcept {
  std::uint32_t physical = 0;
  std::uint32_t configured = 0;
  const esp_err_t physicalStatus = esp_flash_get_physical_size(esp_flash_default_chip, &physical);
  const esp_err_t configuredStatus = esp_flash_get_size(esp_flash_default_chip, &configured);
  Line("flash: the chip says %u KB (%s); the build takes it to be %u KB (%s).", static_cast<unsigned>(physical / 1024),
       esp_err_to_name(physicalStatus), static_cast<unsigned>(configured / 1024), esp_err_to_name(configuredStatus));
}

void SayPartitions() noexcept {
  esp_partition_iterator_t at = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (at != nullptr) {
    const esp_partition_t* partition = esp_partition_get(at);
    Line("partition %s: type %u, subtype 0x%02x, at 0x%06x, %u KB.", partition->label,
         static_cast<unsigned>(partition->type), static_cast<unsigned>(partition->subtype),
         static_cast<unsigned>(partition->address), static_cast<unsigned>(partition->size / 1024));
    at = esp_partition_next(at);  // Releases the iterator after the last.
  }
}

// Maps `size` bytes of the partition from `offset`, and unmaps them again.
esp_err_t TryMapping(const esp_partition_t& partition, std::size_t offset, std::size_t size) noexcept {
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle = 0;
  const esp_err_t status = esp_partition_mmap(&partition, offset, size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
  if (status == ESP_OK) {
    esp_partition_munmap(handle);
  }
  return status;
}

void SayMapping(const esp_partition_t& partition) noexcept {
  const unsigned freePages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
  Line("pages of address space free for data: %u (%u KB).", freePages,
       static_cast<unsigned>(freePages * (kPage / 1024)));
  const esp_err_t whole = TryMapping(partition, 0, partition.size);
  Line("the whole partition at once, %u KB: %s.", static_cast<unsigned>(partition.size / 1024), esp_err_to_name(whole));
  // The most pages that can be mapped at once, found by halving.
  std::size_t fits = 0;
  std::size_t fails = partition.size / kPage + 1;
  while (fails - fits > 1) {
    const std::size_t pages = (fits + fails) / 2;
    if (TryMapping(partition, 0, pages * kPage) == ESP_OK) {
      fits = pages;
    } else {
      fails = pages;
    }
  }
  Line("the most mapped at once: %u pages (%u KB).", static_cast<unsigned>(fits),
       static_cast<unsigned>(fits * (kPage / 1024)));
}

void TimeMapping(const esp_partition_t& partition, std::size_t size) noexcept {
  std::uint64_t mapUs = 0;
  std::uint64_t unmapUs = 0;
  int failed = 0;
  const std::size_t places = partition.size / size;
  for (int i = 0; i < kMapTimes; ++i) {
    const std::size_t offset = (static_cast<std::size_t>(i) * 7 % places) * size;
    const void* mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    const std::uint64_t before = NowUs();
    const esp_err_t status = esp_partition_mmap(&partition, offset, size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    const std::uint64_t mappedAt = NowUs();
    if (status != ESP_OK) {
      ++failed;
      continue;
    }
    esp_partition_munmap(handle);
    unmapUs += NowUs() - mappedAt;
    mapUs += mappedAt - before;
  }
  const int done = kMapTimes - failed;
  Line("a window of %u KB: mapped in %u us and unmapped in %u us, on average of %d (%d failed).",
       static_cast<unsigned>(size / 1024), static_cast<unsigned>(done > 0 ? mapUs / done : 0),
       static_cast<unsigned>(done > 0 ? unmapUs / done : 0), done, failed);
}

// Reads the whole partition a window at a time, timed; then goes over it
// again and compares every word with what it should be. False if a window
// would not map or a word is wrong.
bool ReadEverything(const esp_partition_t& partition) noexcept {
  std::uint64_t readUs = 0;
  std::uint64_t wrong = 0;
  std::uint32_t firstWrong = 0;
  bool mappedAll = true;
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t offset = 0; offset < partition.size; offset += kWindow) {
      const std::size_t size = partition.size - offset < kWindow ? partition.size - offset : kWindow;
      const void* mapped = nullptr;
      esp_partition_mmap_handle_t handle = 0;
      if (esp_partition_mmap(&partition, offset, size, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
        mappedAll = false;
        continue;
      }
      const auto* words = static_cast<const std::uint32_t*>(mapped);
      const std::size_t count = size / 4;
      if (pass == 0) {
        const std::uint64_t before = NowUs();
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
          sum += words[i];
        }
        readUs += NowUs() - before;
        readSum = readSum + sum;
      } else {
        for (std::size_t i = 0; i < count; ++i) {
          const auto at = static_cast<std::uint32_t>(offset + (i * 4));
          if (words[i] != WordAt(at)) {
            firstWrong = wrong == 0 ? at : firstWrong;
            ++wrong;
          }
        }
      }
      esp_partition_munmap(handle);
      LetIdleRun();
    }
  }
  Line("read all %u KB in %u ms (%u KB/s), through windows of %u KB.", static_cast<unsigned>(partition.size / 1024),
       static_cast<unsigned>(readUs / 1000),
       static_cast<unsigned>(readUs > 0 ? partition.size * 1000000ULL / 1024 / readUs : 0),
       static_cast<unsigned>(kWindow / 1024));
  if (!mappedAll) {
    Say("some windows would not map.");
  }
  if (wrong == 0) {
    Line("every word is what it should be (%u words).", static_cast<unsigned>(partition.size / 4));
  } else {
    Line("%u words are not what they should be, the first at 0x%06x.", static_cast<unsigned>(wrong),
         static_cast<unsigned>(firstWrong));
  }
  return mappedAll && wrong == 0;
}

// Reads of kSmallRead bytes at kSmallReads places within `span` bytes
// of a window at the partition's start.
void TimeSmallReads(const esp_partition_t& partition, std::size_t span, const char* what) noexcept {
  const std::size_t size = span < kPage ? kPage : span;
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle = 0;
  if (esp_partition_mmap(&partition, 0, size, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
    Line("%s: the window would not map.", what);
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(mapped);
  std::uint32_t state = 0x2545F491U;
  std::uint32_t sum = 0;
  const std::uint64_t before = NowUs();
  for (int i = 0; i < kSmallReads; ++i) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    const std::size_t at = (state % (span / kSmallRead)) * kSmallRead;
    const auto* words =
        reinterpret_cast<const std::uint32_t*>(bytes + at);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    for (std::size_t w = 0; w < kSmallRead / 4; ++w) {
      sum += words[w];
    }
  }
  const std::uint64_t took = NowUs() - before;
  readSum = readSum + sum;
  esp_partition_munmap(handle);
  Line("%s: %d reads of %u bytes in %u us (%u ns a read).", what, kSmallReads, static_cast<unsigned>(kSmallRead),
       static_cast<unsigned>(took), static_cast<unsigned>(took * 1000 / kSmallReads));
}

}  // namespace

void RunPartitionCheck() noexcept {
  esp_rom_printf("Partition check build: the partition table and the fonts partition are checked at start.\n");
  SayFlash();
  SayPartitions();
  const esp_partition_t* fonts = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLabel);
  if (fonts == nullptr) {
    Say("FAILED. there is no data partition called fonts.");
    return;
  }
  SayMapping(*fonts);
  TimeMapping(*fonts, kPage);
  TimeMapping(*fonts, kWindow);
  const bool right = ReadEverything(*fonts);
  TimeSmallReads(*fonts, kScatteredOver, "scattered over 4096 KB");
  TimeSmallReads(*fonts, kGatheredIn, "within 8 KB");
  Say(right ? "passed." : "FAILED.");
}

}  // namespace bn::firmware
