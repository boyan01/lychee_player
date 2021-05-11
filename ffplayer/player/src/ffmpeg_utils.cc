//
// Created by yangbin on 2021/4/5.
//

#include "base/logging.h"
#include "base/channel_layout.h"

#include "ffmpeg_utils.h"

namespace media {
namespace ffmpeg {

static const AVRational kMicrosBase = {1, 1000000};

double ConvertFromTimeBase(const AVRational &time_base, int64 timestamp) {
  return av_q2d(time_base) * double(timestamp);
}

int64 ConvertToTimeBase(const AVRational &time_base, const TimeDelta &timestamp) {
  return av_rescale_q(timestamp.count(), kMicrosBase, time_base);
}

std::unique_ptr<AVCodecContext, AVCodecContextDeleter> AVStreamToAVCodecContext(const AVStream *stream) {
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context(avcodec_alloc_context3(nullptr));
  if (avcodec_parameters_to_context(codec_context.get(), stream->codecpar) < 0) {
    return nullptr;
  }
  return codec_context;
}

std::string AVErrorToString(int err_num) {
  char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(err_num, err_buf, AV_ERROR_MAX_STRING_SIZE);
  return std::string(err_buf);
}

}
}