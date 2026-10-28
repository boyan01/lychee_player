// Minimal windowed player without Flutter on Linux: GLFW owns the window and
// an OpenGL ES 3 context on EGL. Frames come the way the Flutter Linux plugin
// (flutter/linux) takes them: CPU RGBA frames are uploaded into a texture,
// and hardware-decoded dmabufs (VAAPI) are imported a plane at a time as
// EGLImages and converted from YUV by the shader. Useful to tell core
// problems from Flutter texture bridge problems.
//
// Keys: space play/pause, left/right seek -/+5 s, escape quit. --key
// SEC:NAME presses one at SEC seconds, --seconds N quits after N seconds,
// --size WxH sizes the window, and --snapshot SEC:FILE writes what the
// window drew at SEC seconds as a PPM.
// --soak SEC URL loops, seeks twice, then switches files, every SEC seconds.

#include <lychee/lychee.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// After gl3.h, which defines what it needs.
#include <GLES2/gl2ext.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_EGL
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

// Draws a quad scaled to fit the window. Texture coordinates are rotated
// clockwise in quarter turns, like Flutter's RotatedBox.
const char* kVertexShader = R"(#version 300 es
uniform vec2 scale;
uniform int quarter_turns;
out vec2 uv;
void main() {
  const vec2 corners[4] =
      vec2[4](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
  vec2 corner = corners[gl_VertexID];
  gl_Position = vec4(corner * scale, 0.0, 1.0);
  uv = corner * vec2(0.5, -0.5) + 0.5;
  for (int i = 0; i < quarter_turns; ++i) {
    uv = vec2(uv.y, 1.0 - uv.x);
  }
}
)";

// RGBA from plane0, or YUV from plane0 (luma) and plane1 (chroma).
const char* kFragmentShader = R"(#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform bool yuv;
uniform vec3 offset;
uniform mat3 matrix;
out vec4 color;
void main() {
  if (yuv) {
    vec3 value = vec3(texture(plane0, uv).r, texture(plane1, uv).rg);
    color = vec4(matrix * (value - offset), 1.0);
  } else {
    color = vec4(texture(plane0, uv).rgb, 1.0);
  }
}
)";

bool HasExtension(const char* extensions, const char* name) {
  const size_t length = strlen(name);
  for (const char* p = extensions; p && (p = strstr(p, name)); p += length) {
    if ((p == extensions || p[-1] == ' ') &&
        (p[length] == ' ' || p[length] == '\0')) {
      return true;
    }
  }
  return false;
}

// Whether |display| can import dmabufs, and if so the DRM render node of its
// GPU in |node| ("" when EGL does not say which), as flutter/linux asks.
bool DmabufRenderNode(EGLDisplay display, std::string* node) {
  if (!HasExtension(eglQueryString(display, EGL_EXTENSIONS),
                    "EGL_EXT_image_dma_buf_import") ||
      !HasExtension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
                    "GL_OES_EGL_image")) {
    return false;
  }
  node->clear();
  const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  auto query_display = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDisplayAttribEXT"));
  auto query_device = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
      eglGetProcAddress("eglQueryDeviceStringEXT"));
  EGLAttrib device = 0;
  if ((HasExtension(client, "EGL_EXT_device_query") ||
       HasExtension(client, "EGL_EXT_device_base")) &&
      query_display && query_device &&
      query_display(display, EGL_DEVICE_EXT, &device)) {
    auto egl_device = reinterpret_cast<EGLDeviceEXT>(device);
    if (HasExtension(query_device(egl_device, EGL_EXTENSIONS),
                     "EGL_EXT_device_drm_render_node")) {
      if (const char* file =
              query_device(egl_device, EGL_DRM_RENDER_NODE_FILE_EXT)) {
        *node = file;
      }
    }
  }
  return true;
}

// The YUV to RGB conversion of |dmabuf|'s matrix and range, as an offset
// subtracted from the sampled values and a column-major matrix applied to
// the result. The same as flutter/linux's.
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

