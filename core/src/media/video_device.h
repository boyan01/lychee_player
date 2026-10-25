#ifndef LYCHEE_MEDIA_VIDEO_DEVICE_H_
#define LYCHEE_MEDIA_VIDEO_DEVICE_H_

#include <memory>
#include <string>

namespace lychee {

// The GPU the caller renders with (lychee_video_device). Decoders that keep
// frames on the GPU decode on it, so the caller can open their frames.
struct VideoDevice {
  // Values match lychee_video_device_type.
  enum class Type {
    kNone = 0,
    kD3D11Adapter = 1,
    kDrmRenderNode = 2,
  };

  Type type = Type::kNone;
  // kD3D11Adapter: the retained IDXGIAdapter.
  std::shared_ptr<void> handle;
  // kDrmRenderNode: the node's path, "" for the default one.
  std::string path;

  bool operator==(const VideoDevice& other) const {
    return type == other.type && handle == other.handle &&
           path == other.path;
  }
  bool operator!=(const VideoDevice& other) const { return !(*this == other); }
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_VIDEO_DEVICE_H_
