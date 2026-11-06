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

#include "backends/audio/adts_demuxer.h"
#include "backends/audio/flac_demuxer.h"
#include "backends/audio/mp3_demuxer.h"
#include "backends/audio/ogg_demuxer.h"
#include "backends/audio/wav_demuxer.h"
#include "backends/hls/hls_demuxer.h"
#include "backends/hls/hls_playlist.h"
#include "backends/matroska/matroska_demuxer.h"
#include "backends/mp2t/ts_demuxer.h"
#include "backends/mp2t/ts_reader.h"
#include "backends/mp4/mp4_demuxer.h"
#include "backends/mp4/mp4_subtitle.h"
#include "media/pgs_subtitle_decoder.h"
#include "media/subtitle_file.h"
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
      if (stream.type == MediaType::kSubtitle && stream.subtitle.normalized) {
        SubtitleCue cue;
        UnpackSubtitleCue(packet, &cue);
      } else if (stream.type == MediaType::kSubtitle && stream.subtitle.text) {
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
  EncodedPacket normalized;
  normalized.buffer = PacketBuffer::Copy(data, size);
  normalized.pts = 0;
  normalized.duration = 10000000;
  SubtitleCue unpacked;
  UnpackSubtitleCue(normalized, &unpacked);
  std::vector<SubtitleCue> cues;
  const std::string_view sample(reinterpret_cast<const char*>(data), size);
  ParseMp4Subtitle("wvtt", sample, 0, 10000000, &cues);
  ParseMp4Subtitle("stpp", sample, 0, 10000000, &cues);
  Mp4Demuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzMp3Demuxer(const uint8_t* data, size_t size) {
  Mp3Demuxer::Detect(data, size);
  Mp3Demuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzFlacDemuxer(const uint8_t* data, size_t size) {
  FlacDemuxer::Detect(data, size);
  FlacDemuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzWavDemuxer(const uint8_t* data, size_t size) {
  WavDemuxer::Detect(data, size);
  WavDemuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzAdtsDemuxer(const uint8_t* data, size_t size) {
  AdtsDemuxer::Detect(data, size);
  AdtsDemuxer demuxer;
  std::string error;
  if (demuxer.Open(fuzz::Memory(data, size), &error)) {
    fuzz::Play(&demuxer);
  }
}

inline void FuzzOggDemuxer(const uint8_t* data, size_t size) {
  OggDemuxer::Detect(data, size);
  const auto play = [](const uint8_t* bytes, size_t length) {
    OggDemuxer demuxer;
    std::string error;
    if (demuxer.Open(fuzz::Memory(bytes, length), &error)) {
      fuzz::Play(&demuxer);
    }
  };
  play(data, size);
  // Also repair page checksums after mutation, so the fuzzer reaches
  // packet headers and continuation handling instead of stopping at CRC.
  std::vector<uint8_t> repaired(data, data + size);
  size_t at = 0;
  while (repaired.size() - at >= 27 &&
         std::memcmp(repaired.data() + at, "OggS", 4) == 0) {
    size_t length = 27 + repaired[at + 26];
    if (length > repaired.size() - at) break;
    for (size_t i = 27; i < 27 + size_t{repaired[at + 26]}; ++i) {
      length += repaired[at + i];
    }
    if (length > repaired.size() - at) break;
    std::fill_n(repaired.data() + at + 22, 4, 0);
    uint32_t crc = 0;
    for (size_t i = at; i < at + length; ++i) {
      crc ^= uint32_t{repaired[i]} << 24;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
      }
    }
    for (int i = 0; i < 4; ++i) {
      repaired[at + 22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    at += length;
  }
  if (at > 0) play(repaired.data(), repaired.size());
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

// A subtitle file for lychee_player_add_subtitle().
inline void FuzzSubtitleFile(const uint8_t* data, size_t size) {
  std::vector<SubtitleCue> cues;
  std::string format;
  ParseSubtitleFile(std::string_view(reinterpret_cast<const char*>(data), size),
                    "", &cues, &format);
}

// PGS subtitle packets: the input as one packet, then again after a flush
// as two, split in the middle (segments and objects across packets).
inline void FuzzPgsSubtitle(const uint8_t* data, size_t size) {
  PgsSubtitleDecoder decoder;
  std::vector<SubtitleCue> cues;
  auto feed = [&](size_t from, size_t to, int64_t pts) {
    EncodedPacket packet;
    packet.pts = pts;
    packet.duration = 1000000;
    packet.buffer = PacketBuffer::Copy(data + from, to - from);
    decoder.Decode(packet, &cues);
  };
  feed(0, size, 0);
  decoder.Flush();
  feed(0, size / 2, 1000000);
  feed(size / 2, size, 1000000);
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
