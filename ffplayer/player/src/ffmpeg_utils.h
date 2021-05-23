//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
#define MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_

#include "base/timestamps.h"

#include "video_decode_config.h"
#include "audio_decode_config.h"

#include "ffmpeg_deleters.h"

namespace media {
namespace ffmpeg {

double ConvertFromTimeBase(const AVRational &time_base,
                           int64 timestamp);

int64 ConvertToTimeBase(const AVRational &time_base,
                        const TimeDelta &timestamp);

// Allocates, populates and returns a wrapped AVCodecContext from the
// AVCodecParameters in |stream|. On failure, returns a wrapped nullptr.
// Wrapping helps ensure eventual destruction of the AVCodecContext.
std::unique_ptr<AVCodecContext, AVCodecContextDeleter> AVStreamToAVCodecContext(const AVStream *stream);

std::string AVErrorToString(int err_num);

int ReadFrameAndDiscardEmpty(AVFormatContext *context, AVPacket *packet);

} // namespace ffmpeg
} // namespace media

#endif //MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
