//
// Created by yangbin on 2021/4/5.
//

#include "base/logging.h"
#include "base/channel_layout.h"

#include "ffmpeg_utils.h"

namespace media {
namespace ffmpeg {

void AVCodecContextToAudioDecoderConfig(const AVCodecContext *codec_context, AudioDecoderConfig *config) {
  DCHECK_EQ(codec_context->codec_type, AVMEDIA_TYPE_AUDIO);

  int bytes_per_channel = av_get_bytes_per_sample(codec_context->sample_fmt);
  ChannelLayout channel_layout = AVChannelLayoutToChannelLayout(codec_context->channel_layout, codec_context->channels);
  int samples_per_seconds = codec_context->sample_rate;

  config->Initialize(codec_context->codec_id,
                     bytes_per_channel,
                     channel_layout,
                     samples_per_seconds,
                     codec_context->extradata,
                     codec_context->extradata_size,
                     true);

}

ChannelLayout AVChannelLayoutToChannelLayout(int64_t layout, int channels) {
  switch (layout) {
    case AV_CH_LAYOUT_MONO:return CHANNEL_LAYOUT_MONO;
    case AV_CH_LAYOUT_STEREO:return CHANNEL_LAYOUT_STEREO;
    case AV_CH_LAYOUT_2_1:return CHANNEL_LAYOUT_2_1;
    case AV_CH_LAYOUT_SURROUND:return CHANNEL_LAYOUT_SURROUND;
    case AV_CH_LAYOUT_4POINT0:return CHANNEL_LAYOUT_4_0;
    case AV_CH_LAYOUT_2_2:return CHANNEL_LAYOUT_2_2;
    case AV_CH_LAYOUT_QUAD:return CHANNEL_LAYOUT_QUAD;
    case AV_CH_LAYOUT_5POINT0:return CHANNEL_LAYOUT_5_0;
    case AV_CH_LAYOUT_5POINT1:return CHANNEL_LAYOUT_5_1;
    case AV_CH_LAYOUT_5POINT0_BACK:return CHANNEL_LAYOUT_5_0_BACK;
    case AV_CH_LAYOUT_5POINT1_BACK:return CHANNEL_LAYOUT_5_1_BACK;
    case AV_CH_LAYOUT_7POINT0:return CHANNEL_LAYOUT_7_0;
    case AV_CH_LAYOUT_7POINT1:return CHANNEL_LAYOUT_7_1;
    case AV_CH_LAYOUT_7POINT1_WIDE:return CHANNEL_LAYOUT_7_1_WIDE;
    case AV_CH_LAYOUT_STEREO_DOWNMIX:return CHANNEL_LAYOUT_STEREO_DOWNMIX;
    default:
      // FFmpeg channel_layout is 0 for .wav and .mp3.  We know mono and stereo
      // from the number of channels, otherwise report errors.
      if (channels == 1)
        return CHANNEL_LAYOUT_MONO;
      if (channels == 2)
        return CHANNEL_LAYOUT_STEREO;
      DLOG(WARNING) << "Unsupported channel layout: " << layout;
  }
  return CHANNEL_LAYOUT_UNSUPPORTED;
}

base::Size GetNaturalSize(const base::Size &visible_size,
                          int aspect_ratio_numerator,
                          int aspect_ratio_denominator) {
  if (aspect_ratio_denominator == 0 ||
      aspect_ratio_numerator < 0 ||
      aspect_ratio_denominator < 0)
    return base::Size();

  double aspect_ratio = aspect_ratio_numerator /
      static_cast<double>(aspect_ratio_denominator);

  int width = floor(visible_size.width() * aspect_ratio + 0.5);
  int height = visible_size.height();

  // An even width makes things easier for YV12 and appears to be the behavior
  // expected by WebKit layout tests.
  return base::Size(width & ~1, height);
}

void AVStreamToVideoDecoderConfig(const AVStream *stream, VideoDecoderConfig *config) {
  base::Size coded_size(stream->codec->coded_width, stream->codec->coded_height);

  // TODO(vrk): This assumes decoded frame data starts at (0, 0), which is true
  // for now, but may not always be true forever. Fix this in the future.
  base::Rect visible_rect(stream->codecpar->width, stream->codecpar->height);

  AVRational aspect_ratio = {1, 1};
  if (stream->sample_aspect_ratio.num)
    aspect_ratio = stream->sample_aspect_ratio;
  else if (stream->codecpar->sample_aspect_ratio.num)
    aspect_ratio = stream->codecpar->sample_aspect_ratio;

  base::Size natural_size = GetNaturalSize(
      visible_rect.size(), aspect_ratio.num, aspect_ratio.den);
  config->Initialize(stream->codecpar->codec_id,
                     static_cast<AVPixelFormat>(stream->codecpar->format),
                     coded_size, visible_rect, natural_size,
                     stream->codecpar->extradata, stream->codecpar->extradata_size,
                     false,
                     true);
}

static const AVRational kMicrosBase = {1, 1000000};

TimeDelta ConvertFromTimeBase(const AVRational &time_base, int64 timestamp) {
  int64 microseconds = av_rescale_q(timestamp, time_base, kMicrosBase);
  return TimeDelta(microseconds);
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

}
}