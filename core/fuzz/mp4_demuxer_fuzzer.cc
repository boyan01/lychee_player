// libFuzzer target over Mp4Demuxer (tool/fuzz.sh). Built without
// libFuzzer (LYCHEE_FUZZ_STANDALONE), it runs the files or directories it
// is given once each, e.g. to replay a crash under a sanitizer.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "base/log.h"
#include "mp4_demuxer_fuzz.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  // Thousands of inputs a second: the parser's warnings would be most of
  // the time spent.
  lychee::SetLogSink([](void*, int, const char*) {}, nullptr);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lychee::FuzzMp4Demuxer(data, size);
  return 0;
}

#if defined(LYCHEE_FUZZ_STANDALONE)
int main(int argc, char** argv) {
  LLVMFuzzerInitialize(&argc, &argv);
  int runs = 0;
  auto run = [&runs](const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    ++runs;
  };
  for (int i = 1; i < argc; ++i) {
    if (std::filesystem::is_directory(argv[i])) {
      for (const auto& entry : std::filesystem::directory_iterator(argv[i])) {
        run(entry.path());
      }
    } else {
      run(argv[i]);
    }
  }
  std::printf("%d inputs\n", runs);
  return 0;
}
#endif
