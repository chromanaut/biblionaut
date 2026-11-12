#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "trace/decoder.h"
#include "trace/input.h"
#include "trace/output.h"

namespace {

volatile std::sig_atomic_t interrupted = 0;

void OnInterrupt(int /*signal*/) { interrupted = 1; }

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [options] [INPUT]\n"
               "\n"
               "Decodes a Biblionaut diagnostic stream: prints its events and, with --out,\n"
               "writes a timeline to open at ui.perfetto.dev or chrome://tracing.\n"
               "\n"
               "INPUT is a capture file, '-' for standard input, or a serial device such\n"
               "as /dev/cu.usbmodem101. With no INPUT, the USB serial device is found\n"
               "automatically. A serial device is restarted first, so the capture begins\n"
               "with its boot, and is then read until Ctrl-C.\n"
               "\n"
               "  -o, --out FILE    write the timeline (trace-event JSON) to FILE\n"
               "  -s, --save FILE   also save the bytes read, unchanged, to FILE\n"
               "  -q, --quiet       do not print the events\n"
               "      --seconds N   stop after N seconds\n"
               "      --no-reset    do not restart the device before reading a serial device\n",
               program);
}

struct Options {
  std::string input;
  std::string out;
  std::string save;
  bool quiet = false;
  bool reset = true;
  double seconds = 0;  // 0: no limit.
};

// Returns false if the arguments are wrong.
bool ParseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool hasValue = i + 1 < argc;
    if ((argument == "-o" || argument == "--out") && hasValue) {
      options.out = argv[++i];
    } else if ((argument == "-s" || argument == "--save") && hasValue) {
      options.save = argv[++i];
    } else if (argument == "-q" || argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--no-reset") {
      options.reset = false;
    } else if (argument == "--seconds" && hasValue) {
      char* end = nullptr;
      options.seconds = std::strtod(argv[++i], &end);
      if (end == nullptr || *end != '\0' || options.seconds <= 0) {
        return false;
      }
    } else if (argument == "-" || argument.empty() || argument[0] != '-') {
      if (!options.input.empty()) {
        return false;
      }
      options.input = argument;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, options)) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string error;
  if (options.input.empty()) {
    options.input =
        bn::trace_tool::ChooseDevice(bn::trace_tool::FindDevices(bn::trace_tool::SerialDevicePatterns()), error);
    if (options.input.empty()) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stderr, "Found %s.\n", options.input.c_str());
  }

  bn::trace_tool::Input input;
  if (!input.Open(options.input, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  std::ofstream saved;
  if (!options.save.empty()) {
    saved.open(options.save, std::ios::binary);
    if (!saved) {
      std::fprintf(stderr, "error: cannot write %s\n", options.save.c_str());
      return 1;
    }
  }

  std::signal(SIGINT, OnInterrupt);
  bn::trace_tool::BootFilter bootFilter;
  bootFilter.GiveUp();
  if (input.IsLive()) {
    if (options.reset) {
      // Without this the capture starts wherever the device happens to be,
      // and misses the boot messages and the hello that names the firmware.
      if (input.ResetDevice()) {
        std::fprintf(stderr, "Restarted the device.\n");
        bootFilter = bn::trace_tool::BootFilter();
      } else {
        std::fprintf(stderr, "Could not restart the device; reading from where it is.\n");
      }
    }
    std::fprintf(stderr, "Reading %s; press Ctrl-C to stop.\n", options.input.c_str());
  }

  bn::trace_tool::StreamDecoder decoder;
  bn::trace_tool::PerfettoWriter timeline;
  const auto handleEvents = [&] {
    for (const bn::trace_tool::Event& event : decoder.TakeEvents()) {
      if (!bootFilter.Keep(event)) {
        continue;
      }
      if (!options.quiet) {
        std::puts(bn::trace_tool::FormatEvent(event).c_str());
      }
      timeline.Add(event);
    }
    std::fflush(stdout);
  };

  // How long to wait for a restarted device to show its boot before showing
  // whatever arrives.
  constexpr double kBootWaitSeconds = 3.0;

  const auto started = std::chrono::steady_clock::now();
  std::array<std::uint8_t, 4096> buffer{};
  while (interrupted == 0) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (options.seconds > 0 && elapsed >= options.seconds) {
      break;
    }
    if (bootFilter.Waiting() && elapsed >= kBootWaitSeconds) {
      std::fprintf(stderr, "The device did not show a boot after the restart; showing what it sends.\n");
      bootFilter.GiveUp();
    }
    std::size_t count = 0;
    const auto result = input.Read(buffer, 100, count);
    if (result == bn::trace_tool::Input::Result::kEnd) {
      break;
    }
    if (result == bn::trace_tool::Input::Result::kData) {
      const std::span<const std::uint8_t> bytes = std::span(buffer).first(count);
      if (saved.is_open()) {
        saved.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      }
      decoder.Feed(bytes);
      handleEvents();
    }
  }
  decoder.Finish();
  handleEvents();

  if (!options.out.empty()) {
    std::ofstream out(options.out);
    if (!out) {
      std::fprintf(stderr, "error: cannot write %s\n", options.out.c_str());
      return 1;
    }
    timeline.Write(out);
  }

  if (bootFilter.Skipped() > 0) {
    std::fprintf(stderr, "Left out %zu events the device had buffered from before it was restarted.\n",
                 bootFilter.Skipped());
  }
  const bn::trace_tool::DecoderStats& stats = decoder.Stats();
  std::fprintf(stderr, "%zu bytes read. Frames: %zu. Trace records: %zu. Plain-text lines: %zu. Boots: %d.\n",
               stats.bytes, stats.frames, stats.records, stats.rawTextLines, stats.boots);
  if (stats.droppedRecords > 0) {
    std::fprintf(stderr, "warning: %zu trace records were lost on the device (its buffer filled)\n",
                 stats.droppedRecords);
  }
  if (stats.unsupportedFrames > 0) {
    std::fprintf(stderr, "warning: %zu frames were skipped (unknown type or stream protocol)\n",
                 stats.unsupportedFrames);
  }
  if (!options.out.empty()) {
    std::fprintf(stderr, "Timeline written to %s\n", options.out.c_str());
  }
  return 0;
}
