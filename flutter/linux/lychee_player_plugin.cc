#include "include/lychee_player/lychee_player_plugin.h"

// libepoxy before anything that includes GL headers.
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <lychee/lychee.h>

#include <cstring>
#include <string>
#include <vector>

// Bridges one lychee_player to a Flutter GL texture. The view's frame clock
// ticks once per display refresh on the GTK main thread; each tick asks the
// player whether the frame for that refresh is new, and if so tells
// Flutter, which then pulls it through populate on its raster thread. The
// mutex keeps the two threads and disposal apart.
//
// populate runs in Flutter's EGL render context and draws the frame into
// one RGBA texture: CPU frames are uploaded, and hardware-decoded dmabuf
// frames are imported as EGLImages and converted from YUV by a shader.
// Flutter makes Skia forget its GL state around populate
// (embedder_external_texture_gl.cc), but not Impeller, so populate
// restores everything it changes (ScopedGlState).

namespace {

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | static_cast<uint32_t>(b) << 8 |
         static_cast<uint32_t>(c) << 16 | static_cast<uint32_t>(d) << 24;
}
// drm_fourcc.h, which is not always installed.
constexpr uint32_t kDrmFormatP010 = Fourcc('P', '0', '1', '0');
constexpr uint32_t kDrmFormatR8 = Fourcc('R', '8', ' ', ' ');
constexpr uint32_t kDrmFormatGR88 = Fourcc('G', 'R', '8', '8');
constexpr uint32_t kDrmFormatR16 = Fourcc('R', '1', '6', ' ');
constexpr uint32_t kDrmFormatGR1616 = Fourcc('G', 'R', '3', '2');
constexpr uint64_t kDrmFormatModInvalid = 0x00ffffffffffffffull;

// GLSL ES 1.00: Flutter asks EGL for an OpenGL ES 2 context.
constexpr char kVertexShader[] = R"(
attribute vec2 position;
varying vec2 uv;
void main() {
  uv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 uv;
uniform sampler2D luma;
uniform sampler2D chroma;
uniform vec3 offset;
uniform mat3 matrix;
uniform int color_transfer;
uniform int color_primaries;
// Same tone mapping as core/src/backends/windows/d3d11_video_processor.cc:
// ST 2084 / BT.2100, 203-nit white, and ICtCp gamut compression to BT.709.
// Matrices here are column-major, unlike the HLSL source. HLG uses the
// same 1000-nit reference display and system gamma 1.2.
const float m1 = 2610.0 / 16384.0;
const float m2 = 2523.0 / 4096.0 * 128.0;
const float c1 = 3424.0 / 4096.0;
const float c2 = 2413.0 / 4096.0 * 32.0;
const float c3 = 2392.0 / 4096.0 * 32.0;
const mat3 bt2020ToLms = mat3(
  1688.0, 683.0, 99.0,
  2146.0, 2951.0, 309.0,
  262.0, 462.0, 3688.0) / 4096.0;
const mat3 lmsToIctcp = mat3(
  2048.0, 6610.0, 17933.0,
  2048.0, -13613.0, -17390.0,
  0.0, 7003.0, -543.0) / 4096.0;
const mat3 ictcpToLms = mat3(
  1.0, 1.0, 1.0,
  0.008609, -0.008609, 0.560031,
  0.111030, -0.111030, -0.320627);
const mat3 lmsToBt709 = mat3(
  6.17336, -1.32448, -0.01197,
  -5.32033, 2.56035, -0.26459,
  0.14707, -0.23586, 1.27646);
const mat3 bt709ToBt2020 = mat3(
  0.627404, 0.069097, 0.016391,
  0.329283, 0.919540, 0.088013,
  0.043313, 0.011362, 0.895595);

vec3 pqToNits(vec3 e) {
  vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
  return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 nitsToPq(vec3 nits) {
  vec3 y = pow(max(nits, 0.0) / 10000.0, vec3(m1));
  return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}
vec3 ictcpToSdr(vec3 ictcp) {
  return lmsToBt709 * pqToNits(ictcpToLms * ictcp) / 203.0;
}
bool inside(vec3 rgb) {
  return all(greaterThanEqual(rgb, vec3(-1e-4))) &&
         all(lessThanEqual(rgb, vec3(1.0 + 1e-4)));
}
float hlgScene(float e) {
  return e <= 0.5 ? e * e / 3.0 :
      (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
vec3 hlgToNits(vec3 e) {
  e = clamp(e, 0.0, 1.0);
  vec3 scene = vec3(hlgScene(e.r), hlgScene(e.g), hlgScene(e.b));
  vec3 weights = color_primaries == 9 ? vec3(0.2627, 0.6780, 0.0593) :
                                      vec3(0.2126, 0.7152, 0.0722);
  float y = dot(scene, weights);
  return 1000.0 * pow(max(y, 1e-6), 0.2) * scene;
}
vec3 hdrToSdr(vec3 encoded) {
  // H.273: 16 is PQ, 18 is HLG. SDR stays on the existing fast path.
  if (color_transfer != 16 && color_transfer != 18) return encoded;
  vec3 nits = color_transfer == 16 ? pqToNits(encoded) : hlgToNits(encoded);
  if (color_primaries != 9) nits = bt709ToBt2020 * nits;
  vec3 ictcp = lmsToIctcp * nitsToPq(bt2020ToLms * nits);
  vec3 rgb = ictcpToSdr(ictcp);
  if (!inside(rgb)) {
    float anchor = 0.5 * min(ictcp.x, 0.580689);
    float lo = 0.0;
    float hi = 1.0;
    for (int i = 0; i < 12; ++i) {
      float t = 0.5 * (lo + hi);
      if (inside(ictcpToSdr(vec3(mix(anchor, ictcp.x, t), t * ictcp.yz)))) {
        lo = t;
      } else {
        hi = t;
      }
    }
    rgb = ictcpToSdr(vec3(mix(anchor, ictcp.x, lo), lo * ictcp.yz));
  }
  return pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / 2.4));
}

void main() {
  vec3 yuv = vec3(texture2D(luma, uv).r, texture2D(chroma, uv).rg) - offset;
  gl_FragColor = vec4(hdrToSdr(matrix * yuv), 1.0);
}
)";

// Two triangles covering the viewport.
constexpr GLfloat kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};

