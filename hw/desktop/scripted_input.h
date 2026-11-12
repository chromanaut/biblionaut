#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/clock.h"
#include "core/input.h"

// The desktop's stand-in for the buttons.

namespace bn::hw {

// Plays button presses from a script: "2000:down,2600:down,4000:confirm"
// presses Down two seconds after Start(), Down again at 2.6 s and Confirm at
// 4 s. A third field is how long the button is held, in milliseconds
// ("5000:power:1500"); without it a press lasts 80 ms. The names are back,
// confirm, left, right, up, down and power. A press held for 600 ms or more
// also gives a hold event, as on a device.
class ScriptedInput final : public core::Input {
 public:
  static constexpr std::uint64_t kDefaultPressUs = 80'000;
  static constexpr std::uint64_t kHoldUs = 600'000;

  // Entries of `script` that cannot be read are skipped; Problems() says
  // which.
  ScriptedInput(core::Clock& clock, std::string_view script);

  [[nodiscard]] bool Start() noexcept override;
  [[nodiscard]] bool Next(core::InputEvent& event) noexcept override;

  // The entries of the script that were not understood.
  const std::vector<std::string>& Problems() const noexcept { return problems_; }

 private:
  core::Clock& clock_;
  std::vector<core::InputEvent> events_;  // In time order; times are relative to Start() until it is called.
  std::vector<std::string> problems_;
  std::size_t next_ = 0;
  bool started_ = false;
};

}  // namespace bn::hw
