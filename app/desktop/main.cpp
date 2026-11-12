#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/heap.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  std::fprintf(stderr, "heap allocation failed: %zu bytes, alignment %zu%s\n", failure.size, failure.alignment,
               failure.fatal ? " (fatal)" : "");
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--run-ms N]\n"
               "  --run-ms N   keep running for N milliseconds (default: start and exit)\n"
               "Set BN_SERIAL_OUT to a file name to capture the diagnostic stream.\n"
               "Set BN_PLANE_CHECK to have the panel's planes read back and checked\n"
               "whenever it comes to rest.\n",
               program);
}

}  // namespace

int main(int argc, char** argv) {
  long runMs = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--run-ms") == 0 && i + 1 < argc) {
      char* end = nullptr;
      runMs = std::strtol(argv[++i], &end, 10);
      if (end == nullptr || *end != '\0' || runMs < 0) {
        PrintUsage(argv[0]);
        return 2;
      }
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  bn::core::SetHeapFailureHandler(OnHeapFailure);

  bn::reader::App app(bn::reader::DefaultServices());
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
  if (std::getenv("BN_PLANE_CHECK") != nullptr) {
    app.CheckPanelPlanes();
  }
  app.Start();
  std::printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(runMs);
  while (std::chrono::steady_clock::now() < deadline) {
    app.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  app.Tick();
  app.Flush();
  return 0;
}
