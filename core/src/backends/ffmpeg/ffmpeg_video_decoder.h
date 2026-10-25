#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_

#include <atomic>
#include <memory>

#include "backends/ffmpeg/ffmpeg_common.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

struct SwsContext;

namespace lychee {

#if defined(_WIN32)
class D3D11VideoProcessor;
#endif

// Software decoding, or hardware decoding through FFmpeg's hwaccel
// (VideoToolbox on Apple platforms, D3D11VA on Windows, VAAPI on Linux).
// Frames FFmpeg cannot hand over as they are become CPU frames, RGBA if
// |accepted_storage| takes RGBA but not BGRA.
//
// On Windows |output| is the IDXGIAdapter to decode on. Frames become
// kD3D11Texture only with one: textures are shared with a device on that
// adapter, and without one nobody said which. On Linux it is the
// std::string path of the DRM render node to decode on ("" for FFmpeg's
// default), and frames become kDmabuf only with one, for the same reason.
class FFmpegVideoDecoder : public VideoDecoder {
 public:
  FFmpegVideoDecoder(bool hardware,
                     uint32_t accepted_storage,
                     std::shared_ptr<void> output = nullptr);
  ~FFmpegVideoDecoder() override;

  const char* name() const override;
  // False once FFmpeg fell back to software, e.g. for an unsupported
  // profile.
  bool is_hardware() const override { return hardware_output_; }
  bool Initialize(const VideoConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  static AVPixelFormat GetFormat(AVCodecContext* context,
                                 const AVPixelFormat* formats);
  bool InitializeHardware(const AVCodec* codec, std::string* error);
  void ReceiveFrames(const OutputCallback& output);
  std::shared_ptr<VideoFrame> CreateFrame(AVFrame* frame);
  std::shared_ptr<VideoFrame> ConvertToCpu(AVFrame* frame);

  const bool hardware_;
  const uint32_t accepted_storage_;
  const VideoFrameStorage cpu_storage_;
  const std::shared_ptr<void> output_;
#if defined(_WIN32)
  std::unique_ptr<D3D11VideoProcessor> d3d11_processor_;
  bool logged_d3d11_fallback_ = false;
#elif defined(__linux__) && !defined(__ANDROID__)
  bool logged_dmabuf_fallback_ = false;
#endif
  AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pixel_format_ = AV_PIX_FMT_NONE;
  AVBufferRef* hw_device_context_ = nullptr;
  ScopedAVCodecContext context_;
  ScopedAVFrame frame_;
  ScopedAVPacket av_packet_;
  SwsContext* sws_context_ = nullptr;
  bool logged_output_format_ = false;
  int consecutive_errors_ = 0;
  std::atomic<bool> hardware_output_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_
