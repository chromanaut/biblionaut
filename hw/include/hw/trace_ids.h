#pragma once

#include "core/trace.h"

// Trace ids of the hardware layer, listed once as X(index, constant, name,
// argument), in the same form as every other part's list:
//
//   index     the id's number within this part. It is part of the trace
//             format: add new ids at the end, and never reuse or renumber one.
//   constant  what code passes to the trace macros.
//   name      what tools show for it.
//   argument  what the record's number means, or "" if it carries none.
//
// The names are only compiled into the trace tool, never into the firmware.
#define BN_HW_TRACE_IDS(X)                                                    \
  /* Setting the panel up, from reset to both planes cleared. */              \
  X(0, kSpanPanelStart, "panel_start", "busy_cycles")                         \
  /* Sending rows of pixels to one of the panel's planes. */                  \
  X(1, kSpanPanelWrite, "panel_write", "bytes")                               \
  /* The spans below run from a command being sent to the panel reporting  */ \
  /* that it has finished. On a device they are recorded by the interrupt  */ \
  /* on the panel's BUSY line, so they show under "interrupts".            */ \
  X(2, kSpanPanelReset, "panel_reset", "")                                    \
  X(3, kSpanPanelClear, "panel_clear", "")                                    \
  X(4, kSpanRefreshFull, "refresh_full", "power_off_after")                   \
  X(5, kSpanRefreshHalf, "refresh_half", "power_off_after")                   \
  X(6, kSpanRefreshFast, "refresh_fast", "power_off_after")                   \
  X(7, kSpanPanelPowerOff, "panel_power_off", "")                             \
  /* The SD card, which shares the panel's wires, told to keep off them:   */ \
  /* the argument is its reply (1: done; 255: no card answered).           */ \
  X(8, kInstantSdCardParked, "sd_card_parked", "reply")                       \
  /* A button going down, or coming up: from the reading that first showed */ \
  /* it to the reading that settled it. The argument is the button's       */ \
  /* number (core/input.h). The start of the span is the moment to measure */ \
  /* the device's response from.                                           */ \
  X(9, kSpanButtonPress, "button_press", "button")                            \
  X(10, kSpanButtonRelease, "button_release", "button")                       \
  /* Reading rows of pixels back out of one of the panel's planes. */         \
  X(11, kSpanPanelRead, "panel_read", "bytes")                                \
  /* What the panel's controller says of itself, the first time it is read */ \
  /* from: its status byte, and then the twelve bytes of its display       */ \
  /* options four to a record, the first of each four in the top byte.     */ \
  X(12, kInstantPanelStatus, "panel_status", "byte")                          \
  X(13, kInstantPanelOptions, "panel_options", "bytes")                       \
  /* Getting storage ready: on a device, finding the SD card and the file  */ \
  /* system on it. The argument is how it ended, a core::StorageStatus     */ \
  /* (0: ready; 1: no card; 8: a card, but no file system that can be      */ \
  /* used).                                                                */ \
  X(14, kSpanSdStart, "sd_start", "status")                                   \
  /* One read, write or sync of the card. */                                  \
  X(15, kSpanSdRead, "sd_read", "bytes")                                      \
  X(16, kSpanSdWrite, "sd_write", "bytes")                                    \
  X(17, kSpanSdSync, "sd_sync", "")                                           \
  /* The card that was found: its size, and which kind of file system it  */  \
  /* holds (1: FAT12, 2: FAT16, 3: FAT32).                                 */ \
  X(18, kInstantSdCard, "sd_card", "megabytes")                               \
  X(19, kInstantSdFileSystem, "sd_file_system", "kind")                       \
  /* Why storage could not be got ready: the error the card driver gave    */ \
  /* (an esp_err_t; 263 is "no answer"), or the one the file system gave   */ \
  /* (a FatFS FRESULT; 13 is "no file system it knows").                   */ \
  X(20, kInstantSdCardError, "sd_card_error", "code")                         \
  X(21, kInstantSdMountError, "sd_mount_error", "code")

namespace bn::hw {

#define BN_HW_TRACE_ID_CONSTANT(index, constant, name, argument) \
  inline constexpr core::SpanId constant = core::MakeSpanId(core::TracePart::kHw, (index));
BN_HW_TRACE_IDS(BN_HW_TRACE_ID_CONSTANT)
#undef BN_HW_TRACE_ID_CONSTANT

}  // namespace bn::hw
