#pragma once

#include <driver/spi_master.h>

#include <cstddef>

// The SPI bus that the panel and the SD card share. Both supported devices
// wire it to the same pins.

namespace bn::hw {

inline constexpr spi_host_device_t kSharedSpiHost = SPI2_HOST;

// The most that one transfer carries: a band of 80 rows for the panel.
inline constexpr std::size_t kSpiMaxTransferBytes = 8000;

// Sets the bus up, the first time it is called, and tells an SD card to keep
// off it. Returns false if the bus could not be set up.
[[nodiscard]] bool StartSharedSpiBus() noexcept;

}  // namespace bn::hw
