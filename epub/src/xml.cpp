#include "epub/xml.h"

#include <expat.h>

#include <cstring>

#include "core/metrics.h"

namespace bn::epub {

namespace {

// Documents begun, and documents that could not be read to the end
// (whatever the reason, a stop asked for by the caller included).
core::Counter xmlDocuments{"xml_documents"};
core::Counter xmlFailures{"xml_failures"};
// The most of its memory any parser has used for one document, in bytes.
core::Gauge xmlMemory{"xml_memory"};

// The salt used until another is set: no secret, so no protection, but
// the same tables every time.
constexpr std::array<std::uint8_t, 16> kFixedSalt = {0x42, 0x69, 0x62, 0x6C, 0x69, 0x6F, 0x6E, 0x61,
                                                     0x75, 0x74, 0x2D, 0x78, 0x6D, 0x6C, 0x2D, 0x31};

// expat asks for memory through three plain functions that are told
// nothing of who is asking. So the heap of the parser that is at work is
// kept here while it is, for each task separately: every call into expat
// is made inside a HeapScope.
thread_local core::RegionHeap* heapAtWork = nullptr;

class HeapScope {
 public:
  explicit HeapScope(core::RegionHeap& heap) noexcept : before_(heapAtWork) { heapAtWork = &heap; }
  ~HeapScope() { heapAtWork = before_; }

  HeapScope(const HeapScope&) = delete;
  HeapScope& operator=(const HeapScope&) = delete;

 private:
  // A handler may itself start another parser on the same task; when that
  // one is done, this one's heap is the one at work again.
  core::RegionHeap* before_;
};

void* ExpatAllocate(std::size_t size) { return heapAtWork != nullptr ? heapAtWork->Allocate(size) : nullptr; }

void* ExpatResize(void* piece, std::size_t size) {
  return heapAtWork != nullptr ? heapAtWork->Resize(piece, size) : nullptr;
}

void ExpatFree(void* piece) {
  if (heapAtWork != nullptr) {
    heapAtWork->Free(piece);
  }
}

constexpr XML_Memory_Handling_Suite kMemory = {
    .malloc_fcn = ExpatAllocate, .realloc_fcn = ExpatResize, .free_fcn = ExpatFree};

std::uint32_t Clamped(std::uint64_t value) noexcept {
  return value > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(value);
}

// Where expat has got to in the document.
std::uint32_t LineOf(XML_Parser parser) noexcept { return Clamped(XML_GetCurrentLineNumber64(parser)); }

std::uint32_t ByteOffsetOf(XML_Parser parser) noexcept {
  const std::int64_t offset = XML_GetCurrentByteIndex64(parser);
  return offset < 0 ? 0 : Clamped(static_cast<std::uint64_t>(offset));
}

}  // namespace

const char* XmlStatusName(XmlStatus status) noexcept {
  switch (status) {
    case XmlStatus::kOk:
      return "ok";
    case XmlStatus::kNotStarted:
      return "not started";
    case XmlStatus::kNotWellFormed:
      return "not well-formed";
    case XmlStatus::kUnsupportedEncoding:
      return "unsupported encoding";
    case XmlStatus::kNoMemory:
      return "no memory";
    case XmlStatus::kSourceFailed:
      return "source failed";
    case XmlStatus::kStopped:
      return "stopped";
  }
  return "unknown";
}

std::string_view XmlLocalName(std::string_view name) noexcept {
  const std::size_t colon = name.rfind(':');
  if (colon != std::string_view::npos) {
    name.remove_prefix(colon + 1);
  }
  return name;
}

XmlAttributes::XmlAttributes(const char* const* pairs) noexcept : pairs_(pairs) {
  if (pairs_ != nullptr) {
    while (pairs_[2 * count_] != nullptr) {
      ++count_;
    }
  }
}

std::string_view XmlAttributes::Name(std::size_t index) const noexcept {
  return index < count_ ? std::string_view(pairs_[2 * index]) : std::string_view();
}

std::string_view XmlAttributes::Value(std::size_t index) const noexcept {
  return index < count_ ? std::string_view(pairs_[(2 * index) + 1]) : std::string_view();
}

bool XmlAttributes::Find(std::string_view name, std::string_view& value) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (Name(index) == name) {
      value = Value(index);
      return true;
    }
  }
  return false;
}

XmlParser::XmlParser(std::span<std::uint8_t> memory) noexcept : heap_(memory), salt_(kFixedSalt) {}

XmlParser::~XmlParser() { End(); }

void XmlParser::End() noexcept {
  if (parser_ != nullptr) {
    const HeapScope scope(heap_);
    XML_ParserFree(parser_);
    parser_ = nullptr;
  }
}

