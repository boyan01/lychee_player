#define LOG_TAG "ffmpeg_demuxer"

#include "backends/ffmpeg/ffmpeg_demuxer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "backends/ffmpeg/ffmpeg_common.h"
#include "base/log.h"
#include "base/time.h"

#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
#include <cstddef>
#include <cstdlib>

#include "media/data_source.h"
#endif

#if defined(__ANDROID__)
#include "backends/android/http_data_source.h"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
#include <libavutil/aes.h>
#include <libavutil/opt.h>
#endif
}

namespace lychee {

namespace {

int64_t ToMicros(int64_t value, AVRational time_base) {
  if (value == AV_NOPTS_VALUE) {
    return kNoTimestamp;
  }
  return av_rescale_q(value, time_base, kMicrosTimeBase);
}

// Data that ends this much before the declared duration is truncated.
constexpr int64_t kTruncationTolerance = kMicrosPerSecond;
// A stream's decode time going back by more than this is a discontinuity
// (as the ffmpeg tool's ts_discontinuity_detect() takes it).
constexpr int64_t kDiscontinuityTolerance = kMicrosPerSecond / 10;
// How far before its target a seek reads looking for a keyframe, at most,
// before reading from the start.
constexpr int64_t kMaxSeekBackoff = 64 * kMicrosPerSecond;

bool IsNetworkUrl(const std::string& url) {
  size_t scheme_end = url.find("://");
  return scheme_end != std::string::npos && url.compare(0, scheme_end, "file") != 0;
}

#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
// What OpenDataSource reads: paths, file:// and http(s)://. FFmpeg opens
// the rest itself (data:, and AES-128 HLS segments without a key option).
bool ReadsThroughDataSource(const std::string& url) {
  size_t colon = url.find(':');
  if (colon == std::string::npos || url.find('/') < colon) {
    return true;  // A path.
  }
#if defined(_WIN32)
  if (colon == 1 && isalpha(static_cast<unsigned char>(url[0]))) {
    return true;  // C:/dir/file or C:\dir\file.
  }
#endif
  if (url.compare(colon, 3, "://") != 0) {
    return false;
  }
  std::string scheme = url.substr(0, colon);
  if (scheme == "http" || scheme == "https") {
#if defined(__ANDROID__)
    // Without a Java VM (lychee_cli from adb shell) FFmpeg's http reads it.
    return HttpDataSourceAvailable();
#else
    return true;
#endif
  }
  return scheme == "file";
}

// FFmpeg's HLS opens an AES-128 segment as crypto+URL (crypto:PATH for a
// path), with the key and IV in hex as the "key" and "iv" options. The
// URL inside, or empty.
std::string CryptoInner(const std::string& url) {
  if (url.size() > 7 && url.compare(0, 6, "crypto") == 0 &&
      (url[6] == '+' || url[6] == ':')) {
    return url.substr(7);
  }
  return "";
}

// As the own HLS demuxer bounds a segment it reads whole.
constexpr int64_t kMaxCryptoSegmentBytes = int64_t{256} << 20;

bool HexOption(AVDictionary* options, const char* name, uint8_t out[16]) {
  const AVDictionaryEntry* entry = av_dict_get(options, name, nullptr, 0);
  if (!entry || strlen(entry->value) != 32) {
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    const int c = tolower(static_cast<unsigned char>(entry->value[i]));
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    out[i / 2] =
        static_cast<uint8_t>(i % 2 ? (out[i / 2] << 4) | digit : digit);
  }
  return true;
}

// AES-128-CBC with PKCS#7 padding over a whole segment, as HLS encrypts
// them and FFmpeg's crypto protocol decrypts them.
bool DecryptAes128(AVDictionary* options,
                   std::vector<uint8_t>* data,
                   std::string* error) {
  uint8_t key[16], iv[16];
  if (!HexOption(options, "key", key) || !HexOption(options, "iv", iv)) {
    *error = "invalid AES-128 key or IV";
    return false;
  }
  if (data->empty() || data->size() % 16 != 0) {
    *error = "AES-128 segment is not a whole number of blocks";
    return false;
  }
  std::vector<uint8_t> plain(data->size());
  AVAES* aes = av_aes_alloc();
  if (!aes || av_aes_init(aes, key, 128, 1) < 0) {
    av_free(aes);
    *error = "out of memory";
    return false;
  }
  av_aes_crypt(aes, plain.data(), data->data(),
               static_cast<int>(data->size() / 16), iv, 1);
  av_free(aes);
  const uint8_t padding = plain.back();
  if (padding == 0 || padding > 16 ||
      std::any_of(plain.end() - padding, plain.end(),
                  [padding](uint8_t b) { return b != padding; })) {
    *error = "AES-128 segment has invalid padding";
    return false;
  }
  plain.resize(plain.size() - padding);
  *data = std::move(plain);
  return true;
}

// FFmpeg's default is 32 KB; files and the http buffer hand over more.
constexpr int kIoBufferSize = 64 << 10;

// The options an Io answers through av_opt_get(): HLS asks for the
// "location" of what it opened, the URL after redirects, to resolve the
// playlist's relative URLs against, as FFmpeg's http protocol answers.
struct IoOptions {
  const AVClass* av_class;
  char* location;
};

const AVOption kIoOptions[] = {
    {"location",
     "URL after redirects",
     offsetof(IoOptions, location),
     AV_OPT_TYPE_STRING,
     {.str = nullptr},
     0,
     0,
     AV_OPT_FLAG_DECODING_PARAM},
    {.name = nullptr, .type = AV_OPT_TYPE_FLAGS},
};

const AVClass kIoClass = {
    .class_name = "lychee_io",
    .item_name = av_default_item_name,
    .option = kIoOptions,
    .version = LIBAVUTIL_VERSION_INT,
};

// The AVIOContext's class, whose one child is the IoOptions in opaque.
void* IoChildNext(void* object, void* previous) {
  return previous ? nullptr : static_cast<AVIOContext*>(object)->opaque;
}

const AVClass kAvioClass = {
    .class_name = "AVIOContext",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
    .child_next = IoChildNext,
};
#endif

int RotationFromDisplayMatrix(const AVCodecParameters* parameters) {
  const AVPacketSideData* side_data = av_packet_side_data_get(
      parameters->coded_side_data, parameters->nb_coded_side_data,
      AV_PKT_DATA_DISPLAYMATRIX);
  if (!side_data || side_data->size < 9 * sizeof(int32_t)) {
    return 0;
  }
  // av_display_rotation_get() is counter-clockwise.
  double angle = -av_display_rotation_get(
      reinterpret_cast<const int32_t*>(side_data->data));
  if (std::isnan(angle)) {
    return 0;
  }
  int degrees = static_cast<int>(std::lround(angle / 90.0)) * 90;
  return ((degrees % 360) + 360) % 360;
}

}  // namespace

#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
// An AVIOContext's opaque: a DataSource and the position FFmpeg reads at.
struct FFmpegDemuxer::Io {
  IoOptions options = {&kIoClass, nullptr};  // First: the AVClass child.
  FFmpegDemuxer* owner = nullptr;
  std::unique_ptr<DataSource> source;
  int64_t position = 0;
  int64_t end = -1;
};
#endif

FFmpegDemuxer::FFmpegDemuxer() : av_packet_(av_packet_alloc()) {}

#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
FFmpegDemuxer::FFmpegDemuxer(std::unique_ptr<DataSource> input)
    : FFmpegDemuxer() {
  input_ = std::move(input);
}
#endif

FFmpegDemuxer::~FFmpegDemuxer() {
  avformat_close_input(&format_context_);
  av_packet_free(&av_packet_);
}

int FFmpegDemuxer::InterruptCallback(void* opaque) {
  auto* self = static_cast<FFmpegDemuxer*>(opaque);
  return self->aborted_.load() || self->interrupted_.load() ? 1 : 0;
}

bool FFmpegDemuxer::Open(const std::string& url, std::string* error) {
  format_context_ = avformat_alloc_context();
  format_context_->interrupt_callback.callback = &InterruptCallback;
  format_context_->interrupt_callback.opaque = this;

  AVDictionary* options = nullptr;
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  // The input and whatever demuxers open themselves go through IoOpen().
  format_context_->opaque = this;
  default_io_open_ = format_context_->io_open;
  default_io_close_ = format_context_->io_close2;
  format_context_->io_open = &IoOpen;
  format_context_->io_close2 = &IoClose;
  // HLS would reuse a segment's FFmpeg http connection for the next one,
  // which asserts when that segment came through DataSource.
  av_dict_set(&options, "http_persistent", "0", 0);
  av_dict_set(&options, "http_multiple", "0", 0);
#endif
  // For FFmpeg's http (on Apple and Android, what IoOpen() hands to it).
  if (IsNetworkUrl(url)) {
    // Ride out short outages: reconnect on dropped connections and server
    // errors, give up after about 10 s. Reads block meanwhile, so playback
    // shows buffering; giving up surfaces as a read error.
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "reconnect_on_http_error", "5xx", 0);
    av_dict_set(&options, "reconnect_delay_max", "4", 0);
    av_dict_set(&options, "reconnect_delay_total_max", "10", 0);
    // A connection that stays silent this long counts as dropped.
    av_dict_set(&options, "rw_timeout", "10000000", 0);
  }
#if !defined(LYCHEE_FFMPEG_DATA_SOURCE)
  const int64_t began = MonotonicMicros();
  measure_rate_ = IsNetworkUrl(url);
#endif
  int ret = avformat_open_input(&format_context_, url.c_str(), nullptr,
                                &options);
  av_dict_free(&options);
  if (ret < 0) {
    *error = "avformat_open_input: " + AVErrorString(ret);
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
    if (!io_error_.empty()) {
      *error = io_error_;
    }
#endif
    return false;
  }
  // HLS disables subtitle renditions at open. Enable them before probing
  // reads ahead: enabling later makes HLS catch up to the probe's last
  // packet, discarding cues at the beginning. The player reads every
  // subtitle track so switching subtitles never has to seek.
  for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
    AVStream* stream = format_context_->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      stream->discard = AVDISCARD_DEFAULT;
    }
  }
  ret = avformat_find_stream_info(format_context_, nullptr);
  if (ret < 0) {
    *error = "avformat_find_stream_info: " + AVErrorString(ret);
    return false;
  }
