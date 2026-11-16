#include "desktop/device_layout.h"

namespace bn::hw {

namespace {

// The device's edge around the panel, the gap between the panel and a row
// of buttons, and how deep a row is. A button is inset in its row, and wide
// or deep enough for the longest label, "Confirm", 56 units across.
constexpr int kEdge = 16;
constexpr int kGap = 8;
constexpr int kRow = 72;
constexpr int kInset = 6;
constexpr int kButtonGap = 8;
// How long the buttons down the side are.
constexpr int kPowerLength = 72;
constexpr int kSideLength = 96;

}  // namespace

DeviceLayout::DeviceLayout(core::DisplaySize panel, ViewTurn turn) noexcept {
  // Laid out held as a book is read, with the panel turned to stand upright.
  const int width = panel.height;
  const int height = panel.width;
  panel_ = Rect{.x = kEdge, .y = kEdge, .width = width, .height = height};

  // The row under the panel.
  const int front = (width - (3 * kButtonGap)) / 4;
  const int frontY = kEdge + height + kGap + kInset;
  const int deep = kRow - (2 * kInset);
  const auto inRow = [&](int i) {
    return Rect{.x = kEdge + (i * (front + kButtonGap)), .y = frontY, .width = front, .height = deep};
  };
  // Down the right edge: Power at the top, Up and Down a third of the way down.
  const int sideX = kEdge + width + kGap + kInset;
  const int up = kEdge + (height / 3);
  buttons_ = {{
      {.button = core::Button::kBack, .label = "Back", .rect = inRow(0)},
      {.button = core::Button::kConfirm, .label = "Confirm", .rect = inRow(1)},
      {.button = core::Button::kLeft, .label = "Left", .rect = inRow(2)},
      {.button = core::Button::kRight, .label = "Right", .rect = inRow(3)},
      {.button = core::Button::kPower,
       .label = "Power",
       .rect = {.x = sideX, .y = kEdge, .width = deep, .height = kPowerLength}},
      {.button = core::Button::kUp, .label = "Up", .rect = {.x = sideX, .y = up, .width = deep, .height = kSideLength}},
      {.button = core::Button::kDown,
       .label = "Down",
       .rect = {.x = sideX, .y = up + kSideLength + kButtonGap, .width = deep, .height = kSideLength}},
  }};
  size_ = core::DisplaySize{.width = static_cast<std::uint16_t>(kEdge + width + kGap + kRow),
                            .height = static_cast<std::uint16_t>(kEdge + height + kGap + kRow)};

  if (turn == ViewTurn::kAsPanel) {
    // A quarter-turn anticlockwise: what was at the right is at the top, and
    // what was at the bottom is at the right.
    const int across = size_.width;
    const auto turned = [across](Rect r) {
      return Rect{.x = r.y, .y = across - r.x - r.width, .width = r.height, .height = r.width};
    };
    panel_ = turned(panel_);
    for (ButtonPlace& place : buttons_) {
      place.rect = turned(place.rect);
    }
    size_ = core::DisplaySize{.width = size_.height, .height = size_.width};
  }
}

std::optional<core::Button> DeviceLayout::ButtonAt(float x, float y) const noexcept {
  for (const ButtonPlace& place : buttons_) {
    if (place.rect.Contains(x, y)) {
      return place.button;
    }
  }
  return std::nullopt;
}

}  // namespace bn::hw
