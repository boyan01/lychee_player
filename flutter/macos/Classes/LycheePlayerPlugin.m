#import "LycheePlayerPlugin.h"

#import <AppKit/AppKit.h>
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
// pull it through copyPixelBuffer. The display link follows the screen that
// shows the Flutter view: with screens at different refresh rates, the vsync
// of another screen picks frames for the wrong times and drops them.
@interface LycheeTexture : NSObject <FlutterTexture>
@property(nonatomic) int64_t textureId;
- (instancetype)initWithPlayer:(lychee_player*)player
                      registry:(NSObject<FlutterTextureRegistry>*)registry
                          view:(nullable NSView*)view;
- (void)start;
// After this returns the player is never touched again and may be destroyed.
- (void)dispose;
@end

@implementation LycheeTexture {
  os_unfair_lock _lock;
  lychee_player* _player;  // Guarded by _lock.
  __weak NSObject<FlutterTextureRegistry>* _registry;
  __weak NSView* _view;
  CVDisplayLinkRef _displayLink;
  CGDirectDisplayID _display;
  NSArray<id<NSObject>>* _observers;
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
                      registry:(NSObject<FlutterTextureRegistry>*)registry
                          view:(nullable NSView*)view {
  if (self = [super init]) {
    _lock = OS_UNFAIR_LOCK_INIT;
    _player = player;
    _registry = registry;
    _view = view;
  }
  return self;
}

// Main thread only.
- (void)start {
  if (CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink) != kCVReturnSuccess) {
    NSLog(@"lychee_player: CVDisplayLinkCreateWithActiveCGDisplays failed");
    return;
  }
  CVDisplayLinkSetOutputCallback(_displayLink, &OnDisplayLink,
                                 (__bridge void*)self);
  [self followViewScreen];
  CVDisplayLinkStart(_displayLink);

  // A window moved to another screen, or a screen's refresh rate changed.
  __weak LycheeTexture* weakSelf = self;
  void (^update)(NSNotification*) = ^(NSNotification*) {
    [weakSelf followViewScreen];
  };
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  _observers = @[
    [center addObserverForName:NSWindowDidChangeScreenNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:update],
    [center addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:update],
  ];
}

// Points the display link at the screen showing the view. Main thread only.
- (void)followViewScreen {
  if (!_displayLink) {
    return;
  }
  NSNumber* number = _view.window.screen.deviceDescription[@"NSScreenNumber"];
  if (!number) {
    return;
  }
  CGDirectDisplayID display = number.unsignedIntValue;
  bool running = CVDisplayLinkIsRunning(_displayLink);
  // Also re-applied for the same display: its refresh rate may have changed.
  if (running) {
    CVDisplayLinkStop(_displayLink);
  }
  if (CVDisplayLinkSetCurrentCGDisplay(_displayLink, display) == kCVReturnSuccess) {
    if (display != _display) {
      NSLog(@"lychee_player: display link follows display %u", display);
    }
    _display = display;
  }
  if (running) {
    CVDisplayLinkStart(_displayLink);
  }
}

- (void)dispose {
  for (id<NSObject> observer in _observers) {
    [NSNotificationCenter.defaultCenter removeObserver:observer];
  }
  _observers = nil;
  if (_displayLink) {
    // Waits for a running callback to return, so after this nothing touches
    // self from the display link thread. Undocumented; measured on macOS 27
    // (a callback sleeping 100 ms delayed Stop by 100 ms in 20 of 20 runs,
    // and no callback ran after Stop returned). The callback must therefore
    // never block on the main thread, or this deadlocks.
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
  __weak NSObject<FlutterPluginRegistrar>* _registrar;
  NSObject<FlutterTextureRegistry>* _textures;
  NSMutableDictionary<NSNumber*, LycheeTexture*>* _players;
}

+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
  FlutterMethodChannel* channel =
      [FlutterMethodChannel methodChannelWithName:@"lychee_player"
                                  binaryMessenger:registrar.messenger];
  LycheePlayerPlugin* instance =
      [[LycheePlayerPlugin alloc] initWithRegistrar:registrar];
  [registrar addMethodCallDelegate:instance channel:channel];
}

- (instancetype)initWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
  if (self = [super init]) {
    _registrar = registrar;
    _textures = registrar.textures;
    _players = [NSMutableDictionary dictionary];
  }
  return self;
}

- (void)handleMethodCall:(FlutterMethodCall*)call result:(FlutterResult)result {
  if ([call.method isEqualToString:@"createTexture"]) {
    lychee_player* player =
        (lychee_player*)(intptr_t)[call.arguments[@"player"] longLongValue];
    // The view is looked up now, not at registration: it may not exist yet.
    LycheeTexture* texture = [[LycheeTexture alloc] initWithPlayer:player
                                                          registry:_textures
                                                              view:_registrar.view];
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