#if !defined(LYCHEE_FFMPEG_DATA_SOURCE)
  CountReceived(0, began);
#endif
  BuildMediaInfo();
  enabled_streams_.assign(format_context_->nb_streams, true);
  discontinuous_ = (format_context_->iformat->flags & AVFMT_TS_DISCONT) != 0;
  offsets_.assign(1, 0);
  timelines_.assign(format_context_->nb_streams, Timeline());
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  io_error_.clear();  // From an HLS variant that failed, say.
#endif
  return true;
}

void FFmpegDemuxer::BuildMediaInfo() {
  AVFormatContext* ic = format_context_;
  media_info_.format_name = ic->iformat->name;
  if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
    media_info_.duration = ic->duration;  // AV_TIME_BASE is microseconds.
  }
  media_info_.start_time =
      ic->start_time == AV_NOPTS_VALUE ? 0 : ic->start_time;
  media_info_.seekable =
      ic->pb ? (ic->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0
             : media_info_.duration != kNoTimestamp;
  // An HLS WebVTT cue can begin before the first audio/video packet.
  // Keep the same playback origin whether subtitles are enabled or not.
  if (strcmp(ic->iformat->name, "hls") == 0) {
    int64_t av_start = kNoTimestamp;
    for (unsigned i = 0; i < ic->nb_streams; ++i) {
      const AVStream* stream = ic->streams[i];
      const AVMediaType type = stream->codecpar->codec_type;
      if ((type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_VIDEO) &&
          stream->start_time != AV_NOPTS_VALUE) {
        const int64_t start = ToMicros(stream->start_time, stream->time_base);
        av_start = av_start == kNoTimestamp ? start : std::min(av_start, start);
      }
    }
    if (av_start != kNoTimestamp) {
      media_info_.start_time = av_start;
    }
  }
  // Durations estimated from timestamps (MPEG-TS) or the bitrate (MP3) are
  // not promises the data has to keep.
  declared_duration_ = media_info_.duration != kNoTimestamp &&
                       ic->duration_estimation_method ==
                           AVFMT_DURATION_FROM_STREAM;
  read_end_ = media_info_.start_time;
  const AVDictionaryEntry* tag = nullptr;
  while ((tag = av_dict_iterate(ic->metadata, tag))) {
    std::string key = tag->key;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    media_info_.metadata.emplace_back(std::move(key), tag->value);
  }

  bool artwork_is_front = false;
  for (unsigned i = 0; i < ic->nb_streams; ++i) {
    AVStream* stream = ic->streams[i];
    AVCodecParameters* par = stream->codecpar;
    const AVPacket& picture = stream->attached_pic;
    if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
        picture.size > 0) {
      // ID3 and FLAC say which picture is the front cover; otherwise the
      // first one.
      const AVDictionaryEntry* kind =
          av_dict_get(stream->metadata, "comment", nullptr, 0);
      const bool front = kind && strcmp(kind->value, "Cover (front)") == 0;
      if (!media_info_.artwork || (front && !artwork_is_front)) {
        media_info_.artwork = std::make_shared<const std::vector<uint8_t>>(
            picture.data, picture.data + picture.size);
        artwork_is_front = front;
      }
    }
    StreamInfo info;
    info.index = static_cast<int>(i);
    info.duration = ToMicros(stream->duration, stream->time_base);
    if (const AVDictionaryEntry* language =
            av_dict_get(stream->metadata, "language", nullptr, 0)) {
      info.language = language->value;
    }
    if (const AVDictionaryEntry* title =
            av_dict_get(stream->metadata, "title", nullptr, 0)) {
      info.title = title->value;
    }
    info.default_track = (stream->disposition &
                          (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED)) != 0;
    auto codec_private = std::make_shared<FFmpegCodecParams>(par);
    std::vector<uint8_t> extradata(par->extradata,
                                   par->extradata + par->extradata_size);

    if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
        !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      info.type = MediaType::kVideo;
      VideoConfig& config = info.video;
      config.codec = VideoCodecFromAVCodecID(par->codec_id);
      config.codec_name = avcodec_get_name(par->codec_id);
      config.width = par->width;
      config.height = par->height;
      AVRational sar = av_guess_sample_aspect_ratio(ic, stream, nullptr);
      if (sar.num > 0 && sar.den > 0) {
        config.sar_num = sar.num;
        config.sar_den = sar.den;
      }
      config.rotation = RotationFromDisplayMatrix(par);
      config.color.primaries = static_cast<uint8_t>(par->color_primaries);
      config.color.transfer = static_cast<uint8_t>(par->color_trc);
      config.color.matrix = static_cast<uint8_t>(par->color_space);
      config.color.full_range = par->color_range == AVCOL_RANGE_JPEG;
      config.extradata = std::move(extradata);
      config.codec_private = codec_private;
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
      info.type = MediaType::kAudio;
      AudioConfig& config = info.audio;
      config.codec = AudioCodecFromAVCodecID(par->codec_id);
      config.codec_name = avcodec_get_name(par->codec_id);
      config.sample_rate = par->sample_rate;
      config.channels = par->ch_layout.nb_channels;
      config.extradata = std::move(extradata);
      config.codec_private = codec_private;
    } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      info.type = MediaType::kSubtitle;
      SubtitleConfig& config = info.subtitle;
      config.codec_name = avcodec_get_name(par->codec_id);
      const AVCodecDescriptor* descriptor =
          avcodec_descriptor_get(par->codec_id);
      config.text = descriptor && (descriptor->props & AV_CODEC_PROP_TEXT_SUB);
      config.extradata = std::move(extradata);
      config.codec_private = codec_private;
      if (info.default_track && media_info_.default_subtitle_stream < 0) {
        media_info_.default_subtitle_stream = info.index;
      }
    }
    media_info_.streams.push_back(std::move(info));
  }

  int video = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video >= 0 && media_info_.streams[video].type == MediaType::kVideo) {
    media_info_.default_video_stream = video;
  }
  int audio = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, video, nullptr,
                                  0);
  if (audio >= 0) {
    media_info_.default_audio_stream = audio;
  }
}

void FFmpegDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  enabled_streams_.assign(format_context_->nb_streams, false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_streams_.size())) {
      enabled_streams_[index] = true;
    }
  }
  for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
    format_context_->streams[i]->discard =
        enabled_streams_[i] ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
}

DemuxerReadResult FFmpegDemuxer::Read(EncodedPacket* packet) {
  while (!queued_.empty()) {
    *packet = std::move(queued_.front());
    queued_.pop_front();
    if (enabled_streams_[packet->stream_index]) {
      return DemuxerReadResult::kOk;
    }
  }
  const int ret = ReadNext(packet);
  return ret < 0 ? ReadFailed(ret) : DemuxerReadResult::kOk;
}

DemuxerReadResult FFmpegDemuxer::ReadFailed(int ret) {
  if (aborted_ || interrupted_) {
    return DemuxerReadResult::kAborted;
  }
  if (ret == AVERROR(ENOMEM)) {
    error_ = "out of memory";
    return DemuxerReadResult::kError;
  }
  std::string cause = ret == AVERROR_EOF ? "" : AVErrorString(ret);
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  if (ret != AVERROR_EOF && !io_error_.empty()) {
    cause = io_error_;
  }
#endif
  // A truncated file, or a connection that could not be re-established:
  // FFmpeg's http reports both as the end of the file, DataSource the
  // latter as a read error.
  if (TruncatedAtEnd()) {
    char message[160];
    snprintf(message, sizeof(message),
             "input ended at %.3f s, before its duration of %.3f s "
             "(truncated file or lost connection)",
             (read_end_ - media_info_.start_time) / 1e6,
             media_info_.duration / 1e6);
    error_ = message;
    LOGE("%s%s%s", message, cause.empty() ? "" : ": ", cause.c_str());
    return DemuxerReadResult::kError;
  }
  if (ret == AVERROR_EOF) {
    return DemuxerReadResult::kEndOfStream;
  }
  error_ = "read failed: " + cause;
  LOGE("av_read_frame: %s", cause.c_str());
  return DemuxerReadResult::kError;
}

