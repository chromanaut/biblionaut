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
#define BN_READER_TRACE_IDS(X)                                                 \
  /* Start-up: from installing the tracer to sending the hello. */             \
  X(0, kSpanAppStart, "app_start", "")                                         \
  /* Once a second while the application runs. */                              \
  X(1, kInstantHeartbeat, "heartbeat", "uptime_s")                             \
  /* Reading heap and stack levels for the metrics line. */                    \
  X(2, kSpanSampleMemory, "sample_memory", "")                                 \
  /* Rendering a whole picture, band by band, and sending it to */             \
  /* the panel: one or both planes.                             */             \
  X(3, kSpanPanelSweep, "panel_sweep", "picture")                              \
  /* From a button going down to the refresh it led to being    */             \
  /* started. The panel's own time follows, as one of the       */             \
  /* hardware layer's refresh spans.                            */             \
  X(4, kSpanPressToRefresh, "press_to_refresh", "button")                      \
  /* What each of the panel's planes was found to hold when read */            \
  /* back. 1 to 4: that picture. 8: blank. 9: anything else. */                \
  X(5, kInstantNewPlaneHolds, "new_plane_holds", "picture")                    \
  X(6, kInstantOldPlaneHolds, "old_plane_holds", "picture")                    \
  /* For a plane that held "anything else", the same for each */               \
  /* band of 80 rows, top band first, a digit each. Where bands */             \
  /* of several pictures are alike, the lowest is given: the */                \
  /* pictures differ only in their fourth and fifth bands, and */              \
  /* in the sixth only between odd and even. */                                \
  X(7, kInstantNewPlaneBands, "new_plane_bands", "bands")                      \
  X(8, kInstantOldPlaneBands, "old_plane_bands", "bands")                      \
  /* A plane could not be read back (0: new image, 1: old), so */              \
  /* no more are tried. */                                                     \
  X(9, kInstantPlaneUnreadable, "plane_unreadable", "plane")                   \
  /* Getting storage ready at start-up. The argument is how it */              \
  /* ended, a core::StorageStatus: 0 is ready, 1 no card. */                   \
  X(10, kSpanStorageStart, "storage_start", "status")                          \
  /* A whole run of the storage check (reader/storage_check.h), */             \
  /* and whether it passed (1) or failed (0). */                               \
  X(11, kSpanStorageCheck, "storage_check", "passed")                          \
  /* Where a storage check failed: the step's number times 100, */             \
  /* plus what storage answered (a core::StorageStatus; 0 if it */             \
  /* answered well but the bytes were wrong). */                               \
  X(12, kInstantStorageCheckFailed, "storage_check_failed", "step_and_status") \
  /* A whole run of the book check (reader/book_check.h), and the */           \
  /* reading of one book within it, each with whether it passed */             \
  /* (1) or failed (0). What was found is in the log text. */                  \
  X(13, kSpanBookCheck, "book_check", "passed")                                \
  X(14, kSpanBookCheckBook, "book_check_book", "passed")

namespace bn::reader {

#define BN_READER_TRACE_ID_CONSTANT(index, constant, name, argument) \
  inline constexpr core::SpanId constant = core::MakeSpanId(core::TracePart::kReader, (index));
BN_READER_TRACE_IDS(BN_READER_TRACE_ID_CONSTANT)
#undef BN_READER_TRACE_ID_CONSTANT

}  // namespace bn::reader
