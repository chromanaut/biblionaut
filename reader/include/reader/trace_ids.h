#pragma once

#include "core/trace.h"

// Trace ids of the reader application, listed once as X(index, constant,
// name, argument):
//
//   index     the id's number within this part. It is part of the trace
//             format: add new ids at the end, and never reuse or renumber one.
//   constant  what code passes to the trace macros.
//   name      what tools show for it.
//   argument  what the record's number means, or "" if it carries none.
//
// The constants below are generated from this list, and so is the table the
// trace tool uses to turn ids back into names, so the two cannot disagree.
// The names are only compiled into the tool, never into the firmware.
#define BN_READER_TRACE_IDS(X)                                     \
  /* Start-up: from installing the tracer to sending the hello. */ \
  X(0, kSpanAppStart, "app_start", "")                             \
  /* Once a second while the application runs. */                  \
  X(1, kInstantHeartbeat, "heartbeat", "uptime_s")                 \
  /* Reading heap and stack levels for the metrics line. */        \
  X(2, kSpanSampleMemory, "sample_memory", "")                     \
  /* Rendering a whole picture, band by band, and sending it to */ \
  /* the panel: one or both planes.                             */ \
  X(3, kSpanPanelSweep, "panel_sweep", "picture")                  \
  /* From a button going down to the refresh it led to being    */ \
  /* started. The panel's own time follows, as one of the       */ \
  /* hardware layer's refresh spans.                            */ \
  X(4, kSpanPressToRefresh, "press_to_refresh", "button")

namespace bn::reader {

#define BN_READER_TRACE_ID_CONSTANT(index, constant, name, argument) \
  inline constexpr core::SpanId constant = core::MakeSpanId(core::TracePart::kReader, (index));
BN_READER_TRACE_IDS(BN_READER_TRACE_ID_CONSTANT)
#undef BN_READER_TRACE_ID_CONSTANT

}  // namespace bn::reader
