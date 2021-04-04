//
// Created by yangbin on 2021/3/28.
//
#include "base/logging.h"

#include "ffmpeg_common.h"

namespace media {

void AVCodecContextToAudioDecoderConfig(const AVCodecContext *codec_context, AudioDecoderConfig *config) {
  DCHECK_EQ(codec_context->codec_type, AVMEDIA_TYPE_AUDIO);

  AudioCodec codec = CodecIDToAudioCodec(codec_context->codec_id);
  int bytes_per_channel = av_get_bytes_per_sample(codec_context->sample_fmt);
  ChannelLayout channel_layout = AVChannelLayoutToChannelLayout(codec_context->channel_layout, codec_context->channels);
  int samples_per_seconds = codec_context->sample_rate;

  config->Initialize(codec,
                     bytes_per_channel,
                     channel_layout,
                     samples_per_seconds,
                     codec_context->extradata,
                     codec_context->extradata_size,
                     true);

}

AudioCodec CodecIDToAudioCodec(AVCodecID codec_id) {
  switch (codec_id) {
    case AVCodecID::AV_CODEC_ID_AAC:return kCodecAAC;
    case AVCodecID::AV_CODEC_ID_MP3:return kCodecMP3;
    case AVCodecID::AV_CODEC_ID_VORBIS:return kCodecVorbis;
    case AVCodecID::AV_CODEC_ID_PCM_U8:
    case AVCodecID::AV_CODEC_ID_PCM_S16LE:
    case AVCodecID::AV_CODEC_ID_PCM_S24LE:return kCodecPCM;
    case AVCodecID::AV_CODEC_ID_PCM_S16BE:return kCodecPCM_S16BE;
    case AVCodecID::AV_CODEC_ID_PCM_S24BE:return kCodecPCM_S24BE;
    case AVCodecID::AV_CODEC_ID_FLAC:return kCodecFLAC;
    case AVCodecID::AV_CODEC_ID_AMR_NB:return kCodecAMR_NB;
    case AVCodecID::AV_CODEC_ID_AMR_WB:return kCodecAMR_WB;
    case AVCodecID::AV_CODEC_ID_GSM_MS:return kCodecGSM_MS;
    case AVCodecID::AV_CODEC_ID_PCM_MULAW:return kCodecPCM_MULAW;
    default:DLOG(WARNING) << "Unknown audio CodecID: " << codec_id;
  }
  return kUnknownAudioCodec;
}

VideoCodec CodecIDToVideoCodec(AVCodecID codec_id) {
  switch (codec_id) {
    case AVCodecID::AV_CODEC_ID_VC1:return kCodecVC1;
    case AVCodecID::AV_CODEC_ID_H264:return kCodecH264;
    case AVCodecID::AV_CODEC_ID_THEORA:return kCodecTheora;
    case AVCodecID::AV_CODEC_ID_MPEG2VIDEO:return kCodecMPEG2;
    case AVCodecID::AV_CODEC_ID_MPEG4:return kCodecMPEG4;
    case AVCodecID::AV_CODEC_ID_VP8:return kCodecVP8;
    default:DLOG(WARNING) << "Unknown video CodecID: " << codec_id;
  }
  return kUnknownVideoCodec;
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

static VideoCodecProfile ProfileIDToVideoCodecProfile(int profile) {
  // Clear out the CONSTRAINED & INTRA flags which are strict subsets of the
  // corresponding profiles with which they're used.
  profile &= ~FF_PROFILE_H264_CONSTRAINED;
  profile &= ~FF_PROFILE_H264_INTRA;
  switch (profile) {
    case FF_PROFILE_H264_BASELINE:return H264PROFILE_BASELINE;
    case FF_PROFILE_H264_MAIN:return H264PROFILE_MAIN;
    case FF_PROFILE_H264_EXTENDED:return H264PROFILE_EXTENDED;
    case FF_PROFILE_H264_HIGH:return H264PROFILE_HIGH;
    case FF_PROFILE_H264_HIGH_10:return H264PROFILE_HIGH10PROFILE;
    case FF_PROFILE_H264_HIGH_422:return H264PROFILE_HIGH422PROFILE;
    case FF_PROFILE_H264_HIGH_444_PREDICTIVE:return H264PROFILE_HIGH444PREDICTIVEPROFILE;
    default:DLOG(WARNING) << "Unknown profile id: " << profile;
  }
  return VIDEO_CODEC_PROFILE_UNKNOWN;
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

  VideoCodec codec = CodecIDToVideoCodec(stream->codecpar->codec_id);
  VideoCodecProfile profile = (codec == kCodecVP8) ? VP8PROFILE_MAIN :
                              ProfileIDToVideoCodecProfile(stream->codecpar->profile);
  base::Size natural_size = GetNaturalSize(
      visible_rect.size(), aspect_ratio.num, aspect_ratio.den);
  config->Initialize(codec,
                     profile,
                     PixelFormatToVideoFormat(static_cast<AVPixelFormat>(stream->codecpar->format)),
                     coded_size, visible_rect, natural_size,
                     stream->codecpar->extradata, stream->codecpar->extradata_size,
                     false,
                     true);
}

VideoFrame::Format PixelFormatToVideoFormat(AVPixelFormat pixel_format) {
  switch (pixel_format) {
    case AVPixelFormat::AV_PIX_FMT_YUV422P:return VideoFrame::YV16;
    case AVPixelFormat::AV_PIX_FMT_YUV420P:return VideoFrame::YV12;
    default:DLOG(WARNING) << "Unsupported PixelFormat: " << pixel_format;
  }
  return VideoFrame::INVALID;
}

static const AVRational kMicrosBase = {1, 1000000};

chrono::microseconds ConvertFromTimeBase(const AVRational &time_base, int64 timestamp) {
  int64 microseconds = av_rescale_q(timestamp, time_base, kMicrosBase);
  return chrono::microseconds(microseconds);
}

int64 ConvertToTimeBase(const AVRational &time_base, const chrono::microseconds &timestamp) {
  return av_rescale_q(timestamp.count(), kMicrosBase, time_base);
}

} // namespace media