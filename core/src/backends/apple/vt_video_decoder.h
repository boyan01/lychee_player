#ifndef LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_

#include <VideoToolbox/VideoToolbox.h>

#include <map>
#include <memory>
#include <vector>

#include "media/annexb.h"
#include "media/video_decoder.h"

namespace lychee {

// H.264 and HEVC through VideoToolbox, without FFmpeg. Outputs the
// decoder's CVPixelBuffers (kCVPixelBuffer). |hardware| requires the
// hardware decoder; otherwise only Apple's software decoder is used, the
// fallback when the hardware one is lost or refuses the stream.
class VTVideoDecoder : public VideoDecoder {
 public:
  explicit VTVideoDecoder(bool hardware);
  ~VTVideoDecoder() override;

  const char* name() const override;
  bool is_hardware() const override { return hardware_; }
  bool Initialize(const VideoConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  struct Decoded {
    CVPixelBufferRef image;
    int64_t duration;
  };

  static void OnOutput(void* decoder,
                       void* source_frame,
                       OSStatus status,
                       VTDecodeInfoFlags flags,
                       CVImageBufferRef image,
                       CMTime pts,
                       CMTime duration);

  // A format description for |parameter_sets_|, or null.
  CMVideoFormatDescriptionRef CreateFormat() const;
  // (Re)creates the session for |parameter_sets_|.
  bool CreateSession();
  void DestroySession();
  // Hands out frames with pts <= |up_to|, in pts order; all with
  // kInfiniteTime.
  void Emit(int64_t up_to, const OutputCallback& output);

  const bool hardware_;
  VideoConfig config_;
  AnnexBConverter converter_;
  // Without start codes: VPS, SPS, PPS (HEVC) or SPS, PPS (H.264).
  std::vector<std::vector<uint8_t>> parameter_sets_;
  CMVideoFormatDescriptionRef format_ = nullptr;
  VTDecompressionSessionRef session_ = nullptr;
  // Decoded frames not yet handed out, by pts. VideoToolbox outputs in
  // decode order.
  std::multimap<int64_t, Decoded> reorder_;
  int consecutive_errors_ = 0;
  bool session_lost_ = false;
  std::vector<uint8_t> annexb_;
  std::vector<uint8_t> sample_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_
