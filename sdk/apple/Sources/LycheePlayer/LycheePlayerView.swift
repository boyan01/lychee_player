import CoreVideo
import QuartzCore
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// Shows a player's video, aspect-fit and rotated as the media says. Frames
/// are pulled only while the view is in a window.
public final class LycheePlayerView: LycheePlatformView {
  public var player: LycheePlayer? {
    didSet {
      if player !== oldValue { restart() }
    }
  }

  private let videoLayer = CALayer()
  private var driver: LycheeFrameDriver?
  // Kept while its IOSurface is the layer's contents, so the decoder's pool
  // does not reuse the surface on screen.
  private var shownFrame: CVPixelBuffer?
  private var rotation = 0

  public override init(frame: CGRect) {
    super.init(frame: frame)
    #if os(macOS)
    wantsLayer = true
    layer?.backgroundColor = CGColor.black
    layer?.addSublayer(videoLayer)
    #else
    backgroundColor = .black
    layer.addSublayer(videoLayer)
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
    if let rotation = player?.rotation, rotation != self.rotation {
      self.rotation = rotation
      layoutVideoLayer()
    }
    CATransaction.commit()
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
    CATransaction.commit()
  }
}
