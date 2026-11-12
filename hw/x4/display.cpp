#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_memory_utils.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/trace.h"
#include "esp32c3/spi_bus.h"
#include "hw/platform.h"
#include "hw/trace_ids.h"

// The Xteink X4's panel: 800 x 480 pixels, black and white, driven by an
// SSD1677 controller over SPI.
//
// The controller keeps two images of the screen in its own memory. This
// driver writes rows of pixels into either, tells the controller to refresh
// the panel from them, and watches the controller's BUSY line to know when
// it has finished. It holds no copy of the screen itself.
//
// The panel's maker has set the controller to swap the roles of its two
// memories after every fast refresh ("RAM ping-pong" in its datasheet), and
// switching the drive voltages off was found to put them back. The driver
// passes that on as it is: core/display.h says what it means for whoever
// drives the panel. The commands to write and read a plane go by role, so
// nothing here needs to know which memory is which.
//
// The command numbers and their meanings are the controller's own. The
// values sent while setting the panel up, and the order of the steps, follow
// the OpenX4 community SDK (see THIRD_PARTY.md), which worked them out for
// this panel.

namespace bn::hw {

namespace {

constexpr gpio_num_t kPinSelect = GPIO_NUM_21;
constexpr gpio_num_t kPinDataNotCommand = GPIO_NUM_4;
constexpr gpio_num_t kPinReset = GPIO_NUM_5;
constexpr gpio_num_t kPinBusy = GPIO_NUM_6;  // High while the controller is working.

constexpr core::DisplaySize kSize{.width = 800, .height = 480};
constexpr int kClockHz = 40'000'000;
// The controller answers far more slowly than it listens: 2.5 MHz at most.
constexpr int kReadClockHz = 1'000'000;

// Controller commands.
constexpr std::uint8_t kCmdDriverOutputControl = 0x01;
constexpr std::uint8_t kCmdBoosterSoftStart = 0x0C;
constexpr std::uint8_t kCmdDeepSleep = 0x10;
constexpr std::uint8_t kCmdDataEntryMode = 0x11;
constexpr std::uint8_t kCmdSoftReset = 0x12;
constexpr std::uint8_t kCmdTemperatureSensor = 0x18;
constexpr std::uint8_t kCmdWriteTemperature = 0x1A;
constexpr std::uint8_t kCmdActivate = 0x20;
constexpr std::uint8_t kCmdUpdateControl1 = 0x21;
constexpr std::uint8_t kCmdUpdateControl2 = 0x22;
constexpr std::uint8_t kCmdWriteNewPlane = 0x24;
constexpr std::uint8_t kCmdWriteOldPlane = 0x26;
constexpr std::uint8_t kCmdReadPlane = 0x27;
constexpr std::uint8_t kCmdReadDisplayOptions = 0x2D;
constexpr std::uint8_t kCmdReadStatus = 0x2F;
constexpr std::uint8_t kCmdBorderWaveform = 0x3C;
constexpr std::uint8_t kCmdPlaneToRead = 0x41;
constexpr std::uint8_t kCmdRowAddressRange = 0x45;
constexpr std::uint8_t kCmdColumnAddressRange = 0x44;
constexpr std::uint8_t kCmdFillPlaneA = 0x46;
constexpr std::uint8_t kCmdFillPlaneB = 0x47;
constexpr std::uint8_t kCmdColumnAddress = 0x4E;
constexpr std::uint8_t kCmdRowAddress = 0x4F;

// What an activation does, as bits of "update control 2". The controller
// carries out the steps that are set, in this order. (Plain unsigned, so
// that combining them stays unsigned arithmetic.)
constexpr unsigned kStepClockOn = 0x80;
constexpr unsigned kStepAnalogOn = 0x40;  // The panel's drive voltages.
constexpr unsigned kStepLoadTemperature = 0x20;
constexpr unsigned kStepLoadWaveform = 0x10;
constexpr unsigned kStepDifferential = 0x08;  // Refresh by comparing the two planes.
constexpr unsigned kStepRefresh = 0x04;
constexpr unsigned kStepAnalogOff = 0x02;
constexpr unsigned kStepClockOff = 0x01;

// "Update control 1": treat the old-image plane as blank.
constexpr std::uint8_t kIgnoreOldPlane = 0x40;
constexpr std::uint8_t kUseBothPlanes = 0x00;

// The temperature written before a half refresh, in degrees: telling the
// controller the panel is hot makes it pick its shortest waveform.
constexpr std::uint8_t kHotTemperature = 0x5A;

// A panel that has not reported back after this long never will.
constexpr std::int64_t kMaxBusyUs = 30'000'000;

constexpr std::uint8_t LowByte(std::uint16_t value) noexcept { return static_cast<std::uint8_t>(value); }
constexpr std::uint8_t HighByte(std::uint16_t value) noexcept { return static_cast<std::uint8_t>(value >> 8U); }

// At least `milliseconds`, letting other tasks run meanwhile. A delay of n
// ticks ends at the n-th tick from now, which may be only just more than
// n - 1 tick periods away; hence the one extra.
void SleepAtLeastMs(std::uint32_t milliseconds) noexcept { vTaskDelay((milliseconds / portTICK_PERIOD_MS) + 1); }

class X4Display final : public core::Display {
 public:
  core::DisplaySize Size() const noexcept override { return kSize; }

