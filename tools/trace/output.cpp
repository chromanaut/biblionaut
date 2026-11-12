#include "trace/output.h"

#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <ostream>
#include <set>
#include <string_view>
#include <utility>

#include "trace/names.h"

namespace bn::trace_tool {

namespace {

// Thread ids for the tracks that are not device tasks.
constexpr int kLogTrack = 1000;
constexpr int kConsoleTrack = 1001;

std::string TaskLabel(std::uint8_t task) {
  if (task == core::kTraceTaskInterrupt) {
    return "interrupts";
  }
  if (task == core::kTraceTaskOther) {
    return "other tasks";
  }
  return "task " + std::to_string(task);
}

std::string TimeLabel(std::uint64_t timeUs) {
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "[%6" PRIu64 ".%06" PRIu64 "]", timeUs / 1'000'000, timeUs % 1'000'000);
  return buffer.data();
}

std::string DurationLabel(std::uint64_t durationUs) {
  std::array<char, 32> buffer{};
  if (durationUs < 1'000) {
    std::snprintf(buffer.data(), buffer.size(), "%" PRIu64 " us", durationUs);
  } else if (durationUs < 1'000'000) {
    std::snprintf(buffer.data(), buffer.size(), "%" PRIu64 ".%03" PRIu64 " ms", durationUs / 1'000, durationUs % 1'000);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%" PRIu64 ".%03" PRIu64 " s", durationUs / 1'000'000,
                  (durationUs % 1'000'000) / 1'000);
  }
  return buffer.data();
}

// "reader/app_start", plus the argument when it means something.
std::string SpanLabel(const Event& event, const SpanInfo& info) {
  std::string label = std::string(info.part) + "/" + info.name;
  if (!info.argument.empty()) {
    label += " " + std::string(info.argument) + "=" + std::to_string(event.arg);
  } else if (event.arg != 0) {
    label += " arg=" + std::to_string(event.arg);
  }
  return label;
}

int ProcessId(const Event& event) { return event.boot + 1; }

// One "name=value" or "name=last[min..max]" item of a metrics line. Returns
// false for an item with no number, such as a gauge that was never set.
bool ParseMetric(std::string_view item, std::string_view& name, std::uint64_t& value) {
  const std::size_t equals = item.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    return false;
  }
  name = item.substr(0, equals);
  std::string_view number = item.substr(equals + 1);
  number = number.substr(0, number.find('['));
  const auto result = std::from_chars(number.data(), number.data() + number.size(), value);
  return result.ec == std::errc() && result.ptr == number.data() + number.size();
}

}  // namespace

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::array<char, 8> buffer{};
          std::snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buffer.data();
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string FormatEvent(const Event& event) {
  switch (event.kind) {
    case EventKind::kHello: {
      std::string line = "=== boot " + std::to_string(event.boot) + ": " + event.text + " (stream protocol " +
                         std::to_string(event.arg) + ")";
      if (event.arg != kSupportedProtocol) {
        line += "; this tool reads protocol " + std::to_string(kSupportedProtocol) + ", so its frames are skipped";
      }
      return line + " ===";
    }
    case EventKind::kSpan:
      return TimeLabel(event.timeUs) + " " + TaskLabel(event.task) + "  " + SpanLabel(event, Describe(event.id)) +
             "  " + DurationLabel(event.durationUs);
    case EventKind::kInstant:
      return TimeLabel(event.timeUs) + " " + TaskLabel(event.task) + "  " + SpanLabel(event, Describe(event.id));
    case EventKind::kText:
      return TimeLabel(event.timeUs) + " log  " + event.text;
    case EventKind::kMetrics:
      return TimeLabel(event.timeUs) + " metrics  " + event.text;
    case EventKind::kRawText:
      return "              | " + event.text;
    case EventKind::kDropped:
      return TimeLabel(event.timeUs) + " !! " + std::to_string(event.arg) +
             " trace records were lost on the device before this point";
  }
  return {};
}

