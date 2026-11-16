#include "desktop/panel_view.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "trace_fakes.h"

namespace {

using bn::core::AfterRefresh;
using bn::core::DisplayPlane;
using bn::core::RefreshMode;
using bn::hw::PanelView;
using bn::hw::RefreshStage;
using bn::hw::SimDisplay;
using bn::hw::StageAt;
using bn::hw::ViewTurn;

constexpr std::size_t kWidth = 800;
constexpr std::size_t kHeight = 480;
constexpr std::size_t kRowBytes = 100;

struct ViewFixture {
  bn::test::FakeClock clock;
  SimDisplay display{clock, ""};

  ViewFixture() { EXPECT_TRUE(display.Start()); }

  // Shows a white picture with black pixels at `blacks` (x, y on the panel),
  // by a refresh of `mode`, and waits for it to be over.
  void Show(const std::vector<std::pair<std::size_t, std::size_t>>& blacks, RefreshMode mode = RefreshMode::kFull) {
    std::vector<std::uint8_t> rows(kRowBytes * kHeight, 0xFF);
    for (const auto& [x, y] : blacks) {
      rows[(y * kRowBytes) + (x / 8)] &= static_cast<std::uint8_t>(~(0x80U >> (x % 8)));
    }
    ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, rows));
    ASSERT_TRUE(display.StartRefresh(mode, AfterRefresh::kStayPowered));
    clock.now += 10'000'000;
    ASSERT_FALSE(display.Busy());
  }
};

std::size_t CountBlack(const PanelView& view) {
  std::size_t count = 0;
  for (const std::uint32_t pixel : view.Pixels()) {
    count += pixel == PanelView::kBlack ? 1 : 0;
  }
  return count;
}

TEST(PanelView, ShowsThePanelTurnedTheWayABookIsRead) {
  ViewFixture f;
  // The first pixel of the first row, and the last of the last.
  f.Show({{0, 0}, {kWidth - 1, kHeight - 1}});
  PanelView view({.width = kWidth, .height = kHeight});
  EXPECT_EQ(view.Turn(), ViewTurn::kAsRead);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  ASSERT_EQ(view.Size().width, kHeight);
  ASSERT_EQ(view.Size().height, kWidth);
  ASSERT_EQ(view.Pixels().size(), kWidth * kHeight);
  EXPECT_EQ(CountBlack(view), 2u);
  // A quarter-turn clockwise: the panel's first row runs down the right of
  // the window, and its last row down the left.
  EXPECT_EQ(view.Pixels()[kHeight - 1], PanelView::kBlack);             // Top right.
  EXPECT_EQ(view.Pixels()[(kWidth - 1) * kHeight], PanelView::kBlack);  // Bottom left.
  EXPECT_EQ(view.Pixels()[0], PanelView::kWhite);                       // Top left.
  EXPECT_EQ(view.Pixels()[(kWidth * kHeight) - 1], PanelView::kWhite);  // Bottom right.
}

TEST(PanelView, ShowsThePanelInItsOwnOrientationWhenTurned) {
  ViewFixture f;
  f.Show({{3, 0}, {kWidth - 1, 2}});
  PanelView view({.width = kWidth, .height = kHeight});
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  view.SetTurn(ViewTurn::kAsPanel);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));
  ASSERT_EQ(view.Size().width, kWidth);
  ASSERT_EQ(view.Size().height, kHeight);
  EXPECT_EQ(CountBlack(view), 2u);
  EXPECT_EQ(view.Pixels()[3], PanelView::kBlack);
  EXPECT_EQ(view.Pixels()[(2 * kWidth) + kWidth - 1], PanelView::kBlack);

  // And back.
  view.SetTurn(ViewTurn::kAsRead);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_EQ(view.Size().width, kHeight);
  EXPECT_EQ(view.Pixels()[(3 * kHeight) + kHeight - 1], PanelView::kBlack);
}

