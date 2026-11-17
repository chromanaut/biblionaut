#include "desktop/device_layout.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

using bn::core::Button;
using bn::hw::ButtonPlace;
using bn::hw::ButtonSpot;
using bn::hw::DeviceLayout;
using bn::hw::DeviceProfile;
using bn::hw::Edge;
using bn::hw::PanelView;
using bn::hw::ReadingTurn;
using bn::hw::Rect;
using bn::hw::ViewTurn;

// A device made up for these tests, with a button on each edge, to be held
// whichever way.
constexpr std::array<ButtonSpot, 4> kMadeUpButtons = {{
    {.button = Button::kUp, .label = "Up", .edge = Edge::kTop, .start = 10, .length = 100},
    {.button = Button::kPower, .label = "Power", .edge = Edge::kRight, .start = 150, .length = 100},
    {.button = Button::kConfirm, .label = "Confirm", .edge = Edge::kBottom, .start = 180, .length = 100},
    {.button = Button::kBack, .label = "Back", .edge = Edge::kLeft, .start = 50, .length = 100},
}};

// Another, with Up and Down on both sides, each pair wired as one.
constexpr std::array<ButtonSpot, 5> kBothSidesButtons = {{
    {.button = Button::kUp, .label = "Up", .edge = Edge::kLeft, .start = 40, .length = 80},
    {.button = Button::kDown, .label = "Down", .edge = Edge::kLeft, .start = 130, .length = 80},
    {.button = Button::kUp, .label = "Up", .edge = Edge::kRight, .start = 40, .length = 80},
    {.button = Button::kDown, .label = "Down", .edge = Edge::kRight, .start = 130, .length = 80},
    {.button = Button::kConfirm, .label = "Confirm", .edge = Edge::kBottom, .start = 100, .length = 100},
}};

