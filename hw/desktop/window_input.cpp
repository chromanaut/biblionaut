#include "desktop/window_input.h"

#include <algorithm>

namespace bn::hw {

WindowInput::WindowInput(core::Clock& clock, core::Input& script) noexcept : clock_(clock), script_(script) {}

void WindowInput::Hold(core::ButtonSet down, std::uint64_t timeUs) noexcept {
  if (!started_) {
    return;
  }
  down_ = down;
  Read(timeUs);
}

void WindowInput::Read(std::uint64_t timeUs) noexcept {
  // The sampler takes readings in time order: a hold is worked out from the
  // time of the press.
  lastUs_ = std::max(lastUs_, timeUs);
  for (const core::InputEvent& event : sampler_.Sample(down_, lastUs_)) {
    if (count_ < queue_.size()) {
      queue_[(first_ + count_) % queue_.size()] = event;
      ++count_;
    }
  }
}

bool WindowInput::Start() noexcept {
  started_ = true;
  lastUs_ = std::max(lastUs_, clock_.NowUs());
  return script_.Start();
}

bool WindowInput::Next(core::InputEvent& event) noexcept {
  if (!started_) {
    return false;
  }
  // A reading now, for a button that has been down long enough to be held.
  Read(clock_.NowUs());
  if (!scripted_) {
    core::InputEvent fromScript{};
    if (script_.Next(fromScript)) {
      scripted_ = fromScript;
    }
  }
  if (count_ > 0 && (!scripted_ || queue_[first_].timeUs <= scripted_->timeUs)) {
    event = queue_[first_];
    first_ = (first_ + 1) % queue_.size();
    --count_;
    Tell(event);
    return true;
  }
  if (scripted_) {
    event = *scripted_;
    scripted_.reset();
    Tell(event);
    return true;
  }
  return false;
}

void WindowInput::Tell(const core::InputEvent& event) noexcept {
  const unsigned bit = core::ButtonBit(event.button);
  if (event.kind == core::InputEventKind::kPress) {
    told_ = static_cast<core::ButtonSet>(told_ | bit);
  } else if (event.kind == core::InputEventKind::kRelease) {
    told_ = static_cast<core::ButtonSet>(told_ & ~bit);
  }
}

}  // namespace bn::hw
