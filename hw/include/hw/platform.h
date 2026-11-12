#pragma once

#include "core/clock.h"

namespace bn::hw {

// Implemented by the desktop port (hw/desktop) and by the device hardware
// layer (hw/x4). Only code under hw/ may include ESP-IDF or FreeRTOS headers;
// everything else reaches the hardware through interfaces declared in core
// and handed out here.

// The monotonic clock of this device.
core::Clock& SystemClock() noexcept;

// Short name of the port, for the boot banner and diagnostics.
const char* PortName() noexcept;

}  // namespace bn::hw