void PerfettoWriter::Write(std::ostream& out) const {
  out << "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
  bool first = true;
  const auto begin = [&out, &first] {
    out << (first ? "" : ",\n");
    first = false;
  };

  // Names for the processes (boots) and threads (tasks and tracks) in use.
  std::map<int, std::string> processes;
  std::set<std::pair<int, int>> tasks;
  std::set<std::pair<int, int>> tracks;
  for (const Event& event : events_) {
    const int pid = ProcessId(event);
    if (event.kind == EventKind::kHello) {
      processes[pid] = "Biblionaut " + event.text + " (boot " + std::to_string(event.boot) + ")";
    } else {
      processes.try_emplace(pid, event.boot == 0 ? "Biblionaut (before the first hello)" : "Biblionaut");
    }
    if (event.kind == EventKind::kSpan || event.kind == EventKind::kInstant) {
      tasks.emplace(pid, event.task);
    } else if (event.kind == EventKind::kText || event.kind == EventKind::kDropped) {
      tracks.emplace(pid, kLogTrack);
    } else if (event.kind == EventKind::kRawText) {
      tracks.emplace(pid, kConsoleTrack);
    }
  }
  for (const auto& [pid, name] : processes) {
    begin();
    out << R"({"ph":"M","name":"process_name","pid":)" << pid << R"(,"args":{"name":")" << JsonEscape(name) << "\"}}";
  }
  for (const auto& [pid, task] : tasks) {
    begin();
    out << R"({"ph":"M","name":"thread_name","pid":)" << pid << R"(,"tid":)" << task << R"(,"args":{"name":")"
        << JsonEscape(TaskLabel(static_cast<std::uint8_t>(task))) << "\"}}";
  }
  for (const auto& [pid, track] : tracks) {
    begin();
    out << R"({"ph":"M","name":"thread_name","pid":)" << pid << R"(,"tid":)" << track << R"(,"args":{"name":")"
        << (track == kLogTrack ? "log" : "console") << "\"}}";
  }

  for (const Event& event : events_) {
    const int pid = ProcessId(event);
    switch (event.kind) {
      case EventKind::kHello:
        break;
      case EventKind::kSpan:
      case EventKind::kInstant: {
        const SpanInfo info = Describe(event.id);
        begin();
        out << R"({"ph":")" << (event.kind == EventKind::kSpan ? "X" : "i") << R"(","name":")" << JsonEscape(info.name)
            << R"(","cat":")" << info.part << R"(","pid":)" << pid << R"(,"tid":)" << static_cast<int>(event.task)
            << R"(,"ts":)" << event.timeUs;
        if (event.kind == EventKind::kSpan) {
          out << R"(,"dur":)" << event.durationUs;
        } else {
          out << R"(,"s":"t")";
        }
        out << R"(,"args":{")" << (info.argument.empty() ? std::string_view("arg") : info.argument)
            << "\":" << event.arg << "}}";
        break;
      }
      case EventKind::kText:
      case EventKind::kRawText:
      case EventKind::kDropped: {
        std::string name = event.text;
        if (event.kind == EventKind::kDropped) {
          name = std::to_string(event.arg) + " trace records lost on the device";
        }
        begin();
        out << R"({"ph":"i","s":"t","name":")" << JsonEscape(name) << R"(","cat":")"
            << (event.kind == EventKind::kRawText ? "console" : "log") << R"(","pid":)" << pid << R"(,"tid":)"
            << (event.kind == EventKind::kRawText ? kConsoleTrack : kLogTrack) << R"(,"ts":)" << event.timeUs << "}";
        break;
      }
      case EventKind::kMetrics: {
        std::string_view rest = event.text;
        while (!rest.empty()) {
          const std::size_t space = rest.find(' ');
          const std::string_view item = rest.substr(0, space);
          rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
          std::string_view name;
          std::uint64_t value = 0;
          if (ParseMetric(item, name, value)) {
            begin();
            out << R"({"ph":"C","name":")" << JsonEscape(name) << R"(","pid":)" << pid << R"(,"ts":)" << event.timeUs
                << R"(,"args":{"value":)" << value << "}}";
          }
        }
        break;
      }
    }
  }
  out << "\n]}\n";
}

}  // namespace bn::trace_tool
