#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_COMMON_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_COMMON_H_

#include <memory>
#include <string>

#include "media/codec.h"
#include "media/encoded_packet.h"
#include "media/stream_info.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace lychee {

// Decoders and the demuxer exchange timestamps in microseconds.
inline constexpr AVRational kMicrosTimeBase = {1, 1000000};

// AVCodecParameters attached to configs produced by FFmpegDemuxer.
struct FFmpegCodecParams : public CodecPrivate {
  explicit FFmpegCodecParams(const AVCodecParameters* source);
  ~FFmpegCodecParams() override;

  AVCodecParameters* parameters = nullptr;
};

std::string AVErrorString(int error);

VideoCodec VideoCodecFromAVCodecID(AVCodecID id);
AudioCodec AudioCodecFromAVCodecID(AVCodecID id);
AVCodecID AVCodecIDFromVideoCodec(VideoCodec codec);
AVCodecID AVCodecIDFromAudioCodec(AudioCodec codec);

// Returns codec parameters for a config: the attached FFmpeg parameters if
// present, otherwise rebuilt from the portable fields.
AVCodecParameters* CreateCodecParameters(const VideoConfig& config);
AVCodecParameters* CreateCodecParameters(const AudioConfig& config);

// Copies |packet| into |av_packet|, adding the padding FFmpeg decoders
// require. Returns a negative AVERROR on failure.
int CopyToAVPacket(const EncodedPacket& packet, AVPacket* av_packet);

struct AVPacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct AVFrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct AVCodecContextDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

using ScopedAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;
using ScopedAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;
using ScopedAVCodecContext =
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_COMMON_H_
