#define LOG_TAG "thumbnail"

#include "pipeline/thumbnailer.h"

#include <algorithm>
#include <utility>

#include "base/log.h"
#include "base/time.h"
#include "media/factory.h"
#include "media/frame_scaler.h"
#include "media/video_device.h"
#if defined(__ANDROID__)
#include "backends/android/surface_frame_reader.h"
#endif

namespace lychee {

namespace {

// Packets decoded for one picture at most, keyframe first: one keyframe
// takes a few (decoder delay); an exact one up to a long GOP.
constexpr int kMaxPackets = 120;
constexpr int kMaxExactPackets = 1200;

}  // namespace

Thumbnailer::Thumbnailer(std::string url,
                         std::string headers,
                         int max_width,
                         int max_height)
    : url_(std::move(url)),
      headers_(std::move(headers)),
      max_width_(std::max(max_width, 0)),
      max_height_(std::max(max_height, 0)) {}

Thumbnailer::~Thumbnailer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
  }
  Cancel();
  // Get() calls in progress return promptly once cancelled.
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return calls_ == 0; });
}

void Thumbnailer::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  cancels_++;
  if (demuxer_) {
    // Final for the demuxer: the next Get() opens the input again.
    demuxer_->Abort();
  }
}

std::shared_ptr<VideoFrame> Thumbnailer::Get(int64_t position, bool exact) {
  uint64_t cancels;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_) {
      return nullptr;
    }
    calls_++;
    cancels = cancels_;
  }
  std::shared_ptr<VideoFrame> frame = GetCounted(position, exact, cancels);
  std::lock_guard<std::mutex> lock(mutex_);
  calls_--;
  // Under the lock: once it is released, the destructor may go on.
  condition_.notify_all();
  return frame;
}

bool Thumbnailer::CancelledLocked(uint64_t cancels) {
  return aborted_ || cancels != cancels_;
}

std::shared_ptr<VideoFrame> Thumbnailer::GetCounted(int64_t position,
                                                    bool exact,
                                                    uint64_t cancels) {
  std::lock_guard<std::mutex> lock(get_mutex_);
  {
    std::lock_guard<std::mutex> state(mutex_);
    if (CancelledLocked(cancels)) {
      return nullptr;
    }
    if (opened_cancels_ != cancels_) {
      // Opened before a Cancel(), which aborted the demuxer.
      demuxer_ = nullptr;
    }
  }
  // Also after a failed open, or a decoder that failed with no other left.
  if (!demuxer_ || !decoder_) {
    CloseLocked();
    if (!OpenLocked(cancels)) {
      CloseLocked();
      return nullptr;
    }
  }
  std::shared_ptr<VideoFrame> frame = DecodeLocked(position, exact);
  return frame ? FinishLocked(frame) : nullptr;
}

void Thumbnailer::CloseLocked() {
  decoder_.reset();
  candidates_.clear();
  converter_.reset();
  {
    std::lock_guard<std::mutex> state(mutex_);
    demuxer_ = nullptr;
  }
  demuxer_holder_.reset();
#if defined(__ANDROID__)
  surface_reader_.reset();
#endif
}

bool Thumbnailer::OpenLocked(uint64_t cancels) {
  std::unique_ptr<Demuxer> demuxer = CreateDemuxer(headers_);
  if (!demuxer) {
    LOGE("no demuxer in this build");
    return false;
  }
  {
    std::lock_guard<std::mutex> state(mutex_);
    if (CancelledLocked(cancels)) {
      return false;
    }
    demuxer_holder_ = std::move(demuxer);
    demuxer_ = demuxer_holder_.get();
    opened_cancels_ = cancels_;
  }
  std::string error;
  if (!demuxer_holder_->Open(url_, &error)) {
    LOGW("cannot open %s: %s", url_.c_str(), error.c_str());
    return false;
  }
  const MediaInfo& info = demuxer_holder_->media_info();
  // The variant with the smallest pictures (the least bandwidth) is
  // enough for a thumbnail and the quickest to read.
  stream_ = info.default_video_stream;
  const std::vector<VariantInfo> variants = demuxer_holder_->variants();
  int lowest = -1;
  for (size_t i = 0; i < variants.size(); ++i) {
    if (variants[i].stream >= 0 &&
        (lowest < 0 || variants[i].bandwidth < variants[lowest].bandwidth)) {
      lowest = static_cast<int>(i);
    }
  }
  if (lowest >= 0) {
    stream_ = variants[lowest].stream;
    if (variants[lowest].adaptive) {
      demuxer_holder_->SetVariantPolicy(lowest, 0);
    }
  }
  if (stream_ < 0 || stream_ >= static_cast<int>(info.streams.size()) ||
      info.streams[stream_].type != MediaType::kVideo) {
    LOGW("%s has no video", url_.c_str());
    return false;
  }
  config_ = info.streams[stream_].video;
  demuxer_holder_->SetEnabledStreams({stream_});
  uint32_t accepted = static_cast<uint32_t>(VideoFrameStorage::kCpuRgba);
  std::shared_ptr<void> surface;
#if defined(__ANDROID__)
  // MediaCodec decodes into a window only; this one reads its pictures.
  surface_reader_ = SurfaceFrameReader::Create(config_.width, config_.height);
  if (surface_reader_) {
    accepted |= static_cast<uint32_t>(VideoFrameStorage::kAndroidSurface);
    surface = surface_reader_->surface();
  }
#endif
  candidates_ =
      CreateVideoDecoders(true, accepted, VideoDevice(), surface, false);
  if (!InitializeNextDecoderLocked()) {
    LOGW("no decoder for %s", config_.codec_name.c_str());
    return false;
  }
  converter_ = CreateFrameConverter();
  fresh_ = true;
  LOGI("%s: %s %dx%d, decoder %s", url_.c_str(), config_.codec_name.c_str(),
       config_.width, config_.height, decoder_->name());
  return true;
}

