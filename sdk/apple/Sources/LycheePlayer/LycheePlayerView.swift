import CoreVideo
import QuartzCore
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// Shows a player's video, aspect-fit and rotated as the media says, with
/// its subtitle pictures over it (text subtitles are left to the caller:
/// ``LycheePlayer/subtitle``). Frames are pulled only while the view is in a
/// window.
public final class LycheePlayerView: LycheePlatformView {
  public var player: LycheePlayer? {
    didSet {
      guard player !== oldValue else { return }
      oldValue?.onEventForView = nil
      // An already prepared player may send no event and no video frame.
      clear()
      player?.onEventForView = { [weak self] event in self?.handle(event) }
      restart()
    }
  }

  private let videoLayer = CALayer()
  // Laid out in pixels of the rotated video, scaled onto it.
  private let subtitleLayer = CALayer()
  private var subtitleId: Int64 = 0
  private var driver: LycheeFrameDriver?
  // Kept while its IOSurface is the layer's contents, so the decoder's pool
  // does not reuse the surface on screen.
  private var shownFrame: CVPixelBuffer?
  private var rotation = 0
  private var shownHeadroom: CGFloat = 0

  public override init(frame: CGRect) {
    super.init(frame: frame)
    #if os(macOS)
    wantsLayer = true
    layer?.backgroundColor = CGColor.black
    layer?.addSublayer(videoLayer)
    layer?.addSublayer(subtitleLayer)
    subtitleLayer.isGeometryFlipped = true
    #else
    backgroundColor = .black
    layer.addSublayer(videoLayer)
    layer.addSublayer(subtitleLayer)
    #endif
    videoLayer.contentsGravity = .resizeAspect
  }

  @available(*, unavailable)
  public required init?(coder: NSCoder) {
    fatalError("init(coder:) is not supported")
  }

  #if os(macOS)
  public override func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    restart()
  }

  public override func layout() {
    super.layout()
    layoutVideoLayer()
  }
  #else
  public override func didMoveToWindow() {
    super.didMoveToWindow()
    restart()
  }

  public override func layoutSubviews() {
    super.layoutSubviews()
    layoutVideoLayer()
  }
  #endif

  private func restart() {
    driver?.stop()
    driver = nil
    guard let player else {
      clear()
      return
    }
    guard window != nil else { return }
    let driver = LycheeFrameDriver(player: player) { [weak self] in self?.showFrame() }
    driver.start(following: self)
    self.driver = driver
  }

  private func handle(_ event: LycheePlayer.Event) {
    switch event {
    case .subtitleChanged:
      // Frames pick up animated subtitles; this, changes while paused.
      refreshSubtitle()
    case .stateChanged(.loading):
      // New media: what is on screen belongs to the old one. Pausing and
      // the end keep the last frame.
      clear()
    case .mediaInfo, .tracksChanged, .videoSizeChanged:
      // No video (an audio file, the video track turned off): no frame
      // will replace the last one.
      if player?.videoSize == nil { clear() }
    default:
      break
    }
  }

  private func clear() {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    videoLayer.contents = nil
    shownFrame = nil
    setHDR(0)
    showSubtitle(nil)
    CATransaction.commit()
  }

  private func showFrame() {
    guard let frame = driver?.copyFrame() else { return }
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    // CALayer takes an IOSurface as contents on both platforms; the iOS
    // simulator shows nothing for the CVPixelBuffer itself.
    videoLayer.contents = CVPixelBufferGetIOSurface(frame)?.takeUnretainedValue()
    shownFrame = frame
    setHDR(hdrHeadroom(frame))
    if let rotation = player?.rotation, rotation != self.rotation {
      self.rotation = rotation
      layoutVideoLayer()
    }
    showSubtitle(player?.subtitlePictures(width: Int(canvasSize.width), height: Int(canvasSize.height)))
    CATransaction.commit()
  }

  // How far above SDR white the frame's contents can go, as a multiple of
  // it (BT.2408's 203 nits, where Core Animation puts PQ and HLG's
  // reference white), or 0 for SDR. VideoToolbox attaches no MaxCLL, so PQ
  // gets its whole range and HLG its nominal 1000 nits; Core Animation
  // tone-maps that into the headroom the display has.
  private func hdrHeadroom(_ frame: CVPixelBuffer) -> CGFloat {
    guard let transfer = CVBufferCopyAttachment(frame, kCVImageBufferTransferFunctionKey, nil)
    else { return 0 }
    if CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) { return 10000 / 203 }
    if CFEqual(transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG) { return 1000 / 203 }
    return 0
  }

  // EDR costs power, so only while HDR is on screen. Without it the layer
  // tone-maps HDR contents to SDR. preferredDynamicRange turns EDR on only
  // for contents tagged with a headroom above 1, and VideoToolbox's
  // IOSurfaces carry none, so the layer says it.
  private func setHDR(_ headroom: CGFloat) {
    guard headroom != shownHeadroom else { return }
    shownHeadroom = headroom
    if #available(macOS 26.0, iOS 26.0, *) {
      videoLayer.contentsHeadroom = headroom
      videoLayer.preferredDynamicRange = headroom > 0 ? .high : .standard
    } else if #available(macOS 14.0, iOS 17.0, *) {
      videoLayer.wantsExtendedDynamicRangeContent = headroom > 0
    }
  }

  // The video's display size after rotation.
  private var canvasSize: CGSize {
    guard let size = player?.videoSize else { return .zero }
    return rotation % 180 != 0 ? CGSize(width: size.height, height: size.width) : size
  }

  private func refreshSubtitle() {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    showSubtitle(player?.subtitlePictures(width: Int(canvasSize.width), height: Int(canvasSize.height)))
    CATransaction.commit()
  }

  private func showSubtitle(_ subtitle: LycheePlayer.SubtitlePictures?) {
    guard subtitle?.id ?? 0 != subtitleId else { return }
    subtitleId = subtitle?.id ?? 0
    subtitleLayer.sublayers = subtitle?.pictures.map { picture in
      let layer = CALayer()
      layer.frame = picture.rect
      layer.contents = picture.image
      return layer
    }
    layoutSubtitleLayer()
  }

  private func layoutSubtitleLayer() {
    let canvas = canvasSize
    guard canvas.width > 0, canvas.height > 0 else { return }
    let scale = min(bounds.width / canvas.width, bounds.height / canvas.height)
    subtitleLayer.bounds = CGRect(origin: .zero, size: canvas)
    subtitleLayer.position = CGPoint(x: bounds.midX, y: bounds.midY)
    subtitleLayer.setAffineTransform(CGAffineTransform(scaleX: scale, y: scale))
  }

  private func layoutVideoLayer() {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    var size = bounds.size
    if rotation % 180 != 0 {
      size = CGSize(width: size.height, height: size.width)
    }
    videoLayer.bounds = CGRect(origin: .zero, size: size)
    videoLayer.position = CGPoint(x: bounds.midX, y: bounds.midY)
    // rotation is clockwise; on macOS the layer's y axis points up.
    var angle = CGFloat(rotation) * .pi / 180
    #if os(macOS)
    angle = -angle
    #endif
    videoLayer.setAffineTransform(CGAffineTransform(rotationAngle: angle))
    layoutSubtitleLayer()
    CATransaction.commit()
  }
}
