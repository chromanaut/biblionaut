#include "desktop/device_layout.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

namespace {

using bn::core::Button;
using bn::core::DisplaySize;
using bn::hw::ButtonPlace;
using bn::hw::DeviceLayout;
using bn::hw::PanelView;
using bn::hw::Rect;
using bn::hw::ViewTurn;

constexpr DisplaySize kPanel{.width = 800, .height = 480};

const Rect& PlaceOf(const DeviceLayout& layout, Button button) {
  for (const ButtonPlace& place : layout.Buttons()) {
    if (place.button == button) {
      return place.rect;
    }
  }
  ADD_FAILURE() << "no place for button " << static_cast<int>(button);
  static const Rect kNowhere{};
  return kNowhere;
}

bool Overlap(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

TEST(DeviceLayout, PutsTheFourFrontButtonsInARowUnderThePanel) {
  const DeviceLayout layout(kPanel, ViewTurn::kAsRead);
  const Rect panel = layout.Panel();
  const Button order[] = {Button::kBack, Button::kConfirm, Button::kLeft, Button::kRight};
  int lastX = -1;
  for (const Button button : order) {
    const Rect& rect = PlaceOf(layout, button);
    EXPECT_GE(rect.y, panel.y + panel.height);
    EXPECT_GT(rect.x, lastX);
    EXPECT_GE(rect.x, panel.x);
    EXPECT_LE(rect.x + rect.width, panel.x + panel.width);
    lastX = rect.x;
  }
}

TEST(DeviceLayout, PutsPowerUpAndDownDownTheRightEdge) {
  const DeviceLayout layout(kPanel, ViewTurn::kAsRead);
  const Rect panel = layout.Panel();
  const Rect& power = PlaceOf(layout, Button::kPower);
  const Rect& up = PlaceOf(layout, Button::kUp);
  const Rect& down = PlaceOf(layout, Button::kDown);
  for (const Rect* rect : {&power, &up, &down}) {
    EXPECT_GE(rect->x, panel.x + panel.width);
    EXPECT_LE(rect->y + rect->height, panel.y + panel.height);
  }
  EXPECT_LT(power.y + power.height, up.y);
  EXPECT_LT(up.y + up.height, down.y);
}

TEST(DeviceLayout, HasRoomForThePanelAsItIsShown) {
  for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
    const DeviceLayout layout(kPanel, turn);
    const PanelView view(kPanel, turn);
    EXPECT_EQ(layout.Panel().width, view.Size().width);
    EXPECT_EQ(layout.Panel().height, view.Size().height);
  }
}

TEST(DeviceLayout, TurnsAQuarterTurnAnticlockwiseToThePanelsOwnWay) {
  const DeviceLayout read(kPanel, ViewTurn::kAsRead);
  const DeviceLayout turned(kPanel, ViewTurn::kAsPanel);
  EXPECT_EQ(turned.Size().width, read.Size().height);
  EXPECT_EQ(turned.Size().height, read.Size().width);

  // The row under the panel is down its right edge, Back at the bottom.
  const Rect panel = turned.Panel();
  const Button order[] = {Button::kBack, Button::kConfirm, Button::kLeft, Button::kRight};
  int lastY = turned.Size().height;
  for (const Button button : order) {
    const Rect& rect = PlaceOf(turned, button);
    EXPECT_GE(rect.x, panel.x + panel.width);
    EXPECT_LT(rect.y, lastY);
    lastY = rect.y;
  }
  // And the three down the right edge are along the top, Power on the left.
  const Rect& power = PlaceOf(turned, Button::kPower);
  const Rect& up = PlaceOf(turned, Button::kUp);
  const Rect& down = PlaceOf(turned, Button::kDown);
  for (const Rect* rect : {&power, &up, &down}) {
    EXPECT_LE(rect->y + rect->height, panel.y);
  }
  EXPECT_LT(power.x + power.width, up.x);
  EXPECT_LT(up.x + up.width, down.x);
}

TEST(DeviceLayout, KeepsEveryButtonInsideApartAndRoomyEnoughForItsLabel) {
  for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
    const DeviceLayout layout(kPanel, turn);
    ASSERT_EQ(layout.Buttons().size(), bn::core::kButtonCount);
    for (const ButtonPlace& place : layout.Buttons()) {
      const Rect& rect = place.rect;
      EXPECT_GE(rect.x, 0);
      EXPECT_GE(rect.y, 0);
      EXPECT_LE(rect.x + rect.width, layout.Size().width);
      EXPECT_LE(rect.y + rect.height, layout.Size().height);
      EXPECT_FALSE(Overlap(rect, layout.Panel())) << place.label;
      // In 8 by 8 letters, with a little room either side.
      EXPECT_GE(rect.width, static_cast<int>(std::strlen(place.label) * 8) + 4) << place.label;
      EXPECT_GE(rect.height, 8 + 4) << place.label;
      for (const ButtonPlace& other : layout.Buttons()) {
        if (other.button != place.button) {
          EXPECT_FALSE(Overlap(rect, other.rect)) << place.label << " and " << other.label;
        }
      }
    }
  }
}

TEST(DeviceLayout, FindsEachButtonWhereItIsAndNoneElsewhere) {
  for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
    const DeviceLayout layout(kPanel, turn);
    for (const ButtonPlace& place : layout.Buttons()) {
      const float x = static_cast<float>(place.rect.x) + (static_cast<float>(place.rect.width) / 2);
      const float y = static_cast<float>(place.rect.y) + (static_cast<float>(place.rect.height) / 2);
      EXPECT_EQ(layout.ButtonAt(x, y), std::optional<Button>(place.button)) << place.label;
      // Its edges: the first column and row are in it, the next after the
      // last are not.
      EXPECT_EQ(layout.ButtonAt(static_cast<float>(place.rect.x), y), std::optional<Button>(place.button));
      EXPECT_NE(layout.ButtonAt(static_cast<float>(place.rect.x + place.rect.width), y),
                std::optional<Button>(place.button));
    }
    const Rect panel = layout.Panel();
    EXPECT_EQ(layout.ButtonAt(static_cast<float>(panel.x + 10), static_cast<float>(panel.y + 10)), std::nullopt);
    EXPECT_EQ(layout.ButtonAt(0.0F, 0.0F), std::nullopt);
  }
}

}  // namespace
