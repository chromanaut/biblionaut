#include "desktop/device_profile.h"

#include <array>

namespace bn::hw {

namespace {

// Held to be read, an X4 has four buttons in a row under its panel and
// three down its right edge, in these orders. Where along the edges they
// are is drawn near enough, and was not measured.
constexpr std::array<ButtonSpot, 7> kX4Buttons = {{
    {.button = core::Button::kBack, .label = "Back", .edge = Edge::kBottom, .start = 0, .length = 114},
    {.button = core::Button::kConfirm, .label = "Confirm", .edge = Edge::kBottom, .start = 122, .length = 114},
    {.button = core::Button::kLeft, .label = "Left", .edge = Edge::kBottom, .start = 244, .length = 114},
    {.button = core::Button::kRight, .label = "Right", .edge = Edge::kBottom, .start = 366, .length = 114},
    {.button = core::Button::kPower, .label = "Power", .edge = Edge::kRight, .start = 0, .length = 72},
    {.button = core::Button::kUp, .label = "Up", .edge = Edge::kRight, .start = 266, .length = 96},
    {.button = core::Button::kDown, .label = "Down", .edge = Edge::kRight, .start = 370, .length = 96},
}};

constexpr std::array<DeviceProfile, 1> kDevices = {{
    {.name = "x4",
     .panel = {.width = 800, .height = 480},
     // The panel's rows run across the device turned a quarter-turn
     // anticlockwise from the way a book is read on it.
     .turn = ReadingTurn::kClockwise,
     // As measured on an X4.
     .times = {.fullRefreshUs = 3'579'000, .halfRefreshUs = 1'571'000, .fastRefreshUs = 418'000, .powerOffUs = 139'000},
     .buttons = kX4Buttons},
}};

}  // namespace

const DeviceProfile& X4() noexcept { return kDevices[0]; }

std::span<const DeviceProfile> Devices() noexcept { return kDevices; }

const DeviceProfile* FindDevice(std::string_view name) noexcept {
  for (const DeviceProfile& device : kDevices) {
    if (name == device.name) {
      return &device;
    }
  }
  return nullptr;
}

}  // namespace bn::hw
