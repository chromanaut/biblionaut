#include "desktop/scripted_input.h"

#include <gtest/gtest.h>

#include <vector>

#include "trace_fakes.h"

namespace {

using bn::core::Button;
using bn::core::InputEvent;
using bn::core::InputEventKind;
using bn::hw::ScriptedInput;

std::vector<InputEvent> Drain(ScriptedInput& input) {
  std::vector<InputEvent> events;
  InputEvent event{};
  while (input.Next(event)) {
    events.push_back(event);
  }
  return events;
}

TEST(ScriptedInput, PlaysPressesAtTheirTimesCountedFromStart) {
  bn::test::FakeClock clock;
  clock.now = 50'000;
  ScriptedInput input(clock, "2000:down,2600:confirm");
  EXPECT_TRUE(input.Problems().empty());

  // Nothing before it is started, however late it is.
  clock.now = 9'000'000;
  EXPECT_TRUE(Drain(input).empty());

  clock.now = 1'000'000;
  EXPECT_TRUE(input.Start());
  clock.now = 2'999'999;
  EXPECT_TRUE(Drain(input).empty());

  clock.now = 3'000'000;  // Two seconds after the start.
  std::vector<InputEvent> events = Drain(input);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[0].button, Button::kDown);
  EXPECT_EQ(events[0].timeUs, 3'000'000u);

  // The release 80 ms later, then the next press and its release.
  clock.now = 4'000'000;
  events = Drain(input);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[0].timeUs, 3'080'000u);
  EXPECT_EQ(events[1].kind, InputEventKind::kPress);
  EXPECT_EQ(events[1].button, Button::kConfirm);
  EXPECT_EQ(events[1].timeUs, 3'600'000u);
  EXPECT_EQ(events[2].kind, InputEventKind::kRelease);

  EXPECT_TRUE(Drain(input).empty());
  // Starting again does not move the script.
  EXPECT_TRUE(input.Start());
  EXPECT_TRUE(Drain(input).empty());
}

TEST(ScriptedInput, ALongPressAlsoGivesAHold) {
  bn::test::FakeClock clock;
  ScriptedInput input(clock, "100:power:1500,200:up:599");
  ASSERT_TRUE(input.Start());
  clock.now = 10'000'000;
  const std::vector<InputEvent> events = Drain(input);
  ASSERT_EQ(events.size(), 5u);
  // In time order, whichever button they belong to.
  EXPECT_EQ(events[0].button, Button::kPower);
  EXPECT_EQ(events[0].kind, InputEventKind::kPress);
  EXPECT_EQ(events[1].button, Button::kUp);
  EXPECT_EQ(events[1].kind, InputEventKind::kPress);
  EXPECT_EQ(events[2].button, Button::kPower);
  EXPECT_EQ(events[2].kind, InputEventKind::kHold);
  EXPECT_EQ(events[2].timeUs, 700'000u);
  EXPECT_EQ(events[3].button, Button::kUp);  // 599 ms: not quite a hold.
  EXPECT_EQ(events[3].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[3].timeUs, 799'000u);
  EXPECT_EQ(events[4].button, Button::kPower);
  EXPECT_EQ(events[4].kind, InputEventKind::kRelease);
  EXPECT_EQ(events[4].timeUs, 1'600'000u);
}

TEST(ScriptedInput, KnowsEveryButtonByName) {
  bn::test::FakeClock clock;
  ScriptedInput input(clock, "1:back,2:confirm,3:left,4:right,5:up,6:down,7:power");
  EXPECT_TRUE(input.Problems().empty());
  ASSERT_TRUE(input.Start());
  clock.now = 1'000'000;
  std::vector<Button> pressed;
  for (const InputEvent& event : Drain(input)) {
    if (event.kind == InputEventKind::kPress) {
      pressed.push_back(event.button);
    }
  }
  EXPECT_EQ(pressed, (std::vector<Button>{Button::kBack, Button::kConfirm, Button::kLeft, Button::kRight, Button::kUp,
                                          Button::kDown, Button::kPower}));
}

TEST(ScriptedInput, SkipsEntriesItCannotReadAndSaysWhich) {
  bn::test::FakeClock clock;
  ScriptedInput input(clock, "abc:down,100:sideways,,200:up:x,300,400:left,:right,500:down:");
  EXPECT_EQ(input.Problems(), (std::vector<std::string>{"abc:down", "100:sideways", "200:up:x", "300", ":right"}));
  ASSERT_TRUE(input.Start());
  clock.now = 1'000'000;
  const std::vector<InputEvent> events = Drain(input);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].button, Button::kLeft);
  EXPECT_EQ(events[0].timeUs, 400'000u);
  EXPECT_EQ(events[2].button, Button::kDown);  // An empty third field means the usual length.
  EXPECT_EQ(events[3].timeUs, 580'000u);
}

TEST(ScriptedInput, AnEmptyScriptPressesNothing) {
  bn::test::FakeClock clock;
  ScriptedInput input(clock, "");
  EXPECT_TRUE(input.Start());
  clock.now = 60'000'000;
  EXPECT_TRUE(Drain(input).empty());
  EXPECT_TRUE(input.Problems().empty());
}

}  // namespace