int FFmpegDemuxer::ReadNext(EncodedPacket* packet) {
  for (;;) {
    if (aborted_) {
      return AVERROR_EXIT;
    }
#if !defined(LYCHEE_FFMPEG_DATA_SOURCE)
    const int64_t began = MonotonicMicros();
    const int64_t fetched =
        format_context_->pb ? format_context_->pb->bytes_read : 0;
#endif
    int ret = av_read_frame(format_context_, av_packet_);
#if !defined(LYCHEE_FFMPEG_DATA_SOURCE)
    CountReceived(fetched, began);
#endif
    if (ret == AVERROR(EAGAIN)) {
      continue;
    }
    if (aborted_ || interrupted_) {
      av_packet_unref(av_packet_);
      return AVERROR_EXIT;
    }
    if (ret < 0) {
      return ret;
    }
    int index = av_packet_->stream_index;
    if (index < 0 || index >= static_cast<int>(enabled_streams_.size()) ||
        !enabled_streams_[index]) {
      av_packet_unref(av_packet_);
      continue;
    }
    AVRational time_base = format_context_->streams[index]->time_base;
    packet->stream_index = index;
    packet->pts = ToMicros(av_packet_->pts, time_base);
    packet->dts = ToMicros(av_packet_->dts, time_base);
    packet->duration = av_packet_->duration > 0
                           ? ToMicros(av_packet_->duration, time_base)
                           : 0;
    ExactCodecDelay(index, packet);
    if (discontinuous_) {
      ApplyDiscontinuity(index, packet);
    }
    int64_t start = packet->pts != kNoTimestamp ? packet->pts : packet->dts;
    if (start != kNoTimestamp) {
      read_end_ = std::max(read_end_, start + packet->duration);
    }
    packet->keyframe = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
    packet->end_of_stream = false;
    packet->buffer = TakePacketBuffer(av_packet_);
    if (!packet->buffer) {
      return AVERROR(ENOMEM);
    }
    return 0;
  }
}

void FFmpegDemuxer::ExactCodecDelay(int stream, EncodedPacket* packet) const {
  const AVStream* av_stream = format_context_->streams[stream];
  const AVCodecParameters* parameters = av_stream->codecpar;
  if (parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
      parameters->initial_padding <= 0 || parameters->sample_rate <= 0 ||
      !strstr(format_context_->iformat->name, "matroska") ||
      av_packet_->pts == AV_NOPTS_VALUE) {
    return;
  }
  const AVRational sample = {1, parameters->sample_rate};
  if (av_packet_->pts != -av_rescale_q(parameters->initial_padding, sample,
                                       av_stream->time_base)) {
    return;
  }
  const int64_t exact = -av_rescale_q(parameters->initial_padding, sample,
                                      AVRational{1, kMicrosPerSecond});
  if (packet->dts == packet->pts) {
    packet->dts = exact;
  }
  packet->pts = exact;
}