  [[nodiscard]] bool Start() noexcept override {
    const std::uint32_t began = BN_TRACE_NOW();
    std::uint32_t busyCycles = 0;
    started_ = SetUp(busyCycles);
    analogOn_ = false;
    // The count says how many of the three set-up steps the panel was seen
    // to work on: fewer than three means its BUSY line was not seen moving.
    BN_TRACE_TOP_SPAN_SINCE(kSpanPanelStart, began, busyCycles);
    return started_;
  }

  [[nodiscard]] bool WriteRows(core::DisplayPlane plane, std::uint16_t firstRow,
                               std::span<const std::uint8_t> rows) noexcept override {
    const std::size_t rowBytes = kSize.RowBytes();
    // The controller takes nothing while it is busy: what is sent then is
    // lost without a sign.
    if (!started_ || Busy() || rows.empty() || rows.size() % rowBytes != 0 ||
        firstRow + (rows.size() / rowBytes) > kSize.height) {
      return false;
    }
    BN_TRACE_SCOPE_ARG(kSpanPanelWrite, static_cast<std::uint32_t>(rows.size()));
    if (spi_device_acquire_bus(device_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    const bool sent = SetRows(firstRow, static_cast<std::uint16_t>(rows.size() / rowBytes)) &&
                      SendCommand(plane == core::DisplayPlane::kNew ? kCmdWriteNewPlane : kCmdWriteOldPlane) &&
                      SendData(rows);
    spi_device_release_bus(device_);
    return sent;
  }

  [[nodiscard]] bool ReadRows(core::DisplayPlane plane, std::uint16_t firstRow,
                              std::span<std::uint8_t> rows) noexcept override {
    const std::size_t rowBytes = kSize.RowBytes();
    if (!started_ || Busy() || rows.empty() || rows.size() % rowBytes != 0 ||
        firstRow + (rows.size() / rowBytes) > kSize.height || !AttachReader()) {
      return false;
    }
    BN_TRACE_SCOPE_ARG(kSpanPanelRead, static_cast<std::uint32_t>(rows.size()));
    if (spi_device_acquire_bus(reader_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    // Everything goes at the reading rate until the bus is given back.
    active_ = reader_;
    if (!selfReported_) {
      selfReported_ = true;
      ReportSelf();
    }
    // The same rows, in the same order, as a write to them would cover. The
    // first byte the controller sends back means nothing.
    const bool read = SetRows(firstRow, static_cast<std::uint16_t>(rows.size() / rowBytes)) &&
                      Command(kCmdPlaneToRead, static_cast<std::uint8_t>(plane == core::DisplayPlane::kNew ? 0 : 1)) &&
                      Receive(kCmdReadPlane, 1, rows);
    active_ = device_;
    spi_device_release_bus(reader_);
    return read;
  }

  [[nodiscard]] bool StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept override {
    if (!started_ || Busy()) {
      return false;
    }
    unsigned steps = 0;
    core::SpanId span = kSpanRefreshFull;
    switch (mode) {
      case core::RefreshMode::kFull:
        steps = kStepLoadTemperature | kStepLoadWaveform | kStepRefresh;
        break;
      case core::RefreshMode::kHalf:
        steps = kStepClockOn | kStepAnalogOn | kStepLoadWaveform | kStepRefresh;
        span = kSpanRefreshHalf;
        break;
      case core::RefreshMode::kFast:
        steps = kStepLoadWaveform | kStepDifferential | kStepRefresh;
        span = kSpanRefreshFast;
        break;
    }
    if (!analogOn_) {
      steps |= kStepClockOn | kStepAnalogOn;
    }
    const bool powerOff = after == core::AfterRefresh::kPowerOff;
    if (powerOff) {
      steps |= kStepAnalogOff | kStepClockOff;
    }

    if (spi_device_acquire_bus(device_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    bool sent = Command(kCmdUpdateControl1, mode == core::RefreshMode::kFast ? kUseBothPlanes : kIgnoreOldPlane);
    if (mode == core::RefreshMode::kHalf) {
      sent = sent && Command(kCmdWriteTemperature, kHotTemperature);
    }
    sent = sent && Command(kCmdUpdateControl2, static_cast<std::uint8_t>(steps));
    if (sent) {
      ExpectBusy(span, powerOff ? 1 : 0);
      sent = Command(kCmdActivate);
      if (!sent) {
        expectingIdle_ = false;
      }
    }
    spi_device_release_bus(device_);
    if (sent) {
      analogOn_ = !powerOff;
    }
    return sent;
  }

  bool Busy() noexcept override {
    if (expectingIdle_) {
      if (esp_timer_get_time() - busySinceUs_ <= kMaxBusyUs) {
        return true;
      }
      expectingIdle_ = false;  // It is not going to report back.
    }
    return gpio_get_level(kPinBusy) != 0;
  }

  [[nodiscard]] bool PowerOff() noexcept override {
    if (!started_ || Busy()) {
      return false;
    }
    if (!analogOn_) {
      return true;
    }
    if (spi_device_acquire_bus(device_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    const bool sent = SendPowerOff();
    spi_device_release_bus(device_);
    return sent;
  }

  [[nodiscard]] bool Sleep() noexcept override {
    if (Busy()) {
      return false;
    }
    if (!started_) {
      return true;
    }
    if (spi_device_acquire_bus(device_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    if (analogOn_ && SendPowerOff()) {
      WaitUntilIdle(2000);
    }
    // Deep sleep, keeping the planes. Only a reset wakes the controller.
    Command(kCmdDeepSleep, std::uint8_t{0x01});
    spi_device_release_bus(device_);
    started_ = false;
    analogOn_ = false;
    return true;
  }

 private:
  // Connects to the bus and the panel's pins: once, on the first Start().
  bool Attach() noexcept {
    if (attached_) {
      return true;
    }
    if (!StartSharedSpiBus()) {
      return false;
    }
    spi_device_interface_config_t config = {};
    config.clock_speed_hz = kClockHz;
    config.mode = 0;
    // The select line is driven by hand: it has to stay low across the
    // several transfers that make up one stretch of pixel data.
    config.spics_io_num = -1;
    config.queue_size = 1;
    if (spi_bus_add_device(kSharedSpiHost, &config, &device_) != ESP_OK) {
      return false;
    }
    active_ = device_;

    gpio_set_level(kPinSelect, 1);
    gpio_set_level(kPinDataNotCommand, 1);
    gpio_set_level(kPinReset, 1);
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << static_cast<unsigned>(kPinSelect)) |
                           (1ULL << static_cast<unsigned>(kPinDataNotCommand)) |
                           (1ULL << static_cast<unsigned>(kPinReset));
    outputs.mode = GPIO_MODE_OUTPUT;
    gpio_config(&outputs);

    // The interrupt fires when BUSY falls: the controller has finished.
    gpio_config_t busy = {};
    busy.pin_bit_mask = 1ULL << static_cast<unsigned>(kPinBusy);
    busy.mode = GPIO_MODE_INPUT;
    busy.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&busy);
    idle_ = xSemaphoreCreateBinaryStatic(&idleStorage_);
    const esp_err_t service = gpio_install_isr_service(0);
    if (idle_ == nullptr || (service != ESP_OK && service != ESP_ERR_INVALID_STATE) ||
        gpio_isr_handler_add(kPinBusy, &OnBusyFell, this) != ESP_OK) {
      return false;
    }
    attached_ = true;
    return true;
  }

  // A second way onto the same wires, for reading back: the controller
  // answers on the wire it is sent data on, one direction at a time, and
  // slowly. It is set up the first time something is read, so a firmware
  // that never reads is not changed by it.
  bool AttachReader() noexcept {
    if (reader_ != nullptr) {
      return true;
    }
    spi_device_interface_config_t config = {};
    config.clock_speed_hz = kReadClockHz;
    config.mode = 0;
    config.spics_io_num = -1;
    config.queue_size = 1;
    config.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE;
    return spi_bus_add_device(kSharedSpiHost, &config, &reader_) == ESP_OK;
  }

  // Records what the controller says of itself. The reading device must
  // hold the bus.
  void ReportSelf() noexcept {
    std::array<std::uint8_t, 1> status{};
    if (Receive(kCmdReadStatus, 0, status)) {
      BN_TRACE_INSTANT(kInstantPanelStatus, status[0]);
    }
    std::array<std::uint8_t, 12> options{};
    if (Receive(kCmdReadDisplayOptions, 0, options)) {
      for (std::size_t i = 0; i < options.size(); i += 4) {
        BN_TRACE_INSTANT(kInstantPanelOptions, (static_cast<std::uint32_t>(options[i]) << 24U) |
                                                   (static_cast<std::uint32_t>(options[i + 1]) << 16U) |
                                                   (static_cast<std::uint32_t>(options[i + 2]) << 8U) | options[i + 3]);
      }
    }
  }

  // Starts switching the drive voltages off, which the controller does as an
  // activation with only the "off" steps set. The bus must be held.
  bool SendPowerOff() noexcept {
    if (!Command(kCmdUpdateControl1, kIgnoreOldPlane) ||
        !Command(kCmdUpdateControl2, static_cast<std::uint8_t>(kStepAnalogOff | kStepClockOff))) {
      return false;
    }
    ExpectBusy(kSpanPanelPowerOff, 0);
    if (!Command(kCmdActivate)) {
      expectingIdle_ = false;
      return false;
    }
    analogOn_ = false;
    return true;
  }

  // Resets the controller and configures it for this panel, leaving both
  // planes white. `busyCycles` counts the steps the panel was seen to do.
  bool SetUp(std::uint32_t& busyCycles) noexcept {
    if (!Attach()) {
      return false;
    }
    // A pulse on the reset line. It wakes the controller from deep sleep,
    // and stops whatever it was doing: the panel may be in the middle of a
    // refresh begun before this processor was last restarted.
    expectingIdle_ = false;
    gpio_set_level(kPinReset, 1);
    SleepAtLeastMs(20);
    gpio_set_level(kPinReset, 0);
    esp_rom_delay_us(2000);
    gpio_set_level(kPinReset, 1);
    SleepAtLeastMs(20);

    if (spi_device_acquire_bus(device_, portMAX_DELAY) != ESP_OK) {
      return false;
    }
    ExpectBusy(kSpanPanelReset, 0);
    bool sent = Command(kCmdSoftReset);
    busyCycles += WaitUntilIdle(1000) ? 1 : 0;

    const auto lastRow = static_cast<std::uint16_t>(kSize.height - 1);
    sent = sent &&
           // Use the controller's own temperature sensor.
           Command(kCmdTemperatureSensor, std::uint8_t{0x80}) &&
           // How gently the drive voltages are brought up: values for this panel.
           Command(kCmdBoosterSoftStart, std::uint8_t{0xAE}, std::uint8_t{0xC7}, std::uint8_t{0xC3}, std::uint8_t{0xC0},
                   std::uint8_t{0x40}) &&
           // The number of rows, and the order they are scanned in.
           Command(kCmdDriverOutputControl, LowByte(lastRow), HighByte(lastRow), std::uint8_t{0x02}) &&
           Command(kCmdBorderWaveform, std::uint8_t{0x01}) && SetRows(0, kSize.height);

    // Fill both planes with white, which the controller can do by itself.
    constexpr std::uint8_t kAllWhite = 0xF7;
    for (const std::uint8_t fill : {kCmdFillPlaneA, kCmdFillPlaneB}) {
      if (sent) {
        ExpectBusy(kSpanPanelClear, 0);
        sent = Command(fill, kAllWhite);
        busyCycles += WaitUntilIdle(5000) ? 1 : 0;
      }
    }
    expectingIdle_ = false;
    spi_device_release_bus(device_);
    return sent;
  }

  // Points the controller at `rowCount` whole rows starting at `firstRow`,
  // so that the pixel data that follows lands there.
  bool SetRows(std::uint16_t firstRow, std::uint16_t rowCount) noexcept {
    // The panel is mounted with its rows in the opposite order to the
    // controller's memory, so row 0 is the highest address and the address
    // counts down as rows are written.
    const auto first = static_cast<std::uint16_t>(kSize.height - 1 - firstRow);
    const auto last = static_cast<std::uint16_t>(first - (rowCount - 1));
    const auto lastColumn = static_cast<std::uint16_t>(kSize.width - 1);
    constexpr std::uint8_t kColumnsUpRowsDown = 0x01;
    return Command(kCmdDataEntryMode, kColumnsUpRowsDown) &&
           Command(kCmdColumnAddressRange, std::uint8_t{0}, std::uint8_t{0}, LowByte(lastColumn),
                   HighByte(lastColumn)) &&
           Command(kCmdRowAddressRange, LowByte(first), HighByte(first), LowByte(last), HighByte(last)) &&
           Command(kCmdColumnAddress, std::uint8_t{0}, std::uint8_t{0}) &&
           Command(kCmdRowAddress, LowByte(first), HighByte(first));
  }

  // A command and its parameter bytes. The bus must be held.
  template <typename... Parameters>
  bool Command(std::uint8_t command, Parameters... parameters) noexcept {
    if (!SendCommand(command)) {
      return false;
    }
    if constexpr (sizeof...(Parameters) > 0) {
      const std::array<std::uint8_t, sizeof...(Parameters)> bytes{parameters...};
      return SendData(bytes);
    }
    return true;
  }

  bool SendCommand(std::uint8_t command) noexcept {
    spi_transaction_t transaction = {};
    transaction.flags = SPI_TRANS_USE_TXDATA;
    transaction.length = 8;
    transaction.tx_data[0] = command;
    gpio_set_level(kPinDataNotCommand, 0);
    gpio_set_level(kPinSelect, 0);
    const esp_err_t result = spi_device_polling_transmit(active_, &transaction);
    gpio_set_level(kPinSelect, 1);
    return result == ESP_OK;
  }

  bool SendData(std::span<const std::uint8_t> data) noexcept {
    gpio_set_level(kPinDataNotCommand, 1);
    gpio_set_level(kPinSelect, 0);
    bool sent = true;
    while (sent && !data.empty()) {
      spi_transaction_t transaction = {};
      std::size_t count = 0;
      if (data.size() <= sizeof(transaction.tx_data)) {
        // A few bytes travel inside the transaction itself.
        count = data.size();
        transaction.flags = SPI_TRANS_USE_TXDATA;
        std::memcpy(transaction.tx_data, data.data(), count);
      } else {
        // More go by DMA, which needs memory it can reach, at an aligned
        // address. Data that qualifies is sent from where it is. The SPI
        // driver would copy anything else into memory taken from the heap;
        // to keep off the heap it is copied through a small buffer here.
        const bool direct =
            esp_ptr_dma_capable(data.data()) && (reinterpret_cast<std::uintptr_t>(data.data()) % 4) == 0;
        count = std::min(data.size(), direct ? kSpiMaxTransferBytes : bounce_.size());
        count -= count % 4;  // The odd bytes left over go in the next round, inside the transaction.
        if (direct) {
          transaction.tx_buffer = data.data();
        } else {
          std::memcpy(bounce_.data(), data.data(), count);
          transaction.tx_buffer = bounce_.data();
        }
        transaction.flags = SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;
      }
      transaction.length = count * 8;
      sent = spi_device_polling_transmit(active_, &transaction) == ESP_OK;
      data = data.subspan(count);
    }
    gpio_set_level(kPinSelect, 1);
    return sent;
  }

  // Sends `command` and takes the controller's answer: `skip` bytes that
  // mean nothing, then enough to fill `out`. The select line stays low from
  // the command to the last byte, which is how the controller knows the
  // answer is wanted. The reading device must hold the bus.
  bool Receive(std::uint8_t command, std::size_t skip, std::span<std::uint8_t> out) noexcept {
    spi_transaction_t send = {};
    send.flags = SPI_TRANS_USE_TXDATA;
    send.length = 8;
    send.tx_data[0] = command;
    gpio_set_level(kPinDataNotCommand, 0);
    gpio_set_level(kPinSelect, 0);
    bool received = spi_device_polling_transmit(reader_, &send) == ESP_OK;
    gpio_set_level(kPinDataNotCommand, 1);
    for (; received && skip > 0; --skip) {
      spi_transaction_t discard = {};
      discard.flags = SPI_TRANS_USE_RXDATA;
      discard.rxlength = 8;
      received = spi_device_polling_transmit(reader_, &discard) == ESP_OK;
    }
    while (received && !out.empty()) {
      spi_transaction_t transaction = {};
      std::size_t count = 0;
      if (out.size() <= sizeof(transaction.rx_data)) {
        // A few bytes arrive inside the transaction itself.
        count = out.size();
        transaction.flags = SPI_TRANS_USE_RXDATA;
        transaction.rxlength = count * 8;
        received = spi_device_polling_transmit(reader_, &transaction) == ESP_OK;
        std::memcpy(out.data(), transaction.rx_data, count);
      } else {
        // More come by DMA, through the small buffer, for the reasons given
        // in SendData().
        count = std::min(out.size(), bounce_.size());
        count -= count % 4;
        transaction.rx_buffer = bounce_.data();
        transaction.rxlength = count * 8;
        received = spi_device_polling_transmit(reader_, &transaction) == ESP_OK;
        std::memcpy(out.data(), bounce_.data(), count);
      }
      out = out.subspan(count);
    }
    gpio_set_level(kPinSelect, 1);
    return received;
  }

  // Call just before sending something that makes the controller busy: the
  // interrupt will record `span`, from now until BUSY falls.
  void ExpectBusy(core::SpanId span, std::uint32_t argument) noexcept {
    xSemaphoreTake(idle_, 0);  // Forget any earlier signal.
    busySpan_ = span;
    busyArgument_ = argument;
    busySinceTrace_ = BN_TRACE_NOW();
    busySinceUs_ = esp_timer_get_time();
    expectingIdle_ = true;
  }

  // Waits for what ExpectBusy() announced to finish; for the short pauses of
  // setting up and powering off. Returns true if the controller was seen to
  // go busy and finish, false if it never went busy or did not finish in
  // time.
  bool WaitUntilIdle(std::uint32_t timeoutMs) noexcept {
    // BUSY rises within microseconds of the command; allow it 2 ms.
    for (int i = 0; i < 20 && expectingIdle_ && gpio_get_level(kPinBusy) == 0; ++i) {
      esp_rom_delay_us(100);
    }
    if (gpio_get_level(kPinBusy) == 0) {
      // Either it has already been and gone, which the interrupt will have
      // noted, or it never came.
      const bool finished = !expectingIdle_;
      expectingIdle_ = false;
      return finished;
    }
    const bool finished = xSemaphoreTake(idle_, pdMS_TO_TICKS(timeoutMs) + 1) == pdTRUE;
    expectingIdle_ = false;
    return finished;
  }

  static void OnBusyFell(void* context) noexcept { static_cast<X4Display*>(context)->HandleBusyFell(); }

  // In the interrupt handler.
  void HandleBusyFell() noexcept {
    if (!expectingIdle_) {
      return;
    }
    expectingIdle_ = false;
    BN_TRACE_TOP_SPAN_SINCE(busySpan_, busySinceTrace_, busyArgument_);
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(idle_, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }

  spi_device_handle_t device_ = nullptr;
  spi_device_handle_t reader_ = nullptr;  // For reading back: see AttachReader().
  spi_device_handle_t active_ = nullptr;  // The one of the two that commands and data now go through.
  bool selfReported_ = false;
  SemaphoreHandle_t idle_ = nullptr;  // Given by the interrupt when BUSY falls.
  StaticSemaphore_t idleStorage_ = {};
  bool attached_ = false;
  bool started_ = false;
  bool analogOn_ = false;  // The panel's drive voltages are up.

  // Shared with the interrupt handler.
  volatile bool expectingIdle_ = false;
  core::SpanId busySpan_{};
  std::uint32_t busyArgument_ = 0;
  std::uint32_t busySinceTrace_ = 0;
  std::int64_t busySinceUs_ = 0;

  alignas(4) std::array<std::uint8_t, 256> bounce_{};
};

}  // namespace

core::Display& SystemDisplay() noexcept {
  static X4Display instance;
  return instance;
}

}  // namespace bn::hw
