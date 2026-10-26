// Minimal windowed player without Flutter: GLFW owns the window, Metal draws
// the CVPixelBuffer frames. Useful to tell core problems from Flutter texture
// bridge problems, and as a second real consumer of the C API.
//
// Keys: space play/pause, left/right seek -/+5 s, escape quit. --key
// SEC:NAME presses one at SEC seconds, --seconds N quits after N seconds.

#include <lychee/lychee.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Draws a textured quad scaled to fit the drawable. Texture coordinates are
// rotated clockwise in quarter turns, like Flutter's RotatedBox.
const char* kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct Params {
  float2 scale;
  int quarter_turns;
};

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

vertex VertexOut vertex_main(uint id [[vertex_id]],
                             constant Params& params [[buffer(0)]]) {
  const float2 corners[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  float2 corner = corners[id];
  VertexOut out;
  out.position = float4(corner * params.scale, 0, 1);
  float2 uv = corner * float2(0.5, -0.5) + 0.5;
  for (int i = 0; i < params.quarter_turns; ++i) {
    uv = float2(uv.y, 1.0 - uv.x);
  }
  out.uv = uv;
  return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> frame [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return frame.sample(s, in.uv);
}
)";

struct Params {
  float scale[2];
  int32_t quarter_turns;
};

}  // namespace

@interface Renderer : NSObject <CAMetalDisplayLinkDelegate>
- (instancetype)initWithWindow:(GLFWwindow*)window
                        player:(lychee_player*)player;
- (void)stop;
@end

@implementation Renderer {
  GLFWwindow* _window;
  lychee_player* _player;
  id<MTLDevice> _device;
  id<MTLCommandQueue> _queue;
  id<MTLRenderPipelineState> _pipeline;
  CAMetalLayer* _layer;
  CAMetalDisplayLink* _displayLink;
  CVMetalTextureCacheRef _textureCache;
  // The frame on screen. Retained here because the lychee_frame that owned
  // it is released as soon as it is acquired.
  CVPixelBufferRef _current;
}

- (instancetype)initWithWindow:(GLFWwindow*)window
                        player:(lychee_player*)player {
  if (!(self = [super init])) return nil;
  _window = window;
  _player = player;
  _device = MTLCreateSystemDefaultDevice();
  _queue = [_device newCommandQueue];

  NSError* error = nil;
  id<MTLLibrary> library =
      [_device newLibraryWithSource:@(kShaderSource) options:nil error:&error];
  if (!library) {
    fprintf(stderr, "shader: %s\n", error.localizedDescription.UTF8String);
    return nil;
  }
  MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
  desc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
  desc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!_pipeline) {
    fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
    return nil;
  }
  CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, _device, nullptr,
                            &_textureCache);

  _layer = [CAMetalLayer layer];
  _layer.device = _device;
  _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  NSWindow* nsWindow = glfwGetCocoaWindow(window);
  nsWindow.contentView.layer = _layer;
  nsWindow.contentView.wantsLayer = YES;

  // The display link hands out drawables with their target presentation
  // time, which is in the lychee_now_us() timebase (mach_absolute_time).
  _displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_layer];
  _displayLink.delegate = self;
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  return self;
}

