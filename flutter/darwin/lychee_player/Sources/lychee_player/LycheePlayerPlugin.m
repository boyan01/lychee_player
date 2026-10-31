#import "./include/lychee_player/LycheePlayerPlugin.h"
#import "./include/lychee_player/LycheeHostTexture.h"

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

#if LYCHEE_SDK_DRIVER
@import LycheePlayer;
#endif

// CVDisplayLink is deprecated in favor of NSView.displayLink (macOS 14+), but
// it still works and supports older systems.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#if TARGET_OS_OSX
typedef NSView PlatformView;
#else
typedef UIView PlatformView;
#endif

#if LYCHEE_SDK_DRIVER

// The Swift package build: the frames come through sdk/apple's
// LycheeFrameDriver, which the native SDK uses too.
@compatibility_alias LycheeTextureDriver LycheeFrameDriver;

#else

// The CocoaPods build, which cannot depend on the Swift package, and the
// Swift package build of a checkout without local frameworks, whose pinned
// core may be older than sdk/apple: a port of sdk/apple's LycheeFrameDriver
// with the same Objective-C interface. Fix both until CocoaPods support is
// dropped.

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

@interface LycheePortedFrameDriver : NSObject
- (instancetype)initWithHandle:(nullable void*)handle
                    onNewFrame:(void (^)(void))onNewFrame;
- (void)startFollowing:(nullable PlatformView*)view;
- (void)stop;
- (void)setHandle:(nullable void*)handle;
- (nullable CVPixelBufferRef)copyFrame CF_RETURNS_RETAINED;
@end

#if !TARGET_OS_OSX
@interface LycheePortedFrameDriver ()
- (void)onDisplayLink:(CADisplayLink*)link;
@end

// CADisplayLink retains its target; this breaks the cycle.
@interface LycheeWeakTarget : NSObject
@property(nonatomic, weak) LycheePortedFrameDriver* driver;
@end
#endif

@implementation LycheePortedFrameDriver {
  os_unfair_lock _lock;
  lychee_player* _player;  // Guarded by _lock.
  void (^_onNewFrame)(void);
  __weak PlatformView* _view;
#if TARGET_OS_OSX
  CVDisplayLinkRef _displayLink;
  CGDirectDisplayID _display;
#else
  CADisplayLink* _displayLink;
  BOOL _wasBackgrounded;
#endif
  NSArray<id<NSObject>>* _observers;
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
  [(__bridge LycheePortedFrameDriver*)context onVsync:output];
  return kCVReturnSuccess;
}

#endif

- (instancetype)initWithHandle:(nullable void*)handle
                    onNewFrame:(void (^)(void))onNewFrame {
  if (self = [super init]) {
    _lock = OS_UNFAIR_LOCK_INIT;
    _player = handle;
    _onNewFrame = onNewFrame;
  }
  return self;
}

// The macOS display link holds self unretained: stop it before self goes.
- (void)dealloc {
  [self stop];
}

- (void)setHandle:(nullable void*)handle {
  os_unfair_lock_lock(&_lock);
  _player = handle;
  atomic_store(&_framePending, false);
  os_unfair_lock_unlock(&_lock);
}