void FFmpegDemuxer::ApplyDiscontinuity(int stream, EncodedPacket* packet) {
  Timeline& timeline = timelines_[stream];
  const int64_t time = packet->dts != kNoTimestamp ? packet->dts : packet->pts;
  if (time == kNoTimestamp) {
    return;
  }
  // FFmpeg's HLS demuxer ignores EXT-X-DISCONTINUITY and its MPEG-TS
  // demuxer only unwraps 33 bits, so timestamps that start over reach here
  // as they are. Within a stream decode times only go forward: one that
  // goes back is a new discontinuity for this stream. Every stream takes
  // the offset of the first one to reach it, so they stay in sync; each
  // finds it by itself, since packets on either side of it interleave.
  if (timeline.next != kNoTimestamp &&
      time + offsets_[timeline.passed] + kDiscontinuityTolerance <
          timeline.next) {
    ++timeline.passed;
    if (timeline.passed == offsets_.size()) {
      offsets_.push_back(timeline.next - time);
      if (timeline.passed == 1) {
        first_discontinuity_ = timeline.next;
      }
      LOGI("timestamps start over at %.3f s: offset %.3f s",
           timeline.next / 1e6, offsets_.back() / 1e6);
    }
  }
  const int64_t offset = offsets_[timeline.passed];
  if (packet->pts != kNoTimestamp) {
    packet->pts += offset;
  }
  if (packet->dts != kNoTimestamp) {
    packet->dts += offset;
  }
  timeline.next = time + offset + packet->duration;
}

bool FFmpegDemuxer::Seek(int64_t time) {
  interrupted_ = false;
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  io_error_.clear();
  if (!aborted_) {
    std::lock_guard<std::mutex> lock(ios_mutex_);
    for (const std::unique_ptr<Io>& io : ios_) {
      io->source->ClearInterrupt();
    }
  }
#endif
  // An interrupted read leaves AVERROR_EXIT in the AVIOContext, and FFmpeg
  // later reports the end of the file as that error (read_frame_internal in
  // libavformat/demux.c). The interrupt was ours; forget it.
  if (format_context_->pb && format_context_->pb->error == AVERROR_EXIT) {
    format_context_->pb->error = 0;
  }
  // FFmpeg would read forward looking for |time|, losing the read position
  // for nothing: the target of a resync is behind it.
  if (!media_info_.seekable) {
    return false;
  }
  queued_.clear();
  read_end_ = time;
  // Land on the keyframe at or before |time|; decoders skip ahead to |time|.
  if (!discontinuous_) {
    return SeekFFmpeg(time);
  }
  if (time > media_info_.start_time) {
    return SeekByScanning(time);
  }
  if (!SeekToStart()) {
    return false;
  }
  // The byte seek reads nothing. Read now, so that a seek the input cannot
  // follow fails as a timestamp seek does (a loop: "loop seek failed"),
  // rather than the next Read() after the loop began.
  EncodedPacket packet;
  const int ret = ReadNext(&packet);
  if (ret < 0) {
    // The start of what played is not empty: the end here is FFmpeg's http
    // failing to reopen, which it reports as the end of the file.
    ReadFailed(ret);
    return false;
  }
  queued_.push_back(std::move(packet));
  return true;
}

bool FFmpegDemuxer::SeekToStart() {
  // A bisection for the start time finds the last packet at or before it,
  // which can be the frame after the first keyframe (decoded first, it has
  // the earlier timestamp): go to the first byte instead.
  if (format_context_->iformat->flags & AVFMT_NO_BYTE_SEEK) {
    return SeekFFmpeg(media_info_.start_time);
  }
  int ret = av_seek_frame(format_context_, -1, 0, AVSEEK_FLAG_BYTE);
  if (ret < 0) {
    LOGE("seek to the start failed: %s", AVErrorString(ret).c_str());
    return false;
  }
  std::fill(timelines_.begin(), timelines_.end(), Timeline());
  return true;
}

bool FFmpegDemuxer::SeekMatroskaToFirstBlock() {
  // Matroska drops the blocks before the index entry it seeks to
  // (skip_to_timecode in libavformat/matroskadec.c), and a seek by time
  // lands on the video's first cue, at the start. Opus's first block is
  // before it, its timestamp negative by CodecDelay (AAC's by its
  // priming): decoded from the second block, Opus starts cold and plays
  // ~150 ms of quiet audio at every loop. So seek the stream whose index
  // starts earliest (blocks read so far are indexed, and the open read
  // the first) to its first entry, and every block is read again, as the
  // open did.
  if (!strstr(format_context_->iformat->name, "matroska")) {
    return false;
  }
  int best = -1;
  int64_t best_time = 0;
  int64_t best_timestamp = 0;
  for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
    AVStream* stream = format_context_->streams[i];
    const AVIndexEntry* entry = avformat_index_get_entry(stream, 0);
    if (!entry || entry->timestamp == AV_NOPTS_VALUE) {
      continue;
    }
    const int64_t time = ToMicros(entry->timestamp, stream->time_base);
    if (best < 0 || time < best_time) {
      best = static_cast<int>(i);
      best_time = time;
      best_timestamp = entry->timestamp;
    }
  }
  if (best < 0 || best_time >= media_info_.start_time) {
    return false;
  }
  return av_seek_frame(format_context_, best, best_timestamp,
                       AVSEEK_FLAG_BACKWARD) >= 0;
}

bool FFmpegDemuxer::SeekFFmpeg(int64_t time) {
  if (time <= media_info_.start_time && SeekMatroskaToFirstBlock()) {
    std::fill(timelines_.begin(), timelines_.end(), Timeline());
    return true;
  }
  int ret = avformat_seek_file(format_context_, -1, INT64_MIN, time, time, 0);
  if (ret < 0) {
    LOGW("seek to %lld backward failed: %s, retrying any direction",
         static_cast<long long>(time), AVErrorString(ret).c_str());
    ret = avformat_seek_file(format_context_, -1, INT64_MIN, time, INT64_MAX,
                             0);
  }
  if (ret < 0) {
    LOGE("seek to %lld failed: %s", static_cast<long long>(time),
         AVErrorString(ret).c_str());
    return false;
  }
  // Seeks land before the first discontinuity, or the timestamps are not
  // known to have any: they are taken as they are.
  std::fill(timelines_.begin(), timelines_.end(), Timeline());
  return true;
}

