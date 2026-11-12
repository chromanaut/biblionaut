#include "desktop/scripted_input.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <utility>

namespace bn::hw {

namespace {

constexpr std::array<std::pair<std::string_view, core::Button>, core::kButtonCount> kButtonNames = {{
    {"back", core::Button::kBack},
    {"confirm", core::Button::kConfirm},
    {"left", core::Button::kLeft},
    {"right", core::Button::kRight},
    {"up", core::Button::kUp},
    {"down", core::Button::kDown},
    {"power", core::Button::kPower},
}};

std::optional<std::uint64_t> ParseMilliseconds(std::string_view text) {
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value * 1'000;
}

std::optional<core::Button> ParseButton(std::string_view text) {
  for (const auto& [name, button] : kButtonNames) {
    if (text == name) {
      return button;
    }
  }
  return std::nullopt;
}

// The next field of `rest`, up to `separator`, which is removed with it.
std::string_view TakeField(std::string_view& rest, char separator) {
  const std::size_t end = rest.find(separator);
  const std::string_view field = rest.substr(0, end);
  rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
  return field;
}

}  // namespace

ScriptedInput::ScriptedInput(core::Clock& clock, std::string_view script) : clock_(clock) {
  while (!script.empty()) {
    const std::string_view entry = TakeField(script, ',');
    if (entry.empty()) {
      continue;
    }
    std::string_view fields = entry;
    const std::optional<std::uint64_t> at = ParseMilliseconds(TakeField(fields, ':'));
    const std::optional<core::Button> button = ParseButton(TakeField(fields, ':'));
    const std::optional<std::uint64_t> held =
        fields.empty() ? std::optional<std::uint64_t>(kDefaultPressUs) : ParseMilliseconds(fields);
    if (!at || !button || !held) {
      problems_.emplace_back(entry);
      continue;
    }
    events_.push_back({.timeUs = *at, .button = *button, .kind = core::InputEventKind::kPress});
    if (*held >= kHoldUs) {
      events_.push_back({.timeUs = *at + kHoldUs, .button = *button, .kind = core::InputEventKind::kHold});
    }
    events_.push_back({.timeUs = *at + *held, .button = *button, .kind = core::InputEventKind::kRelease});
  }
  std::stable_sort(events_.begin(), events_.end(),
                   [](const core::InputEvent& a, const core::InputEvent& b) { return a.timeUs < b.timeUs; });
}

bool ScriptedInput::Start() noexcept {
  if (!started_) {
    started_ = true;
    // The script's times count from now.
    const std::uint64_t now = clock_.NowUs();
    for (core::InputEvent& event : events_) {
      event.timeUs += now;
    }
  }
  return true;
}

bool ScriptedInput::Next(core::InputEvent& event) noexcept {
  if (!started_ || next_ >= events_.size() || events_[next_].timeUs > clock_.NowUs()) {
    return false;
  }
  event = events_[next_++];
  return true;
}

}  // namespace bn::hw
