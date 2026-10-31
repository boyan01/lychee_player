import CoreVideo
import lychee
#if os(macOS)
import AppKit
public typealias LycheePlatformView = NSView
#else
import UIKit
public typealias LycheePlatformView = UIView
#endif

/// Pulls a player's frames per vsync, for callers that draw them themselves.
///
/// Each vsync it asks the player whether the frame for that vsync's display
/// time differs from the last one; if so, `onNewFrame` runs on the main
/// queue, and ``copyFrame()`` returns that frame. `onNewFrame` does not run
/// again until ``copyFrame()`` was called.
///
/// On macOS the display link follows the screen that shows the view passed
/// to ``start(following:)``: with screens at different refresh rates, the
/// vsync of another screen picks frames for the wrong times and drops them.
/// On iOS CADisplayLink already follows the device's screen.
///
/// Also available to Objective-C, which the Flutter plugin's texture uses
/// with ``init(handle:onNewFrame:)``.
public final class LycheeFrameDriver: NSObject {
  private let lock = NSLock()
  // Keeps the player alive while frames are pulled. The display link and
  // copyFrame() use only the handle, so the player is never released on
  // their threads.
  private var player: LycheePlayer?  // Guarded by lock.
  private var handle: OpaquePointer?  // Guarded by lock.
  private var nextDisplayTime: Int64 = 0  // Guarded by lock.
  private var framePending = false  // Guarded by lock.
  private var framePts: Int64?  // Guarded by lock.
  private let onNewFrame: () -> Void
  private var observers: [NSObjectProtocol] = []
  #if os(macOS)
  private var displayLink: CVDisplayLink?
  private weak var view: NSView?
  #else
  private var displayLink: CADisplayLink?
  private var wasBackgrounded = false
  #endif

  public init(player: LycheePlayer, onNewFrame: @escaping () -> Void) {
    self.player = player
    handle = player.handle
    self.onNewFrame = onNewFrame
  }

  /// Pulls the frames of a `lychee_player*` the caller owns, or none until
  /// ``setHandle(_:)``, as the Flutter plugin does with Dart's players and
  /// the app's. The driver does not keep it alive: destroy it only after
  /// ``stop()`` or ``setHandle(_:)`` with another one returned.
  @objc public init(handle: UnsafeMutableRawPointer?, onNewFrame: @escaping () -> Void) {
    self.handle = handle.map(OpaquePointer.init)
    self.onNewFrame = onNewFrame
  }

  deinit {
    stop()
  }

