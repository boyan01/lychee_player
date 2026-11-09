#ifndef LYCHEE_BACKENDS_ANDROID_SURFACE_FRAME_READER_H_
#define LYCHEE_BACKENDS_ANDROID_SURFACE_FRAME_READER_H_

#include <memory>

#include "media/stream_info.h"
#include "media/video_frame.h"

struct AImageReader;

namespace lychee {

// Reads kAndroidSurface frames back into memory, for the thumbnailer:
// MediaCodec decodes only into a window, here an ImageReader's in
// YUV_420_888, which this converts to RGBA.
class SurfaceFrameReader {
 public:
  // Null if the ImageReader cannot be created.
  static std::shared_ptr<SurfaceFrameReader> Create(int width, int height);
  ~SurfaceFrameReader();
  SurfaceFrameReader(const SurfaceFrameReader&) = delete;
  SurfaceFrameReader& operator=(const SurfaceFrameReader&) = delete;

  // The ANativeWindow for the decoder, retained as RetainVideoSurface()
  // does.
  std::shared_ptr<void> surface() const;

  // Renders |frame| into the window and returns its picture as kCpuRgba,
  // in |config|'s colors; null if none arrives within a second.
  std::shared_ptr<VideoFrame> Read(const VideoFrame& frame,
                                   const VideoConfig& config);

 private:
  explicit SurfaceFrameReader(AImageReader* reader);

  AImageReader* const reader_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_SURFACE_FRAME_READER_H_
