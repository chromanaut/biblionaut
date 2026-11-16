// A build without a window (BN_DESKTOP_WINDOW off): there is none to open.
#include <cstdio>

#include "desktop/panel_window.h"

namespace bn::hw {

const char* PanelWindow::KeysHelp() noexcept { return ""; }

std::unique_ptr<PanelWindow> PanelWindow::Open(core::Clock& /*clock*/, SimDisplay& /*display*/,
                                               core::Input& /*script*/) {
  std::fprintf(stderr, "this build has no window: configure it with -DBN_DESKTOP_WINDOW=ON (cmake --preset window)\n");
  return nullptr;
}

}  // namespace bn::hw
