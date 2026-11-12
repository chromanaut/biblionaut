#include "esp32c3/spi_bus.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include <array>
#include <cstdint>

#include "core/trace.h"
#include "hw/trace_ids.h"

namespace bn::hw {

namespace {

constexpr gpio_num_t kPinClock = GPIO_NUM_8;
constexpr gpio_num_t kPinDataOut = GPIO_NUM_10;
constexpr gpio_num_t kPinDataIn = GPIO_NUM_7;

// What a card answers to the reset command: "idle, no errors".
constexpr std::uint8_t kSdCardIdle = 0x01;
// What is read when nothing answers.
constexpr std::uint8_t kNoReply = 0xFF;

// Tells an SD card to keep off the bus, and returns its reply.
//
// A card starts out in its native mode, in which it has no select line: it
// treats everything on the clock and data-out wires as addressed to it, and
// may answer by driving the data-out wire itself. Pixel data for the panel
// would reach it as a stream of garbled commands. The first step of talking
// to a card over SPI, the reset command sent with its select line low,
// switches it for good to a mode in which it ignores the bus unless
// selected. That one step is all that is done here; using the card comes
// with the storage layer, which starts with the same command.
std::uint8_t ParkSdCard() noexcept {
  // The select line: an output, high (not selected) from the start.
  gpio_set_level(kPinSdCardSelect, 1);
  gpio_config_t select = {};
  select.pin_bit_mask = 1ULL << static_cast<unsigned>(kPinSdCardSelect);
  select.mode = GPIO_MODE_OUTPUT;
  gpio_config(&select);
  gpio_set_level(kPinSdCardSelect, 1);
  // With no card in the slot the data-in wire would float; this makes it
  // read as "no reply".
  gpio_pullup_en(kPinDataIn);

  // Cards must be addressed slowly until they are set up.
  spi_device_interface_config_t config = {};
  config.clock_speed_hz = 400'000;
  config.mode = 0;
  config.spics_io_num = -1;  // The select line is driven by hand, above and below.
  config.queue_size = 1;
  spi_device_handle_t card = nullptr;
  if (spi_bus_add_device(kSharedSpiHost, &config, &card) != ESP_OK) {
    return kNoReply;
  }
  spi_device_acquire_bus(card, portMAX_DELAY);

  const auto exchange = [card](std::uint8_t out) noexcept -> std::uint8_t {
    spi_transaction_t transaction = {};
    transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    transaction.length = 8;
    transaction.tx_data[0] = out;
    if (spi_device_polling_transmit(card, &transaction) != ESP_OK) {
      return kNoReply;
    }
    return transaction.rx_data[0];
  };

  std::uint8_t reply = kNoReply;
  for (int attempt = 0; attempt < 3 && reply != kSdCardIdle; ++attempt) {
    // A card needs some clock pulses, unselected, before its first command.
    for (int i = 0; i < 10; ++i) {
      exchange(0xFF);
    }
    gpio_set_level(kPinSdCardSelect, 0);
    // The reset command (number 0), no argument, and its fixed checksum.
    constexpr std::array<std::uint8_t, 6> kResetCommand = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (const std::uint8_t byte : kResetCommand) {
      exchange(byte);
    }
    // The reply comes within a few bytes; its top bit is clear.
    reply = kNoReply;
    for (int i = 0; i < 8 && (reply & 0x80U) != 0; ++i) {
      reply = exchange(0xFF);
    }
    gpio_set_level(kPinSdCardSelect, 1);
    exchange(0xFF);  // A few more pulses, for the card to let go of the data-in wire.
  }

  spi_device_release_bus(card);
  spi_bus_remove_device(card);
  return reply;
}

}  // namespace

bool StartSharedSpiBus() noexcept {
  // Called from one task, at start-up.
  static bool attempted = false;
  static bool ready = false;
  if (attempted) {
    return ready;
  }
  attempted = true;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = kPinDataOut;
  bus.miso_io_num = kPinDataIn;
  bus.sclk_io_num = kPinClock;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(kSpiMaxTransferBytes);
  if (spi_bus_initialize(kSharedSpiHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    return false;
  }

  const std::uint8_t reply = ParkSdCard();
  BN_TRACE_INSTANT(kInstantSdCardParked, reply);
  ready = true;
  return true;
}

}  // namespace bn::hw