bool Thumbnailer::InitializeNextDecoderLocked() {
  decoder_.reset();
  while (!candidates_.empty()) {
    std::unique_ptr<VideoDecoder> decoder = std::move(candidates_.front());
    candidates_.erase(candidates_.begin());
    std::string error;
    if (decoder->Initialize(config_, &error)) {
      decoder_ = std::move(decoder);
      decoder_has_output_ = false;
      return true;
    }
    LOGI("%s decoder rejected %s: %s", decoder->name(),
         config_.codec_name.c_str(), error.c_str());
  }
  return false;
}

std::shared_ptr<VideoFrame> Thumbnailer::DecodeLocked(int64_t position,
                                                      bool exact) {
  const MediaInfo& info = demuxer_holder_->media_info();
  const int64_t target = info.start_time + std::max<int64_t>(position, 0);
  if (!demuxer_holder_->Seek(target) && !fresh_) {
    LOGW("cannot seek to %.3fs", position / 1e6);
    return nullptr;
  }
  fresh_ = false;
  decoder_->Flush();
  // Since the flush: a decoder failing now can still be replaced.
  decoder_has_output_ = false;

  // The packets since the keyframe, for a decoder that fails before its
  // first frame and the next candidate.
  std::vector<EncodedPacket> packets;
  int64_t keyframe_pts = kNoTimestamp;
  // Exact: the last frame at or before |target| so far.
  std::shared_ptr<VideoFrame> before;
  std::shared_ptr<VideoFrame> chosen;
  auto output = [&](std::shared_ptr<VideoFrame> frame) {
    decoder_has_output_ = true;
    if (chosen) {
      return;
    }
    const int64_t pts = frame->pts;
    if (!exact) {
      // Leading pictures of an open GOP come before the keyframe.
      if (pts == kNoTimestamp || keyframe_pts == kNoTimestamp ||
          pts >= keyframe_pts) {
        chosen = std::move(frame);
      }
    } else if (pts != kNoTimestamp && pts > target) {
      chosen = before ? std::move(before) : std::move(frame);
    } else {
      before = std::move(frame);
    }
  };
  auto decode = [&](const EncodedPacket& packet) {
    bool ok = decoder_->Decode(packet, output);
    while (!ok && !decoder_has_output_) {
      LOGW("%s decoder failed before its first frame", decoder_->name());
      if (!InitializeNextDecoderLocked()) {
        return false;
      }
      ok = true;
      for (size_t i = 0; ok && i < packets.size(); ++i) {
        ok = decoder_->Decode(packets[i], output);
      }
    }
    return ok;
  };
  const int max_packets = exact ? kMaxExactPackets : kMaxPackets;
  int decoded = 0;
  while (!chosen) {
    EncodedPacket packet;
    const DemuxerReadResult result = demuxer_holder_->Read(&packet);
    if (result == DemuxerReadResult::kAborted) {
      return nullptr;
    }
    if (result == DemuxerReadResult::kError) {
      LOGW("read failed: %s", demuxer_holder_->error().c_str());
      return nullptr;
    }
    if (result == DemuxerReadResult::kEndOfStream || decoded >= max_packets) {
      // The frames the decoder still holds.
      if (decoded > 0) {
        decode(*EncodedPacket::CreateEndOfStream(stream_));
      }
      if (!chosen) {
        chosen = std::move(before);
      }
      break;
    }
    if (packet.stream_index != stream_ || packet.boundary) {
      continue;
    }
    if (keyframe_pts == kNoTimestamp) {
      if (!packet.keyframe) {
        continue;
      }
      keyframe_pts = packet.pts;
    }
    ++decoded;
    if (!decoder_has_output_) {
      packets.push_back(packet);
    }
    if (!decode(packet)) {
      LOGW("decoding failed");
      return nullptr;
    }
    if (decoder_has_output_) {
      packets.clear();
    }
  }
  if (!chosen) {
    LOGW("no frame at %.3fs", position / 1e6);
  }
  return chosen;
}

std::shared_ptr<VideoFrame> Thumbnailer::FinishLocked(
    const std::shared_ptr<VideoFrame>& frame) {
  std::shared_ptr<VideoFrame> rgba;
#if defined(__ANDROID__)
  if (frame->storage == VideoFrameStorage::kAndroidSurface) {
    rgba = surface_reader_->Read(*frame, config_);
  } else
#endif
  {
    rgba = converter_->Convert(
        frame, static_cast<uint32_t>(VideoFrameStorage::kCpuRgba));
  }
  if (!rgba) {
    LOGW("cannot convert frame storage %u",
         static_cast<uint32_t>(frame->storage));
    return nullptr;
  }
  // The decoder's size, not the container's: they can differ (a size
  // change).
  int width = 0;
  int height = 0;
  FitSize(rgba->width, rgba->height, config_.sar_num, config_.sar_den,
          config_.rotation, max_width_, max_height_, &width, &height);
  return ScaleFrame(*rgba, config_.rotation, width, height);
}

}  // namespace lychee