bool FFmpegDemuxer::SeekByScanning(int64_t time) {
  // Video lands on its keyframe at or before |time|, other streams with
  // what plays from that keyframe on.
  int reference = -1;
  for (const StreamInfo& stream : media_info_.streams) {
    if (enabled_streams_[stream.index] && stream.type == MediaType::kVideo) {
      reference = stream.index;
      break;
    }
  }
  for (const StreamInfo& stream : media_info_.streams) {
    if (reference < 0 && enabled_streams_[stream.index]) {
      reference = stream.index;
    }
  }
  if (reference < 0) {
    return SeekFFmpeg(time);
  }
  const bool video = media_info_.streams[reference].type == MediaType::kVideo;
  // FFmpeg's MPEG-TS seek finds a packet before the target by bisection,
  // not a keyframe (mpegts_get_dts() in libavformat/mpegts.c indexes every
  // packet as one), so decoding would start at the keyframe after it. Seek
  // earlier, back off further while the window holds no keyframe, as the
  // FFmpeg-free TsDemuxer does, and keep what follows the last keyframe.
  int64_t previous_first = kNoTimestamp;
  for (int64_t backoff = kMicrosPerSecond;; backoff *= 2) {
    int64_t from = time - backoff;
    // FFmpeg's HLS seek drops packets until one reaches the playlist's time
    // of the target (hls_read_packet() in libavformat/hls.c). After
    // timestamps started over none does, so it drops all up to the end:
    // read on from before the first discontinuity instead. (One not read
    // yet shows as a pass that read nothing; the next reads from the start.)
    if (first_discontinuity_ != kNoTimestamp) {
      from = std::min(from, first_discontinuity_ - kMicrosPerSecond);
    }
    const bool from_start =
        from <= media_info_.start_time || backoff > kMaxSeekBackoff;
    if (!(from_start ? SeekToStart() : SeekFFmpeg(from))) {
      return false;
    }
    queued_.clear();
    bool found = false;
    int64_t first = kNoTimestamp;
    for (;;) {
      EncodedPacket packet;
      const int ret = ReadNext(&packet);
      if (ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        ReadFailed(ret);
        return false;
      }
      if (packet.stream_index == reference) {
        const int64_t at =
            video && packet.dts != kNoTimestamp ? packet.dts : packet.pts;
        if (first == kNoTimestamp) {
          first = at;
        }
        if (packet.keyframe && packet.pts != kNoTimestamp &&
            packet.pts <= time) {
          // Keep other streams' packets that play from the keyframe on:
          // FFmpeg hands on a video frame only once the next one starts, so
          // audio around the keyframe comes before it.
          const int64_t start = packet.pts;
          std::erase_if(queued_, [&](const EncodedPacket& queued) {
            return queued.stream_index == reference ||
                   queued.pts == kNoTimestamp ||
                   queued.pts + queued.duration <= start;
          });
          queued_.push_back(std::move(packet));
          found = true;
          continue;
        }
        if (at != kNoTimestamp && at > time) {
          if (found) {
            queued_.push_back(std::move(packet));
          }
          break;
        }
        if (!found) {
          continue;  // Not decodable without its keyframe.
        }
      }
      queued_.push_back(std::move(packet));
    }
    if (found) {
      return true;
    }
    if (from_start) {
      break;
    }
    // Nothing read, as after FFmpeg's HLS seek past a discontinuity, or no
    // earlier than last time: backing off further will not help.
    if (first == kNoTimestamp ||
        (previous_first != kNoTimestamp && first >= previous_first)) {
      backoff = kMaxSeekBackoff;
    }
    previous_first = first;
  }
  // Before the first keyframe: play from the start.
  queued_.clear();
  return SeekToStart();
}

void FFmpegDemuxer::Interrupt() {
  interrupted_ = true;
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  std::lock_guard<std::mutex> lock(ios_mutex_);
  for (const std::unique_ptr<Io>& io : ios_) {
    io->source->Interrupt();
  }
#endif
}

void FFmpegDemuxer::Abort() {
  aborted_ = true;
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  std::lock_guard<std::mutex> lock(ios_mutex_);
  for (const std::unique_ptr<Io>& io : ios_) {
    io->source->Interrupt();
  }
#endif
}

bool FFmpegDemuxer::TruncatedAtEnd() const {
  if (!declared_duration_ || read_end_ == kNoTimestamp) {
    return false;
  }
  // Where the enabled streams say they end: the container's duration is the
  // longest stream's, which may be one not read. A stream that declares
  // nothing leaves the container's.
  int64_t end = kNoTimestamp;
  for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
    if (!enabled_streams_[i]) {
      continue;
    }
    const AVStream* stream = format_context_->streams[i];
    const int64_t duration = ToMicros(stream->duration, stream->time_base);
    if (duration == kNoTimestamp) {
      end = media_info_.start_time + media_info_.duration;
      break;
    }
    const int64_t start = ToMicros(stream->start_time, stream->time_base);
    end = std::max(end, (start != kNoTimestamp ? start
                                               : media_info_.start_time) +
                            duration);
  }
  return end != kNoTimestamp && read_end_ < end - kTruncationTolerance;
}

