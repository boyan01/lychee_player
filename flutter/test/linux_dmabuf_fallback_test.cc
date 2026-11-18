/* Injects a rejected dmabuf into the Linux Flutter example. From the repo:
   c++ -shared -fPIC -Icore/include flutter/test/linux_dmabuf_fallback_test.cc \
     -o /tmp/lychee-dmabuf-test.so $(pkg-config --cflags --libs epoxy) -ldl
   LD_PRELOAD=/tmp/lychee-dmabuf-test.so BUNDLE/lychee_player_example \
     --seconds 4 /path/to/h264_aac.mp4
*/
//
// A real EGL raster context and a CPU-decodable video are required. The
// shim supplies the dmabuf descriptor and EGL rejection, then observes
// the real device change and subsequent GL uploads.
// With LYCHEE_DMABUF_TEST_PAUSE_AT=1 and a one-frame video, the first frame
// is rejected while paused and must still be uploaded as a CPU frame.
// Set LYCHEE_DMABUF_TEST_EOF_PTS_US to the last frame's PTS (0 for a
// one-frame video). Delay the clear until the clock reaches EOF, then
// pause and check that the same frame is uploaded at the EOF position.

#include <dlfcn.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <lychee/lychee.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

template <typename Function>
Function Next(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

thread_local const lychee_frame* rejected_frame = nullptr;
thread_local const uint8_t* rgba = nullptr;
thread_local int rejected_width = 0;
const char* const pause_at = std::getenv("LYCHEE_DMABUF_TEST_PAUSE_AT");
const char* const eof_pts = std::getenv("LYCHEE_DMABUF_TEST_EOF_PTS_US");
const int64_t last_pts = eof_pts ? std::atoll(eof_pts) : -1;
const int reject_at = pause_at ? std::atoi(pause_at) : 9;
int acquired = 0;
int rejected = 0;
int cleared = 0;
int uploads_after_clear = 0;
int rejected_resizes = 0;
bool resumed = false;
int64_t duration = -1;
int64_t cleared_position = -1;
int64_t rejected_pts = -1;
int64_t acquired_pts = -1;
int64_t acquired_position = -1;
int64_t uploaded_pts = -1;
int64_t uploaded_position = -1;

EGLImageKHR CreateImage(EGLDisplay display,
                        EGLContext context,
                        EGLenum target,
                        EGLClientBuffer buffer,
                        const EGLint* attributes) {
  if (target == EGL_LINUX_DMA_BUF_EXT && rejected_frame) {
    ++rejected;
    return EGL_NO_IMAGE_KHR;
  }
  static auto* next =
      Next<PFNEGLCREATEIMAGEKHRPROC*>("epoxy_eglCreateImageKHR");
  return (*next)(display, context, target, buffer, attributes);
}

void TexSubImage(GLenum target,
                 GLint level,
                 GLint x,
                 GLint y,
                 GLsizei width,
                 GLsizei height,
                 GLenum format,
                 GLenum type,
                 const void* pixels) {
  static const auto next = reinterpret_cast<PFNGLTEXSUBIMAGE2DPROC>(
      eglGetProcAddress("glTexSubImage2D"));
  next(target, level, x, y, width, height, format, type, pixels);
  if (rgba && pixels == rgba && cleared) {
    ++uploads_after_clear;
    uploaded_pts = acquired_pts;
    uploaded_position = acquired_position;
    rgba = nullptr;
  }
}

void TexImage(GLenum target,
              GLint level,
              GLint internal_format,
              GLsizei width,
              GLsizei height,
              GLint border,
              GLenum format,
              GLenum type,
              const void* pixels) {
  static const auto next =
      reinterpret_cast<PFNGLTEXIMAGE2DPROC>(eglGetProcAddress("glTexImage2D"));
  next(target, level, internal_format, width, height, border, format, type,
       pixels);
  if (rejected_frame && width == rejected_width) {
    ++rejected_resizes;
  }
}

__attribute__((destructor)) void CheckResult() {
  std::fprintf(stderr,
               "dmabuf_fallback_test: rejected=%d cleared=%d uploads=%d "
               "rejected_resizes=%d resumed=%d\n",
               rejected, cleared, uploads_after_clear, rejected_resizes,
               resumed);
  if (eof_pts) {
    std::fprintf(stderr,
                 "dmabuf_fallback_test EOF: duration_us=%lld clear_pos_us=%lld "
                 "rejected_pts_us=%lld upload_pts_us=%lld upload_pos_us=%lld\n",
                 static_cast<long long>(duration),
                 static_cast<long long>(cleared_position),
                 static_cast<long long>(rejected_pts),
                 static_cast<long long>(uploaded_pts),
                 static_cast<long long>(uploaded_position));
    if (duration <= 0 || cleared_position != duration ||
        rejected_pts != last_pts || uploaded_pts != rejected_pts ||
        uploaded_position != duration) {
      std::_Exit(EXIT_FAILURE);
    }
  }
  if (rejected != 1 || cleared != 1 ||
      uploads_after_clear < (pause_at || eof_pts ? 1 : 10) ||
      rejected_resizes != 0 || resumed) {
    std::_Exit(EXIT_FAILURE);
  }
}

}  // namespace

