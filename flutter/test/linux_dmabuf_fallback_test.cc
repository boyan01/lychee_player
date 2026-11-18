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

#include <dlfcn.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <lychee/lychee.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

template <typename Function>
Function Next(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

thread_local const lychee_frame* rejected_frame = nullptr;
thread_local const uint8_t* rgba = nullptr;
thread_local int rejected_width = 0;
const char* const pause_at = std::getenv("LYCHEE_DMABUF_TEST_PAUSE_AT");
const int reject_at = pause_at ? std::atoi(pause_at) : 9;
int acquired = 0;
int rejected = 0;
int cleared = 0;
int uploads_after_clear = 0;
int rejected_resizes = 0;
bool resumed = false;

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
  if (rejected != 1 || cleared != 1 ||
      uploads_after_clear < (pause_at ? 1 : 10) || rejected_resizes != 0 ||
      resumed) {
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
  if (frame && ++acquired == reject_at && pause_at) {
    static const auto pause =
        Next<decltype(&lychee_player_pause)>("lychee_player_pause");
    pause(player);
  }
  if (pause_at && frame && cleared) {
    static const auto is_playing =
        Next<decltype(&lychee_player_is_playing)>("lychee_player_is_playing");
    resumed |= is_playing(player) != 0;
  }
  rejected_frame = frame && acquired >= reject_at && !cleared ? frame : nullptr;
  rejected_width = 0;
  rgba = nullptr;
  return frame;
}

int32_t lychee_player_set_video_device(lychee_player* player,
                                       const lychee_video_device* device) {
  static const auto next = Next<decltype(&lychee_player_set_video_device)>(
      "lychee_player_set_video_device");
  if (!device && rejected_frame) {
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
