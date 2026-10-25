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
      if player !== oldValue { restart() }
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
  private var showsHDR = false

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
      videoLayer.contents = nil
      shownFrame = nil
      showSubtitle(nil)
      return
    }
    guard window != nil else { return }
    let driver = LycheeFrameDriver(player: player) { [weak self] in self?.showFrame() }
    driver.start(following: self)
    self.driver = driver
  }

  private func showFrame() {
    guard let frame = driver?.copyFrame() else { return }
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    // CALayer takes an IOSurface as contents on both platforms; the iOS
    // simulator shows nothing for the CVPixelBuffer itself.
    videoLayer.contents = CVPixelBufferGetIOSurface(frame)?.takeUnretainedValue()
    shownFrame = frame
    setHDR(isHDR(frame))
    if let rotation = player?.rotation, rotation != self.rotation {
      self.rotation = rotation
      layoutVideoLayer()
    }
    showSubtitle(player?.subtitlePictures(width: Int(canvasSize.width), height: Int(canvasSize.height)))
    CATransaction.commit()
  }

  private func isHDR(_ frame: CVPixelBuffer) -> Bool {
    guard let transfer = CVBufferCopyAttachment(frame, kCVImageBufferTransferFunctionKey, nil)
    else { return false }
    return CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ)
      || CFEqual(transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG)
  }

  // EDR costs power, so only while HDR is on screen. Without it the layer
  // tone-maps HDR contents to SDR.
  private func setHDR(_ hdr: Bool) {
    guard hdr != showsHDR else { return }
    showsHDR = hdr
    if #available(macOS 26.0, iOS 26.0, *) {
      videoLayer.preferredDynamicRange = hdr ? .high : .standard
    } else if #available(macOS 14.0, iOS 17.0, *) {
      videoLayer.wantsExtendedDynamicRangeContent = hdr
    }
  }

  // The video's display size after rotation.
  private var canvasSize: CGSize {
    guard let size = player?.videoSize else { return .zero }
    return rotation % 180 != 0 ? CGSize(width: size.height, height: size.width) : size
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