GLuint CompileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    fprintf(stderr, "shader: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

void SetSampling(GLuint texture) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

class Renderer {
 public:
  // With the window's context current.
  bool Init() {
    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex && fragment) {
      program_ = glCreateProgram();
      glAttachShader(program_, vertex);
      glAttachShader(program_, fragment);
      glLinkProgram(program_);
      GLint ok = GL_FALSE;
      glGetProgramiv(program_, GL_LINK_STATUS, &ok);
      if (!ok) {
        fprintf(stderr, "cannot link the shader\n");
        glDeleteProgram(program_);
        program_ = 0;
      }
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!program_) return false;
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "plane0"), 0);
    glUniform1i(glGetUniformLocation(program_, "plane1"), 1);
    scale_location_ = glGetUniformLocation(program_, "scale");
    turns_location_ = glGetUniformLocation(program_, "quarter_turns");
    yuv_location_ = glGetUniformLocation(program_, "yuv");
    offset_location_ = glGetUniformLocation(program_, "offset");
    matrix_location_ = glGetUniformLocation(program_, "matrix");
    glGenTextures(1, &rgba_);
    glGenTextures(2, planes_);
    for (GLuint texture : {rgba_, planes_[0], planes_[1]}) {
      SetSampling(texture);
    }
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return true;
  }

  // Shows |frame| from now on, taking it over. The frame on screen is kept
  // until the next one: the decoder must not reuse its buffers while the GPU
  // may still read them.
  void Take(lychee_frame* frame) {
    bool yuv = lychee_frame_get_type(frame) == LYCHEE_FRAME_DMABUF;
    bool ok = yuv ? Import(static_cast<const lychee_dmabuf*>(
                        lychee_frame_get_native_handle(frame)))
                  : Upload(frame);
    if (!ok) {
      if (!logged_error_) {
        logged_error_ = true;
        fprintf(stderr, "cannot draw frames of type %d\n",
                lychee_frame_get_type(frame));
      }
      lychee_frame_release(frame);
      return;
    }
    if (frame_) lychee_frame_release(frame_);
    frame_ = frame;
    yuv_ = yuv;
  }

  // Draws the frame on screen, if any, and returns the pixels it covers
  // (x, y, width, height from the top left), or false.
  bool Draw(lychee_player* player, int width, int height, int rect[4]) {
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int32_t video_width = 0, video_height = 0;
    if (!frame_ ||
        !lychee_player_get_video_size(player, &video_width, &video_height)) {
      return false;
    }
    int quarter_turns = lychee_player_get_video_rotation(player) / 90;
    if (quarter_turns % 2) std::swap(video_width, video_height);
    double video_aspect = static_cast<double>(video_width) / video_height;
    double view_aspect = static_cast<double>(width) / height;
    const double scale[2] = {std::min(1.0, video_aspect / view_aspect),
                             std::min(1.0, view_aspect / video_aspect)};
    // The pixels whose centres the centred quad covers.
    for (int i = 0; i < 2; ++i) {
      const int size = i ? height : width;
      const double low = size * (1 - scale[i]) / 2;
      const double high = size * (1 + scale[i]) / 2;
      rect[i] = static_cast<int>(std::ceil(low - 0.5));
      rect[i + 2] = static_cast<int>(std::ceil(high - 0.5)) - rect[i];
    }

    glUseProgram(program_);
    glUniform2f(scale_location_, static_cast<float>(scale[0]),
                static_cast<float>(scale[1]));
    glUniform1i(turns_location_, quarter_turns);
    glUniform1i(yuv_location_, yuv_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yuv_ ? planes_[0] : rgba_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, planes_[1]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return true;
  }

  // The pts of the frame on screen, on the file's timeline.
  int64_t pts() const { return frame_ ? lychee_frame_get_pts(frame_) : 0; }

  void Stop() {
    if (frame_) lychee_frame_release(frame_);
    frame_ = nullptr;
    glDeleteTextures(1, &rgba_);
    glDeleteTextures(2, planes_);
    glDeleteProgram(program_);
  }

 private:
  bool Upload(lychee_frame* frame) {
    int32_t stride = 0;
    const uint8_t* data = lychee_frame_get_type(frame) == LYCHEE_FRAME_CPU_RGBA
                              ? lychee_frame_get_data(frame, &stride)
                              : nullptr;
    int32_t width = lychee_frame_get_width(frame);
    int32_t height = lychee_frame_get_height(frame);
    if (!data || width <= 0 || height <= 0) return false;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rgba_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (width != width_ || height != height_) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, data);
      width_ = width;
      height_ = height;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                      GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return true;
  }

  // Imports the two planes of |dmabuf| into |planes_| and sets the YUV to
  // RGB uniforms. One single-plane image per plane, as flutter/linux does:
  // R8 and GR88 import on more drivers than NV12, and need no external
  // texture target.
  bool Import(const lychee_dmabuf* dmabuf) {
    if (!dmabuf || dmabuf->plane_count != 2 || !create_image_ ||
        !destroy_image_ || !image_target_) {
      return false;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    const bool ten_bit = dmabuf->fourcc == kDrmFormatP010;
    const uint32_t formats[2] = {ten_bit ? kDrmFormatR16 : kDrmFormatR8,
                                 ten_bit ? kDrmFormatGR1616 : kDrmFormatGR88};
    const bool modifiers =
        dmabuf->modifier != kDrmFormatModInvalid &&
        HasExtension(eglQueryString(display, EGL_EXTENSIONS),
                     "EGL_EXT_image_dma_buf_import_modifiers");
    for (int i = 0; i < 2; ++i) {
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
      EGLImageKHR image = create_image_(
          display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
      if (image == EGL_NO_IMAGE_KHR) return false;
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, planes_[i]);
      image_target_(GL_TEXTURE_2D, image);
      // The texture keeps the buffer it was bound to.
      destroy_image_(display, image);
    }
    GLfloat offset[3];
    GLfloat matrix[9];
    YuvToRgb(dmabuf, offset, matrix);
    glUseProgram(program_);
    glUniform3fv(offset_location_, 1, offset);
    glUniformMatrix3fv(matrix_location_, 1, GL_FALSE, matrix);
    return true;
  }

  GLuint program_ = 0;
  GLuint rgba_ = 0;        // CPU frames.
  GLuint planes_[2] = {};  // Luma and chroma of dmabuf frames.
  GLint scale_location_ = -1;
  GLint turns_location_ = -1;
  GLint yuv_location_ = -1;
  GLint offset_location_ = -1;
  GLint matrix_location_ = -1;
  PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
  lychee_frame* frame_ = nullptr;  // On screen.
  bool yuv_ = false;
  int32_t width_ = 0;  // Of |rgba_|.
  int32_t height_ = 0;
  bool logged_error_ = false;
};

