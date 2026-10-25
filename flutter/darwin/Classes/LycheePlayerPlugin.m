#import "LycheePlayerPlugin.h"

#import <CoreVideo/CoreVideo.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#endif
#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdatomic.h>

#include <lychee/lychee.h>

// CVDisplayLink is deprecated in favor of NSView.displayLink (macOS 14+), but
// it still works and supports older systems.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#if TARGET_OS_OSX
static int64_t HostTimeToMicros(uint64_t host_time) {
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) {
    mach_timebase_info(&timebase);
  }
  return (int64_t)((__uint128_t)host_time * timebase.numer / timebase.denom /
                   1000);
}
#endif

#if TARGET_OS_OSX
typedef NSView PlatformView;
#else
typedef UIView PlatformView;
#endif

// Bridges one lychee_player to a Flutter texture. The display link asks the
// player whether the next vsync shows a new frame; if so, Flutter is told to
// pull it through copyPixelBuffer. On macOS the display link follows the
// screen that shows the Flutter view: with screens at different refresh
// rates, the vsync of another screen picks frames for the wrong times and
// drops them. On iOS CADisplayLink already follows the device's screen.
@interface LycheeTexture : NSObject <FlutterTexture>
@property(nonatomic) int64_t textureId;
- (instancetype)initWithPlayer:(lychee_player*)player
                      registry:(NSObject<FlutterTextureRegistry>*)registry
                          view:(nullable PlatformView*)view;
- (void)start;
// After this returns the player is never touched again and may be destroyed.
- (void)dispose;
@end

#if !TARGET_OS_OSX
@interface LycheeTexture ()
- (void)onDisplayLink:(CADisplayLink*)link;
@end

// CADisplayLink retains its target; this breaks the cycle.
@interface LycheeWeakTarget : NSObject
@property(nonatomic, weak) LycheeTexture* texture;
@end
#endif

@implementation LycheeTexture {
  os_unfair_lock _lock;
  lychee_player* _player;  // Guarded by _lock.
  __weak NSObject<FlutterTextureRegistry>* _registry;
  __weak PlatformView* _view;
#if TARGET_OS_OSX
  CVDisplayLinkRef _displayLink;
  CGDirectDisplayID _display;
  NSArray<id<NSObject>>* _observers;
#else
  CADisplayLink* _displayLink;
#endif
  _Atomic int64_t _nextDisplayTime;
  _Atomic bool _framePending;
}

#if TARGET_OS_OSX

static CVReturn OnDisplayLink(CVDisplayLinkRef displayLink,
                              const CVTimeStamp* now,
                              const CVTimeStamp* output,
                              CVOptionFlags flagsIn,
                              CVOptionFlags* flagsOut,
                              void* context) {
  [(__bridge LycheeTexture*)context onVsync:output];
  return kCVReturnSuccess;
}

#endif

- (instancetype)initWithPlayer:(lychee_player*)player
                      registry:(NSObject<FlutterTextureRegistry>*)registry
                          view:(nullable PlatformView*)view {
  if (self = [super init]) {
    _lock = OS_UNFAIR_LOCK_INIT;
    _player = player;
    _registry = registry;
    _view = view;
  }
  return self;
}

#if TARGET_OS_OSX
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
  void (^update)(NSNotification*) = ^(NSNotification* notification) {
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

#else
// Main thread only.
- (void)start {
  LycheeWeakTarget* target = [[LycheeWeakTarget alloc] init];
  target.texture = self;
  _displayLink = [CADisplayLink displayLinkWithTarget:target
                                             selector:@selector(onDisplayLink:)];
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)dispose {
  // On the main thread, like the display link callback, so no callback runs
  // after this.
  [_displayLink invalidate];
  _displayLink = nil;
  os_unfair_lock_lock(&_lock);
  _player = NULL;
  os_unfair_lock_unlock(&_lock);
}

// Main thread. targetTimestamp is in CACurrentMediaTime() seconds, which is
// mach_absolute_time(): the lychee_now_us() timebase.
- (void)onDisplayLink:(CADisplayLink*)link {
  int64_t displayTime = (int64_t)(link.targetTimestamp * 1e6);
  atomic_store(&_nextDisplayTime, displayTime);
  os_unfair_lock_lock(&_lock);
  bool hasNewFrame =
      _player && lychee_player_has_new_frame(_player, displayTime);
  os_unfair_lock_unlock(&_lock);
  if (!hasNewFrame || atomic_exchange(&_framePending, true)) {
    return;
  }
  [_registry textureFrameAvailable:_textureId];
}

#endif

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

#if !TARGET_OS_OSX
@implementation LycheeWeakTarget
- (void)onDisplayLink:(CADisplayLink*)link {
  [self.texture onDisplayLink:link];
}
@end
#endif

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
#if !TARGET_OS_OSX
  // A video player plays with the silent switch on and keeps playing
  // while other audio is muted. Apps that need otherwise set their own
  // category after registration.
  [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback
                                        mode:AVAudioSessionModeMoviePlayback
                                     options:0
                                       error:nil];
#endif
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
#if TARGET_OS_OSX
    // The view is looked up now, not at registration: it may not exist yet.
    PlatformView* view = _registrar.view;
#else
    PlatformView* view = nil;
#endif
    LycheeTexture* texture = [[LycheeTexture alloc] initWithPlayer:player
                                                          registry:_textures
                                                              view:view];
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
