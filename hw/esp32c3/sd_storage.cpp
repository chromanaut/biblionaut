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

constexpr std::size_t kMaxOpenFiles = 4;  // CONFIG_FATFS_FS_LOCK is set to match.

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
    const bool create = mode == core::OpenMode::kCreate;
    const FatPath name(path);
    const FRESULT result =
        f_open(&place->file, name.Text(), create ? static_cast<BYTE>(FA_READ | FA_WRITE | FA_CREATE_ALWAYS) : FA_READ);
    if (result != FR_OK) {
      Release(*place);
      return FromFatFs(result);
    }
    place->writable = create;
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
      info = {.size = 0, .isDirectory = true};  // FatFS has no entry for the top directory.
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

 private:
  struct OpenFile {
    FIL file;
    bool inUse = false;
    bool writable = false;
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
    return StorageStatus::kOk;
  }

  StorageStatus Check(std::string_view path) const noexcept {
    if (!started_) {
      return StorageStatus::kAbsent;
    }
    return core::IsStoragePath(path) ? StorageStatus::kOk : StorageStatus::kBadPath;
  }

  // A free place for an open file, marked as taken; null if there is none.
  OpenFile* Claim() noexcept {
    OpenFile* claimed = nullptr;
    xSemaphoreTake(lock_, portMAX_DELAY);
    for (OpenFile& candidate : open_) {
      if (!candidate.inUse) {
        candidate.inUse = true;
        claimed = &candidate;
        break;
      }
    }
    xSemaphoreGive(lock_);
    return claimed;
  }

  void Release(OpenFile& place) noexcept {
    xSemaphoreTake(lock_, portMAX_DELAY);
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

  bool started_ = false;
  bool hostReady_ = false;
  sdmmc_card_t card_ = {};
  FATFS fileSystem_ = {};
  SemaphoreHandle_t lock_ = nullptr;  // Guards which places in open_ are taken.
  StaticSemaphore_t lockStorage_ = {};
  std::array<OpenFile, kMaxOpenFiles> open_ = {};
};

}  // namespace

core::Storage& SystemStorage() noexcept {
  static SdCardStorage instance;
  return instance;
}

}  // namespace bn::hw
