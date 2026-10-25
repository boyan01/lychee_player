#define LOG_TAG "ffmpeg_subtitle"

#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"

#include <cstdint>

#include "base/log.h"

namespace lychee {

FFmpegSubtitleDecoder::FFmpegSubtitleDecoder()
    : av_packet_(av_packet_alloc()) {}

FFmpegSubtitleDecoder::~FFmpegSubtitleDecoder() = default;

bool FFmpegSubtitleDecoder::Initialize(const SubtitleConfig& config,
                                       std::string* error) {
  if (!config.text) {
    *error = "bitmap subtitles (" + config.codec_name + ") are not supported";
    return false;
  }
  AVCodecParameters* parameters = CreateCodecParameters(config);
  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  if (!codec) {
    avcodec_parameters_free(&parameters);
    *error = "no decoder for " + config.codec_name;
    return false;
  }
  context_.reset(avcodec_alloc_context3(codec));
  int ret = avcodec_parameters_to_context(context_.get(), parameters);
  avcodec_parameters_free(&parameters);
  if (ret < 0) {
    *error = "avcodec_parameters_to_context: " + AVErrorString(ret);
    return false;
  }
  context_->pkt_timebase = kMicrosTimeBase;
  ret = avcodec_open2(context_.get(), codec, nullptr);
  if (ret < 0) {
    *error = "avcodec_open2: " + AVErrorString(ret);
    return false;
  }
  LOGI("decoder opened for %s", codec->name);
  return true;
}

bool FFmpegSubtitleDecoder::Decode(const EncodedPacket& packet,
                                   std::vector<SubtitleCue>* cues) {
  if (packet.end_of_stream || packet.pts == kNoTimestamp) {
    return true;
  }
  int ret = WrapInAVPacket(packet, av_packet_.get());
  if (ret < 0) {
    return false;
  }
  AVSubtitle subtitle = {};
  int got_subtitle = 0;
  ret = avcodec_decode_subtitle2(context_.get(), &subtitle, &got_subtitle,
                                 av_packet_.get());
  av_packet_unref(av_packet_.get());
  if (ret < 0) {
    LOGW("avcodec_decode_subtitle2: %s", AVErrorString(ret).c_str());
    return ++consecutive_errors_ < kMaxConsecutiveDecodeErrors;
  }
  consecutive_errors_ = 0;
  if (!got_subtitle) {
    return true;
  }
  SubtitleCue cue;
  cue.start = packet.pts + subtitle.start_display_time * kMicrosPerMilli;
  if (packet.duration > 0) {
    cue.end = packet.pts + packet.duration;
  } else if (subtitle.end_display_time > subtitle.start_display_time &&
             subtitle.end_display_time != UINT32_MAX) {
    cue.end = packet.pts + subtitle.end_display_time * kMicrosPerMilli;
  }
  for (unsigned i = 0; i < subtitle.num_rects; ++i) {
    const AVSubtitleRect* rect = subtitle.rects[i];
    std::string text;
    if (rect->type == SUBTITLE_ASS && rect->ass) {
      text = AssToPlainText(rect->ass);
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
      text = rect->text;
    }
    if (text.empty()) {
      continue;
    }
    if (!cue.text.empty()) {
      cue.text += '\n';
    }
    cue.text += text;
  }
  avsubtitle_free(&subtitle);
  // An empty event clears the screen; keep it as a cue so the previous
  // open-ended one ends here.
  cues->push_back(std::move(cue));
  return true;
}

void FFmpegSubtitleDecoder::Flush() {
  if (context_) {
    avcodec_flush_buffers(context_.get());
  }
}

}  // namespace lychee