- (void)stop {
  [_displayLink invalidate];
  _displayLink = nil;
  if (_current) CVPixelBufferRelease(_current);
  _current = nullptr;
  if (_textureCache) CFRelease(_textureCache);
  _textureCache = nullptr;
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)link
             needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  int width = 0, height = 0;
  glfwGetFramebufferSize(_window, &width, &height);
  if (width <= 0 || height <= 0) return;
  CGSize drawableSize = CGSizeMake(width, height);
  if (!CGSizeEqualToSize(_layer.drawableSize, drawableSize)) {
    _layer.drawableSize = drawableSize;
    NSWindow* nsWindow = glfwGetCocoaWindow(_window);
    _layer.contentsScale = nsWindow.backingScaleFactor;
  }

  int64_t display_time =
      static_cast<int64_t>(update.targetPresentationTimestamp * 1e6);
  if (lychee_player_has_new_frame(_player, display_time)) {
    lychee_frame* frame = lychee_player_acquire_frame(_player, display_time);
    if (frame) {
      auto buffer =
          static_cast<CVPixelBufferRef>(lychee_frame_get_native_handle(frame));
      if (buffer) {
        CVPixelBufferRetain(buffer);
        if (_current) CVPixelBufferRelease(_current);
        _current = buffer;
      }
      lychee_frame_release(frame);
    }
  }

  id<CAMetalDrawable> drawable = update.drawable;
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = drawable.texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;

  id<MTLCommandBuffer> commands = [_queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [commands renderCommandEncoderWithDescriptor:pass];

  CVMetalTextureRef texture = nullptr;
  int32_t video_width = 0, video_height = 0;
  if (_current &&
      lychee_player_get_video_size(_player, &video_width, &video_height)) {
    CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, _textureCache, _current, nullptr,
        MTLPixelFormatBGRA8Unorm, CVPixelBufferGetWidth(_current),
        CVPixelBufferGetHeight(_current), 0, &texture);
  }
  if (texture) {
    Params params = {};
    params.quarter_turns = lychee_player_get_video_rotation(_player) / 90;
    if (params.quarter_turns % 2) std::swap(video_width, video_height);
    double video_aspect = static_cast<double>(video_width) / video_height;
    double view_aspect = static_cast<double>(width) / height;
    params.scale[0] = static_cast<float>(
        std::min(1.0, video_aspect / view_aspect));
    params.scale[1] = static_cast<float>(
        std::min(1.0, view_aspect / video_aspect));

    [encoder setRenderPipelineState:_pipeline];
    [encoder setVertexBytes:&params length:sizeof(params) atIndex:0];
    [encoder setFragmentTexture:CVMetalTextureGetTexture(texture) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4];

    // The IOSurface must stay alive until the GPU has read it, and the
    // cache until its texture is gone, which may be after -stop.
    CVPixelBufferRef buffer = CVPixelBufferRetain(_current);
    CFTypeRef cache = CFRetain(_textureCache);
    [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
      CFRelease(texture);
      CVPixelBufferRelease(buffer);
      CFRelease(cache);
    }];
  }
  [encoder endEncoding];
  [commands presentDrawable:drawable];
  [commands commit];
}

@end

namespace {

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

}  // namespace

int main(int argc, char** argv) {
  std::string url;
  double seconds = 0;
  std::vector<ScriptedKey> keys;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = atof(argv[++i]);
    } else if (arg == "--key" && i + 1 < argc && ParseKey(argv[i + 1], &keys)) {
      ++i;
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
            "<file or url>\n",
            argv[0]);
    return 2;
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const ScriptedKey& a, const ScriptedKey& b) {
                     return a.at < b.at;
                   });

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(960, 540, "lychee", nullptr, nullptr);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = LYCHEE_FRAME_CVPIXELBUFFER;
  config.hardware_decoding = 1;
  lychee_player* player = lychee_player_create(&config);
  glfwSetWindowUserPointer(window, player);
  glfwSetKeyCallback(window, &OnKey);

  int exit_code = 0;
  @autoreleasepool {
    Renderer* renderer = [[Renderer alloc] initWithWindow:window player:player];
    if (!renderer || lychee_player_open(player, url.c_str()) != 0) {
      fprintf(stderr, "could not start playback\n");
      exit_code = 1;
    } else {
      lychee_player_play(player);
      const int64_t start = lychee_now_us();
      size_t next_key = 0;
      // glfwWaitEventsTimeout runs the main run loop, which drives the
      // display link.
      while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
        int32_t state = lychee_player_get_state(player);
        if (state == LYCHEE_STATE_ERROR) {
          char error[512];
          lychee_player_get_error(player, error, sizeof(error));
          fprintf(stderr, "error: %s\n", error);
          exit_code = 1;
          break;
        }
        const double elapsed =
            static_cast<double>(lychee_now_us() - start) / 1e6;
        while (next_key < keys.size() && elapsed >= keys[next_key].at) {
          OnKey(window, keys[next_key++].key, 0, GLFW_PRESS, 0);
        }
        if (seconds > 0 && elapsed >= seconds) break;
      }
    }
    // Stop drawing before the player goes away.
    [renderer stop];
  }

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
