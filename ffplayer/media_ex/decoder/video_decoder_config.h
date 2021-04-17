//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_VIDEO_DECODER_CONFIG_H_
#define MEDIA_PLAYER_VIDEO_DECODER_CONFIG_H_

#include <memory>

#include "base/basictypes.h"
#include "base/rect.h"

extern "C" {
#include "libavformat/avformat.h"
};

namespace media {

class VideoDecoderConfig {
 public:
  // Constructs an uninitialized object. Clients should call Initialize() with
  // appropriate values before using.
  VideoDecoderConfig();

  // Constructs an initialized object. It is acceptable to pass in NULL for
  // |extra_data|, otherwise the memory is copied.
  VideoDecoderConfig(AVCodecID codec,
                     AVPixelFormat format,
                     const base::Size &coded_size,
                     const base::Rect &visible_rect,
                     const base::Size &natural_size,
                     const uint8 *extra_data, size_t extra_data_size,
                     bool is_encrypted);

  ~VideoDecoderConfig();

  // Resets the internal state of this object.
  void Initialize(AVCodecID codec,
                  AVPixelFormat format,
                  const base::Size &coded_size,
                  const base::Rect &visible_rect,
                  const base::Size &natural_size,
                  const uint8 *extra_data, size_t extra_data_size,
                  bool is_encrypted,
                  bool record_stats);

  // Deep copies |video_config|.
  void CopyFrom(const VideoDecoderConfig &video_config);

  // Returns true if this object has appropriate configuration values, false
  // otherwise.
  bool IsValidConfig() const;

  // Returns true if all fields in |config| match this config.
  // Note: The contents of |extra_data_| are compared not the raw pointers.
  bool Matches(const VideoDecoderConfig &config) const;

  // Returns a human-readable string describing |*this|.  For debugging & test
  // output only.
  std::string AsHumanReadableString() const;

  AVCodecID CodecId() const;

  // Video format used to determine YUV buffer sizes.
  AVPixelFormat format() const;

  // Width and height of video frame immediately post-decode. Not all pixels
  // in this region are valid.
  base::Size coded_size() const;

  // Region of |coded_size_| that is visible.
  base::Rect visible_rect() const;

  // Final visible width and height of a video frame with aspect ratio taken
  // into account.
  base::Size natural_size() const;

  // Optional byte data required to initialize video decoders, such as H.264
  // AAVC data.
  uint8 *extra_data() const;
  size_t extra_data_size() const;

  // Whether the video stream is potentially encrypted.
  // Note that in a potentially encrypted video stream, individual buffers
  // can be encrypted or not encrypted.
  bool is_encrypted() const;

 private:
  AVCodecID codec_id_;

  AVPixelFormat format_;

  base::Size coded_size_;
  base::Rect visible_rect_;
  base::Size natural_size_;

  uint8 *extra_data_;
  size_t extra_data_size_;

  bool is_encrypted_;

};

}  // namespace media

#endif //MEDIA_PLAYER_VIDEO_DECODER_CONFIG_H_
