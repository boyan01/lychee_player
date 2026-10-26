#define LOG_TAG "mediacodec"

#include "backends/android/media_codec_video_decoder.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "backends/android/media_codec_name.h"
#include "base/log.h"

namespace lychee {

namespace {

// Polling step while waiting for codec buffers.
constexpr int64_t kDequeueTimeoutUs = 10000;
// Waiting for the frames that follow end of stream.
constexpr int64_t kEndOfStreamTimeoutUs = 3 * kMicrosPerSecond;
// A codec takes input buffers back only as it outputs frames, so this many
// packets without a frame means it drops them: Android's software HEVC
// decoder does with Main 10 it cannot decode, without an error.
constexpr int kMaxInputsWithoutOutput = 64;

// One output buffer, owned by the frame that shows it.
class OutputBuffer {
 public:
  OutputBuffer(std::shared_ptr<MediaCodecVideoDecoder::Codec> codec,
               size_t index)
      : codec_(std::move(codec)), index_(index), epoch_(codec_->epoch) {}
  ~OutputBuffer() { Release(false, 0); }

  // |time_ns| in the System.nanoTime() timebase (CLOCK_MONOTONIC).
  void Release(bool render, int64_t time_ns) {
    std::lock_guard<std::mutex> lock(codec_->mutex);
    if (released_) {
      return;
    }
    released_ = true;
    if (!codec_->codec || epoch_ != codec_->epoch) {
      return;
    }
    media_status_t status =
        render ? AMediaCodec_releaseOutputBufferAtTime(codec_->codec, index_,
                                                       time_ns)
               : AMediaCodec_releaseOutputBuffer(codec_->codec, index_, false);
    if (status != AMEDIA_OK) {
      LOGW("releasing output buffer %zu failed: %d", index_, status);
    }
  }

 private:
  const std::shared_ptr<MediaCodecVideoDecoder::Codec> codec_;
  const size_t index_;
  const uint64_t epoch_;
  bool released_ = false;
};

// The platform's software decoders (Codecs says when they are wanted).
bool IsSoftwareCodec(const std::string& name) {
  return name.rfind("OMX.google.", 0) == 0 || name.rfind("c2.android.", 0) == 0;
}

// The visible size from an output format, crop applied.
void VisibleSize(AMediaFormat* format, int* width, int* height) {
  int32_t w = 0;
  int32_t h = 0;
  if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w) ||
      !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h)) {
    return;
  }
  using GetRect = bool (*)(AMediaFormat*, const char*, int32_t*, int32_t*,
                           int32_t*, int32_t*);
  static auto get_rect =
      reinterpret_cast<GetRect>(dlsym(RTLD_DEFAULT, "AMediaFormat_getRect"));
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
  if (get_rect && get_rect(format, "crop", &left, &top, &right, &bottom)) {
    w = right - left + 1;
    h = bottom - top + 1;
  } else {
    if (AMediaFormat_getInt32(format, "crop-left", &left) &&
        AMediaFormat_getInt32(format, "crop-right", &right)) {
      w = right - left + 1;
    }
    if (AMediaFormat_getInt32(format, "crop-top", &top) &&
        AMediaFormat_getInt32(format, "crop-bottom", &bottom)) {
      h = bottom - top + 1;
    }
  }
  *width = w;
  *height = h;
}

void DiscardImages(void*, AImageReader* reader) {
  AImage* image = nullptr;
  while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK) {
    AImage_delete(image);
  }
}

}  // namespace

MediaCodecVideoDecoder::MediaCodecVideoDecoder(std::shared_ptr<void> surface,
                                               bool rotate,
                                               Codecs codecs)
    : codecs_(codecs), surface_(std::move(surface)), rotate_(rotate) {}

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() {
  {
    std::lock_guard<std::mutex> lock(codec_->mutex);
    if (codec_->codec) {
      AMediaCodec_stop(codec_->codec);
      AMediaCodec_delete(codec_->codec);
      codec_->codec = nullptr;
    }
  }
  // After the codec: it may render into the placeholder until deleted.
  if (placeholder_) {
    AImageReader_delete(placeholder_);
  }
}

ANativeWindow* MediaCodecVideoDecoder::TargetWindowLocked() {
  if (surface_) {
    return static_cast<ANativeWindow*>(surface_.get());
  }
  if (!placeholder_) {
    AImageReader* reader = nullptr;
    if (AImageReader_newWithUsage(std::max(coded_width_, 1),
                                  std::max(coded_height_, 1),
                                  AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4,
                                  &reader) != AMEDIA_OK) {
      LOGE("cannot create a placeholder ImageReader");
      return nullptr;
    }
    AImageReader_ImageListener listener = {nullptr, &DiscardImages};
    AImageReader_setImageListener(reader, &listener);
    placeholder_ = reader;
  }
  ANativeWindow* window = nullptr;
  AImageReader_getWindow(placeholder_, &window);
  return window;
}

