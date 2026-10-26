// What the MP4 fuzzer does with one input, shared with the unit test that
// replays the inputs it once crashed on (regressions/).

#ifndef LYCHEE_FUZZ_MP4_DEMUXER_FUZZ_H_
#define LYCHEE_FUZZ_MP4_DEMUXER_FUZZ_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backends/mp4/mp4_demuxer.h"
#include "media/text_subtitle_decoder.h"

namespace lychee {

// Opens |data| as an MP4 file and reads it as the player would: to the
// end (or a few thousand packets), then seeks to the middle, back to the
// start with only the first track, and past the end.
inline void FuzzMp4Demuxer(const uint8_t* data, size_t size) {
  Mp4Demuxer demuxer;
  std::string error;
  if (!demuxer.Open(CreateMemoryDataSource(std::vector<uint8_t>(data, data + size)),
                    &error)) {
    return;
  }
  const MediaInfo& info = demuxer.media_info();
  auto drain = [&](int limit) {
    EncodedPacket packet;
    for (int i = 0; i < limit && demuxer.Read(&packet) == DemuxerReadResult::kOk;
         ++i) {
      if (packet.stream_index >= 0 &&
          packet.stream_index < static_cast<int>(info.streams.size()) &&
          info.streams[packet.stream_index].type == MediaType::kSubtitle &&
          packet.buffer) {
        TextSubtitleDecoder::MovTextToPlainText(packet.buffer->data(), packet.buffer->size());
      }
    }
  };
  drain(4096);
  const int64_t duration = info.duration > 0 ? info.duration : 0;
  demuxer.Seek(info.start_time + duration / 2);
  drain(256);
  demuxer.SetEnabledStreams({0});
  demuxer.Seek(info.start_time);
  drain(256);
  demuxer.SetEnabledStreams({0, 1, 2});
  demuxer.Seek(info.start_time + duration + 1000000);
  drain(16);
}

}  // namespace lychee

#endif  // LYCHEE_FUZZ_MP4_DEMUXER_FUZZ_H_