DeviceProfile BothSides(ReadingTurn turn) {
  return DeviceProfile{.name = "both-sides",
                       .panel = {.width = 300, .height = 400},
                       .turn = turn,
                       .times = {.fullRefreshUs = 1'000, .halfRefreshUs = 500, .fastRefreshUs = 100, .powerOffUs = 50},
                       .buttons = kBothSidesButtons};
}

DeviceProfile MadeUp(ReadingTurn turn) {
  return DeviceProfile{.name = "made-up",
                       .panel = {.width = 600, .height = 300},
                       .turn = turn,
                       .times = {.fullRefreshUs = 1'000, .halfRefreshUs = 500, .fastRefreshUs = 100, .powerOffUs = 50},
                       .buttons = kMadeUpButtons};
}

constexpr std::array<ReadingTurn, 4> kTurns = {ReadingTurn::kNone, ReadingTurn::kClockwise, ReadingTurn::kHalf,
                                               ReadingTurn::kAnticlockwise};

// Every device the desktop knows, and the made-up one held each way.
std::vector<DeviceProfile> EveryDevice() {
  std::vector<DeviceProfile> devices(bn::hw::Devices().begin(), bn::hw::Devices().end());
  for (const ReadingTurn turn : kTurns) {
    devices.push_back(MadeUp(turn));
  }
  devices.push_back(BothSides(ReadingTurn::kNone));
  devices.push_back(BothSides(ReadingTurn::kAnticlockwise));
  return devices;
}

// The first place `button` is.
const Rect& PlaceOf(const DeviceLayout& layout, Button button) {
  for (const ButtonPlace& place : layout.Places()) {
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

TEST(DeviceLayout, PutsTheX4sFourFrontButtonsInARowUnderThePanel) {
  const DeviceLayout layout(bn::hw::X4(), ViewTurn::kAsRead);
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

TEST(DeviceLayout, PutsTheX4sPowerUpAndDownDownTheRightEdge) {
  const DeviceLayout layout(bn::hw::X4(), ViewTurn::kAsRead);
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

TEST(DeviceLayout, DrawsTheX4WithItsPanelAndButtonsWhereTheyHaveBeenSeen) {
  const DeviceLayout layout(bn::hw::X4(), ViewTurn::kAsRead);
  EXPECT_EQ(layout.Size().width, 576);
  EXPECT_EQ(layout.Size().height, 896);
  const Rect panel = layout.Panel();
  EXPECT_EQ((std::array{panel.x, panel.y, panel.width, panel.height}), (std::array{16, 16, 480, 800}));
  const std::pair<Button, std::array<int, 4>> expected[] = {
      {Button::kBack, {16, 830, 114, 60}},  {Button::kConfirm, {138, 830, 114, 60}},
      {Button::kLeft, {260, 830, 114, 60}}, {Button::kRight, {382, 830, 114, 60}},
      {Button::kPower, {510, 16, 60, 72}},  {Button::kUp, {510, 282, 60, 96}},
      {Button::kDown, {510, 386, 60, 96}},
  };
  for (const auto& [button, rect] : expected) {
    const Rect& place = PlaceOf(layout, button);
    EXPECT_EQ((std::array{place.x, place.y, place.width, place.height}), rect) << static_cast<int>(button);
  }
}

TEST(DeviceLayout, TurnsTheX4AQuarterTurnAnticlockwiseToThePanelsOwnWay) {
  const DeviceLayout read(bn::hw::X4(), ViewTurn::kAsRead);
  const DeviceLayout turned(bn::hw::X4(), ViewTurn::kAsPanel);
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

TEST(DeviceLayout, PutsButtonsOnWhicheverEdgesTheDeviceHasThem) {
  const DeviceLayout layout(MadeUp(ReadingTurn::kNone), ViewTurn::kAsRead);
  const Rect panel = layout.Panel();
  const Rect& top = PlaceOf(layout, Button::kUp);
  const Rect& right = PlaceOf(layout, Button::kPower);
  const Rect& bottom = PlaceOf(layout, Button::kConfirm);
  const Rect& left = PlaceOf(layout, Button::kBack);
  EXPECT_LE(top.y + top.height, panel.y);
  EXPECT_GE(right.x, panel.x + panel.width);
  EXPECT_GE(bottom.y, panel.y + panel.height);
  EXPECT_LE(left.x + left.width, panel.x);
  // Each as far along its edge as the device says, from the panel's left or
  // top.
  EXPECT_EQ(top.x, panel.x + 10);
  EXPECT_EQ(right.y, panel.y + 150);
  EXPECT_EQ(bottom.x, panel.x + 180);
  EXPECT_EQ(left.y, panel.y + 50);
  EXPECT_EQ(top.width, 100);
  EXPECT_EQ(left.height, 100);
}

TEST(DeviceLayout, PutsAButtonAtEverySpotItIsWiredTo) {
  const DeviceLayout layout(BothSides(ReadingTurn::kNone), ViewTurn::kAsRead);
  ASSERT_EQ(layout.Places().size(), kBothSidesButtons.size());
  const Rect panel = layout.Panel();
  for (const Button button : {Button::kUp, Button::kDown}) {
    std::vector<Rect> places;
    for (const ButtonPlace& place : layout.Places()) {
      if (place.button == button) {
        places.push_back(place.rect);
      }
    }
    ASSERT_EQ(places.size(), 2u) << static_cast<int>(button);
    // One on each side of the panel, level with each other.
    EXPECT_LE(places[0].x + places[0].width, panel.x);
    EXPECT_GE(places[1].x, panel.x + panel.width);
    EXPECT_EQ(places[0].y, places[1].y);
    // Either is the same button.
    for (const Rect& rect : places) {
      const float x = static_cast<float>(rect.x) + (static_cast<float>(rect.width) / 2);
      const float y = static_cast<float>(rect.y) + (static_cast<float>(rect.height) / 2);
      EXPECT_EQ(layout.ButtonAt(x, y), std::optional<Button>(button));
    }
    EXPECT_TRUE(layout.Has(button));
  }
  EXPECT_FALSE(layout.Has(Button::kPower));
}

TEST(DeviceLayout, HasOnlyTheButtonsTheDeviceHas) {
  const DeviceLayout layout(MadeUp(ReadingTurn::kNone), ViewTurn::kAsRead);
  EXPECT_EQ(layout.Places().size(), kMadeUpButtons.size());
  EXPECT_TRUE(layout.Has(Button::kUp));
  EXPECT_TRUE(layout.Has(Button::kBack));
  EXPECT_FALSE(layout.Has(Button::kDown));
  EXPECT_FALSE(layout.Has(Button::kLeft));
}

// The centre of `rect`, in something `width` wide, turned a quarter-turn
// anticlockwise with it.
std::pair<double, double> CentreTurnedAnticlockwise(std::pair<double, double> centre, double width) {
  return {centre.second, width - centre.first};
}

TEST(DeviceLayout, TurnsToThePanelsOwnWayByUndoingTheTurnForReading) {
  std::vector<DeviceProfile> devices;
  for (const ReadingTurn turn : kTurns) {
    devices.push_back(MadeUp(turn));
    devices.push_back(BothSides(turn));
  }
  for (const DeviceProfile& device : devices) {
    const ReadingTurn turn = device.turn;
    const DeviceLayout read(device, ViewTurn::kAsRead);
    const DeviceLayout own(device, ViewTurn::kAsPanel);
    EXPECT_EQ(own.Panel().width, device.panel.width) << static_cast<int>(turn);
    EXPECT_EQ(own.Panel().height, device.panel.height) << static_cast<int>(turn);

    // Each thing's centre, turned back as many quarter-turns anticlockwise
    // as reading turns it clockwise, is where it is in the panel's way.
    const auto expectTurnedBack = [&](const Rect& asRead, const Rect& asOwn, const char* what) {
      std::pair<double, double> centre{asRead.x + (asRead.width / 2.0), asRead.y + (asRead.height / 2.0)};
      double width = read.Size().width;
      double height = read.Size().height;
      for (unsigned i = 0; i < static_cast<unsigned>(turn); ++i) {
        centre = CentreTurnedAnticlockwise(centre, width);
        std::swap(width, height);
      }
      EXPECT_DOUBLE_EQ(centre.first, asOwn.x + (asOwn.width / 2.0)) << what << " " << static_cast<int>(turn);
      EXPECT_DOUBLE_EQ(centre.second, asOwn.y + (asOwn.height / 2.0)) << what << " " << static_cast<int>(turn);
      EXPECT_DOUBLE_EQ(width, own.Size().width);
      EXPECT_DOUBLE_EQ(height, own.Size().height);
    };
    expectTurnedBack(read.Panel(), own.Panel(), "the panel");
    ASSERT_EQ(read.Places().size(), own.Places().size());
    for (std::size_t i = 0; i < read.Places().size(); ++i) {
      EXPECT_EQ(read.Places()[i].button, own.Places()[i].button);
      expectTurnedBack(read.Places()[i].rect, own.Places()[i].rect, read.Places()[i].label);
    }
  }
}

TEST(DeviceLayout, HasRoomForThePanelAsItIsShown) {
  for (const DeviceProfile& device : EveryDevice()) {
    for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
      const DeviceLayout layout(device, turn);
      const PanelView view(device.panel, device.turn, turn);
      EXPECT_EQ(layout.Panel().width, view.Size().width) << device.name;
      EXPECT_EQ(layout.Panel().height, view.Size().height) << device.name;
    }
  }
}

TEST(DeviceLayout, KeepsEveryButtonInsideApartAndRoomyEnoughForItsLabel) {
  for (const DeviceProfile& device : EveryDevice()) {
    for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
      const DeviceLayout layout(device, turn);
      ASSERT_EQ(layout.Places().size(), device.buttons.size()) << device.name;
      const Rect panel = layout.Panel();
      EXPECT_GE(panel.x, 0) << device.name;
      EXPECT_GE(panel.y, 0) << device.name;
      EXPECT_LE(panel.x + panel.width, layout.Size().width) << device.name;
      EXPECT_LE(panel.y + panel.height, layout.Size().height) << device.name;
      for (const ButtonPlace& place : layout.Places()) {
        const Rect& rect = place.rect;
        EXPECT_GE(rect.x, 0) << device.name << " " << place.label;
        EXPECT_GE(rect.y, 0) << device.name << " " << place.label;
        EXPECT_LE(rect.x + rect.width, layout.Size().width) << device.name << " " << place.label;
        EXPECT_LE(rect.y + rect.height, layout.Size().height) << device.name << " " << place.label;
        EXPECT_FALSE(Overlap(rect, panel)) << device.name << " " << place.label;
        // In 8 by 8 letters, with a little room either side.
        EXPECT_GE(rect.width, static_cast<int>(std::strlen(place.label) * 8) + 4) << device.name << " " << place.label;
        EXPECT_GE(rect.height, 8 + 4) << device.name << " " << place.label;
        for (const ButtonPlace& other : layout.Places()) {
          if (&other != &place) {
            EXPECT_FALSE(Overlap(rect, other.rect)) << device.name << " " << place.label << " and " << other.label;
          }
        }
      }
    }
  }
}

TEST(DeviceLayout, FindsEachButtonWhereItIsAndNoneElsewhere) {
  for (const DeviceProfile& device : EveryDevice()) {
    for (const ViewTurn turn : {ViewTurn::kAsRead, ViewTurn::kAsPanel}) {
      const DeviceLayout layout(device, turn);
      for (const ButtonPlace& place : layout.Places()) {
        const float x = static_cast<float>(place.rect.x) + (static_cast<float>(place.rect.width) / 2);
        const float y = static_cast<float>(place.rect.y) + (static_cast<float>(place.rect.height) / 2);
        EXPECT_EQ(layout.ButtonAt(x, y), std::optional<Button>(place.button)) << device.name << " " << place.label;
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
}

}  // namespace
