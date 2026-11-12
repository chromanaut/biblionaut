#include "core/input.h"

namespace bn::core {

InputSampler::Events InputSampler::Sample(ButtonSet reading, std::uint64_t timeUs) noexcept {
  Events events;
  const auto add = [&events](std::uint64_t time, std::size_t button, InputEventKind kind) {
    events.items[events.count++] = InputEvent{.timeUs = time, .button = static_cast<Button>(button), .kind = kind};
  };

  if (reading != candidate_ || agreeing_ == 0) {
    candidate_ = reading;
    candidateSinceUs_ = timeUs;
    agreeing_ = 1;
  } else if (agreeing_ < debounceReadings_) {
    ++agreeing_;
  }

  if (agreeing_ >= debounceReadings_ && candidate_ != down_) {
    // The change is dated from the first reading that showed it.
    const unsigned changed = static_cast<unsigned>(candidate_) ^ down_;
    for (std::size_t button = 0; button < kButtonCount; ++button) {
      const unsigned bit = 1U << button;
      if ((changed & bit) == 0) {
        continue;
      }
      if ((candidate_ & bit) != 0) {
        add(candidateSinceUs_, button, InputEventKind::kPress);
        pressedAtUs_[button] = candidateSinceUs_;
        holdSent_ = static_cast<ButtonSet>(holdSent_ & ~bit);
      } else {
        add(candidateSinceUs_, button, InputEventKind::kRelease);
      }
    }
    down_ = candidate_;
  }

  for (std::size_t button = 0; button < kButtonCount; ++button) {
    const unsigned bit = 1U << button;
    if ((down_ & bit) != 0 && (holdSent_ & bit) == 0 && timeUs - pressedAtUs_[button] >= holdUs_) {
      add(pressedAtUs_[button] + holdUs_, button, InputEventKind::kHold);
      holdSent_ = static_cast<ButtonSet>(holdSent_ | bit);
    }
  }
  return events;
}

}  // namespace bn::core
