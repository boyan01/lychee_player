// What each fuzz target (fuzzer.cc, tool/fuzz.sh) does with one input,
// shared with the unit test that replays the inputs they once crashed on
// (regressions/TARGET/).

#ifndef LYCHEE_FUZZ_FUZZ_TARGETS_H_
#define LYCHEE_FUZZ_FUZZ_TARGETS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backends/hls/hls_demuxer.h"
#include "backends/hls/hls_playlist.h"
#include "backends/matroska/matroska_demuxer.h"
#include "backends/mp2t/ts_demuxer.h"
#include "backends/mp2t/ts_reader.h"
#include "backends/mp4/mp4_demuxer.h"
#include "media/text_subtitle_decoder.h"

namespace lychee {

namespace fuzz {

// Reads an opened |demuxer| as the player would: to the end (or a few
// thousand packets), then seeks to the middle, back to the start with
// only the first track, and past the end. Text subtitles go through the
// decoder's plain text conversion.
inline void Play(Demuxer* demuxer) {
  const MediaInfo& info = demuxer->media_info();
  auto drain = [&](int limit) {
    EncodedPacket packet;
    for (int i = 0;
         i < limit && demuxer->Read(&packet) == DemuxerReadResult::kOk; ++i) {
      if (packet.stream_index < 0 ||
          packet.stream_index >= static_cast<int>(info.streams.size()) ||
          !packet.buffer) {
        continue;
      }
      const StreamInfo& stream = info.streams[packet.stream_index];
      if (stream.type == MediaType::kSubtitle && stream.subtitle.text) {
        TextSubtitleDecoder::ToPlainText(
            stream.subtitle.codec_name,
            std::string_view(
                reinterpret_cast<const char*>(packet.buffer->data()),
                packet.buffer->size()));
      }
    }
  };
  drain(4096);
  const int64_t duration = info.duration > 0 ? info.duration : 0;
  demuxer->Seek(info.start_time + duration / 2);
  drain(256);
  demuxer->SetEnabledStreams({0});
  demuxer->Seek(info.start_time);
  drain(256);
  demuxer->SetEnabledStreams({0, 1, 2});
  demuxer->Seek(info.start_time + duration + 1000000);
  drain(16);
}

inline std::unique_ptr<DataSource> Memory(const uint8_t* data, size_t size) {
  return CreateMemoryDataSource(std::vector<uint8_t>(data, data + size));
}

// The resources of an HLS input, which are separated by this line.
constexpr char kHlsPartSeparator[] = "\n#PART\n";

}  // namespace fuzz

inline void FuzzMp4Demuxer(const uint8_t* data, size_t size) {
  Mp4Demuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzMatroskaDemuxer(const uint8_t* data, size_t size) {
  MatroskaDemuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzTsDemuxer(const uint8_t* data, size_t size) {
  TsReader::Detect(data, size);
  TsDemuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

// The playlist parser alone, against a URL base and a path base.
inline void FuzzHlsPlaylist(const uint8_t* data, size_t size) {
  HlsDemuxer::Detect(data, size);
  const std::string text(reinterpret_cast<const char*>(data), size);
  std::string error;
  for (const char* base :
       {"http://a.example/b/c/index.m3u8?t=1", "/tmp/b/index.m3u8"}) {
    HlsPlaylist playlist;
    if (ParseHlsPlaylist(text, base, &playlist, &error)) {
      PickHlsVariant(playlist.variants);
    }
  }
}

// The HLS demuxer over an input of resources separated by
// fuzz::kHlsPartSeparator: the playlist, then what its URLs name, in the
// order the demuxer first opens them (the variant playlist, segments,
// renditions, keys). A URL opened again gets the same resource, except
// that a playlist reloaded (live) gets the next unused part if that is a
// playlist: its next version. Live reloads wait on a clock that jumps
// ahead, at most 32 times.
inline void FuzzHlsDemuxer(const uint8_t* data, size_t size) {
  std::vector<std::vector<uint8_t>> parts;
  const size_t separator = std::strlen(fuzz::kHlsPartSeparator);
  size_t begin = 0;
  for (;;) {
    const void* found =
        memmem(data + begin, size - begin, fuzz::kHlsPartSeparator, separator);
    const size_t end = found ? static_cast<const uint8_t*>(found) - data : size;
    parts.emplace_back(data + begin, data + end);
    if (!found) {
      break;
    }
    begin = end + separator;
  }
  auto playlist = [&](size_t part) {
    return part < parts.size() &&
           HlsDemuxer::Detect(parts[part].data(), parts[part].size());
  };
  static constexpr char kUrl[] = "http://fuzz/index.m3u8";
  std::map<std::string, size_t> opened = {{kUrl, 0}};
  size_t next = 1;
  HlsDemuxer demuxer;
  demuxer.set_opener([&](const std::string& url, std::string* error) {
    auto it = opened.find(url);
    if (it == opened.end()) {
      it = opened.emplace(url, next++).first;
    } else if (playlist(it->second) && playlist(next)) {
      it->second = next++;
    }
    if (it->second >= parts.size()) {
      *error = "not found";
      return std::unique_ptr<DataSource>();
    }
    return CreateMemoryDataSource(parts[it->second]);
  });
  int64_t now = 0;
  int waits = 0;
  demuxer.set_clock({[&] { return now; },
                     [&](int64_t until) {
                       now = std::max(now, until);
                       return ++waits <= 32;
                     }});
  std::string error;
  if (demuxer.Open(CreateMemoryDataSource(parts[0]), kUrl, &error)) {
    fuzz::Play(&demuxer);
  }
}

}  // namespace lychee

#endif  // LYCHEE_FUZZ_FUZZ_TARGETS_H_
