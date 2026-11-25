#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <driver/sdspi_host.h>
#include <esp_err.h>
#include <ff.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdmmc_cmd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/metrics.h"
#include "core/storage.h"
#include "core/trace.h"
#include "esp32c3/spi_bus.h"
#include "hw/platform.h"
#include "hw/trace_ids.h"

// The SD card, on the wires it shares with the panel.
//
// ESP-IDF's driver speaks to the card and its copy of FatFS understands
// the file system on it. FatFS is called directly: nothing here goes
// through the C library's files.
//
// Only FAT file systems are understood, which is what cards of up to 32 GB
// are sold with. Bigger cards come with exFAT, which ESP-IDF's FatFS is
// built without; such a card is reported as having no usable file system.

namespace bn::hw {

namespace {

using core::StorageStatus;

// CONFIG_FATFS_FS_LOCK is set to match: FatFS keeps a record of each open
// file and each directory being listed, and has room for that many.
constexpr std::size_t kMaxOpenFiles = core::Storage::kMaxOpen;

// FatFS's map of a file (see MakeMap() below) takes two numbers for each
// run of the file that lies in one piece on the card, and two more: this
// is room for a file in 63 pieces, and is half a kilobyte.
constexpr std::size_t kMapItems = 128;

// How many open files can have a map at once. Fewer than can be open: a
// map is for a large file read from places all over it, which is a book,
// and what else is open beside a book is small or is being written.
constexpr std::size_t kMaxMapped = 4;

// A file is given a map if its chain is longer than this many clusters,
// which is what one sector of a FAT32 card's table holds: a shorter chain
// is one read of the table however it is followed, and a map of it would
// cost that read at every opening for nothing.
constexpr std::uint32_t kMapFromClusters = 128;

// A card's sector, which is what it is read and written in.
constexpr std::size_t kSectorBytes = 512;

// How many bytes of a read or a write of `size` bytes at `offset` in a
// file, into or out of `memory`, the card's driver moves by way of a block
// it takes from the heap for the purpose and gives back after.
//
// FatFS moves the part of a sector at either end through a sector of its
// own, and hands the driver the whole sectors between, at their place in
// the caller's memory. The driver goes straight between the card and
// memory that begins at a multiple of four. Anything else it copies
// through a block of its own, as large as the sectors if the heap has one.
// So it is the whole sectors that count, and where in memory the first of
// them falls: a read or write that begins where a sector does, with memory
// that begins at a multiple of four, never costs this.
std::uint32_t CopiedByTheDriver(std::uint32_t offset, std::size_t size, const void* memory) noexcept {
  const std::size_t before = (kSectorBytes - (offset % kSectorBytes)) % kSectorBytes;
  if (size < before + kSectorBytes) {
    return 0;  // No whole sector in it.
  }
  if ((reinterpret_cast<std::uintptr_t>(memory) + before) % 4 == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(((size - before) / kSectorBytes) * kSectorBytes);
}

// Files given a map, files in too many pieces to have one, files that
// went without because every map was in use, and how many numbers the map
// of each file needed: the most is the room that would have done for all
// of them. And the size of the card's clusters.
core::Counter sdMaps{"sd_maps"};
// Reads and writes that the card's driver could not make straight between
// the card and the caller's memory, and the bytes of the last such (so the
// most of any, as a gauge keeps it): see CopiedByTheDriver().
core::Counter sdReadsCopied{"sd_reads_copied"};
core::Counter sdWritesCopied{"sd_writes_copied"};
core::Gauge sdCopiedBytes{"sd_copied_bytes"};
core::Counter sdMapsRefused{"sd_maps_refused"};
core::Counter sdMapsNoneFree{"sd_maps_none_free"};
core::Gauge sdMapItems{"sd_map_items"};
core::Gauge sdClusterBytes{"sd_cluster_bytes"};

// The size of a cluster of a mounted file system, in bytes.
std::uint32_t ClusterBytes(const FATFS& fileSystem) noexcept {
#if FF_MAX_SS == FF_MIN_SS
  return std::uint32_t{fileSystem.csize} * static_cast<std::uint32_t>(FF_MAX_SS);
#else
  return std::uint32_t{fileSystem.csize} * std::uint32_t{fileSystem.ssize};
#endif
}

// What a FatFS result comes to.
StorageStatus FromFatFs(FRESULT result) noexcept {
  switch (result) {
    case FR_OK:
      return StorageStatus::kOk;
    case FR_NO_FILE:
    case FR_NO_PATH:
      return StorageStatus::kNotFound;
    case FR_EXIST:
      return StorageStatus::kExists;
    case FR_INVALID_NAME:
      return StorageStatus::kBadPath;
    case FR_TOO_MANY_OPEN_FILES:
      return StorageStatus::kTooManyOpen;
    default:
      // Among them: the card gave an error, a directory still has something
      // in it, the file is open.
      return StorageStatus::kFailed;
  }
}

// A path as FatFS wants it: on drive 0, and ending in a zero byte.
class FatPath {
 public:
  explicit FatPath(std::string_view path) noexcept {
    text_[0] = '0';
    text_[1] = ':';
    std::memcpy(text_.data() + 2, path.data(), path.size());
    text_[2 + path.size()] = '\0';
  }
  const char* Text() const noexcept { return text_.data(); }

 private:
  std::array<char, core::Storage::kMaxPath + 3> text_{};
};

class SdCardStorage final : public core::Storage {
 public:
  [[nodiscard]] StorageStatus Start() noexcept override {
    if (started_) {
      return StorageStatus::kOk;
    }
    const std::uint32_t began = BN_TRACE_NOW();
    const StorageStatus status = Mount();
    started_ = status == StorageStatus::kOk;
    BN_TRACE_TOP_SPAN_SINCE(kSpanSdStart, began, static_cast<std::uint32_t>(status));
    return status;
  }

  [[nodiscard]] StorageStatus Open(std::string_view path, core::OpenMode mode,
                                   core::FileHandle& file) noexcept override {
    file = core::kNoFile;
    if (const StorageStatus status = Check(path); status != StorageStatus::kOk) {
      return status;
    }
    OpenFile* place = Claim();
    if (place == nullptr) {
      return StorageStatus::kTooManyOpen;
    }
    // FatFS itself keeps a file to one writer or any number of readers: it
    // refuses to open one that is open to be written, and to open one that
    // is open so as to write it.
    BYTE access = FA_READ;
    if (mode == core::OpenMode::kCreate) {
      access = static_cast<BYTE>(FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    } else if (mode == core::OpenMode::kUpdate) {
      access = static_cast<BYTE>(FA_READ | FA_WRITE);
    }
    const FatPath name(path);
    const FRESULT result = f_open(&place->file, name.Text(), access);
    if (result != FR_OK) {
      Release(*place);
      return FromFatFs(result);
    }
    place->writable = mode != core::OpenMode::kRead;
    // A map only for a file that is read: with one, FatFS cannot make a
    // file longer.
    if (!place->writable && f_size(&place->file) > std::uint64_t{kMapFromClusters} * ClusterBytes(fileSystem_)) {
      MakeMap(*place);
    }
    file = static_cast<core::FileHandle>(place - open_.data());
    return StorageStatus::kOk;
  }

  void Close(core::FileHandle file) noexcept override {
    if (OpenFile* found = Find(file); found != nullptr) {
      f_close(&found->file);
      Release(*found);
    }
  }

  [[nodiscard]] StorageStatus Size(core::FileHandle file, std::uint32_t& size) noexcept override {
    OpenFile* found = Find(file);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    size = f_size(&found->file);
    return StorageStatus::kOk;
  }

  [[nodiscard]] StorageStatus ReadAt(core::FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                     std::size_t& read) noexcept override {
    read = 0;
    OpenFile* found = Find(file);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    if (offset >= f_size(&found->file)) {
      return StorageStatus::kOk;  // Nothing there to read.
    }
    BN_TRACE_SCOPE_ARG(kSpanSdRead, static_cast<std::uint32_t>(dst.size()));
    FRESULT result = FR_OK;
    if (f_tell(&found->file) != offset) {
      result = f_lseek(&found->file, offset);
    }
    UINT count = 0;
    if (result == FR_OK) {
      if (const std::uint32_t copied = CopiedByTheDriver(offset, dst.size(), dst.data()); copied != 0) {
        sdReadsCopied.Add();
        sdCopiedBytes.Set(copied);
      }
      result = f_read(&found->file, dst.data(), static_cast<UINT>(dst.size()), &count);
    }
    read = count;
    return FromFatFs(result);
  }

  [[nodiscard]] StorageStatus WriteAt(core::FileHandle file, std::uint32_t offset,
                                      std::span<const std::uint8_t> src) noexcept override {
    OpenFile* found = Find(file);
    // No gaps: moving past the end of a file being written would stretch it.
    if (found == nullptr || !found->writable || offset > f_size(&found->file)) {
      return StorageStatus::kFailed;
    }
    if (src.size() > UINT32_MAX - offset) {
      return StorageStatus::kTooBig;
    }
    BN_TRACE_SCOPE_ARG(kSpanSdWrite, static_cast<std::uint32_t>(src.size()));
    FRESULT result = FR_OK;
    if (f_tell(&found->file) != offset) {
      result = f_lseek(&found->file, offset);
    }
    UINT count = 0;
    if (result == FR_OK) {
      if (const std::uint32_t copied = CopiedByTheDriver(offset, src.size(), src.data()); copied != 0) {
        sdWritesCopied.Add();
        sdCopiedBytes.Set(copied);
      }
      result = f_write(&found->file, src.data(), static_cast<UINT>(src.size()), &count);
    }
    if (result != FR_OK) {
      return FromFatFs(result);
    }
    // FatFS reports a full card by writing less than it was given.
    return count == src.size() ? StorageStatus::kOk : StorageStatus::kNoSpace;
  }

  [[nodiscard]] StorageStatus Sync(core::FileHandle file) noexcept override {
    OpenFile* found = Find(file);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    BN_TRACE_SCOPE(kSpanSdSync);
    return FromFatFs(f_sync(&found->file));
  }

  [[nodiscard]] StorageStatus Stat(std::string_view path, core::FileInfo& info) noexcept override {
    if (const StorageStatus status = Check(path); status != StorageStatus::kOk) {
      return status;
    }
    if (path.size() == 1) {
      info = {.size = 0, .isDirectory = true, .modified = 0};  // FatFS has no entry for the top directory.
      return StorageStatus::kOk;
    }
    const FatPath name(path);
    FILINFO found = {};
    const FRESULT result = f_stat(name.Text(), &found);
    if (result != FR_OK) {
      return FromFatFs(result);
    }
    info.isDirectory = (found.fattrib & AM_DIR) != 0;
    info.size = info.isDirectory ? 0 : found.fsize;
    info.modified = (std::uint32_t{found.fdate} << 16U) | found.ftime;
    return StorageStatus::kOk;
  }

  [[nodiscard]] StorageStatus Remove(std::string_view path) noexcept override {
    if (const StorageStatus status = Check(path); status != StorageStatus::kOk) {
      return status;
    }
    if (path.size() == 1) {
      return StorageStatus::kFailed;
    }
    const FatPath name(path);
    return FromFatFs(f_unlink(name.Text()));
  }

  [[nodiscard]] StorageStatus Rename(std::string_view from, std::string_view to) noexcept override {
    if (const StorageStatus status = Check(from); status != StorageStatus::kOk) {
      return status;
    }
    if (!core::IsStoragePath(to)) {
      return StorageStatus::kBadPath;
    }
    if (from.size() == 1 || to.size() == 1) {
      return StorageStatus::kFailed;
    }
    const FatPath oldName(from);
    const FatPath newName(to);
    return FromFatFs(f_rename(oldName.Text(), newName.Text()));
  }

  [[nodiscard]] StorageStatus MakeDirectory(std::string_view path) noexcept override {
    if (const StorageStatus status = Check(path); status != StorageStatus::kOk) {
      return status;
    }
    if (path.size() == 1) {
      return StorageStatus::kExists;
    }
    const FatPath name(path);
    return FromFatFs(f_mkdir(name.Text()));
  }

  [[nodiscard]] StorageStatus OpenDirectory(std::string_view path, core::DirectoryHandle& directory) noexcept override {
    directory = core::kNoDirectory;
    if (const StorageStatus status = Check(path); status != StorageStatus::kOk) {
      return status;
    }
    ListedDirectory* place = ClaimDirectory();
    if (place == nullptr) {
      return StorageStatus::kTooManyOpen;
    }
    const FatPath name(path);
    const FRESULT result = f_opendir(&place->directory, name.Text());
    if (result != FR_OK) {
      ReleaseDirectory(*place);
      return FromFatFs(result);  // Among them: it is a file.
    }
    directory = static_cast<core::DirectoryHandle>(place - listed_.data());
    return StorageStatus::kOk;
  }

  [[nodiscard]] StorageStatus NextInDirectory(core::DirectoryHandle directory, std::span<char> name,
                                              std::size_t& length, core::FileInfo& info) noexcept override {
    length = 0;
    ListedDirectory* found = FindDirectory(directory);
    if (found == nullptr) {
      return StorageStatus::kFailed;
    }
    for (;;) {
      // FatFS gives the long name where there is one that fits in 255
      // bytes, and the short one otherwise; either opens the file.
      FILINFO item = {};
      const FRESULT result = f_readdir(&found->directory, &item);
      if (result != FR_OK) {
        return FromFatFs(result);
      }
      const std::string_view itemName(static_cast<const char*>(item.fname));
      if (itemName.empty()) {
        return StorageStatus::kOk;  // Nothing more.
      }
      if (itemName.size() > name.size() || !core::IsStorageName(itemName)) {
        continue;
      }
      info.isDirectory = (item.fattrib & AM_DIR) != 0;
      info.size = info.isDirectory ? 0 : item.fsize;
      info.modified = (std::uint32_t{item.fdate} << 16U) | item.ftime;
      std::memcpy(name.data(), itemName.data(), itemName.size());
      length = itemName.size();
      return StorageStatus::kOk;
    }
  }

  void CloseDirectory(core::DirectoryHandle directory) noexcept override {
    if (ListedDirectory* found = FindDirectory(directory); found != nullptr) {
      f_closedir(&found->directory);
      ReleaseDirectory(*found);
    }
  }

 private:
  struct Map {
    bool inUse = false;
    std::array<DWORD, kMapItems> items;
  };

  struct OpenFile {
    FIL file;
    bool inUse = false;
    bool writable = false;
    Map* map = nullptr;  // The map it was given, if it has one.
  };

  // Has FatFS make its map of a file just opened to be read: where each
  // run of the file lies on the card. To go to a place in a file FatFS
  // otherwise follows the file's chain of clusters, from the file's start
  // if the place is behind where it is, reading the card's table of
  // chains as it goes, which takes longer the larger the file. With the
  // map it reads nothing. Making the map follows the chain once, here. A
  // file in more runs than a map has room for goes without one, and so
  // does one opened while every map is in use: either is read as it would
  // have been.
  void MakeMap(OpenFile& place) noexcept {
    Map* map = ClaimMap();
    if (map == nullptr) {
      sdMapsNoneFree.Add();
      return;
    }
    map->items[0] = static_cast<DWORD>(map->items.size());
    place.file.cltbl = map->items.data();
    const FRESULT mapped = f_lseek(&place.file, CREATE_LINKMAP);
    if (mapped == FR_OK || mapped == FR_NOT_ENOUGH_CORE) {
      // Either way FatFS has said how many numbers the map needs.
      sdMapItems.Set(static_cast<std::uint32_t>(map->items[0]));
    }
    if (mapped == FR_OK) {
      place.map = map;
      sdMaps.Add();
    } else {
      place.file.cltbl = nullptr;
      ReleaseMap(*map);
      sdMapsRefused.Add();
    }
  }

  // A free map, marked as taken; null if there is none.
  Map* ClaimMap() noexcept {
    Map* claimed = nullptr;
    xSemaphoreTake(lock_, portMAX_DELAY);
    for (Map& candidate : maps_) {
      if (!candidate.inUse) {
        candidate.inUse = true;
        claimed = &candidate;
        break;
      }
    }
    xSemaphoreGive(lock_);
    return claimed;
  }

  void ReleaseMap(Map& map) noexcept {
    xSemaphoreTake(lock_, portMAX_DELAY);
    map.inUse = false;
    xSemaphoreGive(lock_);
  }

  struct ListedDirectory {
    FF_DIR directory;
    bool inUse = false;
  };

  // Finds the card and the file system on it.
  StorageStatus Mount() noexcept {
    if (!StartSharedSpiBus()) {
      return StorageStatus::kFailed;
    }
    if (lock_ == nullptr) {
      lock_ = xSemaphoreCreateMutexStatic(&lockStorage_);
    }

    // The card, as a device on the bus the panel is on.
    if (!hostReady_) {
      if (sdspi_host_init() != ESP_OK) {
        return StorageStatus::kFailed;
      }
      hostReady_ = true;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = kSharedSpiHost;
    slot.gpio_cs = kPinSdCardSelect;
    sdspi_dev_handle_t device = 0;
    esp_err_t error = sdspi_host_init_device(&slot, &device);
    if (error != ESP_OK) {
      BN_TRACE_INSTANT(kInstantSdCardError, static_cast<std::uint32_t>(error));
      return StorageStatus::kFailed;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = device;
    error = sdmmc_card_init(&host, &card_);
    if (error != ESP_OK) {
      // Nothing in the slot, or nothing that answers as a card should.
      BN_TRACE_INSTANT(kInstantSdCardError, static_cast<std::uint32_t>(error));
      sdspi_host_remove_device(device);
      return StorageStatus::kAbsent;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(card_.csd.capacity) * card_.csd.sector_size;
    BN_TRACE_INSTANT(kInstantSdCard, static_cast<std::uint32_t>(bytes >> 20U));

    // The file system on it.
    BYTE drive = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&drive) != ESP_OK || drive != 0) {
      sdspi_host_remove_device(device);
      return StorageStatus::kFailed;  // Paths here are written for drive 0.
    }
    ff_diskio_register_sdmmc(drive, &card_);
    const FRESULT result = f_mount(&fileSystem_, "0:", 1);
    if (result != FR_OK) {
      BN_TRACE_INSTANT(kInstantSdMountError, static_cast<std::uint32_t>(result));
      f_mount(nullptr, "0:", 0);
      ff_diskio_unregister(drive);
      sdspi_host_remove_device(device);
      return StorageStatus::kFailed;
    }
    BN_TRACE_INSTANT(kInstantSdFileSystem, fileSystem_.fs_type);
    sdClusterBytes.Set(ClusterBytes(fileSystem_));
    return StorageStatus::kOk;
  }

  StorageStatus Check(std::string_view path) const noexcept {
    if (!started_) {
      return StorageStatus::kAbsent;
    }
    return core::IsStoragePath(path) ? StorageStatus::kOk : StorageStatus::kBadPath;
  }

  // Whether one more file may be opened or directory listed. The lock
  // must be held.
  bool HasRoomForOneMore() const noexcept {
    std::size_t taken = 0;
    for (const OpenFile& file : open_) {
      taken += file.inUse ? 1 : 0;
    }
    for (const ListedDirectory& directory : listed_) {
      taken += directory.inUse ? 1 : 0;
    }
    return taken < kMaxOpenFiles;
  }

  // A free place for an open file, marked as taken; null if there is none.
  OpenFile* Claim() noexcept {
    OpenFile* claimed = nullptr;
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (HasRoomForOneMore()) {
      for (OpenFile& candidate : open_) {
        if (!candidate.inUse) {
          candidate.inUse = true;
          claimed = &candidate;
          break;
        }
      }
    }
    xSemaphoreGive(lock_);
    return claimed;
  }

  // The same, for a directory to be listed.
  ListedDirectory* ClaimDirectory() noexcept {
    ListedDirectory* claimed = nullptr;
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (HasRoomForOneMore()) {
      for (ListedDirectory& candidate : listed_) {
        if (!candidate.inUse) {
          candidate.inUse = true;
          claimed = &candidate;
          break;
        }
      }
    }
    xSemaphoreGive(lock_);
    return claimed;
  }

  void ReleaseDirectory(ListedDirectory& place) noexcept {
    xSemaphoreTake(lock_, portMAX_DELAY);
    place.inUse = false;
    xSemaphoreGive(lock_);
  }

  void Release(OpenFile& place) noexcept {
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (place.map != nullptr) {
      place.map->inUse = false;
      place.map = nullptr;
    }
    place.inUse = false;
    place.writable = false;
    xSemaphoreGive(lock_);
  }

  // The open file `file` names, or null. A file belongs to one task at a
  // time, so nothing is locked to look it up.
  OpenFile* Find(core::FileHandle file) noexcept {
    if (!started_ || file < 0 || static_cast<std::size_t>(file) >= open_.size()) {
      return nullptr;
    }
    OpenFile& found = open_[static_cast<std::size_t>(file)];
    return found.inUse ? &found : nullptr;
  }

  // The same, for a directory being listed.
  ListedDirectory* FindDirectory(core::DirectoryHandle directory) noexcept {
    if (!started_ || directory < 0 || static_cast<std::size_t>(directory) >= listed_.size()) {
      return nullptr;
    }
    ListedDirectory& found = listed_[static_cast<std::size_t>(directory)];
    return found.inUse ? &found : nullptr;
  }

  bool started_ = false;
  bool hostReady_ = false;
  sdmmc_card_t card_ = {};
  FATFS fileSystem_ = {};
  SemaphoreHandle_t lock_ = nullptr;  // Guards which places in open_, listed_ and maps_ are taken.
  StaticSemaphore_t lockStorage_ = {};
  std::array<OpenFile, kMaxOpenFiles> open_ = {};
  std::array<Map, kMaxMapped> maps_ = {};
  std::array<ListedDirectory, core::Storage::kMaxListed> listed_ = {};
};

}  // namespace

core::Storage& SystemStorage() noexcept {
  static SdCardStorage instance;
  return instance;
}

}  // namespace bn::hw