struct GlObjects {
  GLuint output = 0;  // What Flutter draws.
  GLuint planes[2] = {};
  GLuint framebuffer = 0;
  GLuint buffer = 0;
  GLuint program = 0;
};

// GL objects of finalized textures. They belong to Flutter's render
// context, which is current only inside populate, so the next populate of
// any texture deletes them.
GMutex orphans_mutex;
std::vector<GlObjects> orphans;

void DeleteOrphans() {
  g_mutex_lock(&orphans_mutex);
  for (GlObjects& gl : orphans) {
    glDeleteTextures(1, &gl.output);
    glDeleteTextures(2, gl.planes);
    glDeleteFramebuffers(1, &gl.framebuffer);
    glDeleteBuffers(1, &gl.buffer);
    glDeleteProgram(gl.program);
  }
  orphans.clear();
  g_mutex_unlock(&orphans_mutex);
}

// The GL state populate changes, saved on construction and restored on
// destruction. Skia's path resets Skia's GL state around the callback;
// ResolveTextureImpeller in embedder_external_texture_gl.cc does not.
class ScopedGlState {
 public:
  ScopedGlState()
      : es3_(epoxy_gl_version() >= 30),
        oes_vao_(!es3_ && epoxy_has_gl_extension("GL_OES_vertex_array_object")) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture_);
    for (int i = 0; i < 2; ++i) {
      glActiveTexture(GL_TEXTURE0 + i);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures_[i]);
    }
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment_);
    if (es3_) {
      glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_);
      glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length_);
      glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows_);
      glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels_);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer_);
    glGetIntegerv(GL_VIEWPORT, viewport_);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_);
    for (int i = 0; i < kCapabilityCount; ++i) {
      capabilities_[i] = glIsEnabled(kCapabilities[i]);
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer_);
    if (es3_ || oes_vao_) {
      glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array_);
      BindVertexArray(0);
    }
    // Attribute 0 of the default vertex array, which the shader uses.
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute_.enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribute_.buffer);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute_.size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute_.type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribute_.normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute_.stride);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribute_.pointer);
    if (es3_ || oes_vao_) {
      BindVertexArray(vertex_array_);
    }
  }

  ~ScopedGlState() {
    if (es3_ || oes_vao_) {
      BindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, attribute_.buffer);
    glVertexAttribPointer(0, attribute_.size, attribute_.type,
                          attribute_.normalized, attribute_.stride,
                          attribute_.pointer);
    if (attribute_.enabled) {
      glEnableVertexAttribArray(0);
    } else {
      glDisableVertexAttribArray(0);
    }
    if (es3_ || oes_vao_) {
      BindVertexArray(vertex_array_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer_);
    glUseProgram(program_);
    for (int i = 0; i < kCapabilityCount; ++i) {
      if (capabilities_[i]) {
        glEnable(kCapabilities[i]);
      } else {
        glDisable(kCapabilities[i]);
      }
    }
    glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    if (es3_) {
      glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels_);
      glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows_);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length_);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer_);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment_);
    for (int i = 0; i < 2; ++i) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, textures_[i]);
    }
    glActiveTexture(active_texture_);
  }

  ScopedGlState(const ScopedGlState&) = delete;
  ScopedGlState& operator=(const ScopedGlState&) = delete;

 private:
  static constexpr GLenum kCapabilities[] = {
      GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE};
  static constexpr int kCapabilityCount = 5;

  void BindVertexArray(GLint array) const {
    if (es3_) {
      glBindVertexArray(array);
    } else {
      glBindVertexArrayOES(array);
    }
  }

  const bool es3_;
  const bool oes_vao_;
  GLint active_texture_ = GL_TEXTURE0;
  GLint textures_[2] = {};
  GLint unpack_alignment_ = 4;
  GLint unpack_buffer_ = 0;
  GLint unpack_row_length_ = 0;
  GLint unpack_skip_rows_ = 0;
  GLint unpack_skip_pixels_ = 0;
  GLint framebuffer_ = 0;
  GLint viewport_[4] = {};
  GLboolean color_mask_[4] = {};
  GLboolean capabilities_[kCapabilityCount] = {};
  GLint program_ = 0;
  GLint array_buffer_ = 0;
  GLint vertex_array_ = 0;
  struct {
    GLint enabled = 0;
    GLint buffer = 0;
    GLint size = 4;
    GLint type = GL_FLOAT;
    GLint normalized = 0;
    GLint stride = 0;
    void* pointer = nullptr;
  } attribute_;
};

