// What a fuzz target is in a build without a fuzzing engine: a program
// that gives the target each of the inputs kept for it, once.
//
//   fuzz_zip tests/fuzz/inputs/zip        every file in the directory
//   fuzz_zip some-file another-file       or the files named
//
// It ends with 0 if the target took every input without complaint. A
// target that finds something wrong ends the program there, having said
// what; the input it was given is named just before.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

// The files to go through: those named, and for a directory every file in
// it, in the order of their names.
std::vector<std::filesystem::path> InputsNamedBy(int argc, char** argv) {
  std::vector<std::filesystem::path> inputs;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path path(argv[i]);
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
      inputs.push_back(path);
      continue;
    }
    std::vector<std::filesystem::path> inside;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, error)) {
      if (entry.is_regular_file(error)) {
        inside.push_back(entry.path());
      }
    }
    std::sort(inside.begin(), inside.end());
    inputs.insert(inputs.end(), inside.begin(), inside.end());
  }
  return inputs;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::filesystem::path> inputs = InputsNamedBy(argc, argv);
  if (inputs.empty()) {
    // A directory that has gone missing must not pass for one whose inputs
    // were all taken.
    std::fprintf(stderr, "no inputs: give the files, or a directory of them\n");
    return 2;
  }
  for (const std::filesystem::path& path : inputs) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "%s: cannot be read\n", path.string().c_str());
      return 2;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // The target gets the input in memory of exactly its size, as the
    // engine gives it, so that reading past its end is seen.
    const std::size_t size = bytes.size();
    const std::unique_ptr<std::uint8_t[]> data(new std::uint8_t[size]);
    std::copy(bytes.begin(), bytes.end(), data.get());
    std::fprintf(stderr, "%s\n", path.string().c_str());
    (void)LLVMFuzzerTestOneInput(data.get(), size);
  }
  std::fprintf(stderr, "%zu inputs\n", inputs.size());
  return 0;
}
