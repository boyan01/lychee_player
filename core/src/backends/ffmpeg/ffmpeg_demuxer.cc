#define LOG_TAG "ffmpeg_demuxer"

#include "backends/ffmpeg/ffmpeg_demuxer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "backends/ffmpeg/ffmpeg_common.h"
#include "base/log.h"

#if defined(__ANDROID__)
#include <dirent.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

namespace lychee {

namespace {

#if defined(__ANDROID__)
// FFmpeg's TLS on Android is mbedTLS (tool/build_ffmpeg.sh), which trusts
// only the certificates in its ca_file option, while Android keeps its CAs
// one per file. Collect them once per process into the app's cache
// directory. Returns "" if that fails; https then fails verification.
std::string WriteAndroidCaFile() {
  // Android 14+ updates its CAs through the Conscrypt module.
  const char* source = "/apex/com.android.conscrypt/cacerts";
  DIR* dir = opendir(source);
  if (!dir) {
    source = "/system/etc/security/cacerts";
    dir = opendir(source);
  }
  if (!dir) {
    LOGW("no CA certificates found");
    return "";
  }
  // Each file holds one PEM certificate followed by a text dump of it.
  std::string bundle;
  const std::string kEnd = "-----END CERTIFICATE-----";
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    std::ifstream file(std::string(source) + "/" + entry->d_name);
    std::stringstream text;
    text << file.rdbuf();
    std::string pem = text.str();
    size_t begin = pem.find("-----BEGIN CERTIFICATE-----");
    size_t end = pem.find(kEnd, begin);
    if (begin != std::string::npos && end != std::string::npos) {
      bundle += pem.substr(begin, end + kEnd.size() - begin) + "\n";
    }
  }
  closedir(dir);