void SetSampling(GLuint texture) {
  glBindTexture(GL_TEXTURE_2D, texture);
  // The default minification filter wants mipmaps, which nothing here has.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

GLuint CompileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    g_warning("lychee_player: cannot compile shader: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// The YUV to RGB conversion of |dmabuf|'s matrix and range, as an offset
// subtracted from the sampled values and a column-major matrix applied to
// the result.
void YuvToRgb(const lychee_dmabuf* dmabuf,
              GLfloat offset[3],
              GLfloat matrix[9]) {
  // H.273 matrix coefficients: 1 BT.709, 9 and 10 BT.2020, else BT.601.
  float kr = 0.299f;
  float kb = 0.114f;
  if (dmabuf->color_matrix == 1) {
    kr = 0.2126f;
    kb = 0.0722f;
  } else if (dmabuf->color_matrix == 9 || dmabuf->color_matrix == 10) {
    kr = 0.2627f;
    kb = 0.0593f;
  }
  const float kg = 1 - kr - kb;
  const bool ten_bit = dmabuf->fourcc == kDrmFormatP010;
  const float max = ten_bit ? 1023.f : 255.f;
  // An 8-bit code value at this depth.
  const float unit = ten_bit ? 4.f : 1.f;
  // P010 keeps its 10 bits in the high bits of 16: a sample is
  // value * 64 / 65535, not value / 1023.
  const float sample = ten_bit ? 65535.f / (1023.f * 64.f) : 1.f;
  const bool full = dmabuf->full_range != 0;
  const float y = full ? 1.f : max / (219.f * unit);
  const float c = full ? 1.f : max / (224.f * unit);
  offset[0] = (full ? 0.f : 16.f * unit / max) / sample;
  offset[1] = offset[2] = 128.f * unit / max / sample;
  const GLfloat columns[9] = {
      y,
      y,
      y,  // Y
      0,
      -c * 2 * kb * (1 - kb) / kg,
      c * 2 * (1 - kb),  // Cb
      c * 2 * (1 - kr),
      -c * 2 * kr * (1 - kr) / kg,
      0,  // Cr
  };
  for (int i = 0; i < 9; ++i) {
    matrix[i] = columns[i] * sample;
  }
}

// What a frame shows: the same pixels come back from acquire_frame() until
// the player has a newer frame.
const void* FrameContents(lychee_frame* frame) {
  if (lychee_frame_get_type(frame) == LYCHEE_FRAME_DMABUF) {
    return lychee_frame_get_native_handle(frame);
  }
  int32_t stride = 0;
  return lychee_frame_get_data(frame, &stride);
}

// The DRM render node of the GPU Flutter draws with, "" when EGL does not
// say which, or null when Flutter's EGL display cannot import dmabufs.
// Flutter's display is EGL's for GDK's display (fl_opengl_manager.cc), so
// asking for it again returns the same one. Main thread.
const char* DmabufRenderNode() {
  static std::string node;
  static const bool supported = [] {
    GdkDisplay* gdk_display = gdk_display_get_default();
    EGLDisplay display = EGL_NO_DISPLAY;
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
      display = eglGetPlatformDisplayEXT(
          EGL_PLATFORM_WAYLAND_EXT,
          gdk_wayland_display_get_wl_display(gdk_display), nullptr);
    }
#endif
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdk_display)) {
      display = eglGetPlatformDisplayEXT(
          EGL_PLATFORM_X11_EXT, gdk_x11_display_get_xdisplay(gdk_display),
          nullptr);
    }
#endif
    // Flutter has initialized it already; this only reads the version.
    if (display == EGL_NO_DISPLAY ||
        !eglInitialize(display, nullptr, nullptr) ||
        !epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) {
      return false;
    }
    EGLAttrib device = 0;
    if ((epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_query") ||
         epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_base")) &&
        eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &device)) {
      auto* egl_device = reinterpret_cast<EGLDeviceEXT>(device);
      const char* extensions =
          eglQueryDeviceStringEXT(egl_device, EGL_EXTENSIONS);
      const char* file =
          extensions && strstr(extensions, "EGL_EXT_device_drm_render_node")
              ? eglQueryDeviceStringEXT(egl_device,
                                        EGL_DRM_RENDER_NODE_FILE_EXT)
              : nullptr;
      if (file) {
        node = file;
      }
    }
    return true;
  }();
  return supported ? node.c_str() : nullptr;
}

}  // namespace

