#pragma once

#include <cstdint>
#include <deque>

#include "core/input.h"

namespace bn::test {

// Buttons a test presses by hand.
class FakeInput final : public core::Input {
 public:
  [[nodiscard]] bool Start() noexcept override {
    started = true;
    return present;
  }

  [[nodiscard]] bool Next(core::InputEvent& event) noexcept override {
    if (events.empty()) {
      return false;
    }
    event = events.front();
    events.pop_front();
    return true;
  }

  void Press(core::Button button, std::uint64_t timeUs) {
    events.push_back({.timeUs = timeUs, .button = button, .kind = core::InputEventKind::kPress});
  }
  void Release(core::Button button, std::uint64_t timeUs) {
    events.push_back({.timeUs = timeUs, .button = button, .kind = core::InputEventKind::kRelease});
  }

  bool present = true;
  bool started = false;
  std::deque<core::InputEvent> events;
};

}  // namespace bn::test