#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
int FFmpegDemuxer::IoOpen(AVFormatContext* s,
                          AVIOContext** pb,
                          const char* url,
                          int flags,
                          AVDictionary** options) {
  auto* self = static_cast<FFmpegDemuxer*>(s->opaque);
  AVDictionary* opts = options ? *options : nullptr;
  // AES-128 HLS segments: their URL inside goes through DataSource too, and
  // the segment is decrypted here; FFmpeg's crypto protocol would open it
  // with FFmpeg's own http, which on Android has no TLS.
  std::string target = CryptoInner(url);
  const bool decrypt =
      !target.empty() && av_dict_get(opts, "key", nullptr, 0) != nullptr;
  if (!decrypt) {
    target = url;
  }
  if ((flags & AVIO_FLAG_WRITE) || !ReadsThroughDataSource(target)) {
    return self->default_io_open_(s, pb, url, flags, options);
  }
  std::string error;
  bool cleartext_refused = false;
  *pb = self->OpenIo(target, &error, &cleartext_refused, opts, decrypt);
  if (*pb) {
    return 0;
  }
  if (cleartext_refused) {
#if defined(__ANDROID__)
    // Probe failures also arrive here, so both cleartext fallback entrances
    // hand HTTPS to the system before FFmpeg can follow a redirect itself.
    // An origin that did not redirect goes to FFmpeg directly. If FFmpeg
    // fails, it may redirect to HTTPS now (a segment moved to an HTTPS
    // CDN, or a redirect that changed after the probe): it is probed once
    // more.
    int ret = 0;
    bool failed = false;
    if (self->redirects_.Known(target)) {
      ret = self->default_io_open_(s, pb, url, flags, options);
      if (ret >= 0 || self->aborted_ || self->interrupted_) {
        return ret;
      }
      self->redirects_.Forget(target);
      failed = true;
    }
    for (;;) {
      error.clear();
      const std::string secure =
          self->redirects_.Find(target, s->interrupt_callback, &error);
      if (!secure.empty()) {
        LOGI("HTTP redirected to HTTPS: using the platform client");
        *pb = self->OpenIo(secure, &error, &cleartext_refused, opts, decrypt);
        if (*pb) {
          return 0;
        }
        break;
      }
      if (!error.empty()) {
        break;
      }
      if (failed) {
        return ret;  // Still no redirect: FFmpeg's error stands.
      }
      ret = self->default_io_open_(s, pb, url, flags, options);
      if (ret >= 0 || self->aborted_ || self->interrupted_) {
        return ret;
      }
      self->redirects_.Forget(target);
      failed = true;
    }
#else
    return self->default_io_open_(s, pb, url, flags, options);
#endif
  }
  if (self->aborted_ || self->interrupted_) {
    return AVERROR_EXIT;
  }
  LOGE("cannot open: %s", error.c_str());
  self->io_error_ = error;
  return AVERROR(EIO);
}

int FFmpegDemuxer::IoClose(AVFormatContext* s, AVIOContext* pb) {
  auto* self = static_cast<FFmpegDemuxer*>(s->opaque);
  if (!pb || pb->read_packet != &ReadPacket) {
    return self->default_io_close_(s, pb);
  }
  self->CloseIo(pb);
  return 0;
}

int FFmpegDemuxer::ReadPacket(void* opaque, uint8_t* data, int size) {
  auto* io = static_cast<Io*>(opaque);
  if (io->end >= 0) {
    if (io->position >= io->end) {
      return AVERROR_EOF;
    }
    size = static_cast<int>(std::min<int64_t>(size, io->end - io->position));
  }
  int64_t n = io->source->Read(io->position, data, static_cast<size_t>(size));
  if (n > 0) {
    io->position += n;
    return static_cast<int>(n);
  }
  if (n == 0) {
    return AVERROR_EOF;
  }
  if (io->source->interrupted()) {
    return AVERROR_EXIT;
  }
  // Demuxers searching for a timestamp try many offsets: say it once.
  std::string error = io->source->error();
  if (error != io->owner->io_error_) {
    LOGE("read at %lld failed: %s", static_cast<long long>(io->position),
         error.c_str());
    io->owner->io_error_ = std::move(error);
  }
  return AVERROR(EIO);
}

int64_t FFmpegDemuxer::SeekPacket(void* opaque, int64_t offset, int whence) {
  auto* io = static_cast<Io*>(opaque);
  whence &= ~AVSEEK_FORCE;
  const int64_t size = io->source->size();
  if (whence == AVSEEK_SIZE) {
    return size >= 0 ? size : AVERROR(ENOSYS);
  }
  int64_t position;
  switch (whence) {
    case SEEK_SET:
      position = offset;
      break;
    case SEEK_CUR:
      position = io->position + offset;
      break;
    case SEEK_END:
      if (size < 0) {
        return AVERROR(ENOSYS);
      }
      position = size + offset;
      break;
    default:
      return AVERROR(EINVAL);
  }
  if (position < 0) {
    return AVERROR(EINVAL);
  }
  // The next read goes there; DataSource reads at any offset.
  io->position = position;
  return position;
}

int64_t FFmpegDemuxer::download_rate() const {
  // The source opened last: an HLS stream's newest segment.
  std::lock_guard<std::mutex> lock(ios_mutex_);
  for (auto it = ios_.rbegin(); it != ios_.rend(); ++it) {
    const int64_t rate = (*it)->source->transfer_rate();
    if (rate >= 0) {
      return rate;
    }
  }
  return -1;
}