bool MediaCodecVideoDecoder::Initialize(const VideoConfig& config,
                                        std::string* error) {
  if (!converter_.Initialize(config)) {
    *error = config.codec == VideoCodec::kH264 || config.codec == VideoCodec::kHEVC
                 ? "malformed " + config.codec_name + " extradata"
                 : "MediaCodec decodes only H.264 and HEVC here";
    return false;
  }
  const char* mime = converter_.hevc() ? "video/hevc" : "video/avc";
  AMediaCodec* codec = nullptr;
  if (codecs_ == Codecs::kSoftwareOnly) {
    // Codec2's names from Android 10, OMX's before.
    const char* names[] = {
        converter_.hevc() ? "c2.android.hevc.decoder" : "c2.android.avc.decoder",
        converter_.hevc() ? "OMX.google.hevc.decoder" : "OMX.google.h264.decoder"};
    for (const char* name : names) {
      if ((codec = AMediaCodec_createCodecByName(name))) {
        break;
      }
    }
  } else {
    codec = AMediaCodec_createDecoderByType(mime);
  }
  if (!codec) {
    *error = std::string("no MediaCodec decoder for ") + mime;
    return false;
  }
  const std::string codec_name = MediaCodecName(codec);
  hardware_ = !IsSoftwareCodec(codec_name);
  if (!hardware_ && codecs_ == Codecs::kHardwareOnly) {
    AMediaCodec_delete(codec);
    *error = "only a software MediaCodec decoder (" + codec_name + ") for " +
             mime;
    return false;
  }

  // A live input may not know the size; the stream's SPS decides it then.
  const int width = config.width > 0 ? config.width : 1920;
  const int height = config.height > 0 ? config.height : 1080;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                        std::max(width * height, 2 << 20));
  if (rotate_ && config.rotation != 0) {
    // MediaCodec applies it as the buffers' transform when rendering into
    // the window. Clockwise, like VideoConfig::rotation. The key's constant,
    // AMEDIAFORMAT_KEY_ROTATION, is API 28; the key itself works from 23.
    AMediaFormat_setInt32(format, "rotation-degrees", config.rotation);
  }
  // Parameter sets also go in band before every keyframe; codec specific
  // data covers decoders that want them at configure time.
  std::vector<uint8_t> csd[2];
  for (const auto& nal : converter_.parameter_sets()) {
    const int type = converter_.NalType(nal);
    // H.264: SPS in csd-0, PPS in csd-1. HEVC: all in csd-0.
    const int slot = !converter_.hevc() && type == 8 ? 1 : 0;
    AnnexBConverter::AppendNal(nal.data(), nal.size(), &csd[slot]);
  }
  for (int slot = 0; slot < 2; ++slot) {
    if (!csd[slot].empty()) {
      AMediaFormat_setBuffer(format, slot ? "csd-1" : "csd-0",
                             csd[slot].data(), csd[slot].size());
    }
  }

  coded_width_ = width;
  coded_height_ = height;
  width_ = width;
  height_ = height;
  std::lock_guard<std::mutex> lock(codec_->mutex);
  window_ = TargetWindowLocked();
  media_status_t status =
      window_ ? AMediaCodec_configure(codec, format, window_, nullptr, 0)
              : AMEDIA_ERROR_UNKNOWN;
  AMediaFormat_delete(format);
  if (status == AMEDIA_OK) {
    status = AMediaCodec_start(codec);
  }
  if (status != AMEDIA_OK) {
    AMediaCodec_delete(codec);
    *error = codec_name + " failed to start: " + std::to_string(status);
    return false;
  }
  codec_->codec = codec;
  LOGI("%s opened for %s %dx%d", codec_name.c_str(), mime, width, height);
  return true;
}

void MediaCodecVideoDecoder::SetVideoSurface(std::shared_ptr<void> surface) {
  std::lock_guard<std::mutex> lock(codec_->mutex);
  surface_ = std::move(surface);
  if (!codec_->codec) {
    return;
  }
  ANativeWindow* window = TargetWindowLocked();
  if (!window || window == window_) {
    return;
  }
  media_status_t status = AMediaCodec_setOutputSurface(codec_->codec, window);
  if (status != AMEDIA_OK) {
    LOGW("setOutputSurface failed: %d", status);
    return;
  }
  window_ = window;
}

