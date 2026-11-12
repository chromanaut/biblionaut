#pragma once

#include <cstdint>
#include <span>

namespace bn::core {

// Somewhere bytes can be sent: a serial port, a file. The counterpart of
// ByteSource.
class ByteSink {
 public:
  // Takes all of `bytes` or none of them, without waiting. Returns false if
  // there was no room, in which case nothing was sent and the caller may try
  // again later. Because a write is all or nothing, something sent with one
  // call is never interleaved with another caller's bytes.
  [[nodiscard]] virtual bool Write(std::span<const std::uint8_t> bytes) noexcept = 0;

 protected:
  ~ByteSink() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
