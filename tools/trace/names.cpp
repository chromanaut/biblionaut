#include "trace/names.h"

#include <array>

#include "epub/trace_ids.h"
#include "hw/trace_ids.h"
#include "reader/trace_ids.h"

namespace bn::trace_tool {

namespace {

struct KnownSpan {
  core::SpanId id;
  std::string_view name;
  std::string_view argument;
};

// One row per id, generated from each part's list. A part that gains trace
// ids adds its list here.
constexpr KnownSpan kKnownSpans[] = {
#define BN_TOOL_HW_ROW(index, constant, name, argument) \
  {core::MakeSpanId(core::TracePart::kHw, (index)), (name), (argument)},
    BN_HW_TRACE_IDS(BN_TOOL_HW_ROW)
#undef BN_TOOL_HW_ROW
#define BN_TOOL_EPUB_ROW(index, constant, name, argument) \
  {core::MakeSpanId(core::TracePart::kEpub, (index)), (name), (argument)},
        BN_EPUB_TRACE_IDS(BN_TOOL_EPUB_ROW)
#undef BN_TOOL_EPUB_ROW
#define BN_TOOL_READER_ROW(index, constant, name, argument) \
  {core::MakeSpanId(core::TracePart::kReader, (index)), (name), (argument)},
            BN_READER_TRACE_IDS(BN_TOOL_READER_ROW)
#undef BN_TOOL_READER_ROW
};

}  // namespace

std::string_view PartName(core::TracePart part) noexcept {
  switch (part) {
    case core::TracePart::kCore:
      return "core";
    case core::TracePart::kHw:
      return "hw";
    case core::TracePart::kEpub:
      return "epub";
    case core::TracePart::kDraw:
      return "draw";
    case core::TracePart::kLayout:
      return "layout";
    case core::TracePart::kReader:
      return "reader";
  }
  return "unknown";
}

SpanInfo Describe(core::SpanId id) {
  SpanInfo info;
  info.part = PartName(core::PartOf(id));
  for (const KnownSpan& span : kKnownSpans) {
    if (span.id == id) {
      info.name = span.name;
      info.argument = span.argument;
      info.known = true;
      return info;
    }
  }
  info.name = "#" + std::to_string(static_cast<unsigned>(id) & 0xFFU);
  return info;
}

std::size_t KnownSpanCount() noexcept { return std::size(kKnownSpans); }

}  // namespace bn::trace_tool