bool MediaCodecVideoDecoder::Decode(const EncodedPacket& packet,
                                    const OutputCallback& output) {
  if (packet.end_of_stream) {
    if (!QueueInput(nullptr, 0, last_pts_, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM,
                    output)) {
      return false;
    }
    const int64_t deadline = MonotonicMicros() + kEndOfStreamTimeoutUs;
    while (!end_of_stream_ && MonotonicMicros() < deadline) {
      if (!DrainOutput(output)) {
        return false;
      }
      if (!end_of_stream_) {
        usleep(kDequeueTimeoutUs);
      }
    }
    return true;
  }

  annexb_.clear();
  if (!converter_.Convert(packet.buffer->data(), packet.size(), packet.keyframe,
                          &annexb_)) {
    LOGW("skipping a malformed packet at %lld",
         static_cast<long long>(packet.pts));
    return true;
  }
  int64_t pts = packet.pts != kNoTimestamp ? packet.pts : packet.dts;
  if (pts == kNoTimestamp) {
    pts = last_pts_ + 1;
  }
  last_pts_ = pts;
  // Bounded in case a codec never returns some frames.
  if (durations_.size() >= 64) {
    durations_.pop_front();
  }
  durations_.emplace_back(pts, packet.duration);
  if (++inputs_without_output_ > kMaxInputsWithoutOutput) {
    LOGE("no frame from %d packets: the codec cannot decode this stream",
         kMaxInputsWithoutOutput);
    return false;
  }
  return QueueInput(annexb_.data(), annexb_.size(), pts, 0, output) &&
         DrainOutput(output);
}

bool MediaCodecVideoDecoder::QueueInput(const uint8_t* data,
                                        size_t size,
                                        int64_t pts,
                                        uint32_t flags,
                                        const OutputCallback& output) {
  AMediaCodec* codec = codec_->codec;
  for (;;) {
    // Input buffers come back only as frames are decoded, which needs free
    // output buffers: hand those out while waiting.
    if (!DrainOutput(output)) {
      return false;
    }
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec, kDequeueTimeoutUs);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      continue;
    }
    if (index < 0) {
      LOGE("dequeueInputBuffer failed: %zd", index);
      return false;
    }
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec, index, &capacity);
    if (!buffer || capacity < size) {
      LOGE("input buffer of %zu bytes for a %zu-byte packet", capacity, size);
      return false;
    }
    if (size) {
      memcpy(buffer, data, size);
    }
    media_status_t status =
        AMediaCodec_queueInputBuffer(codec, index, 0, size, pts, flags);
    if (status != AMEDIA_OK) {
      LOGE("queueInputBuffer failed: %d", status);
      return false;
    }
    return true;
  }
}

bool MediaCodecVideoDecoder::DrainOutput(const OutputCallback& output) {
  AMediaCodec* codec = codec_->codec;
  for (;;) {
    AMediaCodecBufferInfo info = {};
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      return true;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
      VisibleSize(format, &width_, &height_);
      LOGI("output format: %s", AMediaFormat_toString(format));
      AMediaFormat_delete(format);
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (index < 0) {
      LOGE("dequeueOutputBuffer failed: %zd", index);
      return false;
    }
    auto buffer = std::make_shared<OutputBuffer>(codec_, index);
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      end_of_stream_ = true;
      if (info.size == 0) {
        continue;
      }
    }
    auto frame = std::make_shared<VideoFrame>();
    frame->storage = VideoFrameStorage::kAndroidSurface;
    frame->width = width_;
    frame->height = height_;
    frame->pts = info.presentationTimeUs;
    auto duration =
        std::find_if(durations_.begin(), durations_.end(), [&](const auto& d) {
          return d.first == info.presentationTimeUs;
        });
    if (duration != durations_.end()) {
      frame->duration = duration->second;
      durations_.erase(duration);
    }
    frame->present = [buffer](int64_t display_time) {
      buffer->Release(true, display_time * 1000);
    };
    frame->owner = std::move(buffer);
    inputs_without_output_ = 0;
    output(std::move(frame));
  }
}

void MediaCodecVideoDecoder::Flush() {
  {
    std::lock_guard<std::mutex> lock(codec_->mutex);
    media_status_t status = AMediaCodec_flush(codec_->codec);
    if (status != AMEDIA_OK) {
      LOGW("flush failed: %d", status);
    }
    codec_->epoch++;
  }
  durations_.clear();
  end_of_stream_ = false;
  inputs_without_output_ = 0;
}

}  // namespace lychee
