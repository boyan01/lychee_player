//
// Created by yangbin on 2021/3/28.
//

#include "base/logging.h"
#include "base/limits.h"

#include "decoder/video_frame.h"

extern "C" {
#include "libavutil/mem.h"
}

namespace {

using media::VideoFrame;

void FillYUV(VideoFrame *frame, uint8 y, uint8 u, uint8 v) {
  // Fill the Y plane.
  uint8 *y_plane = frame->data(VideoFrame::kYPlane);
  int y_rows = frame->rows(VideoFrame::kYPlane);
  int y_row_bytes = frame->row_bytes(VideoFrame::kYPlane);
  for (int i = 0; i < y_rows; ++i) {
    memset(y_plane, y, y_row_bytes);
    y_plane += frame->stride(VideoFrame::kYPlane);
  }

  // Fill the U and V planes.
  uint8 *u_plane = frame->data(VideoFrame::kUPlane);
  uint8 *v_plane = frame->data(VideoFrame::kVPlane);
  int uv_rows = frame->rows(VideoFrame::kUPlane);
  int u_row_bytes = frame->row_bytes(VideoFrame::kUPlane);
  int v_row_bytes = frame->row_bytes(VideoFrame::kVPlane);
  for (int i = 0; i < uv_rows; ++i) {
    memset(u_plane, u, u_row_bytes);
    memset(v_plane, v, v_row_bytes);
    u_plane += frame->stride(VideoFrame::kUPlane);
    v_plane += frame->stride(VideoFrame::kVPlane);
  }
}

}

