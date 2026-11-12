#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "trace/decoder.h"

namespace bn::trace_tool {

// Readable text for an event, without a line ending: one line, except for a
// crash report, which is several.
std::string FormatEvent(const Event& event);

// Whether `address` is somewhere a program for `arch` can run from, which
// makes a value found on the stack a possible return address.
bool IsCodeAddress(core::CrashArch arch, std::uint32_t address) noexcept;

// Collects events and writes them as a Chrome trace-event JSON file, which
// ui.perfetto.dev and chrome://tracing open as a timeline.
//
//   - each boot of the device is a process, each task a thread;
//   - spans are slices, instants are markers;
//   - log lines and plain console text are markers on their own tracks;
//   - each counter and gauge is a counter track.
class PerfettoWriter {
 public:
  void Add(const Event& event) { events_.push_back(event); }
  void Write(std::ostream& out) const;

 private:
  std::vector<Event> events_;
};

// Escapes text for use inside a JSON string.
std::string JsonEscape(std::string_view text);

}  // namespace bn::trace_tool
