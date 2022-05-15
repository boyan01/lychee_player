//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
#define MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_

//#include "../ffplayer/player/src/audio_decode_config.h"
#include "base/time_delta.h"
//#include "../ffplayer/player/src/ffmpeg_deleters.h"
//#include "../ffplayer/player/src/video_decode_config.h

extern "C" {
#include "libavutil/rational.h"
#include <libavformat/avformat.h>
};

namespace lychee::ffmpeg {

using namespace media;

double ConvertFromTimeBase(const AVRational &time_base, int64 timestamp);

int64 ConvertToTimeBase(const AVRational &time_base,
                        const TimeDelta &timestamp);

// Allocates, populates and returns a wrapped AVCodecContext from the
// AVCodecParameters in |stream|. On failure, returns a wrapped nullptr.
// Wrapping helps ensure eventual destruction of the AVCodecContext.
//std::unique_ptr<AVCodecContext, AVCodecContextDeleter> AVStreamToAVCodecContext(
//    const AVStream *stream);

std::string AVErrorToString(int err_num);

int ReadFrameAndDiscardEmpty(AVFormatContext *context, AVPacket *packet);

}  // namespace media

#endif  // MEDIA_PLAYER_FFMPEG_FFMPEG_UTILS_H_