#if TARGET_OS_OSX
// Main thread only.
- (void)startFollowing:(nullable PlatformView*)view {
  if (_displayLink) {
    return;
  }
  _view = view;
  if (CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink) !=
      kCVReturnSuccess) {
    NSLog(@"lychee_player: CVDisplayLinkCreateWithActiveCGDisplays failed");
    return;
  }
  CVDisplayLinkSetOutputCallback(_displayLink, &OnDisplayLink,
                                 (__bridge void*)self);
  [self followViewScreen];
  CVDisplayLinkStart(_displayLink);

  // A window moved to another screen, or a screen's refresh rate changed.
  __weak LycheePortedFrameDriver* weakSelf = self;
  void (^update)(NSNotification*) = ^(NSNotification* notification) {
    [weakSelf followViewScreen];
  };
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  _observers = @[
    [center addObserverForName:NSWindowDidChangeScreenNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:update],
    [center
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
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
  if (CVDisplayLinkSetCurrentCGDisplay(_displayLink, display) ==
      kCVReturnSuccess) {
    if (display != _display) {
      NSLog(@"lychee_player: display link follows display %u", display);
    }
    _display = display;
  }
  if (running) {
    CVDisplayLinkStart(_displayLink);
  }
}

- (void)stop {
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
  [self setHandle:NULL];
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
  // Not self: stop() may be releasing it on the main thread by then.
  dispatch_async(dispatch_get_main_queue(), _onNewFrame);
}

#else
// Main thread only.
- (void)startFollowing:(nullable PlatformView*)view {
  if (_displayLink) {
    return;
  }
  LycheeWeakTarget* target = [[LycheeWeakTarget alloc] init];
  target.driver = self;
  _displayLink =
      [CADisplayLink displayLinkWithTarget:target
                                  selector:@selector(onDisplayLink:)];
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                     forMode:NSRunLoopCommonModes];
  _wasBackgrounded = UIApplication.sharedApplication.applicationState ==
                     UIApplicationStateBackground;
  __weak LycheePortedFrameDriver* weakSelf = self;
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  _observers = @[
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification* notification) {
                      LycheePortedFrameDriver* driver = weakSelf;
                      if (driver) driver->_wasBackgrounded = YES;
                    }],
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification* notification) {
                      [weakSelf restoreVideoDecoder];
                    }],
  ];
}

- (void)restoreVideoDecoder {
  if (!_wasBackgrounded) return;
  _wasBackgrounded = NO;
  os_unfair_lock_lock(&_lock);
#if LYCHEE_API_VERSION >= 12
  // A checkout may still use the older release pinned by prebuilt.json.
  // Also when the stats say software: rebuilds in the background fail
  // into software, and the core tells that fallback from a choice.
  if (_player) {
    lychee_player_rebuild_video_decoder(_player);
  }
#endif
  os_unfair_lock_unlock(&_lock);
}

- (void)stop {
  for (id<NSObject> observer in _observers) {
    [NSNotificationCenter.defaultCenter removeObserver:observer];
  }
  _observers = nil;
  // On the main thread, like the display link callback, so no callback runs
  // after this.
  [_displayLink invalidate];
  _displayLink = nil;
  [self setHandle:NULL];
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
  _onNewFrame();
}

#endif

// Any thread.
- (nullable CVPixelBufferRef)copyFrame {
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
  [self.driver onDisplayLink:link];
}
@end
#endif

@compatibility_alias LycheeTextureDriver LycheePortedFrameDriver;

#endif  // LYCHEE_SDK_DRIVER

// What a texture shows after its player changed, until the new one has a
// frame: a transparent pixel, rather than the last frame of the previous
// player, which the engine would otherwise keep.
static CVPixelBufferRef BlankPixelBuffer(void) {
  static CVPixelBufferRef buffer;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSDictionary* attributes = @{
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    CVPixelBufferCreate(kCFAllocatorDefault, 1, 1, kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)attributes, &buffer);
    CVPixelBufferLockBaseAddress(buffer, 0);
    memset(CVPixelBufferGetBaseAddress(buffer), 0,
           CVPixelBufferGetDataSize(buffer));
    CVPixelBufferUnlockBaseAddress(buffer, 0);
  });
  return buffer;
}

// Both the plugin's textures, for Dart's players, and the app's. On macOS
// the display link follows the screen that shows the Flutter view, if given
// one: with screens at different refresh rates, the vsync of another screen
// picks frames for the wrong times and drops them. On iOS CADisplayLink
// already follows the device's screen.
@interface LycheeHostTexture () <FlutterTexture>
- (instancetype)initWithRegistry:(NSObject<FlutterTextureRegistry>*)registry
                            view:(nullable PlatformView*)view;
@end

@implementation LycheeHostTexture {
  LycheeTextureDriver* _driver;
  __weak NSObject<FlutterTextureRegistry>* _registry;
  BOOL _disposed;
  // Bumped by setPlayer:. copyPixelBuffer remembers which player its last
  // buffer was for, so a change shows the blank buffer until a frame comes.
  _Atomic int64_t _generation;
  _Atomic int64_t _shownGeneration;
}

