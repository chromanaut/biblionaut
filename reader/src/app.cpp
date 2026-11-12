#include "reader/app.h"

#include "hw/platform.h"

namespace bn::reader {

Services DefaultServices() noexcept { return Services{hw::SystemClock()}; }

void App::Start() noexcept {
  startedAtUs_ = services_.clock.NowUs();
  started_ = true;
}

std::uint64_t App::UptimeUs() const noexcept { return started_ ? services_.clock.NowUs() - startedAtUs_ : 0; }

const char* Version() noexcept { return BN_VERSION_STRING; }

}  // namespace bn::reader
