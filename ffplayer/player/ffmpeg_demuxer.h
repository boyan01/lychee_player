//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_FFMPEG_DEMUXER_H_
#define MEDIA_PLAYER_FFMPEG_DEMUXER_H_

#include <memory>

extern "C" {
#include "libavformat/avformat.h"
};

#include "base/message_loop.h"

#include "pipeline_status.h"
#include "decoder_buffer.h"
#include "data_source.h"
#include "ranges.h"

#include "audio_decoder_config.h"
#include "video_decoder_config.h"

#include "ffmpeg_glue.h"
#include "ffmpeg_common.h"

namespace media {



class FFmpegDemuxer;

class FFmpegDemuxerStream {

};

class FFmpegDemuxer : public FFmpegUrlProtocol {



};

}

#endif //MEDIA_PLAYER_FFMPEG_DEMUXER_H_