TEST(PanelView, FlashesThroughAFullRefreshAndEndsOnTheNewPicture) {
  ViewFixture f;
  f.Show({{10, 10}});
  PanelView view({.width = kWidth, .height = kHeight});
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  // A picture with no black at all, by a full refresh.
  const std::uint64_t start = f.clock.now;
  std::vector<std::uint8_t> white(kRowBytes * kHeight, 0xFF);
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kNew, 0, white));
  ASSERT_TRUE(f.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  const std::uint64_t length = SimDisplay::kFullRefreshUs;
  EXPECT_EQ(f.display.LastRefresh().doneUs, start + length);

  // The old picture, black for white.
  ASSERT_TRUE(view.Update(f.display, start));
  EXPECT_EQ(view.Stage(), RefreshStage::kInverted);
  EXPECT_EQ(CountBlack(view), (kWidth * kHeight) - 1);
  // Then all black, then all white.
  ASSERT_TRUE(view.Update(f.display, start + (length / 2)));
  EXPECT_EQ(view.Stage(), RefreshStage::kBlack);
  EXPECT_EQ(CountBlack(view), kWidth * kHeight);
  ASSERT_TRUE(view.Update(f.display, start + length - 1));
  EXPECT_EQ(view.Stage(), RefreshStage::kWhite);
  EXPECT_EQ(CountBlack(view), 0u);
  // Then the new picture, once the refresh is over.
  ASSERT_TRUE(view.Update(f.display, start + length));
  EXPECT_EQ(view.Stage(), RefreshStage::kAfter);
  EXPECT_EQ(CountBlack(view), 0u);
}

TEST(PanelView, ShowsAFastRefreshsPictureOnceItIsOver) {
  ViewFixture f;
  f.Show({{10, 10}});
  PanelView view({.width = kWidth, .height = kHeight});
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  const std::uint64_t start = f.clock.now;
  std::vector<std::uint8_t> rows(kRowBytes * kHeight, 0xFF);
  rows[(20 * kRowBytes) + 2] = 0x00;  // Eight black pixels.
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kNew, 0, rows));
  ASSERT_TRUE(f.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));

  // Until it is over, the picture from before.
  ASSERT_TRUE(view.Update(f.display, start));
  EXPECT_EQ(view.Stage(), RefreshStage::kBefore);
  EXPECT_EQ(CountBlack(view), 1u);
  EXPECT_FALSE(view.Update(f.display, start + SimDisplay::kFastRefreshUs - 1));

  // Then what the fast refresh left: its own pixels and, where the old-image
  // plane was white, the one from before as well.
  ASSERT_TRUE(view.Update(f.display, start + SimDisplay::kFastRefreshUs));
  EXPECT_EQ(view.Stage(), RefreshStage::kAfter);
  EXPECT_EQ(CountBlack(view), 9u);
}

TEST(PanelView, ChangesOnlyWhenWhatThePanelLooksLikeDoes) {
  ViewFixture f;
  f.Show({{0, 0}});
  PanelView view({.width = kWidth, .height = kHeight});
  EXPECT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now + 5'000'000));

  // A new refresh, even of the same picture, is a change.
  f.Show({{0, 0}}, RefreshMode::kHalf);
  EXPECT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now));
}

TEST(PanelView, TakesAHalfRefreshThroughBlackAndWhite) {
  const SimDisplay::Refresh half{.mode = RefreshMode::kHalf, .startUs = 1'000, .doneUs = 1'600};
  EXPECT_EQ(StageAt(half, 0), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(half, 1'299), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(half, 1'300), RefreshStage::kWhite);
  EXPECT_EQ(StageAt(half, 1'599), RefreshStage::kWhite);
  EXPECT_EQ(StageAt(half, 1'600), RefreshStage::kAfter);

  const SimDisplay::Refresh full{.mode = RefreshMode::kFull, .startUs = 1'000, .doneUs = 1'600};
  EXPECT_EQ(StageAt(full, 1'199), RefreshStage::kInverted);
  EXPECT_EQ(StageAt(full, 1'200), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(full, 1'399), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(full, 1'400), RefreshStage::kWhite);

  // Before any refresh, the one that was over at 0.
  EXPECT_EQ(StageAt(SimDisplay::Refresh{}, 0), RefreshStage::kAfter);
}

}  // namespace
