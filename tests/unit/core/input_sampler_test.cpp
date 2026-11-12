#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/input.h"

namespace {

using bn::core::Button;
using bn::core::ButtonBit;
using bn::core::ButtonSet;
using bn::core::InputEvent;
using bn::core::InputEventKind;
using bn::core::InputSampler;

constexpr ButtonSet kNone = 0;
constexpr ButtonSet kDown = ButtonBit(Button::kDown);
constexpr ButtonSet kBack = ButtonBit(Button::kBack);
constexpr ButtonSet kPower = ButtonBit(Button::kPower);

// Feeds readings 5 ms apart, starting at `timeUs`, and collects the events.
std::vector<InputEvent> Feed(InputSampler& sampler, std::uint64_t& timeUs, std::initializer_list<ButtonSet> readings) {
  std::vector<InputEvent> events;
  for (const ButtonSet reading : readings) {
    for (const InputEvent& event : sampler.Sample(reading, timeUs)) {
      events.push_back(event);
    }
    timeUs += 5'000;
  }
  return events;
}

TEST(InputSampler, ReportsAPressOnceTwoReadingsAgreeDatedFromTheFirst) {
  InputSampler sampler;
  std::uint64_t time = 100'000;
  EXPECT_TRUE(Feed(sampler, time, {kNone, kNone}).empty());

  // The first reading with the button down: not believed yet.
  EXPECT_TRUE(Feed(sampler, time, {kDown}).empty());
  EXPECT_EQ(sampler.Down(), kNone);

  // The second: believed, and dated from the first.
  const std::vector<InputEvent> events = Feed(sampler, time, {kDown});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[0].timeUs, 110'000u);
  EXPECT_EQ(sampler.Down(), kDown);

  // Staying down says nothing more.
  EXPECT_TRUE(Feed(sampler, time, {kDown, kDown, kDown}).empty());
}

TEST(InputSampler, ReportsTheReleaseTheSameWay) {
  InputSampler sampler;
  std::uint64_t time = 0;
  Feed(sampler, time, {kDown, kDown, kDown});  // Down from 0, believed at 5 ms.

  const std::vector<InputEvent> events = Feed(sampler, time, {kNone, kNone});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[0].timeUs, 15'000u);
  EXPECT_EQ(sampler.Down(), kNone);
}

TEST(InputSampler, IgnoresReadingsThatDoNotSettle) {
  // A contact bouncing, or a reading caught half-way between two levels of
  // an analogue pin, which can look like another button altogether.
  InputSampler sampler;
  std::uint64_t time = 0;
  EXPECT_TRUE(Feed(sampler, time, {kNone, kBack, kNone, kDown, kBack, kNone, kNone}).empty());
  EXPECT_EQ(sampler.Down(), kNone);

  // The press is dated from where the steady run began, not from the noise.
  const std::vector<InputEvent> events = Feed(sampler, time, {kBack, kDown, kDown});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[0].timeUs, 40'000u);
}

TEST(InputSampler, CanNeedMoreOrFewerReadingsToAgree) {
  InputSampler immediate(1);
  std::uint64_t time = 7'000;
  std::vector<InputEvent> events = Feed(immediate, time, {kDown});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].timeUs, 7'000u);

  InputSampler careful(4);
  time = 0;
  EXPECT_TRUE(Feed(careful, time, {kDown, kDown, kDown}).empty());
  events = Feed(careful, time, {kDown});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].timeUs, 0u);

  // Nothing less than one reading makes sense; it is treated as one.
  InputSampler zero(0);
  time = 0;
  EXPECT_EQ(Feed(zero, time, {kDown}).size(), 1u);
}

TEST(InputSampler, HandlesSeveralButtonsAtOnce) {
  InputSampler sampler;
  std::uint64_t time = 0;
  const auto both = static_cast<ButtonSet>(kDown | kPower);
  std::vector<InputEvent> events = Feed(sampler, time, {both, both});
  ASSERT_EQ(events.size(), 2u);
  // In the order the buttons are numbered.
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[1].button, Button::kPower);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[1].kind, InputEventKind::kPress);

  // One comes up while the other stays down, and another goes down.
  const auto next = static_cast<ButtonSet>(kPower | kBack);
  events = Feed(sampler, time, {next, next});
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].button, Button::kBack);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[1].button, Button::kDown);
  EXPECT_EQ(events[1].kind, InputEventKind::kRelease);
  EXPECT_EQ(sampler.Down(), next);
}

TEST(InputSampler, ReportsAHoldOnceForEachLongPress) {
  InputSampler sampler(2, 50'000);  // A hold after 50 ms.
  std::uint64_t time = 1'000'000;
  std::vector<InputEvent> events = Feed(sampler, time, {kPower, kPower});  // Pressed at 1.000 s.
  ASSERT_EQ(events.size(), 1u);

  // Readings at 1.010 s to 1.045 s: not yet.
  EXPECT_TRUE(Feed(sampler, time, {kPower, kPower, kPower, kPower, kPower, kPower, kPower, kPower}).empty());
  // The reading at 1.050 s: held, dated from when it became a hold.
  events = Feed(sampler, time, {kPower});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kHold);
  EXPECT_EQ(events[0].button, Button::kPower);
  EXPECT_EQ(events[0].timeUs, 1'050'000u);
  // Only once, however long it stays down.
  EXPECT_TRUE(Feed(sampler, time, {kPower, kPower, kPower, kPower}).empty());

  // Released and pressed again: a new press can become a new hold.
  events = Feed(sampler, time, {kNone, kNone, kPower, kPower});
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[1].kind, InputEventKind::kPress);
  const std::uint64_t pressedAgain = events[1].timeUs;
  time = pressedAgain + 60'000;
  events = Feed(sampler, time, {kPower});
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kHold);
  EXPECT_EQ(events[0].timeUs, pressedAgain + 50'000);
}

TEST(InputSampler, AShortPressIsNeverAHold) {
  InputSampler sampler(2, 50'000);
  std::uint64_t time = 0;
  std::vector<InputEvent> events = Feed(sampler, time, {kDown, kDown, kDown, kNone, kNone});
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[1].kind, InputEventKind::kRelease);
  time += 200'000;
  EXPECT_TRUE(Feed(sampler, time, {kNone, kNone}).empty());
}

TEST(InputSampler, EveryButtonChangingAndHoldingAtOnceFitsInOneAnswer) {
  InputSampler sampler(1, 0);  // Believed at once, and held at once.
  const auto all = static_cast<ButtonSet>((1U << bn::core::kButtonCount) - 1);
  const InputSampler::Events events = sampler.Sample(all, 5);
  EXPECT_EQ(events.count, 2 * bn::core::kButtonCount);
  EXPECT_EQ(events.end() - events.begin(), static_cast<std::ptrdiff_t>(2 * bn::core::kButtonCount));
}

TEST(ButtonBit, GivesEachButtonItsOwnBit) {
  ButtonSet seen = 0;
  for (std::size_t i = 0; i < bn::core::kButtonCount; ++i) {
    const ButtonSet bit = ButtonBit(static_cast<Button>(i));
    EXPECT_EQ(seen & bit, 0);
    seen = static_cast<ButtonSet>(seen | bit);
  }
  EXPECT_EQ(seen, 0x7F);
}

}  // namespace
