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
};

namespace media {

void AVCodecContextToAudioDecoderConfig(
    const AVCodecContext *codec_context,
    AudioDecoderConfig *config);

void AVStreamToVideoDecoderConfig(
    const AVStream *stream,
    VideoDecoderConfig *config);

// Converts an FFmpeg video codec ID into its corresponding supported codec id.
VideoCodec CodecIDToVideoCodec(AVCodecID codec_id);

// Converts an FFmpeg audio codec ID into its corresponding supported codec id.
AudioCodec CodecIDToAudioCodec(AVCodecID codec_id);

// Converts FFmpeg's channel layout to chrome's ChannelLayout.  |channels| can
// be used when FFmpeg's channel layout is not informative in order to make a
// good guess about the plausible channel layout based on number of channels.
ChannelLayout AVChannelLayoutToChannelLayout(int64_t layout,
                                             int channels);

VideoFrame::Format PixelFormatToVideoFormat(AVPixelFormat pixel_format);

chrono::microseconds ConvertFromTimeBase(const AVRational &time_base,
                                         int64 timestamp);

int64 ConvertToTimeBase(const AVRational &time_base,
                        const chrono::microseconds &timestamp);
} // namespace media

#endif //MEDIA_PLAYER_FFMPEG_COMMON_H_
