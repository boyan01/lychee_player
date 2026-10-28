#ifndef LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_

#include <VideoToolbox/VideoToolbox.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "media/annexb.h"
#include "media/video_decoder.h"

namespace lychee {

// H.264, HEVC, VP9 and AV1 through VideoToolbox, without FFmpeg. Outputs
// the decoder's CVPixelBuffers (kCVPixelBuffer). |hardware| requires the
// hardware decoder; otherwise only Apple's software decoder is used, the
// fallback when the hardware one is lost or refuses the stream. VP9 is a
// supplemental decoder (macOS 11, iOS 26.2), AV1 needs hardware with it
// (M3, A17 Pro).
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
  // What a VP9 stream's vpcC says, from the container until a keyframe's
  // header says otherwise.
  struct Vp9Format {
    int profile = 0;
    int level = 0;
    int bit_depth = 8;
    // vpcC's chromaSubsampling: 0 (4:2:0), 2 (4:2:2), 3 (4:4:4).
    int subsampling = 0;
    bool full_range = false;
    int width = 0;
    int height = 0;

    bool operator==(const Vp9Format&) const = default;
  };

  static void OnOutput(void* decoder,
                       void* source_frame,
                       OSStatus status,
                       VTDecodeInfoFlags flags,
                       CVImageBufferRef image,
                       CMTime pts,
                       CMTime duration);

  // A format description for |parameter_sets_| (H.264, HEVC), |vp9_| or
  // the av1C, or null.
  CMVideoFormatDescriptionRef CreateFormat() const;
  // The keyframe header's format of a VP9 packet; false if it is none.
  static bool ParseVp9Keyframe(const uint8_t* data,
                               size_t size,
                               Vp9Format* format);
  bool DecodeSample(const EncodedPacket& packet, const OutputCallback& output);
  // (Re)creates the session for |parameter_sets_|.
  bool CreateSession();
  void DestroySession();
  // Hands out frames with pts <= |up_to|, in pts order; all with
  // kInfiniteTime.
  void Emit(int64_t up_to, const OutputCallback& output);

  const bool hardware_;
  VideoConfig config_;
  // Authoritative H.264/HEVC container/format color, including unspecified
  // fields.
  std::optional<ColorInfo> output_color_;
  // kCMVideoCodecType_VP9 or _AV1, or 0 for H.264 and HEVC.
  CMVideoCodecType codec_type_ = 0;
  Vp9Format vp9_;
  int bit_depth_ = 8;
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
  // The status OnOutput() got for the frame being decoded.
  OSStatus output_status_ = noErr;
  // Whether a frame came out since the session was made.
  bool decoded_ = false;
  // After bad data: packets are dropped up to the next keyframe.
  bool await_keyframe_ = false;
  std::vector<uint8_t> annexb_;
  std::vector<uint8_t> sample_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_VT_VIDEO_DECODER_H_