  /// Starts pulling frames. Main thread only.
  @objc public func start(following view: LycheePlatformView? = nil) {
    guard displayLink == nil else { return }
    #if os(macOS)
    var link: CVDisplayLink?
    guard CVDisplayLinkCreateWithActiveCGDisplays(&link) == kCVReturnSuccess, let link else {
      NSLog("LycheeFrameDriver: CVDisplayLinkCreateWithActiveCGDisplays failed")
      return
    }
    displayLink = link
    self.view = view
    CVDisplayLinkSetOutputCallback(link, { _, _, output, _, _, context in
      let driver = Unmanaged<LycheeFrameDriver>.fromOpaque(context!).takeUnretainedValue()
      driver.vsync(displayTime: hostTimeToMicros(output.pointee.hostTime))
      return kCVReturnSuccess
    }, Unmanaged.passUnretained(self).toOpaque())
    followViewScreen()
    CVDisplayLinkStart(link)

    // A window moved to another screen, or a screen's refresh rate changed.
    let update: (Notification) -> Void = { [weak self] _ in self?.followViewScreen() }
    let center = NotificationCenter.default
    observers = [
      center.addObserver(forName: NSWindow.didChangeScreenNotification, object: nil,
                         queue: .main, using: update),
      center.addObserver(forName: NSApplication.didChangeScreenParametersNotification,
                         object: nil, queue: .main, using: update),
    ]
    #else
    let link = CADisplayLink(target: WeakTarget(self), selector: #selector(WeakTarget.onDisplayLink))
    link.add(to: .main, forMode: .common)
    displayLink = link
    wasBackgrounded = UIApplication.shared.applicationState == .background
    let center = NotificationCenter.default
    observers = [
      center.addObserver(forName: UIApplication.didEnterBackgroundNotification,
                         object: nil, queue: .main) { [weak self] _ in
        self?.wasBackgrounded = true
      },
      center.addObserver(forName: UIApplication.didBecomeActiveNotification,
                         object: nil, queue: .main) { [weak self] _ in
        self?.restoreVideoDecoder()
      },
    ]
    #endif
  }

  /// Stops pulling frames and lets go of the player; after it returns,
  /// nothing runs on the display link and the player is not touched again.
  /// Main thread only.
  @objc public func stop() {
    observers.forEach(NotificationCenter.default.removeObserver)
    observers = []
    #if os(macOS)
    if let displayLink {
      // Waits for a running callback to return (measured on macOS 27, see
      // the Flutter plugin's LycheePlayerPlugin.m), so the callback never
      // blocks on the main thread.
      CVDisplayLinkStop(displayLink)
    }
    #else
    // On the main thread, like the callback, so none runs after this.
    displayLink?.invalidate()
    #endif
    displayLink = nil
    setHandle(nil)
  }

  /// Pulls the frames of another `lychee_player*` the caller owns from the
  /// next vsync on, or none if nil; ``copyFrame()`` returns nil until it
  /// has a frame. Any thread. Returns once no call into the previous player
  /// is in progress, so it may be destroyed then.
  @objc public func setHandle(_ handle: UnsafeMutableRawPointer?) {
    lock.lock()
    self.handle = handle.map(OpaquePointer.init)
    let player = self.player
    self.player = nil
    framePending = false
    framePts = nil
    lock.unlock()
    // Released outside the lock.
    withExtendedLifetime(player) {}
  }

  /// The frame for the latest vsync, or nil if there is none yet: an
  /// IOSurface-backed buffer in the decoder's format (bi-planar YCbCr, 8- or
  /// 10-bit, or 32BGRA from software decoding), with color attachments that
  /// say how to show it. Any thread. Objective-C callers own the buffer
  /// (the copy rule), as `FlutterTexture.copyPixelBuffer` wants it.
  @objc public func copyFrame() -> CVPixelBuffer? {
    lock.lock()
    defer { lock.unlock() }
    framePending = false
    guard let handle else { return nil }
    let displayTime = max(nextDisplayTime, lychee_now_us())
    guard let frame = lychee_player_acquire_frame(handle, displayTime) else { return nil }
    defer { lychee_frame_release(frame) }
    guard let buffer = lychee_frame_get_native_handle(frame) else { return nil }
    framePts = lychee_frame_get_pts(frame)
    return Unmanaged<CVPixelBuffer>.fromOpaque(buffer).takeUnretainedValue()
  }

  /// The presentation time of the frame `copyFrame()` last returned, in
  /// microseconds on the media's timeline, or nil before the first.
  public var lastFramePts: Int64? {
    lock.lock()
    defer { lock.unlock() }
    return framePts
  }

  // The display link's thread on macOS, the main thread on iOS.
  private func vsync(displayTime: Int64) {
    lock.lock()
    nextDisplayTime = displayTime
    let notify = handle.map { lychee_player_has_new_frame($0, displayTime) != 0 } == true
      && !framePending
    if notify { framePending = true }
    lock.unlock()
    guard notify else { return }
    #if os(macOS)
    // Not self: stop() may be releasing it on the main thread by then.
    DispatchQueue.main.async(execute: onNewFrame)
    #else
    onNewFrame()
    #endif
  }

  #if os(macOS)
  // Points the display link at the screen showing the view. Main thread.
  private func followViewScreen() {
    guard let displayLink,
          let number = view?.window?.screen?.deviceDescription[
            NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return }
    let display = CGDirectDisplayID(number.uint32Value)
    let running = CVDisplayLinkIsRunning(displayLink)
    // Also re-applied for the same display: its refresh rate may have
    // changed.
    if running { CVDisplayLinkStop(displayLink) }
    CVDisplayLinkSetCurrentCGDisplay(displayLink, display)
    if running { CVDisplayLinkStart(displayLink) }
  }
  #else
  // VideoToolbox may reject valid packets after background suspension. A
  // foreground transition gets a new session without lowering the decoder's
  // tolerance of corrupt input. The core preserves paused/live playback.
  private func restoreVideoDecoder() {
    guard wasBackgrounded else { return }
    wasBackgrounded = false
    lock.lock()
    defer { lock.unlock() }
    guard let handle else { return }
    // Also when the stats say software: rebuilds in the background fail
    // into software, and the core tells that fallback from a choice.
    lychee_player_rebuild_video_decoder(handle)
  }

  // CADisplayLink retains its target; this breaks the cycle.
  private final class WeakTarget: NSObject {
    weak var driver: LycheeFrameDriver?

    init(_ driver: LycheeFrameDriver) {
      self.driver = driver
    }

    // targetTimestamp is in CACurrentMediaTime() seconds, which is
    // mach_absolute_time(): the lychee_now_us() timebase.
    @objc func onDisplayLink(_ link: CADisplayLink) {
      driver?.vsync(displayTime: Int64(link.targetTimestamp * 1e6))
    }
  }
  #endif
}

#if os(macOS)
private let timebase: mach_timebase_info_data_t = {
  var timebase = mach_timebase_info_data_t()
  mach_timebase_info(&timebase)
  return timebase
}()

// lychee_now_us() is mach_absolute_time() in microseconds.
private func hostTimeToMicros(_ hostTime: UInt64) -> Int64 {
  let product = hostTime.multipliedFullWidth(by: UInt64(timebase.numer))
  return Int64((UInt64(timebase.denom) * 1000).dividingFullWidth(product).quotient)
}
#endif
