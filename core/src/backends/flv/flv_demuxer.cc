#include "backends/flv/flv_demuxer.h"

#include "backends/mp2t/elementary_stream.h"
#include "media/annexb.h"

namespace lychee {
bool FlvDemuxer::Parse() {
  uint8_t header[15];
  if (!Exact(0, header, 9) || !Detect(header, 9)) return false;
  int64_t offset = BE32(header + 5);
  if (offset < 9 || offset > INT32_MAX) return false;
  if (!Exact(offset, header, 4) || BE32(header) != 0) return false;
  offset += 4;
  int video = -1, audio = -1;
  std::vector<int64_t> previous;
  while (source_->size() < 0 || offset < source_->size()) {
    if (!Exact(offset, header, 11)) return false;
    const uint32_t size = BE24(header + 1);
    if (BE24(header + 8) != 0 || (header[0] & 0xe0) || size > 16 * 1024 * 1024)
      return false;
    const uint8_t type = header[0];
    const int64_t dts =
        (int64_t(BE24(header + 4)) | int64_t(header[7]) << 24) * 1000;
    const int64_t payload = offset + 11;
    uint8_t first[5] = {};
    if ((type == 8 || type == 9) &&
        (size < 2 || !Exact(payload, first, std::min<size_t>(size, 5))))
      return false;
    int stream = -1;
    size_t skip = 0;
    int64_t pts = dts;
    bool key = true;
    if (type == 9) {
      if ((first[0] & 15) != 7 || size < 5) {
        error_ = "flv: only AVC video is supported";
        return false;
      }
      if (first[1] == 0) {
        if (video >= 0) {
          error_ = "flv: changing AVC configuration";
          return false;
        }
        StreamInfo s;
        s.type = MediaType::kVideo;
        s.video.codec = VideoCodec::kH264;
        s.video.codec_name = "h264";
        s.video.extradata.resize(size - 5);
        if (!Exact(payload + 5, s.video.extradata.data(), size - 5))
          return false;
        AnnexBConverter converter;
        if (!converter.Initialize(s.video)) return false;
        for (const auto& nal : converter.parameter_sets()) {
          std::vector<uint8_t> annex;
          AnnexBConverter::AppendNal(nal.data(), nal.size(), &annex);
          AccessUnitInfo au;
          if (ParseAccessUnit(annex.data(), annex.size(), false, &au) &&
              au.has_sps) {
            s.video.width = au.width;
            s.video.height = au.height;
            s.video.sar_num = au.sar_num;
            s.video.sar_den = au.sar_den;
            s.video.color = au.color;
          }
        }
        video = static_cast<int>(info_.streams.size());
        info_.streams.push_back(std::move(s));
        previous.push_back(-1);
      } else if (first[1] == 1) {
        if (video < 0) return false;
        stream = video;
        skip = 5;
        key = (first[0] >> 4) == 1;
        int32_t cts = static_cast<int32_t>(BE24(first + 2));
        if (cts & 0x800000) cts -= 0x1000000;
        pts += int64_t(cts) * 1000;
      } else if (first[1] != 2)
        return false;
    } else if (type == 8) {
      if ((first[0] >> 4) != 10) {
        error_ = "flv: only AAC audio is supported";
        return false;
      }
      if (first[1] == 0) {
        if (audio >= 0 || size < 4) return false;
        StreamInfo s;
        s.type = MediaType::kAudio;
        s.audio.codec = AudioCodec::kAAC;
        s.audio.codec_name = "aac";
        s.audio.extradata.resize(size - 2);
        if (!Exact(payload + 2, s.audio.extradata.data(), size - 2))
          return false;
        static constexpr int rates[] = {96000, 88200, 64000, 48000, 44100,
                                        32000, 24000, 22050, 16000, 12000,
                                        11025, 8000,  7350};
        const auto* asc = s.audio.extradata.data();
        const int rate = ((asc[0] & 7) << 1) | (asc[1] >> 7);
        const int channels = (asc[1] >> 3) & 15;
        if (rate >= 13 || channels < 1 || channels > 7) return false;
        s.audio.sample_rate = rates[rate];
        s.audio.channels = channels == 7 ? 8 : channels;
        audio = static_cast<int>(info_.streams.size());
        info_.streams.push_back(std::move(s));
        previous.push_back(-1);
      } else if (first[1] == 1) {
        if (audio < 0) return false;
        stream = audio;
        skip = 2;
      } else
        return false;
    }
    if (stream >= 0 && size > skip) {
      const int64_t prior = previous[stream];
      if (prior >= 0) {
        Sample& p = samples_[static_cast<size_t>(prior)];
        if (dts < p.dts) return false;
        p.duration = dts - p.dts;
      }
      previous[stream] = static_cast<int64_t>(samples_.size());
      if (!Add({stream, payload + static_cast<int64_t>(skip),
                static_cast<uint32_t>(size - skip), pts, dts,
                stream == audio
                    ? 1024LL * 1000000 / info_.streams[audio].audio.sample_rate
                    : 33333,
                key}))
        return false;
    }
    offset = payload + size;
    if (!Exact(offset, header, 4) || BE32(header) != size + 11) return false;
    offset += 4;
  }
  return true;
}
}  // namespace lychee