  // Android 11+ points TMPDIR at the app's cache directory. Before that,
  // derive it from the package name, which is the process name up to ':'.
  std::string cache;
  if (const char* tmpdir = getenv("TMPDIR"); tmpdir && *tmpdir) {
    cache = tmpdir;
  } else {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string package;
    std::getline(cmdline, package, '\0');
    package = package.substr(0, package.find(':'));
    cache = "/data/user/" + std::to_string(getuid() / 100000) + "/" +
            package + "/cache";
  }
  std::string path = cache + "/lychee-cacerts.pem";
  std::string temporary = path + "." + std::to_string(getpid());
  std::ofstream(temporary, std::ios::binary) << bundle;
  if (bundle.empty() || rename(temporary.c_str(), path.c_str()) != 0) {
    LOGW("cannot write CA certificates to %s", path.c_str());
    unlink(temporary.c_str());
    return "";
  }
  return path;
}

const std::string& AndroidCaFile() {
  static const std::string path = WriteAndroidCaFile();
  return path;
}

using IoOpen = int (*)(AVFormatContext*, AVIOContext**, const char*, int,
                       AVDictionary**);
// FFmpeg's own io_open, the same for every context.
std::atomic<IoOpen> default_io_open{nullptr};

// Passes the CA file to every connection the demuxer opens: the input and
// what it opens itself (HLS playlists, segments and keys), which do not
// inherit the input's options.
int IoOpenWithCaFile(AVFormatContext* s, AVIOContext** pb, const char* url,
                     int flags, AVDictionary** options) {
  AVDictionary* local = nullptr;
  AVDictionary** used = options ? options : &local;
  av_dict_set(used, "ca_file", AndroidCaFile().c_str(),
              AV_DICT_DONT_OVERWRITE);
  int ret = default_io_open.load()(s, pb, url, flags, used);
  av_dict_free(&local);
  return ret;
}
#endif

int64_t ToMicros(int64_t value, AVRational time_base) {
  if (value == AV_NOPTS_VALUE) {
    return kNoTimestamp;
  }
  return av_rescale_q(value, time_base, kMicrosTimeBase);
}

// Data that ends this much before the declared duration is truncated.
constexpr int64_t kTruncationTolerance = kMicrosPerSecond;

bool IsNetworkUrl(const std::string& url) {
  size_t scheme_end = url.find("://");
  return scheme_end != std::string::npos && url.compare(0, scheme_end, "file") != 0;
}

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

FFmpegDemuxer::FFmpegDemuxer() : av_packet_(av_packet_alloc()) {}

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
#if defined(__ANDROID__)
    if (!AndroidCaFile().empty()) {
      default_io_open = format_context_->io_open;
      format_context_->io_open = &IoOpenWithCaFile;
    }
#endif
  }
  int ret = avformat_open_input(&format_context_, url.c_str(), nullptr,
                                &options);
  av_dict_free(&options);
  if (ret < 0) {
    *error = "avformat_open_input: " + AVErrorString(ret);
    return false;
  }
  ret = avformat_find_stream_info(format_context_, nullptr);
  if (ret < 0) {
    *error = "avformat_find_stream_info: " + AVErrorString(ret);
    return false;
  }
  BuildMediaInfo();
  enabled_streams_.assign(format_context_->nb_streams, true);
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
  // Durations estimated from timestamps (MPEG-TS) or the bitrate (MP3) are
  // not promises the data has to keep.
  declared_duration_ = media_info_.duration != kNoTimestamp &&
                       ic->duration_estimation_method ==
                           AVFMT_DURATION_FROM_STREAM;
  read_end_ = media_info_.start_time;
  const AVDictionaryEntry* tag = nullptr;
  while ((tag = av_dict_iterate(ic->metadata, tag))) {
    std::string key = tag->key;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
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
    if (par->codec_type == AVMEDIA_TYPE_ATTACHMENT && par->extradata_size > 0 &&
        (par->codec_id == AV_CODEC_ID_TTF || par->codec_id == AV_CODEC_ID_OTF)) {
      const AVDictionaryEntry* filename =
          av_dict_get(stream->metadata, "filename", nullptr, 0);
      media_info_.fonts.push_back(
          {filename ? filename->value : "",
           std::make_shared<const std::vector<uint8_t>>(
               par->extradata, par->extradata + par->extradata_size)});
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
  for (;;) {
    if (aborted_) {
      return DemuxerReadResult::kAborted;
    }
    int ret = av_read_frame(format_context_, av_packet_);
    if (ret == AVERROR(EAGAIN)) {
      continue;
    }
    if (aborted_ || interrupted_) {
      av_packet_unref(av_packet_);
      return DemuxerReadResult::kAborted;
    }
    if (ret == AVERROR_EOF) {
      // A truncated file, or a connection that could not be re-established:
      // FFmpeg reports both as the end of the file.
      if (TruncatedAtEnd()) {
        char message[160];
        snprintf(message, sizeof(message),
                 "input ended at %.3f s, before its duration of %.3f s "
                 "(truncated file or lost connection)",
                 (read_end_ - media_info_.start_time) / 1e6,
                 media_info_.duration / 1e6);
        error_ = message;
        LOGE("%s", message);
        return DemuxerReadResult::kError;
      }
      return DemuxerReadResult::kEndOfStream;
    }
    if (ret < 0) {
      error_ = "read failed: " + AVErrorString(ret);
      LOGE("av_read_frame: %s", AVErrorString(ret).c_str());
      return DemuxerReadResult::kError;
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
    int64_t start = packet->pts != kNoTimestamp ? packet->pts : packet->dts;
    if (start != kNoTimestamp) {
      read_end_ = std::max(read_end_, start + packet->duration);
    }
    packet->keyframe = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
    packet->end_of_stream = false;
    packet->buffer = TakePacketBuffer(av_packet_);
    if (!packet->buffer) {
      error_ = "out of memory";
      return DemuxerReadResult::kError;
    }
    return DemuxerReadResult::kOk;
  }
}

bool FFmpegDemuxer::Seek(int64_t time) {
  interrupted_ = false;
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
  // Land on the keyframe at or before |time|; decoders skip ahead to |time|.
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
  read_end_ = time;
  return true;
}

void FFmpegDemuxer::Interrupt() {
  interrupted_ = true;
}

void FFmpegDemuxer::Abort() {
  aborted_ = true;
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

}  // namespace lychee
