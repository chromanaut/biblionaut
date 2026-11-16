#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/clock.h"
#include "core/input.h"

// The desktop's buttons when it has a window: keys on the keyboard, and the
// buttons drawn in the window, clicked.

namespace bn::hw {

// Buttons from the window, and from a script alongside them
// (ScriptedInput), so that presses at set times and presses by hand can be
// mixed.
//
// The window says which buttons are held down, by keys and the mouse
// together, whenever that changes. It goes through an InputSampler, as a
// reading of the buttons does on a device, so a press, a hold and a release
// are told as a device tells them. Nothing in a window bounces, so a change
// is believed at the first reading that shows it. The window and the script
// are not made into one state of the buttons: a key held while the script
// presses the same button gives the events of both.
//
// Not for use from two tasks at once.
class WindowInput final : public core::Input {
 public:
  // The most events from the window kept for Next(); more are dropped. Far
  // more than anyone presses between two turns of the application.
  static constexpr std::size_t kQueued = 64;

  WindowInput(core::Clock& clock, core::Input& script) noexcept;

  // `down` is held in the window from `timeUs`, on the clock's time. A
  // time before one already given is taken as that one, so events never go
  // back in time. Nothing is taken before Start().
  void Hold(core::ButtonSet down, std::uint64_t timeUs) noexcept;

  // Starts the script as well.
  [[nodiscard]] bool Start() noexcept override;
  // The oldest event, of the window's and the script's.
  [[nodiscard]] bool Next(core::InputEvent& event) noexcept override;

  // The buttons that the events handed out so far leave down, from either
  // source: what the application has been told is held.
  core::ButtonSet Told() const noexcept { return told_; }

 private:
  void Read(std::uint64_t timeUs) noexcept;
  void Tell(const core::InputEvent& event) noexcept;

  core::Clock& clock_;
  core::Input& script_;
  core::InputSampler sampler_{1};
  core::ButtonSet down_ = 0;
  core::ButtonSet told_ = 0;
  std::uint64_t lastUs_ = 0;
  std::array<core::InputEvent, kQueued> queue_{};
  std::size_t first_ = 0;
  std::size_t count_ = 0;
  std::optional<core::InputEvent> scripted_;  // The script's next event, taken from it already.
  bool started_ = false;
};

}  // namespace bn::hw
