#ifndef LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_VIDEO_DECODER_H_

#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "media/annexb.h"
#include "media/video_decoder.h"

struct ANativeWindow;

namespace lychee {

// H.264 and HEVC through NDK MediaCodec, decoding straight into the
// ANativeWindow from lychee_player_set_video_surface(). Frames are
// kAndroidSurface: each holds a MediaCodec output buffer, released to the
// window when the frame is first acquired (with the display time), or
// released unseen when the frame is dropped. Without a window, frames go to
// a placeholder ImageReader, so the window can come and go (Flutter
// destroys a SurfaceProducer's Surface in the background) without
// restarting the codec.
//
// Ported from flutter-airplay's native/backends/android/android_video.cpp.
// Files need no low-latency mode or decode-order output: MediaCodec
// reorders by itself, and the renderer picks frames per vsync.
class MediaCodecVideoDecoder : public VideoDecoder {
 public:
  explicit MediaCodecVideoDecoder(std::shared_ptr<void> surface);
  ~MediaCodecVideoDecoder() override;

  const char* name() const override { return "mediacodec"; }
  bool is_hardware() const override { return true; }
  bool Initialize(const VideoConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;
  void SetVideoSurface(std::shared_ptr<void> surface) override;

  // The codec and the epoch its output buffer indices belong to, shared with
  // the frames, which may outlive the decoder.
  struct Codec {
    std::mutex mutex;
    AMediaCodec* codec = nullptr;
    // Bumped by every flush, which invalidates outstanding indices (and
    // MediaCodec reuses them).
    uint64_t epoch = 0;
  };

 private:
  // Waits for an input buffer, handing out decoded frames meanwhile.
  bool QueueInput(const uint8_t* data,
                  size_t size,
                  int64_t pts,
                  uint32_t flags,
                  const OutputCallback& output);
  // Hands out every decoded frame available. False on a codec error.
  bool DrainOutput(const OutputCallback& output);
  // Where the codec renders: the caller's window, or the placeholder.
  ANativeWindow* TargetWindowLocked();

  std::shared_ptr<Codec> codec_ = std::make_shared<Codec>();
  AnnexBConverter converter_;
  std::vector<uint8_t> annexb_;
  // Guarded by codec_->mutex.
  std::shared_ptr<void> surface_;
  ANativeWindow* window_ = nullptr;
  AImageReader* placeholder_ = nullptr;
  int coded_width_ = 0;
  int coded_height_ = 0;
  // Decode thread only.
  int width_ = 0;
  int height_ = 0;
  // MediaCodec drops packet durations; matched back by timestamp.
  std::deque<std::pair<int64_t, int64_t>> durations_;
  int64_t last_pts_ = 0;
  bool end_of_stream_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_VIDEO_DECODER_H_