XmlStatus XmlParser::Begin(core::ByteSource& source, XmlHandler& handler) noexcept {
  End();
  // The last document's memory is all given back: each starts with the
  // whole of it, in one piece.
  heap_.Reset();
  source_ = &source;
  handler_ = &handler;
  status_ = XmlStatus::kOk;
  finished_ = false;
  inHandler_ = false;
  errorCode_ = 0;
  line_ = 1;
  byteOffset_ = 0;
  xmlDocuments.Add();

  const HeapScope scope(heap_);
  parser_ = XML_ParserCreate_MM(nullptr, &kMemory, nullptr);
  if (parser_ == nullptr) {
    Fail(XmlStatus::kNoMemory);
    return status_;
  }
  XML_SetHashSalt16Bytes(parser_, salt_.data());
  XML_SetUserData(parser_, this);
  XML_SetElementHandler(parser_, HandleStart, HandleEnd);
  XML_SetCharacterDataHandler(parser_, HandleText);
  XML_SetSkippedEntityHandler(parser_, HandleEntity);
  return status_;
}

bool XmlParser::Step() noexcept {
  if (parser_ == nullptr || finished_ || status_ != XmlStatus::kOk) {
    return false;
  }
  const HeapScope scope(heap_);
  // The source reads straight into expat's own buffer.
  void* buffer = XML_GetBuffer(parser_, static_cast<int>(kChunkBytes));
  if (buffer == nullptr) {
    return Fail(XmlStatus::kNoMemory);
  }
  const std::ptrdiff_t read = source_->Read(std::span(static_cast<std::uint8_t*>(buffer), kChunkBytes));
  if (read < 0 || static_cast<std::size_t>(read) > kChunkBytes) {
    return Fail(XmlStatus::kSourceFailed);
  }
  const bool last = read == 0;
  const XML_Status parsed = XML_ParseBuffer(parser_, static_cast<int>(read), last ? XML_TRUE : XML_FALSE);
  xmlMemory.Set(Clamped(heap_.HighWater()));
  if (parsed != XML_STATUS_OK) {
    const XML_Error error = XML_GetErrorCode(parser_);
    switch (error) {
      case XML_ERROR_NO_MEMORY:
        return Fail(XmlStatus::kNoMemory);
      case XML_ERROR_ABORTED:
        return Fail(XmlStatus::kStopped);
      case XML_ERROR_UNKNOWN_ENCODING:
      case XML_ERROR_INCORRECT_ENCODING:
        errorCode_ = static_cast<int>(error);
        return Fail(XmlStatus::kUnsupportedEncoding);
      default:
        errorCode_ = static_cast<int>(error);
        return Fail(XmlStatus::kNotWellFormed);
    }
  }
  if (last) {
    finished_ = true;
    line_ = LineOf(parser_);
    byteOffset_ = ByteOffsetOf(parser_);
    return false;
  }
  return true;
}

XmlStatus XmlParser::Parse(core::ByteSource& source, XmlHandler& handler) noexcept {
  if (Begin(source, handler) == XmlStatus::kOk) {
    while (Step()) {
    }
  }
  return status_;
}

void XmlParser::Stop() noexcept {
  if (parser_ == nullptr || finished_ || status_ != XmlStatus::kOk) {
    return;
  }
  if (inHandler_) {
    // expat ends the call it is in, and Step() sees why.
    XML_StopParser(parser_, XML_FALSE);
  } else {
    Fail(XmlStatus::kStopped);
  }
}

bool XmlParser::Fail(XmlStatus status) noexcept {
  status_ = status;
  if (parser_ != nullptr) {
    line_ = LineOf(parser_);
    byteOffset_ = ByteOffsetOf(parser_);
  }
  xmlFailures.Add();
  return false;
}

std::uint32_t XmlParser::Line() const noexcept {
  if (parser_ != nullptr && status_ == XmlStatus::kOk && !finished_) {
    return LineOf(parser_);
  }
  return line_;
}

std::uint32_t XmlParser::ByteOffset() const noexcept {
  if (parser_ != nullptr && status_ == XmlStatus::kOk && !finished_) {
    return ByteOffsetOf(parser_);
  }
  return byteOffset_;
}

// While a handler runs, the heap at work is still this parser's, so that a
// Stop() from inside it, which calls into expat, finds it there.

void XmlParser::HandleStart(void* self, const char* name, const char** attributes) noexcept {
  auto* parser = static_cast<XmlParser*>(self);
  parser->inHandler_ = true;
  parser->handler_->OnStartElement(name, XmlAttributes(attributes));
  parser->inHandler_ = false;
}

void XmlParser::HandleEnd(void* self, const char* name) noexcept {
  auto* parser = static_cast<XmlParser*>(self);
  parser->inHandler_ = true;
  parser->handler_->OnEndElement(name);
  parser->inHandler_ = false;
}

void XmlParser::HandleText(void* self, const char* text, int length) noexcept {
  auto* parser = static_cast<XmlParser*>(self);
  if (length <= 0) {
    return;
  }
  parser->inHandler_ = true;
  parser->handler_->OnText(std::string_view(text, static_cast<std::size_t>(length)));
  parser->inHandler_ = false;
}

void XmlParser::HandleEntity(void* self, const char* name, int isParameterEntity) noexcept {
  auto* parser = static_cast<XmlParser*>(self);
  if (isParameterEntity != 0) {
    return;  // Those belong to DTDs, which are not read.
  }
  parser->inHandler_ = true;
  parser->handler_->OnEntity(name);
  parser->inHandler_ = false;
}

}  // namespace bn::epub
