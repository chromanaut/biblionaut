#pragma once

#include "core/byte_sink.h"
#include "core/clock.h"
#include "core/crash.h"
#include "core/display.h"
#include "core/memory_probe.h"
#include "core/trace.h"

namespace bn::hw {

// Implemented by the desktop port (hw/desktop) and by the device hardware
// layer: hw/esp32c3 for what every supported device shares through its chip,
// hw/x4 for what is specific to one device. Only code under hw/ may include
// ESP-IDF or FreeRTOS headers; everything else reaches the hardware through
// interfaces declared in core and handed out here.

// The monotonic clock of this device.
core::Clock& SystemClock() noexcept;

// The lock and task numbering the tracer uses on this device.
core::TraceSync& SystemTraceSync() noexcept;

// The diagnostic serial line: USB on a device. On the desktop it is the file
// named by the BN_SERIAL_OUT environment variable, and is discarded when
// that is not set.
core::ByteSink& SystemSerial() noexcept;

// How much heap and stack this device has left.
core::MemoryProbe& SystemMemoryProbe() noexcept;

// The report of the last crash, kept across restarts. On a device, calling
// this also arms the crash handler's use of the store, so call it early. The
// desktop has no crashes to report; it makes one up when the BN_FAKE_CRASH
// environment variable is set, so the path a report takes can be exercised.
core::CrashStore& SystemCrashStore() noexcept;

// The e-ink panel. On the desktop it is a simulation that keeps the two
// planes in memory and works out what a real panel would show; when the
// BN_DISPLAY_OUT environment variable is set, it saves that picture at every
// refresh as BN_DISPLAY_OUT-001.png, -002.png and so on.
core::Display& SystemDisplay() noexcept;

// Short name of the port, for the boot banner and diagnostics.
const char* PortName() noexcept;

}  // namespace bn::hw