void OnKey(GLFWwindow* window, int key, int, int action, int) {
  if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
  auto* player = static_cast<lychee_player*>(glfwGetWindowUserPointer(window));
  switch (key) {
    case GLFW_KEY_SPACE:
      if (lychee_player_is_playing(player)) {
        lychee_player_pause(player);
      } else {
        lychee_player_play(player);
      }
      break;
    case GLFW_KEY_LEFT:
    case GLFW_KEY_RIGHT: {
      int64_t step = key == GLFW_KEY_LEFT ? -5000000 : 5000000;
      int64_t target = lychee_player_get_position(player) + step;
      lychee_player_seek(player, std::max<int64_t>(0, target));
      break;
    }
    case GLFW_KEY_ESCAPE:
      glfwSetWindowShouldClose(window, GLFW_TRUE);
      break;
  }
}

// A key press --key SEC:NAME plays at SEC seconds, for unattended runs.
struct ScriptedKey {
  double at;
  int key;
};

bool ParseKey(const std::string& arg, std::vector<ScriptedKey>* keys) {
  const size_t colon = arg.find(':');
  if (colon == std::string::npos) return false;
  const std::string name = arg.substr(colon + 1);
  int key = name == "space"    ? GLFW_KEY_SPACE
            : name == "left"   ? GLFW_KEY_LEFT
            : name == "right"  ? GLFW_KEY_RIGHT
            : name == "escape" ? GLFW_KEY_ESCAPE
                               : GLFW_KEY_UNKNOWN;
  if (key == GLFW_KEY_UNKNOWN) return false;
  keys->push_back({atof(arg.substr(0, colon).c_str()), key});
  return true;
}

// What the window drew --snapshot SEC:FILE writes, at the first frame drawn
// at or after SEC seconds.
struct Snapshot {
  double at;
  std::string file;
};

