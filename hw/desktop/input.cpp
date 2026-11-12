#include <cstdio>
#include <cstdlib>
#include <string>

#include "desktop/scripted_input.h"
#include "hw/platform.h"

namespace bn::hw {

core::Input& SystemInput() noexcept {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
  const char* script = std::getenv("BN_INPUT");
  static ScriptedInput instance(SystemClock(), script != nullptr ? script : "");
  static bool reported = false;
  if (!reported) {
    reported = true;
    for (const std::string& problem : instance.Problems()) {
      std::fprintf(stderr, "warning: BN_INPUT entry '%s' is not understood and is ignored\n", problem.c_str());
    }
  }
  return instance;
}

}  // namespace bn::hw
