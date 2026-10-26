// The inputs the fuzz targets once crashed on, leaked on or hit undefined
// behaviour with (core/fuzz/regressions/TARGET); here they must just run
// (tool/fuzz.sh replays them under ASan and UBSan). FFmpeg-free builds
// only.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../fuzz/fuzz_targets.h"
#include "test.h"

namespace lychee {
namespace {

TEST(Fuzz, Regressions) {
  // A directory per target in core/fuzz/CMakeLists.txt.
  const std::map<std::string, void (*)(const uint8_t*, size_t)> targets = {
      {"mp4", FuzzMp4Demuxer},   {"matroska", FuzzMatroskaDemuxer},
      {"mpegts", FuzzTsDemuxer}, {"hls_playlist", FuzzHlsPlaylist},
      {"hls", FuzzHlsDemuxer},
  };
  int inputs = 0;
  for (const auto& dir :
       std::filesystem::directory_iterator(LYCHEE_FUZZ_REGRESSIONS)) {
    if (!dir.is_directory()) {
      continue;
    }
    auto target = targets.find(dir.path().filename().string());
    EXPECT_TRUE(target != targets.end());
    if (target == targets.end()) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
      if (entry.path().filename().string()[0] == '.') {
        continue;
      }
      std::ifstream file(entry.path(), std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
      target->second(bytes.data(), bytes.size());
      ++inputs;
    }
  }
  EXPECT_TRUE(inputs > 0);
}

}  // namespace
}  // namespace lychee
