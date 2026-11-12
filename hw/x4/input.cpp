#include "core/input.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>

#include "core/metrics.h"
#include "core/trace.h"
#include "hw/platform.h"
#include "hw/trace_ids.h"

// The Xteink X4's buttons.
//
// Six of the seven share two analogue pins. Each button connects its pin to
// a different point on a chain of resistors, so which button is down is read
// as a voltage; nothing down reads as the full voltage. Only the power
// button has a pin of its own. A voltage cannot raise an interrupt, so the
// pins are read every few milliseconds by a timer, which runs whatever the
// rest of the firmware is doing, and what the readings show is queued as
// events.
//
// The pins and the voltage levels follow the OpenX4 community SDK (see
// THIRD_PARTY.md), which measured them on real devices.

namespace bn::hw {

namespace {

constexpr adc_channel_t kFrontButtons = ADC_CHANNEL_1;  // GPIO 1: Back, Confirm, Left, Right.
constexpr adc_channel_t kSideButtons = ADC_CHANNEL_2;   // GPIO 2: Up, Down.
constexpr gpio_num_t kPinPower = GPIO_NUM_3;            // Low while the button is down.

// How often the buttons are read. With two readings needed to agree, a
// press is known 5 ms after it is first seen, and is dated from then.
constexpr std::uint64_t kReadPeriodUs = 5'000;

// Events waiting to be collected. A press and its release are two.
constexpr UBaseType_t kQueueLength = 16;

// A reading above this (of 4095) means no button on the pin is down.
constexpr int kNothingDown = 3900;

// One step of a pin's chain: readings above `above`, and below the step
// before, are this button.
struct Level {
  int above;
  core::Button button;
};

// Readings measured with each button down: Back about 3510, Confirm 2690,
// Left 1490, Right 5; Up about 2240, Down 5. The boundaries are half-way
// between neighbours.
constexpr std::array<Level, 4> kFrontLevels = {{
    {3100, core::Button::kBack},
    {2090, core::Button::kConfirm},
    {750, core::Button::kLeft},
    {INT_MIN, core::Button::kRight},
}};
constexpr std::array<Level, 2> kSideLevels = {{
    {1120, core::Button::kUp},
    {INT_MIN, core::Button::kDown},
}};

// Events lost because nothing collected them in time.
core::Counter inputDropped{"input_dropped"};

template <std::size_t Count>
core::ButtonSet ButtonAt(int reading, const std::array<Level, Count>& levels) noexcept {
  if (reading > kNothingDown) {
    return 0;
  }
  for (const Level& level : levels) {
    if (reading > level.above) {
      return core::ButtonBit(level.button);
    }
  }
  return 0;
}

class X4Input final : public core::Input {
 public:
  [[nodiscard]] bool Start() noexcept override {
    if (started_) {
      return true;
    }
    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = ADC_UNIT_1;
    // The widest input range, which the chains' voltages need.
    adc_oneshot_chan_cfg_t channel = {};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_ == nullptr && adc_oneshot_new_unit(&unit, &adc_) != ESP_OK) {
      adc_ = nullptr;
      return false;
    }
    if (adc_oneshot_config_channel(adc_, kFrontButtons, &channel) != ESP_OK ||
        adc_oneshot_config_channel(adc_, kSideButtons, &channel) != ESP_OK) {
      return false;
    }

    gpio_config_t power = {};
    power.pin_bit_mask = 1ULL << static_cast<unsigned>(kPinPower);
    power.mode = GPIO_MODE_INPUT;
    power.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&power);

    if (queue_ == nullptr) {
      queue_ = xQueueCreateStatic(kQueueLength, sizeof(core::InputEvent), queueStorage_.data(), &queueControl_);
    }
    esp_timer_create_args_t timer = {};
    timer.callback = &OnTimer;
    timer.arg = this;
    timer.dispatch_method = ESP_TIMER_TASK;
    timer.name = "buttons";
    // If the chip was asleep through several periods, one reading on waking
    // is enough.
    timer.skip_unhandled_events = true;
    if (queue_ == nullptr || (timer_ == nullptr && esp_timer_create(&timer, &timer_) != ESP_OK) ||
        esp_timer_start_periodic(timer_, kReadPeriodUs) != ESP_OK) {
      return false;
    }
    started_ = true;
    return true;
  }

  [[nodiscard]] bool Next(core::InputEvent& event) noexcept override {
    return queue_ != nullptr && xQueueReceive(queue_, &event, 0) == pdTRUE;
  }

 private:
  static void OnTimer(void* self) noexcept { static_cast<X4Input*>(self)->Read(); }

  // In the timer's task, every kReadPeriodUs.
  void Read() noexcept {
    const auto now = static_cast<std::uint64_t>(esp_timer_get_time());
    core::ButtonSet reading = ReadPin(kFrontButtons, kFrontLevels) | ReadPin(kSideButtons, kSideLevels);
    if (gpio_get_level(kPinPower) == 0) {
      reading |= core::ButtonBit(core::Button::kPower);
    }

    for (const core::InputEvent& event : sampler_.Sample(reading, now)) {
      // From the reading that first showed the change to this one, which
      // settled it. Trace times are the low half of the same clock.
      if (event.kind == core::InputEventKind::kPress) {
        BN_TRACE_TOP_SPAN_SINCE(kSpanButtonPress, static_cast<std::uint32_t>(event.timeUs),
                                static_cast<std::uint32_t>(event.button));
      } else if (event.kind == core::InputEventKind::kRelease) {
        BN_TRACE_TOP_SPAN_SINCE(kSpanButtonRelease, static_cast<std::uint32_t>(event.timeUs),
                                static_cast<std::uint32_t>(event.button));
      }
      if (xQueueSend(queue_, &event, 0) != pdTRUE) {
        inputDropped.Add();
      }
    }
  }

  // Which button on one pin is down. A reading that fails counts as none;
  // one such reading alone changes nothing, since two must agree.
  template <std::size_t Count>
  core::ButtonSet ReadPin(adc_channel_t pin, const std::array<Level, Count>& levels) noexcept {
    int reading = 0;
    if (adc_oneshot_read(adc_, pin, &reading) != ESP_OK) {
      return 0;
    }
    return ButtonAt(reading, levels);
  }

  adc_oneshot_unit_handle_t adc_ = nullptr;
  esp_timer_handle_t timer_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  StaticQueue_t queueControl_ = {};
  std::array<std::uint8_t, kQueueLength * sizeof(core::InputEvent)> queueStorage_{};
  core::InputSampler sampler_;
  bool started_ = false;
};

}  // namespace

core::Input& SystemInput() noexcept {
  static X4Input instance;
  return instance;
}

}  // namespace bn::hw
