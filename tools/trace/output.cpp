#include "trace/output.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <ostream>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "core/endian.h"
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

// The line for a span or an instant.
std::string FormatTraceEvent(const Event& event) {
  std::string line =
      TimeLabel(event.timeUs) + " " + TaskLabel(event.task) + "  " + SpanLabel(event, Describe(event.id));
  if (event.kind == EventKind::kSpan) {
    line += "  " + DurationLabel(event.durationUs);
  }
  return line;
}

int ProcessId(const Event& event) { return event.boot + 1; }

std::string Hex(std::uint32_t value) {
  std::array<char, 16> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "0x%08" PRIx32, value);
  return buffer.data();
}

// Names of the registers of a report, in the order they are stored.
std::span<const std::string_view> RegisterNames(core::CrashArch arch) {
  static constexpr std::array<std::string_view, 36> kEsp32C3 = {
      "mepc", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2", "s0",      "s1",    "a0",     "a1",
      "a2",   "a3", "a4",  "a5",  "a6", "a7", "s2", "s3", "s4",      "s5",    "s6",     "s7",
      "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6", "mstatus", "mtvec", "mcause", "mtval"};
  return arch == core::CrashArch::kEsp32C3 ? std::span<const std::string_view>(kEsp32C3)
                                           : std::span<const std::string_view>();
}

std::string RegisterName(std::span<const std::string_view> names, std::size_t index) {
  return index < names.size() ? std::string(names[index]) : "r" + std::to_string(index);
}

std::string BootsAgo(std::uint32_t boots) {
  if (boots <= 1) {
    return "the run before this one";
  }
  return std::to_string(boots) + " restarts ago";
}

// The words of the saved stack that could be return addresses, with where
// each was found.
std::vector<std::pair<std::size_t, std::uint32_t>> ReturnAddressCandidates(const core::CrashReport& report) {
  std::vector<std::pair<std::size_t, std::uint32_t>> found;
  const std::size_t size = std::min<std::size_t>(report.stackSize, report.stack.size());
  for (std::size_t at = 0; at + 4 <= size; at += 4) {
    const std::uint32_t word = core::LoadLe32(std::span(report.stack).subspan(at).first<4>());
    if (IsCodeAddress(report.arch, word)) {
      found.emplace_back(at, word);
    }
  }
  return found;
}

// A trace record of a crash report as an event, with its time worked out
// back from the time of the crash.
Event CrashTraceEvent(const core::CrashReport& report, const core::TraceRecord& record) {
  Event event;
  event.kind = record.kind == core::TraceKind::kSpan ? EventKind::kSpan : EventKind::kInstant;
  event.id = record.id;
  event.task = record.task;
  event.arg = record.arg;
  event.durationUs = event.kind == EventKind::kSpan ? record.durationUs : 0;
  const std::uint32_t end = record.startUs + record.durationUs;
  // How long before the crash it ended; 32-bit arithmetic undoes a wrap of the clock in between.
  const std::uint32_t beforeCrash = static_cast<std::uint32_t>(report.timeUs) - end;
  const std::uint64_t endUs = beforeCrash > report.timeUs ? 0 : report.timeUs - beforeCrash;
  event.timeUs = endUs >= event.durationUs ? endUs - event.durationUs : 0;
  return event;
}

