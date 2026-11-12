#pragma once

#include <cstdint>

#include "core/clock.h"

namespace bn::reader {

// Everything the application needs from the device, as interfaces. Tests
// pass fakes; the real set comes from DefaultServices().
struct Services {
  core::Clock& clock;
};

// The services of the hardware port this build was linked with.
Services DefaultServices() noexcept;

// The application. Empty for now: it starts and reports how long it has been
// running, which is enough to prove the parts link together on both targets.
class App {
 public:
  explicit App(const Services& services) noexcept : services_(services) {}

  void Start() noexcept;
  bool Started() const noexcept { return started_; }

  // Microseconds since Start(); 0 before it.
  std::uint64_t UptimeUs() const noexcept;

 private:
  Services services_;
  std::uint64_t startedAtUs_ = 0;
  bool started_ = false;
};

// Version of this build, for example "0.1.0-dev+5e068ca".
const char* Version() noexcept;

}  // namespace bn::reader
