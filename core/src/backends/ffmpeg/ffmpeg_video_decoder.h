#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_

#include <atomic>

#include "backends/ffmpeg/ffmpeg_common.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

struct SwsContext;

namespace lychee {

// Software decoding, or hardware decoding through FFmpeg's hwaccel
// (VideoToolbox on Apple platforms).
class FFmpegVideoDecoder : public VideoDecoder {
 public:
  explicit FFmpegVideoDecoder(bool hardware);
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
  bool ReceiveFrames(const OutputCallback& output);
  std::shared_ptr<VideoFrame> CreateFrame(AVFrame* frame);
  std::shared_ptr<VideoFrame> ConvertToBgra(AVFrame* frame);

  const bool hardware_;
  AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pixel_format_ = AV_PIX_FMT_NONE;
  AVBufferRef* hw_device_context_ = nullptr;
  ScopedAVCodecContext context_;
  ScopedAVFrame frame_;
  ScopedAVPacket av_packet_;
  SwsContext* sws_context_ = nullptr;
  bool logged_output_format_ = false;
  std::atomic<bool> hardware_output_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_VIDEO_DECODER_H_
