//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_FFMPEG_COMMON_H_
#define MEDIA_PLAYER_FFMPEG_COMMON_H_

#include "audio_decoder_config.h"
#include "video_decoder_config.h"

#include "channel_layout.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavformat/url.h"
};

namespace media {

void AVCodecContextToAudioDecoderConfig(
    const AVCodecContext *codec_context,
    AudioDecoderConfig *config);

void AVStreamToVideoDecoderConfig(
    const AVStream *stream,
    VideoDecoderConfig *config);

// Converts an FFmpeg video codec ID into its corresponding supported CodecId id.
VideoCodec CodecIDToVideoCodec(AVCodecID codec_id);

// Converts an FFmpeg audio codec ID into its corresponding supported CodecId id.
AudioCodec CodecIDToAudioCodec(AVCodecID codec_id);


VideoFrame::Format PixelFormatToVideoFormat(AVPixelFormat pixel_format);

chrono::microseconds ConvertFromTimeBase(const AVRational &time_base,
                                         int64 timestamp);

int64 ConvertToTimeBase(const AVRational &time_base,
                        const chrono::microseconds &timestamp);

class UniquePtrAVFreePacket {
 public:
  inline void operator()(void *ptr) const {
    auto *packet = static_cast<AVPacket *>(ptr);
    av_free_packet(packet);
    delete packet;
  }
};

typedef std::unique_ptr<AVPacket, UniquePtrAVFreePacket> AVPacketUniquePtr;

} // namespace media

#endif //MEDIA_PLAYER_FFMPEG_COMMON_H_