G_DECLARE_FINAL_TYPE(LycheeTexture,
                     lychee_texture,
                     LYCHEE,
                     TEXTURE,
                     FlTextureGL)

struct _LycheeTexture {
  FlTextureGL parent_instance;

  GMutex mutex;
  lychee_player* player;      // Guarded by mutex; NULL once disposed.
  int64_t next_display_time;  // Guarded by mutex.
  // The player it was created for, even after dispose: the plugin destroys
  // it when Dart, which owns it, is gone.
  lychee_player* created_player;

  // Raster thread only (and finalize). The frame drawn into |gl.output|:
  // the decoder must not reuse its buffers while the GPU may still read
  // them.
  lychee_frame* frame;
  GlObjects gl;
  int32_t width;  // Of |gl.output|.
  int32_t height;
  GLint offset_location;
  GLint matrix_location;
  GLint transfer_location;
  GLint primaries_location;
  // Tightly packed copy, for padded rows where GL cannot skip the padding.
  uint8_t* packed;
  size_t packed_size;
  gboolean logged_error;

  // Main thread only.
  FlTextureRegistrar* registrar;
  GtkWidget* view;  // Weak.
  guint tick_id;
};

G_DEFINE_TYPE(LycheeTexture, lychee_texture, fl_texture_gl_get_type())

static gboolean lychee_texture_initialize_program(LycheeTexture* self) {
  if (self->gl.program) {
    return TRUE;
  }
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  GLuint program = 0;
  if (vertex && fragment) {
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
      g_warning("lychee_player: cannot link the YUV shader");
      glDeleteProgram(program);
      program = 0;
    }
  }
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  if (!program) {
    return FALSE;
  }
  self->gl.program = program;
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "luma"), 0);
  glUniform1i(glGetUniformLocation(program, "chroma"), 1);
  self->offset_location = glGetUniformLocation(program, "offset");
  self->matrix_location = glGetUniformLocation(program, "matrix");
  self->transfer_location = glGetUniformLocation(program, "color_transfer");
  self->primaries_location = glGetUniformLocation(program, "color_primaries");
  glGenTextures(2, self->gl.planes);
  for (GLuint plane : self->gl.planes) {
    SetSampling(plane);
  }
  glGenFramebuffers(1, &self->gl.framebuffer);
  glGenBuffers(1, &self->gl.buffer);
  glBindBuffer(GL_ARRAY_BUFFER, self->gl.buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  return TRUE;
}

