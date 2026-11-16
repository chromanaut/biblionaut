#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/display.h"
#include "core/input.h"

// The devices the desktop can simulate, each described once: its panel,
// how long the panel takes over what it does, how the device is held to be
// read, and its buttons. What the desktop's panel (SimDisplay) and window
// (PanelView, DeviceLayout) do for a device comes from here.

namespace bn::hw {

// How the panel's picture is turned, from the panel's own orientation, to
// stand the way a book is read on the device.
enum class ReadingTurn : std::uint8_t {
  kNone = 0,
  kClockwise = 1,      // A quarter-turn clockwise.
  kHalf = 2,           // Upside down.
  kAnticlockwise = 3,  // A quarter-turn anticlockwise.
};

// An edge of the device, held the way a book is read on it.
enum class Edge : std::uint8_t { kTop, kRight, kBottom, kLeft };

// Where a button is: on `edge`, `length` long, from `start` along it. Both
// are in the panel's pixels, measured from the panel's top along the left
// and right edges, and from its left along the top and bottom.
struct ButtonSpot {
  core::Button button = core::Button::kBack;
  const char* label = "";  // Drawn on it, in 8 by 8 letters.
  Edge edge = Edge::kBottom;
  int start = 0;
  int length = 0;
};

// How long the panel stays busy over each thing it does.
struct PanelTimes {
  std::uint64_t fullRefreshUs = 0;
  std::uint64_t halfRefreshUs = 0;
  std::uint64_t fastRefreshUs = 0;
  std::uint64_t powerOffUs = 0;
};

struct DeviceProfile {
  const char* name = "";      // As BN_DEVICE names it.
  core::DisplaySize panel{};  // In the panel's own orientation.
  ReadingTurn turn = ReadingTurn::kNone;
  PanelTimes times;
  // Every spot a button is at. A button wired to more than one spot, as
  // a device with Up and Down on both sides may have them, is listed once
  // for each, and is the same button whichever is pressed.
  std::span<const ButtonSpot> buttons;
};

// The most spots a device's buttons may be at.
inline constexpr std::size_t kMaxButtonSpots = 16;

// The Xteink X4.
const DeviceProfile& X4() noexcept;

// Every device the desktop can simulate, the X4 first.
std::span<const DeviceProfile> Devices() noexcept;

// The device of that name; null if there is none.
const DeviceProfile* FindDevice(std::string_view name) noexcept;

}  // namespace bn::hw
