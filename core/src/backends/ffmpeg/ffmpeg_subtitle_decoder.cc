#define LOG_TAG "ffmpeg_subtitle"

#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "base/log.h"

namespace lychee {

FFmpegSubtitleDecoder::FFmpegSubtitleDecoder()
    : av_packet_(av_packet_alloc()) {}

FFmpegSubtitleDecoder::~FFmpegSubtitleDecoder() = default;

bool FFmpegSubtitleDecoder::Initialize(const SubtitleConfig& config,
                                       std::string* error) {
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

// static
SubtitleBitmap FFmpegSubtitleDecoder::ToBitmap(const AVSubtitleRect& rect) {
  SubtitleBitmap bitmap;
  bitmap.x = rect.x;
  bitmap.y = rect.y;
  bitmap.width = rect.w;
  bitmap.height = rect.h;
  bitmap.rgba.resize(static_cast<size_t>(rect.w) * rect.h * 4);
  // PAL8: one palette index per pixel; the palette holds 0xAARRGGBB words.
  const auto* palette = reinterpret_cast<const uint32_t*>(rect.data[1]);
  const int colors = std::clamp(rect.nb_colors, 0, 256);
  uint8_t* out = bitmap.rgba.data();
  for (int y = 0; y < rect.h; ++y) {
    const uint8_t* in = rect.data[0] + static_cast<ptrdiff_t>(y) * rect.linesize[0];
    for (int x = 0; x < rect.w; ++x, out += 4) {
      const uint32_t argb = in[x] < colors ? palette[in[x]] : 0;
      const uint32_t a = argb >> 24;
      out[0] = static_cast<uint8_t>((((argb >> 16) & 0xff) * a + 127) / 255);
      out[1] = static_cast<uint8_t>((((argb >> 8) & 0xff) * a + 127) / 255);
      out[2] = static_cast<uint8_t>(((argb & 0xff) * a + 127) / 255);
      out[3] = static_cast<uint8_t>(a);
    }
  }
  return bitmap;
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
  // FFmpeg's muxers write an unknown duration as UINT32_MAX ms.
  if (packet.duration > 0 &&
      packet.duration < int64_t{UINT32_MAX} * kMicrosPerMilli) {
    cue.end = packet.pts + packet.duration;
  } else if (subtitle.end_display_time > subtitle.start_display_time &&
             subtitle.end_display_time != UINT32_MAX) {
    cue.end = packet.pts + subtitle.end_display_time * kMicrosPerMilli;
  }
  std::vector<SubtitleBitmap> bitmaps;
  for (unsigned i = 0; i < subtitle.num_rects; ++i) {
    const AVSubtitleRect* rect = subtitle.rects[i];
    if (rect->type == SUBTITLE_BITMAP) {
      if (rect->w > 0 && rect->h > 0 && rect->data[0] && rect->data[1]) {
        bitmaps.push_back(ToBitmap(*rect));
      }
      continue;
    }
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
  if (!bitmaps.empty()) {
    // PGS and DVB say their canvas once decoded; VobSub in its header.
    cue.canvas_width = context_->width;
    cue.canvas_height = context_->height;
    cue.bitmaps = std::make_shared<const std::vector<SubtitleBitmap>>(
        std::move(bitmaps));
  }
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