// Uploads a CPU RGBA frame into |gl.output|, bound.
static gboolean lychee_texture_upload(LycheeTexture* self,
                                      lychee_frame* frame) {
  int32_t stride = 0;
  const uint8_t* data = lychee_frame_get_type(frame) == LYCHEE_FRAME_CPU_RGBA
                            ? lychee_frame_get_data(frame, &stride)
                            : nullptr;
  if (!data) {
    return FALSE;
  }
  const size_t row = static_cast<size_t>(self->width) * 4;
  const bool es3 = epoxy_gl_version() >= 30;
  if (es3) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  } else if (static_cast<size_t>(stride) != row) {
    const size_t size = row * self->height;
    if (self->packed_size != size) {
      g_free(self->packed);
      self->packed = static_cast<uint8_t*>(g_malloc(size));
      self->packed_size = size;
    }
    for (int32_t y = 0; y < self->height; ++y) {
      memcpy(self->packed + row * y, data + static_cast<size_t>(stride) * y,
             row);
    }
    data = self->packed;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, self->width, self->height, GL_RGBA,
                  GL_UNSIGNED_BYTE, data);
  if (es3) {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  return TRUE;
}

// Imports the planes of |dmabuf| and converts them into |gl.output|.
static gboolean lychee_texture_convert(LycheeTexture* self,
                                       const lychee_dmabuf* dmabuf) {
  // libepoxy aborts on calls the context cannot make.
  if (dmabuf->plane_count != 2 || !epoxy_has_gl_extension("GL_OES_EGL_image") ||
      !lychee_texture_initialize_program(self)) {
    return FALSE;
  }
  const EGLDisplay display = eglGetCurrentDisplay();
  const bool ten_bit = dmabuf->fourcc == kDrmFormatP010;
  const uint32_t formats[2] = {ten_bit ? kDrmFormatR16 : kDrmFormatR8,
                               ten_bit ? kDrmFormatGR1616 : kDrmFormatGR88};
  const bool modifiers = dmabuf->modifier != kDrmFormatModInvalid &&
                         epoxy_has_egl_extension(
                             display, "EGL_EXT_image_dma_buf_import_modifiers");
  EGLImageKHR images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
  gboolean ok = TRUE;
  // One single-plane image per plane: R8 and GR88 import on more drivers
  // than NV12, and need no external texture target.
  for (int i = 0; i < 2 && ok; ++i) {
    EGLint attributes[] = {
        EGL_WIDTH,
        i ? (dmabuf->width + 1) / 2 : dmabuf->width,
        EGL_HEIGHT,
        i ? (dmabuf->height + 1) / 2 : dmabuf->height,
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(formats[i]),
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dmabuf->fd[i],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        static_cast<EGLint>(dmabuf->offset[i]),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>(dmabuf->pitch[i]),
        EGL_NONE,
        0,
        EGL_NONE,
        0,
        EGL_NONE,
    };
    if (modifiers) {
      attributes[12] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
      attributes[13] = static_cast<EGLint>(dmabuf->modifier & 0xffffffff);
      attributes[14] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
      attributes[15] = static_cast<EGLint>(dmabuf->modifier >> 32);
    }
    images[i] = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    ok = images[i] != EGL_NO_IMAGE_KHR;
    if (ok) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, self->gl.planes[i]);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images[i]);
    }
  }
  if (ok) {
    glBindFramebuffer(GL_FRAMEBUFFER, self->gl.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           self->gl.output, 0);
    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }
  if (ok) {
    GLfloat offset[3];
    GLfloat matrix[9];
    YuvToRgb(dmabuf, offset, matrix);
    glViewport(0, 0, self->width, self->height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(self->gl.program);
    glUniform3fv(self->offset_location, 1, offset);
    glUniformMatrix3fv(self->matrix_location, 1, GL_FALSE, matrix);
    // API 13 appended these fields. Old descriptors remain SDR.
    const bool color_metadata =
        dmabuf->struct_size >= offsetof(lychee_dmabuf, color_transfer) +
                                   sizeof(dmabuf->color_transfer);
    int primaries = color_metadata ? dmabuf->color_primaries : 0;
    if (primaries == 0 || primaries == 2) {
      primaries =
          dmabuf->color_matrix == 9 || dmabuf->color_matrix == 10 ? 9 : 1;
    }
    glUniform1i(self->transfer_location,
                color_metadata ? dmabuf->color_transfer : 0);
    glUniform1i(self->primaries_location, primaries);
    // The renderer's vertex array would otherwise take the attribute.
    if (epoxy_gl_version() >= 30) {
      glBindVertexArray(0);
    } else if (epoxy_has_gl_extension("GL_OES_vertex_array_object")) {
      glBindVertexArrayOES(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, self->gl.buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
  }
  // The textures keep the buffers they were bound to.
  for (EGLImageKHR image : images) {
    if (image != EGL_NO_IMAGE_KHR) {
      eglDestroyImageKHR(display, image);
    }
  }
  glActiveTexture(GL_TEXTURE0);
  return ok;
}

static gboolean lychee_texture_draw(LycheeTexture* self, lychee_frame* frame) {
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  if (width <= 0 || height <= 0) {
    return FALSE;
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, self->gl.output);
  if (width != self->width || height != self->height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    self->width = width;
    self->height = height;
  }
  if (lychee_frame_get_type(frame) == LYCHEE_FRAME_DMABUF) {
    return lychee_texture_convert(self,
                                  static_cast<const lychee_dmabuf*>(
                                      lychee_frame_get_native_handle(frame)));
  }
  return lychee_texture_upload(self, frame);
}

static gboolean lychee_texture_populate(FlTextureGL* texture,
                                        uint32_t* target,
                                        uint32_t* name,
                                        uint32_t* width,
                                        uint32_t* height,
                                        GError** error) {
  LycheeTexture* self = LYCHEE_TEXTURE(texture);
  // First, so the saved state no longer names what it deletes.
  DeleteOrphans();
  ScopedGlState state;
  if (!self->gl.output) {
    // Flutter paints the texture once before the first frame is marked
    // available; show nothing until then.
    static const uint8_t kTransparent[4] = {0, 0, 0, 0};
    glGenTextures(1, &self->gl.output);
    SetSampling(self->gl.output);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 kTransparent);
    self->width = 1;
    self->height = 1;
  }

  lychee_frame* frame = nullptr;
  g_mutex_lock(&self->mutex);
  if (self->player) {
    int64_t display_time = self->next_display_time;
    const int64_t now = lychee_now_us();
    if (display_time < now) {
      display_time = now;
    }
    frame = lychee_player_acquire_frame(self->player, display_time);
  }
  g_mutex_unlock(&self->mutex);

  if (frame && self->frame &&
      FrameContents(frame) == FrameContents(self->frame)) {
    lychee_frame_release(frame);
  } else if (frame && lychee_texture_draw(self, frame)) {
    if (self->frame) {
      lychee_frame_release(self->frame);
    }
    self->frame = frame;
  } else if (frame) {
    if (!self->logged_error) {
      self->logged_error = TRUE;
      g_warning("lychee_player: cannot draw frames of type %d",
                lychee_frame_get_type(frame));
    }
    lychee_frame_release(frame);
  }
  *target = GL_TEXTURE_2D;
  *name = self->gl.output;
  *width = static_cast<uint32_t>(self->width);
  *height = static_cast<uint32_t>(self->height);
  return TRUE;
}

static void lychee_texture_finalize(GObject* object) {
  LycheeTexture* self = LYCHEE_TEXTURE(object);
  if (self->frame) {
    lychee_frame_release(self->frame);
  }
  if (self->gl.output) {
    g_mutex_lock(&orphans_mutex);
    orphans.push_back(self->gl);
    g_mutex_unlock(&orphans_mutex);
  }
  g_free(self->packed);
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(lychee_texture_parent_class)->finalize(object);
}

static void lychee_texture_class_init(LycheeTextureClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = lychee_texture_finalize;
  FL_TEXTURE_GL_CLASS(klass)->populate = lychee_texture_populate;
}

static void lychee_texture_init(LycheeTexture* self) {
  g_mutex_init(&self->mutex);
  self->gl = GlObjects();
}

// Main thread, once per frame of the view's frame clock.
static gboolean lychee_texture_on_tick(GtkWidget*,
                                       GdkFrameClock* clock,
                                       gpointer user_data) {
  LycheeTexture* self = LYCHEE_TEXTURE(user_data);
  // Frame clock times are g_get_monotonic_time(): CLOCK_MONOTONIC in
  // microseconds, the lychee_now_us() timebase. The predicted presentation
  // time is when what is drawn now reaches the screen.
  const gint64 frame_time = gdk_frame_clock_get_frame_time(clock);
  gint64 interval = 0;
  gint64 presentation = 0;
  gdk_frame_clock_get_refresh_info(clock, frame_time, &interval, &presentation);
  const int64_t display_time =
      presentation > 0 ? presentation : frame_time + interval;

  g_mutex_lock(&self->mutex);
  self->next_display_time = display_time;
  // Called every refresh, drawn or not: the player estimates the display's
  // refresh interval from these calls.
  const bool has_new_frame =
      self->player &&
      lychee_player_has_new_frame(self->player, display_time) != 0;
  g_mutex_unlock(&self->mutex);
  if (has_new_frame) {
    fl_texture_registrar_mark_texture_frame_available(self->registrar,
                                                      FL_TEXTURE(self));
  }
  return G_SOURCE_CONTINUE;
}

static LycheeTexture* lychee_texture_new(lychee_player* player,
                                         FlTextureRegistrar* registrar,
                                         GtkWidget* view) {
  LycheeTexture* self =
      LYCHEE_TEXTURE(g_object_new(lychee_texture_get_type(), nullptr));
  self->player = player;
  self->created_player = player;
  self->registrar = registrar;
  // Hardware-decoded frames then come as dmabufs on Flutter's GPU.
  if (const char* node = DmabufRenderNode()) {
    lychee_video_device video_device = {};
    video_device.struct_size = sizeof(video_device);
    video_device.type = LYCHEE_VIDEO_DEVICE_DRM_RENDER_NODE;
    video_device.path = node;
    lychee_player_set_video_device(player, &video_device);
  }
  if (view) {
    self->view = view;
    g_object_add_weak_pointer(G_OBJECT(view),
                              reinterpret_cast<gpointer*>(&self->view));
    self->tick_id = gtk_widget_add_tick_callback(view, lychee_texture_on_tick,
                                                 self, nullptr);
  } else {
    g_warning("lychee_player: no Flutter view, video will not update");
  }
  return self;
}

// Main thread. After this returns the player is never touched again and may
// be destroyed.
static void lychee_texture_dispose_player(LycheeTexture* self) {
  if (self->view) {
    // Tick callbacks run on this thread, so none is running now.
    gtk_widget_remove_tick_callback(self->view, self->tick_id);
    g_object_remove_weak_pointer(G_OBJECT(self->view),
                                 reinterpret_cast<gpointer*>(&self->view));
    self->view = nullptr;
  }
  g_mutex_lock(&self->mutex);
  self->player = nullptr;
  g_mutex_unlock(&self->mutex);
}

G_DECLARE_FINAL_TYPE(LycheePlayerPlugin,
                     lychee_player_plugin,
                     LYCHEE,
                     PLAYER_PLUGIN,
                     GObject)

struct _LycheePlayerPlugin {
  GObject parent_instance;
  // Weak: the registrar owns the messenger, which owns this plugin through
  // the channel handler; a strong reference would be a cycle.
  GWeakRef view;
  FlTextureRegistrar* registrar;
  // Texture id -> LycheeTexture (owned).
  GHashTable* textures;
};

G_DEFINE_TYPE(LycheePlayerPlugin, lychee_player_plugin, G_TYPE_OBJECT)

// The textures table's key for texture |id|. The id is the FlTexture's
// address, not a small counter as on other platforms, so it does not fit a
// gint: GPOINTER_TO_INT() of a key is no id.
static gpointer texture_key(int64_t id) {
  return reinterpret_cast<gpointer>(static_cast<intptr_t>(id));
}

static gboolean release_texture(gpointer texture) {
  g_object_unref(texture);
  return G_SOURCE_REMOVE;
}

static FlMethodResponse* create_texture(LycheePlayerPlugin* self,
                                        FlValue* args) {
  FlValue* player_value = args && fl_value_get_type(args) == FL_VALUE_TYPE_MAP
                              ? fl_value_lookup_string(args, "player")
                              : nullptr;
  if (!player_value || fl_value_get_type(player_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "player missing", nullptr));
  }
  auto* player =
      reinterpret_cast<lychee_player*>(fl_value_get_int(player_value));
  g_autoptr(GObject) view = static_cast<GObject*>(g_weak_ref_get(&self->view));
  LycheeTexture* texture = lychee_texture_new(
      player, self->registrar, view ? GTK_WIDGET(view) : nullptr);
  if (!fl_texture_registrar_register_texture(self->registrar,
                                             FL_TEXTURE(texture))) {
    lychee_texture_dispose_player(texture);
    g_object_unref(texture);
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "texture", "cannot register texture", nullptr));
  }
  const int64_t id = fl_texture_get_id(FL_TEXTURE(texture));
  g_hash_table_insert(self->textures, texture_key(id), texture);
  g_autoptr(FlValue) result = fl_value_new_int(id);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

