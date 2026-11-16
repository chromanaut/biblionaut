#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "core/heap.h"
#include "desktop/panel_window.h"
#include "desktop/sim_display.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  std::fprintf(stderr, "heap allocation failed: %zu bytes, alignment %zu%s\n", failure.size, failure.alignment,
               failure.fatal ? " (fatal)" : "");
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--run-ms N] [--window]\n"
               "  --run-ms N   keep running for N milliseconds (default: start and exit;\n"
               "               with --window, until the window is closed)\n"
               "  --window     show the panel in a window, and take the buttons from the\n"
               "               keyboard as well as from BN_INPUT (a build configured with\n"
               "               -DBN_DESKTOP_WINDOW=ON)\n"
               "Set BN_SERIAL_OUT to a file name to capture the diagnostic stream.\n"
               "Set BN_PLANE_CHECK to have the panel's planes read back and checked\n"
               "whenever it comes to rest.\n"
               "Set BN_STORAGE to a folder to stand for the SD card, and BN_STORAGE_CHECK\n"
               "to have it put through its paces at start and at each (scripted) press\n"
               "of Power.\n"
               "Set BN_BOOK_CHECK instead to have every EPUB in that folder's books folder\n"
               "read from end to end, at the same moments; and BN_BOOK_CHECK_BRIEF with it\n"
               "to have each only opened, and none read through.\n",
               program);
}

// `services` with other buttons.
bn::reader::Services WithButtons(const bn::reader::Services& services, bn::core::Input& input) {
  return bn::reader::Services{.clock = services.clock,
                              .traceSync = services.traceSync,
                              .serial = services.serial,
                              .memory = services.memory,
                              .crash = services.crash,
                              .display = services.display,
                              .input = input,
                              .storage = services.storage,
                              .startCause = services.startCause};
}

}  // namespace

int main(int argc, char** argv) {
  long runMs = 0;
  bool runMsGiven = false;
  bool withWindow = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--run-ms") == 0 && i + 1 < argc) {
      char* end = nullptr;
      runMs = std::strtol(argv[++i], &end, 10);
      runMsGiven = true;
      if (end == nullptr || *end != '\0' || runMs < 0) {
        PrintUsage(argv[0]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--window") == 0) {
      withWindow = true;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  bn::core::SetHeapFailureHandler(OnHeapFailure);

  // The window is opened first, so that the application is given its
  // buttons. It shows the same simulated panel as there is without it.
  std::unique_ptr<bn::hw::PanelWindow> window;
  if (withWindow) {
    window = bn::hw::PanelWindow::Open(bn::hw::SystemClock(), bn::hw::SystemSimDisplay(), bn::hw::SystemInput());
    if (window == nullptr) {
      return 1;
    }
  }
  const bn::reader::Services defaults = bn::reader::DefaultServices();
  bn::reader::App app(window != nullptr ? WithButtons(defaults, window->Buttons()) : defaults);
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
  if (std::getenv("BN_PLANE_CHECK") != nullptr) {
    app.CheckPanelPlanes();
  }
  // NOLINTNEXTLINE(concurrency-mt-unsafe): as above.
  const bool checkStorage = std::getenv("BN_STORAGE_CHECK") != nullptr;
  // NOLINTNEXTLINE(concurrency-mt-unsafe): as above.
  const bool checkBooks = std::getenv("BN_BOOK_CHECK") != nullptr;
  if (checkStorage && checkBooks) {
    std::fprintf(stderr, "BN_STORAGE_CHECK and BN_BOOK_CHECK would get in each other's way: set one of them.\n");
    return 2;
  }
  if (checkStorage) {
    app.CheckStorage();
  }
  if (checkBooks) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): as above.
    app.CheckBooks(std::getenv("BN_BOOK_CHECK_BRIEF") != nullptr);
  }
  app.Start();
  std::printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());
  if (window != nullptr) {
    std::printf("%s", bn::hw::PanelWindow::KeysHelp());
  }
  std::fflush(stdout);

  const bool untilClosed = window != nullptr && !runMsGiven;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(runMs);
  while (untilClosed || std::chrono::steady_clock::now() < deadline) {
    app.Tick();
    if (window != nullptr && !window->Pump()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  app.Tick();
  app.Flush();
  return 0;
}
