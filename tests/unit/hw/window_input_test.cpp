#include "desktop/window_input.h"

#include <gtest/gtest.h>

#include <vector>

#include "input_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::Button;
using bn::core::ButtonBit;
using bn::core::ButtonSet;
using bn::core::InputEvent;
using bn::core::InputEventKind;
using bn::hw::WindowInput;

std::vector<InputEvent> Drain(WindowInput& input) {
  std::vector<InputEvent> events;
  InputEvent event{};
  while (input.Next(event)) {
    events.push_back(event);
  }
  return events;
}

struct InputFixture {
  bn::test::FakeClock clock;
  bn::test::FakeInput script;
  WindowInput input{clock, script};
};

TEST(WindowInput, TellsOfAPressAndAReleaseAtTheTimesTheyHappened) {
  InputFixture f;
  EXPECT_TRUE(f.input.Start());
  EXPECT_TRUE(f.script.started);

  f.input.Hold(ButtonBit(Button::kDown), 1'000);
  f.input.Hold(0, 81'000);
  f.clock.now = 90'000;
  const std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[0].timeUs, 1'000u);
  EXPECT_EQ(events[1].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[1].button, Button::kDown);
  EXPECT_EQ(events[1].timeUs, 81'000u);
}

TEST(WindowInput, TellsOfAHoldOnceAButtonHasBeenDownLongEnough) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  f.input.Hold(ButtonBit(Button::kPower), 1'000'000);
  f.clock.now = 1'000'000;
  ASSERT_EQ(Drain(f.input).size(), 1u);

  // Not yet held.
  f.clock.now = 1'599'999;
  EXPECT_TRUE(Drain(f.input).empty());

  // Held, dated from the press, however late the reading that sees it.
  f.clock.now = 1'750'000;
  std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kHold);
  EXPECT_EQ(events[0].button, Button::kPower);
  EXPECT_EQ(events[0].timeUs, 1'600'000u);

  // Once for each press.
  f.clock.now = 3'000'000;
  EXPECT_TRUE(Drain(f.input).empty());
  f.input.Hold(0, 3'000'000);
  events = Drain(f.input);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kRelease);
}

TEST(WindowInput, TakesNothingBeforeItHasStarted) {
  InputFixture f;
  f.input.Hold(ButtonBit(Button::kConfirm), 1'000);
  InputEvent event{};
  EXPECT_FALSE(f.input.Next(event));

  ASSERT_TRUE(f.input.Start());
  f.clock.now = 2'000'000;
  EXPECT_TRUE(Drain(f.input).empty());
  // Nor does the button held before count as down: letting it go gives
  // nothing.
  f.input.Hold(0, 2'000'000);
  EXPECT_TRUE(Drain(f.input).empty());
}

TEST(WindowInput, HandsOutTheWindowsAndTheScriptsEventsOldestFirst) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  f.script.Press(Button::kDown, 2'000);
  f.script.Release(Button::kDown, 6'000);
  f.input.Hold(ButtonBit(Button::kLeft), 1'000);
  f.input.Hold(0, 4'000);
  f.input.Hold(ButtonBit(Button::kRight), 8'000);
  f.clock.now = 10'000;

  const std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[0].button, Button::kLeft);
  EXPECT_EQ(events[0].timeUs, 1'000u);
  EXPECT_EQ(events[1].button, Button::kDown);
  EXPECT_EQ(events[1].timeUs, 2'000u);
  EXPECT_EQ(events[2].button, Button::kLeft);
  EXPECT_EQ(events[2].timeUs, 4'000u);
  EXPECT_EQ(events[3].button, Button::kDown);
  EXPECT_EQ(events[3].timeUs, 6'000u);
  EXPECT_EQ(events[4].button, Button::kRight);
  EXPECT_EQ(events[4].timeUs, 8'000u);
}

TEST(WindowInput, SaysWhichButtonsTheApplicationHasBeenToldAreDown) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  f.script.Press(Button::kBack, 2'000);
  f.input.Hold(ButtonBit(Button::kUp), 1'000);
  // Until the events are handed out, nothing has been told.
  EXPECT_EQ(f.input.Told(), 0);

  f.clock.now = 3'000;
  InputEvent event{};
  ASSERT_TRUE(f.input.Next(event));
  EXPECT_EQ(f.input.Told(), ButtonBit(Button::kUp));
  ASSERT_TRUE(f.input.Next(event));
  EXPECT_EQ(f.input.Told(), static_cast<ButtonSet>(ButtonBit(Button::kUp) | ButtonBit(Button::kBack)));

  // A hold leaves a button down; a release takes it up.
  f.clock.now = 700'000;
  ASSERT_TRUE(f.input.Next(event));
  EXPECT_EQ(event.kind, InputEventKind::kHold);
  EXPECT_EQ(f.input.Told(), static_cast<ButtonSet>(ButtonBit(Button::kUp) | ButtonBit(Button::kBack)));
  f.input.Hold(0, 700'000);
  ASSERT_TRUE(f.input.Next(event));
  EXPECT_EQ(f.input.Told(), ButtonBit(Button::kBack));
}

TEST(WindowInput, LetsEveryButtonGoTogether) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  f.input.Hold(ButtonBit(Button::kUp), 1'000);
  f.input.Hold(static_cast<ButtonSet>(ButtonBit(Button::kUp) | ButtonBit(Button::kBack)), 2'000);
  f.input.Hold(0, 5'000);
  f.clock.now = 5'000;

  const std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), 4u);
  int releases = 0;
  for (const InputEvent& event : events) {
    if (event.kind == InputEventKind::kRelease) {
      ++releases;
      EXPECT_EQ(event.timeUs, 5'000u);
    }
  }
  EXPECT_EQ(releases, 2);
  EXPECT_EQ(f.input.Told(), 0);
}

TEST(WindowInput, NeverDatesAnEventBeforeOneItHasAlreadyGiven) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  f.input.Hold(ButtonBit(Button::kRight), 50'000);
  // A release dated before the press, as clocks read a moment apart can
  // give: it is taken as at the press, and is not a hold.
  f.input.Hold(0, 40'000);
  f.clock.now = 60'000;

  const std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[1].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[1].timeUs, 50'000u);
}

TEST(WindowInput, DropsEventsItHasNoRoomFor) {
  InputFixture f;
  ASSERT_TRUE(f.input.Start());
  for (std::uint64_t i = 0; i < WindowInput::kQueued; ++i) {
    f.input.Hold(i % 2 == 0 ? ButtonBit(Button::kDown) : ButtonSet{0}, 1'000 + i);
  }
  f.input.Hold(ButtonBit(Button::kUp), 9'000);
  f.clock.now = 10'000;

  const std::vector<InputEvent> events = Drain(f.input);
  ASSERT_EQ(events.size(), WindowInput::kQueued);
  EXPECT_EQ(events.back().button, Button::kDown);
}

}  // namespace
