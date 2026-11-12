#include <cstdio>

#include "core/heap.h"
#include "hw/platform.h"
#include "reader/app.h"

namespace {

void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  std::fprintf(stderr, "heap allocation failed: %zu bytes, alignment %zu%s\n", failure.size, failure.alignment,
               failure.fatal ? " (fatal)" : "");
}

}  // namespace

int main() {
  bn::core::SetHeapFailureHandler(OnHeapFailure);

  bn::reader::App app(bn::reader::DefaultServices());
  app.Start();

  std::printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());
  return 0;
}
