#include <driver/usb_serial_jtag.h>
#include <esp_err.h>

#include "hw/platform.h"

namespace bn::hw {

namespace {

// Room for several of the largest frames, so a burst of trace does not have
// to wait for the host to read.
constexpr std::uint32_t kTxBufferSize = 2048;
constexpr std::uint32_t kRxBufferSize = 256;

// The chip's built-in USB Serial/JTAG port, through ESP-IDF's driver.
class UsbSerial final : public core::ByteSink {
 public:
  UsbSerial() noexcept {
    if (usb_serial_jtag_is_driver_installed()) {
      ready_ = true;
      return;
    }
    usb_serial_jtag_driver_config_t config = {};
    config.tx_buffer_size = kTxBufferSize;
    config.rx_buffer_size = kRxBufferSize;
    ready_ = usb_serial_jtag_driver_install(&config) == ESP_OK;
  }

  // The driver copies the bytes into its transmit buffer whole or not at
  // all; with no time to wait, it refuses when the buffer is full, which is
  // also what happens when no host is reading.
  [[nodiscard]] bool Write(std::span<const std::uint8_t> bytes) noexcept override {
    if (!ready_) {
      return false;
    }
    if (bytes.empty()) {
      return true;
    }
    const int written = usb_serial_jtag_write_bytes(bytes.data(), bytes.size(), 0);
    return written == static_cast<int>(bytes.size());
  }

 private:
  bool ready_ = false;
};

}  // namespace

core::ByteSink& SystemSerial() noexcept {
  static UsbSerial instance;
  return instance;
}

}  // namespace bn::hw