// For players whose Dart side is gone: their event callback points into a
// dead isolate.
static void destroy_orphan(lychee_player* player) {
  lychee_player_set_event_callback(player, nullptr, nullptr);
  lychee_player_destroy(player);
}

// Stops |texture|, already out of the table, pulling frames and unregisters
// it. Returns its player.
static lychee_player* retire_texture(LycheePlayerPlugin* self,
                                     LycheeTexture* texture) {
  lychee_player* player = texture->created_player;
  lychee_texture_dispose_player(texture);
  fl_texture_registrar_unregister_texture(self->registrar, FL_TEXTURE(texture));
  // The raster thread looks textures up without a reference and may still
  // be inside copy_pixels; keep ours a while so it does not see the texture
  // freed under it.
  g_timeout_add_seconds(1, release_texture, texture);
  return player;
}

// Stops texture |id| pulling frames and unregisters it. Returns its player,
// or NULL if there is no such texture.
static lychee_player* remove_texture(LycheePlayerPlugin* self, int64_t id) {
  gpointer texture = nullptr;
  if (!g_hash_table_steal_extended(self->textures, texture_key(id), nullptr,
                                   &texture)) {
    return nullptr;
  }
  return retire_texture(self, LYCHEE_TEXTURE(texture));
}

// After a hot restart: the previous isolate's players are still playing.
// Walks the table rather than its keys: a key is no int (texture_key).
static void dispose_all(LycheePlayerPlugin* self) {
  GHashTableIter iter;
  gpointer texture = nullptr;
  g_hash_table_iter_init(&iter, self->textures);
  while (g_hash_table_iter_next(&iter, nullptr, &texture)) {
    g_hash_table_iter_steal(&iter);
    destroy_orphan(retire_texture(self, LYCHEE_TEXTURE(texture)));
  }
}

