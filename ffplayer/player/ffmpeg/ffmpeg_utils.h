//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
#define MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_

#include "base/timestamps.h"

#include "decoder/video_decoder_config.h"
#include "decoder/audio_decoder_config.h"

#include "ffmpeg/ffmpeg_deleters.h"

namespace media {
namespace ffmpeg {

void AVCodecContextToAudioDecoderConfig(const AVCodecContext *codec_context, AudioDecoderConfig *config);

void AVStreamToVideoDecoderConfig(const AVStream *stream, VideoDecoderConfig *config);

// Converts FFmpeg's channel layout to chrome's ChannelLayout.  |channels| can
// be used when FFmpeg's channel layout is not informative in order to make a
// good guess about the plausible channel layout based on number of channels.
ChannelLayout AVChannelLayoutToChannelLayout(int64_t layout, int channels);

TimeDelta ConvertFromTimeBase(const AVRational &time_base,
                              int64 timestamp);

int64 ConvertToTimeBase(const AVRational &time_base,
                        const TimeDelta &timestamp);

// Allocates, populates and returns a wrapped AVCodecContext from the
// AVCodecParameters in |stream|. On failure, returns a wrapped nullptr.
// Wrapping helps ensure eventual destruction of the AVCodecContext.
std::unique_ptr<AVCodecContext, AVCodecContextDeleter> AVStreamToAVCodecContext(const AVStream *stream);

std::string AVErrorToString(int err_num);

void VideoDecoderConfigToAVCodecContext(const VideoDecoderConfig &config, AVCodecContext *codec_context);

}
}

#endif //MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
