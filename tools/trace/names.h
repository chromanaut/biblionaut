#pragma once

#include <string>
#include <string_view>

#include "core/trace.h"

namespace bn::trace_tool {

// What is known about a trace id.
struct SpanInfo {
  std::string_view part;      // "reader", "layout", ...
  std::string name;           // The id's name, or "#<index>" if this tool does not know it.
  std::string_view argument;  // What the record's number means; empty if nothing.
  bool known = false;
};

// Name of a part as it appears in output.
std::string_view PartName(core::TracePart part) noexcept;

// Looks an id up in the tables generated from each part's list of ids.
SpanInfo Describe(core::SpanId id);

// How many ids this tool knows, for tests.
std::size_t KnownSpanCount() noexcept;

}  // namespace bn::trace_tool
