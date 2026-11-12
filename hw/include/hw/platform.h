#pragma once

#include "core/byte_sink.h"
#include "core/clock.h"
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

// Short name of the port, for the boot banner and diagnostics.
const char* PortName() noexcept;

}  // namespace bn::hw
