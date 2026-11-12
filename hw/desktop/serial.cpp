#include <cstdio>
#include <cstdlib>

#include "hw/platform.h"

namespace bn::hw {

namespace {

// Stands in for the device's USB serial line: appends everything to a file,
// so a desktop run can be decoded with the same tools as a device capture.
class FileSerial final : public core::ByteSink {
 public:
  FileSerial() noexcept {
    const char* path = std::getenv("BN_SERIAL_OUT");
    if (path != nullptr && *path != '\0') {
      file_ = std::fopen(path, "wb");
      if (file_ == nullptr) {
        std::fprintf(stderr, "warning: cannot open BN_SERIAL_OUT file '%s'; serial output is discarded\n", path);
      }
    }
  }

  FileSerial(const FileSerial&) = delete;
  FileSerial& operator=(const FileSerial&) = delete;

  ~FileSerial() {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  [[nodiscard]] bool Write(std::span<const std::uint8_t> bytes) noexcept override {
    if (file_ == nullptr || bytes.empty()) {
      return true;  // Nowhere to send it; behave like a line nobody is listening to.
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
    std::fflush(file_);
    return written;
  }

 private:
  std::FILE* file_ = nullptr;
};

}  // namespace

core::ByteSink& SystemSerial() noexcept {
  static FileSerial instance;
  return instance;
}

}  // namespace bn::hw
