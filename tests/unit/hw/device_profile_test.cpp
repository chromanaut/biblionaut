#include "desktop/device_profile.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <set>

namespace {

using bn::hw::ButtonSpot;
using bn::hw::DeviceProfile;
using bn::hw::Devices;
using bn::hw::Edge;
using bn::hw::FindDevice;
using bn::hw::ReadingTurn;
using bn::hw::X4;

TEST(DeviceProfile, FindsEachDeviceByItsName) {
  ASSERT_FALSE(Devices().empty());
  EXPECT_EQ(&Devices()[0], &X4());
  for (const DeviceProfile& device : Devices()) {
    EXPECT_EQ(FindDevice(device.name), &device) << device.name;
  }
  EXPECT_EQ(FindDevice("x4"), &X4());
  EXPECT_EQ(FindDevice("X4"), nullptr);
  EXPECT_EQ(FindDevice(""), nullptr);
  EXPECT_EQ(FindDevice("x9"), nullptr);
}

TEST(DeviceProfile, PutsEachDevicesButtonsWithinItsEdgesAndApart) {
  for (const DeviceProfile& device : Devices()) {
    EXPECT_GT(device.panel.width, 0) << device.name;
    EXPECT_GT(device.panel.height, 0) << device.name;
    EXPECT_GT(device.times.fullRefreshUs, 0u) << device.name;
    EXPECT_GT(device.times.halfRefreshUs, 0u) << device.name;
    EXPECT_GT(device.times.fastRefreshUs, 0u) << device.name;
    EXPECT_GT(device.times.powerOffUs, 0u) << device.name;
    ASSERT_LE(device.buttons.size(), bn::hw::kMaxButtonSpots) << device.name;

    // Held for reading, the panel is this wide and high.
    const bool sideways = static_cast<unsigned>(device.turn) % 2 == 1;
    const int across = sideways ? device.panel.height : device.panel.width;
    const int down = sideways ? device.panel.width : device.panel.height;
    for (std::size_t i = 0; i < device.buttons.size(); ++i) {
      const ButtonSpot& spot = device.buttons[i];
      const int edgeLength = spot.edge == Edge::kTop || spot.edge == Edge::kBottom ? across : down;
      EXPECT_GE(spot.start, 0) << spot.label;
      EXPECT_LE(spot.start + spot.length, edgeLength) << spot.label;
      // Long enough along its edge, and the strip deep enough across it,
      // for its label in 8 by 8 letters.
      EXPECT_GE(spot.length, static_cast<int>(std::strlen(spot.label) * 8) + 4) << spot.label;
      EXPECT_LE(std::strlen(spot.label), 7u) << spot.label;
      // No two spots on an edge overlap, whatever buttons they are.
      for (std::size_t j = i + 1; j < device.buttons.size(); ++j) {
        const ButtonSpot& other = device.buttons[j];
        if (other.edge == spot.edge) {
          EXPECT_TRUE(spot.start + spot.length <= other.start || other.start + other.length <= spot.start)
              << spot.label << " and " << other.label;
        }
      }
    }
  }
}

TEST(DeviceProfile, DescribesTheX4AsItWasMeasured) {
  const DeviceProfile& x4 = X4();
  EXPECT_STREQ(x4.name, "x4");
  EXPECT_EQ(x4.panel.width, 800);
  EXPECT_EQ(x4.panel.height, 480);
  EXPECT_EQ(x4.turn, ReadingTurn::kClockwise);
  EXPECT_EQ(x4.times.fullRefreshUs, 3'579'000u);
  EXPECT_EQ(x4.times.halfRefreshUs, 1'571'000u);
  EXPECT_EQ(x4.times.fastRefreshUs, 418'000u);
  EXPECT_EQ(x4.times.powerOffUs, 139'000u);
  // Its seven buttons, each at one spot.
  std::set<bn::core::Button> buttons;
  for (const ButtonSpot& spot : x4.buttons) {
    EXPECT_TRUE(buttons.insert(spot.button).second) << spot.label;
  }
  EXPECT_EQ(buttons.size(), bn::core::kButtonCount);
}

}  // namespace