extern "C" {

PFNEGLCREATEIMAGEKHRPROC epoxy_eglCreateImageKHR = CreateImage;
PFNGLTEXSUBIMAGE2DPROC epoxy_glTexSubImage2D = TexSubImage;
PFNGLTEXIMAGE2DPROC epoxy_glTexImage2D = TexImage;

bool epoxy_has_egl_extension(EGLDisplay display, const char* extension) {
  if (std::strcmp(extension, "EGL_EXT_image_dma_buf_import") == 0) {
    return true;
  }
  static const auto next =
      Next<decltype(&epoxy_has_egl_extension)>("epoxy_has_egl_extension");
  return next(display, extension);
}

lychee_frame* lychee_player_acquire_frame(lychee_player* player,
                                          int64_t time_us) {
  static const auto next = Next<decltype(&lychee_player_acquire_frame)>(
      "lychee_player_acquire_frame");
  lychee_frame* frame = next(player, time_us);
  if (frame && ++acquired == reject_at && pause_at && !eof_pts) {
    static const auto pause =
        Next<decltype(&lychee_player_pause)>("lychee_player_pause");
    pause(player);
  }
  if ((pause_at || eof_pts) && frame && cleared) {
    static const auto is_playing =
        Next<decltype(&lychee_player_is_playing)>("lychee_player_is_playing");
    resumed |= is_playing(player) != 0;
  }
  if (eof_pts && frame) {
    static const auto pts =
        Next<decltype(&lychee_frame_get_pts)>("lychee_frame_get_pts");
    static const auto position = Next<decltype(&lychee_player_get_position)>(
        "lychee_player_get_position");
    acquired_pts = pts(frame);
    acquired_position = position(player);
  }
  const bool should_reject =
      eof_pts ? acquired_pts == last_pts : acquired >= reject_at;
  rejected_frame = frame && should_reject && !cleared ? frame : nullptr;
  rejected_width = 0;
  rgba = nullptr;
  return frame;
}

int32_t lychee_player_set_video_device(lychee_player* player,
                                       const lychee_video_device* device) {
  static const auto next = Next<decltype(&lychee_player_set_video_device)>(
      "lychee_player_set_video_device");
  if (!device && rejected_frame) {
    if (eof_pts) {
      static const auto get_duration =
          Next<decltype(&lychee_player_get_duration)>(
              "lychee_player_get_duration");
      static const auto position = Next<decltype(&lychee_player_get_position)>(
          "lychee_player_get_position");
      static const auto pause =
          Next<decltype(&lychee_player_pause)>("lychee_player_pause");
      duration = get_duration(player);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (position(player) < duration &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      pause(player);
      cleared_position = position(player);
      rejected_pts = acquired_pts;
    }
    ++cleared;
  }
  return next(player, device);
}

int32_t lychee_frame_get_type(const lychee_frame* frame) {
  static const auto next =
      Next<decltype(&lychee_frame_get_type)>("lychee_frame_get_type");
  return frame == rejected_frame ? LYCHEE_FRAME_DMABUF : next(frame);
}

int32_t lychee_frame_get_width(const lychee_frame* frame) {
  static const auto next =
      Next<decltype(&lychee_frame_get_width)>("lychee_frame_get_width");
  const int32_t width = next(frame);
  if (frame != rejected_frame) {
    return width;
  }
  rejected_width = width + 8;
  return rejected_width;
}

void* lychee_frame_get_native_handle(const lychee_frame* frame) {
  if (frame == rejected_frame) {
    static lychee_dmabuf dmabuf = {};
    dmabuf.struct_size = sizeof(dmabuf);
    dmabuf.fourcc = 0x3231564e;  // NV12
    dmabuf.width = lychee_frame_get_width(frame);
    dmabuf.height = lychee_frame_get_height(frame);
    dmabuf.plane_count = 2;
    dmabuf.fd[0] = dmabuf.fd[1] = -1;
    return &dmabuf;
  }
  static const auto next = Next<decltype(&lychee_frame_get_native_handle)>(
      "lychee_frame_get_native_handle");
  return next(frame);
}

const uint8_t* lychee_frame_get_data(const lychee_frame* frame,
                                     int32_t* stride) {
  static const auto next =
      Next<decltype(&lychee_frame_get_data)>("lychee_frame_get_data");
  rgba = next(frame, stride);
  return rgba;
}

}  // extern "C"
