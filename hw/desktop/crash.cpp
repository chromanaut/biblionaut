#include <cstdlib>

#include "hw/platform.h"

namespace bn::hw {

namespace {

// A process that crashes on the desktop is examined with a debugger, so
// nothing is saved here. For trying out what happens to a report afterwards,
// BN_FAKE_CRASH makes this store hold an invented one.
class DesktopCrashStore final : public core::CrashStore {
 public:
  DesktopCrashStore() noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any other thread exists.
    if (std::getenv("BN_FAKE_CRASH") == nullptr) {
      return;
    }
    hasReport_ = true;
    report_.timeUs = 12'345'678;
    report_.bootsSince = 1;
    report_.arch = core::CrashArch::kEsp32C3;
    core::SetCrashText(report_.reason, "Load access fault (invented by BN_FAKE_CRASH)");
    core::SetCrashText(report_.task, "main");
    core::SetCrashText(report_.buildId, "0123456789");

    // Addresses shaped like an ESP32-C3's: code in flash, data in RAM.
    report_.registerCount = core::kCrashMaxRegisters;
    for (std::size_t i = 0; i < report_.registers.size(); ++i) {
      report_.registers[i] = 0x3FC90000U + static_cast<std::uint32_t>(i * 16);
    }
    report_.registers[0] = 0x4200A1B2;   // mepc
    report_.registers[1] = 0x4200C3D4;   // ra
    report_.registers[2] = 0x3FC9F000;   // sp
    report_.registers[34] = 5;           // mcause: load access fault
    report_.registers[35] = 0x00000010;  // mtval: the address that was read

    report_.stackAddress = report_.registers[2];
    report_.stackSize = 64;
    const std::uint32_t words[] = {0x00000001, 0x4200C3D4, 0x3FC9F040, 0x42001122, 0x00000000, 0x40381234};
    std::size_t at = 0;
    for (const std::uint32_t word : words) {
      for (unsigned shift = 0; shift < 32; shift += 8) {
        report_.stack[at++] = static_cast<std::uint8_t>(word >> shift);
      }
    }

    report_.traceCount = 2;
    report_.trace[0] = core::TraceRecord{.startUs = 12'000'000,
                                         .durationUs = 0,
                                         .arg = 12,
                                         .id = core::MakeSpanId(core::TracePart::kReader, 1),
                                         .task = 0,
                                         .kind = core::TraceKind::kInstant};
    report_.trace[1] = core::TraceRecord{.startUs = 12'300'000,
                                         .durationUs = 250,
                                         .arg = 0,
                                         .id = core::MakeSpanId(core::TracePart::kReader, 2),
                                         .task = 0,
                                         .kind = core::TraceKind::kSpan};
  }

  const core::CrashReport* Saved() noexcept override { return hasReport_ ? &report_ : nullptr; }

  void SetVersion(const char* version) noexcept override {
    // The invented crash is said to come from this same build.
    if (hasReport_) {
      core::SetCrashText(report_.version, version);
    }
  }

 private:
  core::CrashReport report_{};
  bool hasReport_ = false;
};

}  // namespace

core::CrashStore& SystemCrashStore() noexcept {
  static DesktopCrashStore instance;
  return instance;
}

}  // namespace bn::hw