namespace media {

// static
std::shared_ptr<VideoFrame> VideoFrame::CreateFrame(
    VideoFrame::Format format,
    const base::Size &data_size,
    const base::Size &natural_size,
    chrono::microseconds timestamp) {
  DCHECK(IsValidConfig(format, data_size, natural_size));
  std::shared_ptr<VideoFrame> frame = std::make_shared<VideoFrame>(
      format, data_size, natural_size, timestamp);
  switch (format) {
    case VideoFrame::RGB32:frame->AllocateRGB(4u);
      break;
    case VideoFrame::YV12:
    case VideoFrame::YV16:frame->AllocateYUV();
      break;
    default: {
      LOG(FATAL) << "Unsupported frame format: " << format;
    }
  }
  return frame;
}

// static
bool VideoFrame::IsValidConfig(VideoFrame::Format format,
                               const base::Size &data_size,
                               const base::Size &natural_size) {
  return (format != VideoFrame::INVALID &&
      data_size.width() > 0 && data_size.height() > 0 &&
      data_size.width() <= limits::kMaxDimension &&
      data_size.height() <= limits::kMaxDimension &&
      data_size.width() * data_size.height() <= limits::kMaxCanvas &&
      natural_size.width() > 0 && natural_size.height() > 0 &&
      natural_size.width() <= limits::kMaxDimension &&
      natural_size.height() <= limits::kMaxDimension &&
      natural_size.width() * natural_size.height() <= limits::kMaxCanvas);
}

// static
std::shared_ptr<VideoFrame> VideoFrame::WrapNativeTexture(
    uint32 texture_id,
    uint32 texture_target,
    const base::Size &data_size,
    const base::Size &natural_size,
    chrono::microseconds timestamp,
    const std::function<void(void)> &no_longer_needed) {
  auto frame = std::make_shared<VideoFrame>(NATIVE_TEXTURE, data_size, natural_size, timestamp);
  frame->texture_id_ = texture_id;
  frame->texture_target_ = texture_target;
  frame->texture_no_longer_needed_ = no_longer_needed;
  return frame;
}

// static
std::shared_ptr<VideoFrame> VideoFrame::CreateEmptyFrame() {
  return std::make_shared<VideoFrame>(VideoFrame::EMPTY, base::Size(), base::Size(), chrono::microseconds::zero());
}

// static
std::shared_ptr<VideoFrame> VideoFrame::CreateBlackFrame(
    const base::Size &data_size) {
  DCHECK(IsValidConfig(VideoFrame::YV12, data_size, data_size));

  // Create our frame.
  const chrono::microseconds kZero = chrono::microseconds::zero();
  std::shared_ptr<VideoFrame> frame =
      VideoFrame::CreateFrame(VideoFrame::YV12, data_size, data_size, kZero);

  // Now set the data to YUV(0,128,128).
  const uint8 kBlackY = 0x00;
  const uint8 kBlackUV = 0x80;
  FillYUV(frame.get(), kBlackY, kBlackUV, kBlackUV);
  return frame;
}

static inline size_t RoundUp(size_t value, size_t alignment) {
  // Check that |alignment| is a power of 2.
  DCHECK((alignment + (alignment - 1)) == (alignment | (alignment - 1)));
  return ((value + (alignment - 1)) & ~(alignment - 1));
}

static const int kFrameSizeAlignment = 16;
// Allows faster SIMD YUV convert. Also, FFmpeg overreads/-writes occasionally.
static const int kFramePadBytes = 15;

void VideoFrame::AllocateRGB(size_t bytes_per_pixel) {
  // Round up to align at least at a 16-byte boundary for each row.
  // This is sufficient for MMX and SSE2 reads (movq/movdqa).
  size_t bytes_per_row = RoundUp(data_size_.width(),
                                 kFrameSizeAlignment) * bytes_per_pixel;
  size_t aligned_height = RoundUp(data_size_.height(), kFrameSizeAlignment);
  strides_[VideoFrame::kRGBPlane] = bytes_per_row;
#if !defined(OS_ANDROID)
  // TODO(dalecurtis): use DataAligned or so, so this #ifdef hackery
  // doesn't need to be repeated in every single user of aligned data.
  data_[VideoFrame::kRGBPlane] = reinterpret_cast<uint8 *>(
      av_malloc(bytes_per_row * aligned_height + kFramePadBytes));
#else
  data_[VideoFrame::kRGBPlane] = new uint8_t[bytes_per_row * aligned_height];
#endif
  DCHECK(!(reinterpret_cast<intptr_t>(data_[VideoFrame::kRGBPlane]) & 7));
  COMPILE_ASSERT(0 == VideoFrame::kRGBPlane, RGB_data_must_be_index_0);
}

void VideoFrame::AllocateYUV() {
  DCHECK(format_ == VideoFrame::YV12 || format_ == VideoFrame::YV16);
  // Align Y rows at least at 16 byte boundaries.  The stride for both
  // YV12 and YV16 is 1/2 of the stride of Y.  For YV12, every row of bytes for
  // U and V applies to two rows of Y (one byte of UV for 4 bytes of Y), so in
  // the case of YV12 the strides are identical for the same width surface, but
  // the number of bytes allocated for YV12 is 1/2 the amount for U & V as
  // YV16. We also round the height of the surface allocated to be an even
  // number to avoid any potential of faulting by code that attempts to access
  // the Y values of the final row, but assumes that the last row of U & V
  // applies to a full two rows of Y.
  size_t y_stride = RoundUp(row_bytes(VideoFrame::kYPlane),
                            kFrameSizeAlignment);
  size_t uv_stride = RoundUp(row_bytes(VideoFrame::kUPlane),
                             kFrameSizeAlignment);
  // The *2 here is because some formats (e.g. h264) allow interlaced coding,
  // and then the size needs to be a multiple of two macroblocks (vertically).
  // See libavcodec/utils.c:avcodec_align_dimensions2().
  size_t y_height = RoundUp(data_size_.height(), kFrameSizeAlignment * 2);
  size_t uv_height = format_ == VideoFrame::YV12 ? y_height / 2 : y_height;
  size_t y_bytes = y_height * y_stride;
  size_t uv_bytes = uv_height * uv_stride;

#if !defined(OS_ANDROID)
  // TODO(dalecurtis): use DataAligned or so, so this #ifdef hackery
  // doesn't need to be repeated in every single user of aligned data.
  // The extra line of UV being allocated is because h264 chroma MC
  // overreads by one line in some cases, see libavcodec/utils.c:
  // avcodec_align_dimensions2() and libavcodec/x86/h264_chromamc.asm:
  // put_h264_chroma_mc4_ssse3().
  auto *data = reinterpret_cast<uint8 *>(
      av_malloc(y_bytes + (uv_bytes * 2 + uv_stride) + kFramePadBytes));
#else
  uint8* data = new uint8_t[y_bytes + (uv_bytes * 2)];
#endif
  COMPILE_ASSERT(0 == VideoFrame::kYPlane, y_plane_data_must_be_index_0);
  data_[VideoFrame::kYPlane] = data;
  data_[VideoFrame::kUPlane] = data + y_bytes;
  data_[VideoFrame::kVPlane] = data + y_bytes + uv_bytes;
  strides_[VideoFrame::kYPlane] = y_stride;
  strides_[VideoFrame::kUPlane] = uv_stride;
  strides_[VideoFrame::kVPlane] = uv_stride;
}

VideoFrame::VideoFrame(VideoFrame::Format format,
                       const base::Size &data_size,
                       const base::Size &natural_size,
                       chrono::microseconds timestamp)
    : format_(format),
      data_size_(data_size),
      natural_size_(natural_size),
      texture_id_(0),
      texture_target_(0),
      timestamp_(timestamp) {
  memset(&strides_, 0, sizeof(strides_));
  memset(&data_, 0, sizeof(data_));
}

VideoFrame::~VideoFrame() {
  if (format_ == NATIVE_TEXTURE && !texture_no_longer_needed_) {
    texture_no_longer_needed_();
    texture_no_longer_needed_ = nullptr;
  }

  // In multi-plane allocations, only a single block of memory is allocated
  // on the heap, and other |data| pointers point inside the same, single block
  // so just delete index 0.
  if (data_[0]) {
#if !defined(OS_ANDROID)
    av_free(data_[0]);
#else
    delete[] data_[0];
#endif
  }
}

bool VideoFrame::IsValidPlane(size_t plane) const {
  switch (format_) {
    case RGB32:return plane == kRGBPlane;

    case YV12:
    case YV16:return plane == kYPlane || plane == kUPlane || plane == kVPlane;

    case NATIVE_TEXTURE:NOTREACHED() << "NATIVE_TEXTUREs don't use plane-related methods!";
      return false;

    default:break;
  }

  // Intentionally leave out non-production formats.
  NOTREACHED() << "Unsupported video frame format: " << format_;
  return false;
}

int VideoFrame::stride(size_t plane) const {
  DCHECK(IsValidPlane(plane));
  return strides_[plane];
}

int VideoFrame::row_bytes(size_t plane) const {
  DCHECK(IsValidPlane(plane));
  int width = data_size_.width();
  switch (format_) {
    // 32bpp.
    case RGB32:return width * 4;

      // Planar, 8bpp.
    case YV12:
    case YV16:
      if (plane == kYPlane)
        return width;
      return RoundUp(width, 2) / 2;

    default:break;
  }

  // Intentionally leave out non-production formats.
  NOTREACHED() << "Unsupported video frame format: " << format_;
  return 0;
}

int VideoFrame::rows(size_t plane) const {
  DCHECK(IsValidPlane(plane));
  int height = data_size_.height();
  switch (format_) {
    case RGB32:
    case YV16:return height;

    case YV12:
      if (plane == kYPlane)
        return height;
      return RoundUp(height, 2) / 2;

    default:break;
  }

  // Intentionally leave out non-production formats.
  NOTREACHED() << "Unsupported video frame format: " << format_;
  return 0;
}

uint8 *VideoFrame::data(size_t plane) const {
  DCHECK(IsValidPlane(plane));
  return data_[plane];
}

uint32 VideoFrame::texture_id() const {
  DCHECK_EQ(format_, NATIVE_TEXTURE);
  return texture_id_;
}

uint32 VideoFrame::texture_target() const {
  DCHECK_EQ(format_, NATIVE_TEXTURE);
  return texture_target_;
}

bool VideoFrame::IsEndOfStream() const {
  return format_ == VideoFrame::EMPTY;
}

} // namespace media