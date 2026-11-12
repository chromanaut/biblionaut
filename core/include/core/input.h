#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Buttons.
//
// The devices have seven and no touch screen. Most cannot be watched by an
// interrupt (they share two analogue pins), so the hardware is read every few
// milliseconds, and the readings are turned into events here: a button went
// down, came up, or has been held. Every event carries the time the change
// was first seen, which is where the measurement of how long the device
// takes to respond to a press begins.

namespace bn::core {

enum class Button : std::uint8_t {
  kBack = 0,
  kConfirm = 1,
  kLeft = 2,
  kRight = 3,
  kUp = 4,    // The upper of the two buttons on the side.
  kDown = 5,  // The lower of them.
  kPower = 6,
};

inline constexpr std::size_t kButtonCount = 7;

// Which buttons are down: one bit each.
using ButtonSet = std::uint8_t;

constexpr ButtonSet ButtonBit(Button button) noexcept {
  return static_cast<ButtonSet>(1U << static_cast<unsigned>(button));
}

enum class InputEventKind : std::uint8_t {
  kPress = 0,
  kRelease = 1,
  kHold = 2,  // Still down a while after being pressed. Sent once for each press.
};

struct InputEvent {
  // On the same clock as core::Clock. For a press or a release, when the
  // change was first seen; for a hold, when the press became one.
  std::uint64_t timeUs;
  Button button;
  InputEventKind kind;
};

// Turns readings of the buttons into events. A change is believed once a
// number of readings in a row agree, which keeps a bouncing contact, or a
// reading taken while a pin is still settling, from being taken for a press.
class InputSampler {
 public:
  // The events one reading can give rise to: every button could change at
  // once, and every button already down could become a hold.
  struct Events {
    std::array<InputEvent, 2 * kButtonCount> items{};
    std::size_t count = 0;

    // So that the events can be gone through with a range-based for, which
    // looks for exactly these names.
    // NOLINTBEGIN(readability-identifier-naming)
    const InputEvent* begin() const noexcept { return items.data(); }
    const InputEvent* end() const noexcept { return items.data() + count; }
    // NOLINTEND(readability-identifier-naming)
  };

  // `debounceReadings`: how many readings in a row must agree; at least 1.
  // `holdUs`: how long a button is down before it counts as held.
  constexpr explicit InputSampler(std::uint8_t debounceReadings = 2, std::uint64_t holdUs = 600'000) noexcept
      : debounceReadings_(debounceReadings < 1 ? std::uint8_t{1} : debounceReadings), holdUs_(holdUs) {}

  // Takes one reading, made at `timeUs`, and returns what it shows to have
  // happened. Call at a steady rate.
  Events Sample(ButtonSet reading, std::uint64_t timeUs) noexcept;

  // The buttons believed to be down.
  ButtonSet Down() const noexcept { return down_; }

 private:
  std::uint8_t debounceReadings_;
  std::uint64_t holdUs_;
  ButtonSet down_ = 0;                  // What is believed.
  ButtonSet candidate_ = 0;             // The latest reading,
  std::uint8_t agreeing_ = 0;           // how many readings in a row have shown it,
  std::uint64_t candidateSinceUs_ = 0;  // and when the first of them was taken.
  std::array<std::uint64_t, kButtonCount> pressedAtUs_{};
  ButtonSet holdSent_ = 0;
};

// The device's buttons, as events. Implemented by each hardware port, which
// does the reading on its own schedule so that it is never held up by
// whatever the caller is busy with.
class Input {
 public:
  // Starts watching the buttons. Returns false if they cannot be read.
  [[nodiscard]] virtual bool Start() noexcept = 0;

  // Takes the oldest event not yet handed out. Returns false if there is
  // none waiting.
  [[nodiscard]] virtual bool Next(InputEvent& event) noexcept = 0;

 protected:
  ~Input() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
