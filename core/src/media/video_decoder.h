#ifndef LYCHEE_MEDIA_VIDEO_DECODER_H_
#define LYCHEE_MEDIA_VIDEO_DECODER_H_

#include <functional>
#include <memory>
#include <string>

#include "media/encoded_packet.h"
#include "media/stream_info.h"
#include "media/video_frame.h"

namespace lychee {

class VideoDecoder {
 public:
  using OutputCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

  virtual ~VideoDecoder() = default;

  virtual const char* name() const = 0;
  virtual bool is_hardware() const = 0;

  // Returns false if this decoder cannot handle |config|, so the next
  // candidate can be tried.
  virtual bool Initialize(const VideoConfig& config, std::string* error) = 0;

  // Frames are delivered through |output| in presentation order. An
  // end-of-stream packet drains the decoder. Returns false on fatal errors.
  virtual bool Decode(const EncodedPacket& packet,
                      const OutputCallback& output) = 0;

  // Drops all pending frames, e.g. after a seek.
  virtual void Flush() = 0;

  // lychee_player_set_video_surface() changed. Called on any thread.
  // By value: overrides keep the pointer.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  virtual void SetVideoSurface(std::shared_ptr<void> /*surface*/) {}
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_VIDEO_DECODER_H_