bool ParseSnapshot(const std::string& arg, std::vector<Snapshot>* snapshots) {
  const size_t colon = arg.find(':');
  if (colon == std::string::npos || colon + 1 == arg.size()) return false;
  snapshots->push_back(
      {atof(arg.substr(0, colon).c_str()), arg.substr(colon + 1)});
  return true;
}

// Reads back the |width| x |height| back buffer just drawn into |file| as a
// binary PPM, top row first.
bool WriteSnapshot(const std::string& file, int width, int height) {
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  FILE* out = fopen(file.c_str(), "wb");
  if (!out) return false;
  fprintf(out, "P6\n%d %d\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  // GL's rows go bottom up.
  for (int y = height - 1; y >= 0; --y) {
    const uint8_t* pixel = rgba.data() + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x, pixel += 4) {
      std::copy(pixel, pixel + 3, row.data() + x * 3);
    }
    fwrite(row.data(), 1, row.size(), out);
  }
  return fclose(out) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string url;
  double seconds = 0;
  int window_width = 960, window_height = 540;
  std::vector<ScriptedKey> keys;
  std::vector<Snapshot> snapshots;
  double soak_every = -1;
  std::string soak_url;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = atof(argv[++i]);
    } else if (arg == "--key" && i + 1 < argc && ParseKey(argv[i + 1], &keys)) {
      ++i;
    } else if (arg == "--size" && i + 1 < argc &&
               sscanf(argv[i + 1], "%dx%d", &window_width, &window_height) ==
                   2 &&
               window_width > 0 && window_height > 0) {
      ++i;
    } else if (arg == "--snapshot" && i + 1 < argc &&
               ParseSnapshot(argv[i + 1], &snapshots)) {
      ++i;
    } else if (arg == "--soak" && i + 2 < argc) {
      char* end = nullptr;
      soak_every = std::strtod(argv[++i], &end);
      if (*end != '\0' || !std::isfinite(soak_every) || soak_every <= 0) {
        url.clear();
        break;
      }
      soak_url = argv[++i];
    } else if (!arg.empty() && arg[0] != '-') {
      url = arg;
    } else {
      url.clear();
      break;
    }
  }
  if (url.empty()) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--key SEC:space|left|right|escape]... "
            "[--size WxH] [--snapshot SEC:FILE]... [--soak SEC URL] "
            "<file or url>\n",
            argv[0]);
    return 2;
  }
  std::stable_sort(
      keys.begin(), keys.end(),
      [](const ScriptedKey& a, const ScriptedKey& b) { return a.at < b.at; });
  std::stable_sort(
      snapshots.begin(), snapshots.end(),
      [](const Snapshot& a, const Snapshot& b) { return a.at < b.at; });

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  // EGL, so dmabufs can be imported into the context.
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  GLFWwindow* window =
      glfwCreateWindow(window_width, window_height, "lychee", nullptr, nullptr);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  Renderer renderer;
  bool initialized = renderer.Init();
  std::string node;
  bool dmabuf = initialized && DmabufRenderNode(glfwGetEGLDisplay(), &node);
  fprintf(stderr, "gl: %s, dmabuf import: %s\n",
          reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
          dmabuf ? (node.empty() ? "yes" : node.c_str()) : "no");

  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types =
      LYCHEE_FRAME_CPU_RGBA | (dmabuf ? LYCHEE_FRAME_DMABUF : 0);
  config.hardware_decoding = 1;
  lychee_player* player = lychee_player_create(&config);
  // Hardware-decoded frames then come as dmabufs on this GPU.
  if (dmabuf) {
    lychee_video_device video_device = {};
    video_device.struct_size = sizeof(video_device);
    video_device.type = LYCHEE_VIDEO_DEVICE_DRM_RENDER_NODE;
    video_device.path = node.c_str();
    lychee_player_set_video_device(player, &video_device);
  }
  glfwSetWindowUserPointer(window, player);
  glfwSetKeyCallback(window, &OnKey);

  // The display's refresh interval, for when a frame drawn now is shown.
  const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
  const int64_t refresh_us =
      mode && mode->refreshRate > 0 ? 1000000 / mode->refreshRate : 16667;

  int exit_code = 0;
  if (!initialized || lychee_player_open(player, url.c_str()) != 0) {
    fprintf(stderr, "could not start playback\n");
    exit_code = 1;
  } else {
    if (soak_every > 0) {
      lychee_player_set_looping(player, 1);
    }
    lychee_player_play(player);
    const int64_t start = lychee_now_us();
    // Repeatable stress actions, not a source of unpredictable values.
    std::mt19937_64 random(1);  // NOLINT(bugprone-random-generator-seed)
    double soak_next = soak_every;
    int64_t soak_actions = 0;
    bool soak_on_alternate = false;
    int64_t tick = start;
    size_t next_key = 0;
    size_t next_snapshot = 0;
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      int32_t state = lychee_player_get_state(player);
      if (state == LYCHEE_STATE_ERROR) {
        char error[512];
        lychee_player_get_error(player, error, sizeof(error));
        fprintf(stderr, "error: %s\n", error);
        exit_code = 1;
        break;
      }
      const double elapsed = static_cast<double>(lychee_now_us() - start) / 1e6;
      while (next_key < keys.size() && elapsed >= keys[next_key].at) {
        OnKey(window, keys[next_key++].key, 0, GLFW_PRESS, 0);
      }
      if (soak_every > 0 && elapsed >= soak_next) {
        soak_next = elapsed + soak_every;
        const int64_t duration = lychee_player_get_duration(player);
        if (soak_actions++ % 3 == 2) {
          soak_on_alternate = !soak_on_alternate;
          const std::string& next = soak_on_alternate ? soak_url : url;
          printf("soak: open -> %s\n", next.c_str());
          if (lychee_player_open(player, next.c_str()) != 0) {
            exit_code = 1;
            break;
          }
          lychee_player_play(player);
        } else if (duration > 0) {
          const int64_t target =
              std::uniform_int_distribution<int64_t>(0, duration - 1)(random);
          printf("soak: seek -> %.3fs\n", target / 1e6);
          lychee_player_seek(player, target);
        }
        fflush(stdout);
      }
      if (seconds > 0 && elapsed >= seconds) break;

      // Asked every refresh, drawn or not: the player estimates the
      // display's refresh interval from these calls. lychee_now_us() is
      // CLOCK_MONOTONIC, and what is drawn now is shown at the next vsync.
      int64_t display_time = lychee_now_us() + refresh_us;
      if (lychee_player_has_new_frame(player, display_time)) {
        if (lychee_frame* frame =
                lychee_player_acquire_frame(player, display_time)) {
          renderer.Take(frame);
        }
      }
      int width = 0, height = 0;
      glfwGetFramebufferSize(window, &width, &height);
      int rect[4] = {};
      if (width > 0 && height > 0 &&
          renderer.Draw(player, width, height, rect)) {
        // Before the swap, which may leave the back buffer undefined.
        while (next_snapshot < snapshots.size() &&
               elapsed >= snapshots[next_snapshot].at) {
          const std::string& file = snapshots[next_snapshot++].file;
          if (!WriteSnapshot(file, width, height)) {
            fprintf(stderr, "cannot write %s\n", file.c_str());
            continue;
          }
          printf("snapshot %s pts=%lld size=%dx%d video=%d,%d,%d,%d\n",
                 file.c_str(), static_cast<long long>(renderer.pts()), width,
                 height, rect[0], rect[1], rect[2], rect[3]);
          fflush(stdout);
        }
      }
      glfwSwapBuffers(window);

      // Where swapping does not wait for vsync (Xvfb; GNOME on the VMware
      // VM ran this loop at 100-140 Hz), wait out the rest of the refresh
      // instead.
      int64_t now = lychee_now_us();
      if (now - tick < refresh_us / 2) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(refresh_us - (now - tick)));
        now = lychee_now_us();
      }
      tick = now;
    }
  }
  // Stop drawing before the player goes away.
  renderer.Stop();

  lychee_stats stats = {};
  stats.struct_size = sizeof(stats);
  lychee_player_get_stats(player, &stats);
  printf("summary: decoder=%s hw=%d rendered=%lld dropped=%lld position=%.3f\n",
         stats.video_decoder, stats.hardware_decoding,
         static_cast<long long>(stats.frames_rendered),
         static_cast<long long>(stats.frames_dropped),
         static_cast<double>(lychee_player_get_position(player)) / 1e6);
  lychee_player_destroy(player);
  glfwDestroyWindow(window);
  glfwTerminate();
  return exit_code;
}