AVIOContext* FFmpegDemuxer::OpenIo(const std::string& url,
                                   std::string* error,
                                   bool* cleartext_refused,
                                   AVDictionary* options,
                                   bool decrypt) {
  // The first open is the input's.
  std::unique_ptr<DataSource> source = std::move(input_);
  if (!source) {
    source = OpenDataSource(url, error);
  }
  if (!source) {
    return nullptr;
  }
  auto owned = std::make_unique<Io>();
  Io* io = owned.get();
  io->owner = this;
  io->source = std::move(source);
  // FFmpeg's HLS skips avio_seek for HTTP segments: it passes their byte
  // range through these protocol options instead (end_offset is exclusive).
  if (const auto* offset = av_dict_get(options, "offset", nullptr, 0)) {
    io->position = std::strtoll(offset->value, nullptr, 10);
  }
  if (const auto* end = av_dict_get(options, "end_offset", nullptr, 0)) {
    io->end = std::strtoll(end->value, nullptr, 10);
  }
  if (io->position < 0 || (io->end >= 0 && io->end < io->position)) {
    *error = "invalid byte range";
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(ios_mutex_);
    if (aborted_ || interrupted_) {
      io->source->Interrupt();
    }
    ios_.push_back(std::move(owned));
  }
  auto fail = [&](const std::string& reason) -> AVIOContext* {
    *error = reason;
    RemoveIo(io);
    return nullptr;
  };
  // The first bytes surface errors (no such host, HTTP 404) as an open
  // failure rather than as unrecognised data, and tell whether the server
  // takes range requests: FFmpeg's http reports one that does not as
  // unseekable, and demuxers then neither seek nor look at the end of the
  // file, which would mean reading it all.
  uint8_t first;
  if (io->source->Read(io->position, &first, 1) < 0) {
    // FFmpeg's http has no such policy (App Transport Security, Android's
    // network security config), and read these URLs before.
    *cleartext_refused = io->source->cleartext_refused();
    if (*cleartext_refused) {
      LOGI(
          "the platform refuses cleartext http: trying the cleartext fallback");
    }
    return fail(io->source->error());
  }
  // HLS takes whatever "location" says, so it is never empty.
  std::string location = io->source->url();
  if (location.empty()) {
    location = url;
  }
  if (decrypt) {
    // CBC and the padding at the end want the segment whole.
    const int64_t end = io->end >= 0 ? io->end : io->source->size();
    std::vector<uint8_t> data;
    while (end < 0 || io->position < end) {
      if (static_cast<int64_t>(data.size()) >= kMaxCryptoSegmentBytes) {
        return fail("AES-128 segment larger than 256 MB");
      }
      int64_t want = kIoBufferSize;
      if (end >= 0) {
        want = std::min(want, end - io->position);
      }
      const size_t old = data.size();
      data.resize(old + static_cast<size_t>(want));
      const int64_t n = io->source->Read(io->position, data.data() + old,
                                         static_cast<size_t>(want));
      if (n < 0) {
        return fail(io->source->error());
      }
      data.resize(old + static_cast<size_t>(n));
      if (n == 0) {
        break;
      }
      io->position += n;
    }
    std::string reason;
    if (!DecryptAes128(options, &data, &reason)) {
      return fail(reason);
    }
    LOGI("decrypted an AES-128 segment: %zu bytes", data.size());
    std::unique_ptr<DataSource> encrypted;
    {
      // Interrupt() reaches the source from other threads.
      std::lock_guard<std::mutex> lock(ios_mutex_);
      encrypted =
          std::exchange(io->source, CreateMemoryDataSource(std::move(data)));
    }
    io->position = 0;
    io->end = -1;
  }
  auto* buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
  AVIOContext* pb = buffer
                        ? avio_alloc_context(buffer, kIoBufferSize, 0, io,
                                             &ReadPacket, nullptr, &SeekPacket)
                        : nullptr;
  if (!pb) {
    av_free(buffer);
    return fail("out of memory");
  }
  pb->pos = io->position;
  pb->seekable = io->source->seekable() ? AVIO_SEEKABLE_NORMAL : 0;
  pb->av_class = &kAvioClass;
  io->options.location = av_strdup(location.c_str());
  return pb;
}

void FFmpegDemuxer::CloseIo(AVIOContext* pb) {
  auto* io = static_cast<Io*>(pb->opaque);
  av_freep(&pb->buffer);
  avio_context_free(&pb);
  av_freep(&io->options.location);
  RemoveIo(io);
}

void FFmpegDemuxer::RemoveIo(Io* io) {
  std::unique_ptr<Io> removed;
  std::lock_guard<std::mutex> lock(ios_mutex_);
  auto it = std::find_if(
      ios_.begin(), ios_.end(),
      [io](const std::unique_ptr<Io>& i) { return i.get() == io; });
  // Destroyed after the unlock: an http source cancels its session.
  removed = std::move(*it);
  ios_.erase(it);
}
#else
void FFmpegDemuxer::CountReceived(int64_t fetched, int64_t began) {
  // Only reads that took bytes from the network count, so time spent
  // demuxing from FFmpeg's buffer does not lower the rate.
  const AVIOContext* pb = format_context_->pb;
  if (!measure_rate_ || !pb || pb->bytes_read <= fetched) {
    return;
  }
  received_ += pb->bytes_read - fetched;
  receive_micros_ += MonotonicMicros() - began;
}

int64_t FFmpegDemuxer::download_rate() const {
  // What the input's AVIOContext fetched over the time its reads took.
  // HLS reads its segments through other contexts and reports none here.
  const int64_t received = received_;
  const int64_t micros = receive_micros_;
  return received > 0 && micros > 0 ? received * 8 * kMicrosPerSecond / micros
                                    : -1;
}
#endif

}  // namespace lychee