static FlMethodResponse* dispose_texture(LycheePlayerPlugin* self,
                                         FlValue* args) {
  FlValue* id_value = args && fl_value_get_type(args) == FL_VALUE_TYPE_MAP
                          ? fl_value_lookup_string(args, "textureId")
                          : nullptr;
  if (!id_value || fl_value_get_type(id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "textureId missing", nullptr));
  }
  // Dart destroys the player once this returns.
  remove_texture(self, fl_value_get_int(id_value));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static void method_call_cb(FlMethodChannel*,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  LycheePlayerPlugin* self = LYCHEE_PLAYER_PLUGIN(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(method, "createTexture") == 0) {
    response = create_texture(self, args);
  } else if (strcmp(method, "disposeTexture") == 0) {
    response = dispose_texture(self, args);
  } else if (strcmp(method, "disposeAll") == 0) {
    dispose_all(self);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }
  fl_method_call_respond(method_call, response, nullptr);
}

// The engine is shutting down with textures Dart never disposed.
static void dispose_texture_entry(gpointer texture) {
  lychee_texture_dispose_player(LYCHEE_TEXTURE(texture));
  destroy_orphan(LYCHEE_TEXTURE(texture)->created_player);
  g_object_unref(texture);
}

static void lychee_player_plugin_dispose(GObject* object) {
  LycheePlayerPlugin* self = LYCHEE_PLAYER_PLUGIN(object);
  g_clear_pointer(&self->textures, g_hash_table_unref);
  g_clear_object(&self->registrar);
  g_weak_ref_clear(&self->view);
  G_OBJECT_CLASS(lychee_player_plugin_parent_class)->dispose(object);
}

static void lychee_player_plugin_class_init(LycheePlayerPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = lychee_player_plugin_dispose;
}

static void lychee_player_plugin_init(LycheePlayerPlugin* self) {
  g_weak_ref_init(&self->view, nullptr);
  self->textures = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr,
                                         dispose_texture_entry);
}

void lychee_player_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  LycheePlayerPlugin* plugin = LYCHEE_PLAYER_PLUGIN(
      g_object_new(lychee_player_plugin_get_type(), nullptr));
  // The runner creates the view before registering plugins; NULL when
  // headless.
  g_weak_ref_set(&plugin->view, fl_plugin_registrar_get_view(registrar));
  plugin->registrar = FL_TEXTURE_REGISTRAR(
      g_object_ref(fl_plugin_registrar_get_texture_registrar(registrar)));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "lychee_player", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      channel, method_call_cb, g_object_ref(plugin), g_object_unref);
  g_object_unref(plugin);
}
