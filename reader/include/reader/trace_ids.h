#pragma once

#include "core/trace.h"

// Trace ids of the reader application. A value is part of the trace format:
// add new ids at the end and never renumber one that is in use.

namespace bn::reader {

// Application start-up, from installing the tracer to the hello frame.
inline constexpr core::SpanId kSpanAppStart = core::MakeSpanId(core::TracePart::kReader, 0);
// Once a second while the application runs; the argument is uptime in seconds.
inline constexpr core::SpanId kInstantHeartbeat = core::MakeSpanId(core::TracePart::kReader, 1);

}  // namespace bn::reader
