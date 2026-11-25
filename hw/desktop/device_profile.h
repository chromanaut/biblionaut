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
// stand the way a book is read on the device: the panel's own word for it.
using ReadingTurn = core::PanelTurn;

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
  std::uint64_t grayRefreshUs = 0;  // The gray pass, on a panel with grays.
  std::uint64_t powerOffUs = 0;
};

struct DeviceProfile {
  const char* name = "";      // As BN_DEVICE names it.
  core::DisplaySize panel{};  // In the panel's own orientation.
  ReadingTurn turn = ReadingTurn::kNone;
  // The levels of gray the panel shows (core::Display::Levels()): 2, or 4
  // with a gray pass.
  std::uint32_t levels = 2;
  PanelTimes times;
  // Every spot a button is at. A button wired to more than one spot, as
  // a device with Up and Down on both sides may have them, is listed once
  // for each, and is the same button whichever is pressed.
  std::span<const ButtonSpot> buttons;
  // How many pages of kFontPageBytes of the fonts image can be mapped at
  // once: what the processor's address space has left beside the firmware.
  std::size_t fontPages = 0;
};

// The size of the pages a device maps its flash in.
inline constexpr std::size_t kFontPageBytes = 65536;

// The most spots a device's buttons may be at.
inline constexpr std::size_t kMaxButtonSpots = 16;

// The Xteink X4.
const DeviceProfile& X4() noexcept;

// Every device the desktop can simulate, the X4 first.
std::span<const DeviceProfile> Devices() noexcept;

// The device of that name; null if there is none.
const DeviceProfile* FindDevice(std::string_view name) noexcept;

// The device the BN_DEVICE environment variable names, or the X4 if it
// names none, or none the desktop knows, which is said once.
const DeviceProfile& ChosenDevice() noexcept;

}  // namespace bn::hw