std::string FormatCrash(const Event& event) {
  const core::CrashReport& report = *event.crash;
  const std::span<const std::string_view> names = RegisterNames(report.arch);
  const std::string_view version = core::CrashText(report.version);
  const std::string_view buildId = core::CrashText(report.buildId);
  const std::string_view task = core::CrashText(report.task);
  const std::size_t registerCount = std::min<std::size_t>(report.registerCount, report.registers.size());

  std::string out = "=== crash report: " + BootsAgo(report.bootsSince) + " ended in a crash ===\n";
  out += "  reason     " + std::string(core::CrashText(report.reason)) + "\n";
  out += "  firmware   " + (version.empty() ? std::string("(not recorded)") : std::string(version));
  if (!buildId.empty()) {
    out += "  (ELF " + std::string(buildId) + ")";
  }
  out += '\n';

  std::array<char, 64> when{};
  std::snprintf(when.data(), when.size(), "%" PRIu64 ".%06" PRIu64 " s after it started", report.timeUs / 1'000'000,
                report.timeUs % 1'000'000);
  out += "  when       " + std::string(when.data()) + ", in task " +
         (task.empty() ? std::string("(unknown)") : std::string(task)) + "\n";

  out += "  registers";
  for (std::size_t i = 0; i < registerCount; ++i) {
    out += (i % 4 == 0) ? (i == 0 ? "  " : "\n             ") : "  ";
    std::string name = RegisterName(names, i);
    name.resize(std::max<std::size_t>(name.size(), 7), ' ');
    out += name + " " + Hex(report.registers[i]);
  }
  out += '\n';

  out += "  stack      " + std::to_string(report.stackSize) + " bytes saved from " + Hex(report.stackAddress);
  const auto candidates = ReturnAddressCandidates(report);
  if (candidates.empty()) {
    out += "; nothing in them looks like a return address\n";
  } else {
    out += "; values in them that could be return addresses:\n";
    for (const auto& [at, address] : candidates) {
      std::array<char, 48> line{};
      std::snprintf(line.data(), line.size(), "             sp+0x%03zx  ", at);
      out += line.data() + Hex(address) + "\n";
    }
  }

  if (report.traceCount == 0) {
    out += "  trace      none recorded before the crash\n";
  } else {
    out += "  trace      the last " + std::to_string(report.traceCount) + " records before the crash:\n";
    for (std::size_t i = 0; i < report.traceCount && i < report.trace.size(); ++i) {
      out += "             " + FormatTraceEvent(CrashTraceEvent(report, report.trace[i])) + "\n";
    }
  }
  out += "=== end of crash report; tools/bn-symbolize turns its addresses into source lines ===";
  return out;
}

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

bool IsCodeAddress(core::CrashArch arch, std::uint32_t address) noexcept {
  if (arch != core::CrashArch::kEsp32C3) {
    return false;
  }
  // The chip's ROM, code kept in RAM, and code run from flash.
  const bool rom = address >= 0x40000000 && address < 0x40060000;
  const bool ram = address >= 0x40380000 && address < 0x403E0000;
  const bool flash = address >= 0x42000000 && address < 0x42800000;
  // Instructions are 2 or 4 bytes long, so an odd value is not one's address.
  return (rom || ram || flash) && (address & 1U) == 0;
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
    case EventKind::kInstant:
      return FormatTraceEvent(event);
    case EventKind::kText:
      return TimeLabel(event.timeUs) + " log  " + event.text;
    case EventKind::kMetrics:
      return TimeLabel(event.timeUs) + " metrics  " + event.text;
    case EventKind::kRawText:
      return "              | " + event.text;
    case EventKind::kDropped:
      return TimeLabel(event.timeUs) + " !! " + std::to_string(event.arg) +
             " trace records were lost on the device before this point";
    case EventKind::kCrash:
      return event.crash != nullptr ? FormatCrash(event) : std::string();
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
    } else if (event.kind == EventKind::kText || event.kind == EventKind::kDropped || event.kind == EventKind::kCrash) {
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
      case EventKind::kCrash: {
        if (event.crash == nullptr) {
          break;
        }
        // A marker at the start of the run that reported it; the details are
        // its arguments, shown when it is selected.
        const core::CrashReport& report = *event.crash;
        begin();
        out << R"({"ph":"i","s":"p","name":")" << JsonEscape("crash report: " + event.text)
            << R"(","cat":"crash","pid":)" << pid << R"(,"tid":)" << kLogTrack << R"(,"ts":)" << event.timeUs
            << R"(,"args":{"reason":")" << JsonEscape(event.text) << R"(","firmware":")"
            << JsonEscape(core::CrashText(report.version)) << R"(","elf":")"
            << JsonEscape(core::CrashText(report.buildId)) << R"(","task":")"
            << JsonEscape(core::CrashText(report.task)) << R"(","restarts_since":)" << report.bootsSince
            << R"(,"crashed_at_us":)" << report.timeUs << R"(,"pc":")"
            << (report.registerCount > 0 ? Hex(report.registers[0]) : std::string()) << "\"}}";
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