+ (uint32_t)frameTypes {
  // What copyPixelBuffer hands to the engine, and the plugin's Dart side
  // asks for (platformFrameType).
  return LYCHEE_FRAME_CVPIXELBUFFER;
}

- (instancetype)initWithRegistry:(NSObject<FlutterTextureRegistry>*)registry {
  return [self initWithRegistry:registry view:nil];
}

- (instancetype)initWithRegistry:(NSObject<FlutterTextureRegistry>*)registry
                            view:(nullable PlatformView*)view {
  if (self = [super init]) {
    _registry = registry;
    __weak LycheeHostTexture* weakSelf = self;
    // On the main thread.
    _driver = [[LycheeTextureDriver alloc] initWithHandle:NULL
                                               onNewFrame:^{
                                                 [weakSelf frameAvailable];
                                               }];
    _textureId = [registry registerTexture:self];
    [_driver startFollowing:view];
  }
  return self;
}

- (void)setPlayer:(nullable lychee_player*)player {
  // Waits for a pull of the previous player in progress.
  [_driver setHandle:player];
  atomic_fetch_add(&_generation, 1);
  __weak LycheeHostTexture* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    [weakSelf frameAvailable];
  });
}

// Main thread.
- (void)frameAvailable {
  if (!_disposed) {
    [_registry textureFrameAvailable:_textureId];
  }
}

- (void)dispose {
  if (_disposed) {
    return;
  }
  _disposed = YES;
  // After this the player is never touched again.
  [_driver stop];
  [_registry unregisterTexture:_textureId];
}

// Called on the raster thread.
- (CVPixelBufferRef)copyPixelBuffer {
  // Read first: a frame pulled while setPlayer: runs may be the previous
  // player's, and must not count for the new one.
  int64_t generation = atomic_load(&_generation);
  CVPixelBufferRef frame = [_driver copyFrame];
  if (frame) {
    atomic_store(&_shownGeneration, generation);
    return frame;
  }
  if (atomic_exchange(&_shownGeneration, generation) != generation) {
    return CVPixelBufferRetain(BlankPixelBuffer());
  }
  return NULL;
}

@end

#pragma clang diagnostic pop

@implementation LycheePlayerPlugin {
  __weak NSObject<FlutterPluginRegistrar>* _registrar;
  NSObject<FlutterTextureRegistry>* _registry;
  // By texture id. The plugin destroys the players when Dart, which owns
  // them, is gone.
  NSMutableDictionary<NSNumber*, LycheeHostTexture*>* _textures;
  NSMutableDictionary<NSNumber*, NSValue*>* _players;
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
    _registry = registrar.textures;
    _textures = [NSMutableDictionary dictionary];
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
    LycheeHostTexture* texture =
        [[LycheeHostTexture alloc] initWithRegistry:_registry view:view];
    [texture setPlayer:player];
    _textures[@(texture.textureId)] = texture;
    _players[@(texture.textureId)] = [NSValue valueWithPointer:player];
    result(@(texture.textureId));
  } else if ([call.method isEqualToString:@"disposeAll"]) {
    [self destroyAll];
    result(nil);
  } else if ([call.method isEqualToString:@"disposeTexture"]) {
    NSNumber* textureId = call.arguments[@"textureId"];
    [_textures[textureId] dispose];
    [_textures removeObjectForKey:textureId];
    [_players removeObjectForKey:textureId];
    result(nil);
  } else {
    result(FlutterMethodNotImplemented);
  }
}

// Disposes every texture and destroys its player. For when the Dart side
// that owns them is gone: a hot restart (disposeAll from the new isolate)
// or the engine going away.
- (void)destroyAll {
  for (NSNumber* textureId in _textures) {
    [_textures[textureId] dispose];
    lychee_player* player = _players[textureId].pointerValue;
    // Its event callback points into the old isolate.
    lychee_player_set_event_callback(player, NULL, NULL);
    lychee_player_destroy(player);
  }
  [_textures removeAllObjects];
  [_players removeAllObjects];
}

- (void)detachFromEngineForRegistrar:
    (NSObject<FlutterPluginRegistrar>*)registrar {
  [self destroyAll];
}

@end
