#import "LycheePlayerPlugin.h"

#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdatomic.h>

#include <lychee/lychee.h>

// CVDisplayLink is deprecated in favor of NSView.displayLink (macOS 14+), but
// it still works and supports older systems.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static int64_t HostTimeToMicros(uint64_t host_time) {
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) {
    mach_timebase_info(&timebase);
  }
  return (int64_t)((__uint128_t)host_time * timebase.numer / timebase.denom /
                   1000);
}

// Bridges one lychee_player to a Flutter texture. The display link asks the
// player whether the next vsync shows a new frame; if so, Flutter is told to
// pull it through copyPixelBuffer.
@interface LycheeTexture : NSObject <FlutterTexture>
@property(nonatomic) int64_t textureId;
- (instancetype)initWithPlayer:(lychee_player*)player
                      registry:(NSObject<FlutterTextureRegistry>*)registry;
- (void)start;
// After this returns the player is never touched again and may be destroyed.
- (void)dispose;
@end

@implementation LycheeTexture {
  os_unfair_lock _lock;
  lychee_player* _player;  // Guarded by _lock.
  __weak NSObject<FlutterTextureRegistry>* _registry;
  CVDisplayLinkRef _displayLink;
  _Atomic int64_t _nextDisplayTime;
  _Atomic bool _framePending;
}

static CVReturn OnDisplayLink(CVDisplayLinkRef displayLink,
                              const CVTimeStamp* now,
                              const CVTimeStamp* output,
                              CVOptionFlags flagsIn,
                              CVOptionFlags* flagsOut,
                              void* context) {
  [(__bridge LycheeTexture*)context onVsync:output];
  return kCVReturnSuccess;
}

- (instancetype)initWithPlayer:(lychee_player*)player
                      registry:(NSObject<FlutterTextureRegistry>*)registry {
  if (self = [super init]) {
    _lock = OS_UNFAIR_LOCK_INIT;
    _player = player;
    _registry = registry;
  }
  return self;
}

- (void)start {
  if (CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink) != kCVReturnSuccess) {
    NSLog(@"lychee_player: CVDisplayLinkCreateWithActiveCGDisplays failed");
    return;
  }
  CVDisplayLinkSetOutputCallback(_displayLink, &OnDisplayLink,
                                 (__bridge void*)self);
  CVDisplayLinkStart(_displayLink);
}

- (void)dispose {
  if (_displayLink) {
    // Waits for a running callback to return.
    CVDisplayLinkStop(_displayLink);
    CVDisplayLinkRelease(_displayLink);
    _displayLink = NULL;
  }
  os_unfair_lock_lock(&_lock);
  _player = NULL;
  os_unfair_lock_unlock(&_lock);
}

- (void)onVsync:(const CVTimeStamp*)output {
  int64_t displayTime = HostTimeToMicros(output->hostTime);
  atomic_store(&_nextDisplayTime, displayTime);
  os_unfair_lock_lock(&_lock);
  bool hasNewFrame =
      _player && lychee_player_has_new_frame(_player, displayTime);
  os_unfair_lock_unlock(&_lock);
  if (!hasNewFrame || atomic_exchange(&_framePending, true)) {
    return;
  }
  int64_t textureId = _textureId;
  NSObject<FlutterTextureRegistry>* registry = _registry;
  dispatch_async(dispatch_get_main_queue(), ^{
    [registry textureFrameAvailable:textureId];
  });
}

// Called on the raster thread.
- (CVPixelBufferRef)copyPixelBuffer {
  atomic_store(&_framePending, false);
  int64_t displayTime = atomic_load(&_nextDisplayTime);
  int64_t now = lychee_now_us();
  if (displayTime < now) {
    displayTime = now;
  }
  CVPixelBufferRef result = NULL;
  os_unfair_lock_lock(&_lock);
  if (_player) {
    lychee_frame* frame = lychee_player_acquire_frame(_player, displayTime);
    if (frame) {
      if (lychee_frame_get_type(frame) == LYCHEE_FRAME_CVPIXELBUFFER) {
        result = CVPixelBufferRetain(
            (CVPixelBufferRef)lychee_frame_get_native_handle(frame));
      }
      lychee_frame_release(frame);
    }
  }
  os_unfair_lock_unlock(&_lock);
  return result;
}

@end

#pragma clang diagnostic pop

@implementation LycheePlayerPlugin {
  NSObject<FlutterTextureRegistry>* _textures;
  NSMutableDictionary<NSNumber*, LycheeTexture*>* _players;
}

+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
  FlutterMethodChannel* channel =
      [FlutterMethodChannel methodChannelWithName:@"lychee_player"
                                  binaryMessenger:registrar.messenger];
  LycheePlayerPlugin* instance =
      [[LycheePlayerPlugin alloc] initWithTextures:registrar.textures];
  [registrar addMethodCallDelegate:instance channel:channel];
}

- (instancetype)initWithTextures:(NSObject<FlutterTextureRegistry>*)textures {
  if (self = [super init]) {
    _textures = textures;
    _players = [NSMutableDictionary dictionary];
  }
  return self;
}

- (void)handleMethodCall:(FlutterMethodCall*)call result:(FlutterResult)result {
  if ([call.method isEqualToString:@"createTexture"]) {
    lychee_player* player =
        (lychee_player*)(intptr_t)[call.arguments[@"player"] longLongValue];
    LycheeTexture* texture = [[LycheeTexture alloc] initWithPlayer:player
                                                          registry:_textures];
    texture.textureId = [_textures registerTexture:texture];
    _players[@(texture.textureId)] = texture;
    [texture start];
    result(@(texture.textureId));
  } else if ([call.method isEqualToString:@"disposeTexture"]) {
    NSNumber* textureId = call.arguments[@"textureId"];
    LycheeTexture* texture = _players[textureId];
    if (texture) {
      [texture dispose];
      [_textures unregisterTexture:textureId.longLongValue];
      [_players removeObjectForKey:textureId];
    }
    result(nil);
  } else {
    result(FlutterMethodNotImplemented);
  }
}

@end
