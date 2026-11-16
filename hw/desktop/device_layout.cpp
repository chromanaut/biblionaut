#include "desktop/device_layout.h"

#include <array>
#include <utility>

namespace bn::hw {

namespace {

// The device's edge where it has no buttons; the gap between the panel and
// a strip of buttons; how deep a strip is; and how far a button is inset in
// its strip. Across a strip a button is 60 deep, enough for "Confirm", 56.
constexpr int kEdge = 16;
constexpr int kGap = 8;
constexpr int kStrip = 72;
constexpr int kInset = 6;

// `rect`, in something `height` high, turned a quarter-turn clockwise with
// it.
Rect TurnedClockwise(const Rect& rect, int height) {
  return Rect{.x = height - rect.y - rect.height, .y = rect.x, .width = rect.height, .height = rect.width};
}

}  // namespace

DeviceLayout::DeviceLayout(const DeviceProfile& device, ViewTurn turn) noexcept {
  // Laid out held as a book is read, with the panel turned to stand that way.
  const bool sideways = static_cast<unsigned>(device.turn) % 2 == 1;
  const int width = sideways ? device.panel.height : device.panel.width;
  const int height = sideways ? device.panel.width : device.panel.height;
  std::array<bool, 4> hasStrip{};
  for (const ButtonSpot& spot : device.buttons) {
    hasStrip[static_cast<std::size_t>(spot.edge)] = true;
  }
  const auto beside = [&hasStrip](Edge edge) {
    return hasStrip[static_cast<std::size_t>(edge)] ? kGap + kStrip : kEdge;
  };
  const int left = beside(Edge::kLeft);
  const int top = beside(Edge::kTop);
  int across = left + width + beside(Edge::kRight);
  int down = top + height + beside(Edge::kBottom);
  panel_ = Rect{.x = left, .y = top, .width = width, .height = height};

  constexpr int kDeep = kStrip - (2 * kInset);
  for (const ButtonSpot& spot : device.buttons) {
    if (count_ == places_.size()) {
      break;
    }
    Rect rect;
    switch (spot.edge) {
      case Edge::kTop:
        rect = Rect{.x = left + spot.start, .y = kInset, .width = spot.length, .height = kDeep};
        break;
      case Edge::kBottom:
        rect = Rect{.x = left + spot.start, .y = top + height + kGap + kInset, .width = spot.length, .height = kDeep};
        break;
      case Edge::kLeft:
        rect = Rect{.x = kInset, .y = top + spot.start, .width = kDeep, .height = spot.length};
        break;
      case Edge::kRight:
        rect = Rect{.x = left + width + kGap + kInset, .y = top + spot.start, .width = kDeep, .height = spot.length};
        break;
    }
    places_[count_++] = ButtonPlace{.button = spot.button, .label = spot.label, .rect = rect};
  }

  if (turn == ViewTurn::kAsPanel) {
    // Turned back to the panel's own way: as many quarter-turns clockwise
    // as, with the turn for reading, make a whole turn.
    const unsigned turns = (4 - static_cast<unsigned>(device.turn)) % 4;
    for (unsigned i = 0; i < turns; ++i) {
      panel_ = TurnedClockwise(panel_, down);
      for (ButtonPlace& place : std::span(places_).first(count_)) {
        place.rect = TurnedClockwise(place.rect, down);
      }
      std::swap(across, down);
    }
  }
  size_ = core::DisplaySize{.width = static_cast<std::uint16_t>(across), .height = static_cast<std::uint16_t>(down)};
}

bool DeviceLayout::Has(core::Button button) const noexcept {
  for (const ButtonPlace& place : Places()) {
    if (place.button == button) {
      return true;
    }
  }
  return false;
}

std::optional<core::Button> DeviceLayout::ButtonAt(float x, float y) const noexcept {
  for (const ButtonPlace& place : Places()) {
    if (place.rect.Contains(x, y)) {
      return place.button;
    }
  }
  return std::nullopt;
}

}  // namespace bn::hw
