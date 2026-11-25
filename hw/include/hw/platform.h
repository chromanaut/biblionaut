#pragma once

#include "core/byte_map.h"
#include "core/byte_sink.h"
#include "core/clock.h"
#include "core/crash.h"
#include "core/display.h"
#include "core/input.h"
#include "core/memory_probe.h"
#include "core/power.h"
#include "core/start.h"
#include "core/storage.h"
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
// refresh as BN_DISPLAY_OUT-001.png, -002.png and so on. It is the panel of
// the device BN_DEVICE names, an X4 by default (hw/desktop/device_profile.h),
// and takes as long over a refresh as that device's does;
// BN_DISPLAY_SPEED=20 makes it twenty times quicker.
core::Display& SystemDisplay() noexcept;

// The buttons. The desktop has none: there the BN_INPUT environment variable
// can script some, as presses at times in milliseconds after the start,
// "2000:down,2600:down,4000:confirm"; a third field gives how long a button
// is held, also in milliseconds ("5000:power:1500"). The names are back,
// confirm, left, right, up, down and power.
core::Input& SystemInput() noexcept;

// Where files are kept: the SD card on a device. On the desktop it is the
// folder named by the BN_STORAGE environment variable, and when that is not
// set there is none, as on a device with no card in it.
core::Storage& SystemStorage() noexcept;

// The bytes of the fonts image (draw/font.h). On a device they are the
// flash partition called `fonts`, a stretch of which is read by being
// mapped into the processor's address space; a device whose flash has no
// such partition has no bytes. On the desktop they are the file the BN_TEXT
// environment variable names, none if it names none, with room to map as
// much of it at once as the device simulated has (hw/desktop/device_profile.h).
core::ByteMap& SystemFonts() noexcept;

// Why this run began: what the hardware says ended the one before. On the
// desktop every run begins with power coming on.
core::StartCause WhyStarted() noexcept;

// Putting the device to sleep. No device can be yet: on the X4 it stays
// awake. On the desktop sleep is simulated: Sleep() notes that the device is
// asleep, and the desktop application ends the run and, when Power is
// pressed, begins a new one as after a deep sleep (app/desktop/main.cpp).
core::Power& SystemPower() noexcept;

// Short name of the port, for the boot banner and diagnostics.
const char* PortName() noexcept;

}  // namespace bn::hw
