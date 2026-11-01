import CoreText
import CoreVideo
import QuartzCore
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// Shows a player's video, aspect-fit and rotated as the media says, with
/// its subtitles over it: the pictures of bitmap subtitles, and text
/// subtitles (``LycheePlayer/subtitle``) in ``subtitleStyle``. Frames are
/// pulled only while the view is in a window.
public final class LycheePlayerView: LycheePlatformView {
  /// How text subtitles are drawn. Sizes are fractions of the video's
  /// height, so the text scales with the picture.
  public struct SubtitleStyle {
    /// A font's PostScript name; nil for the system font.
    public var fontName: String?
    public var fontSize: CGFloat
    public var textColor: CGColor
    /// Drawn around each glyph, `outlineWidth` times the font size wide;
    /// nil for none.
    public var outlineColor: CGColor?
    public var outlineWidth: CGFloat
    /// Behind the text; nil for none.
    public var backgroundColor: CGColor?
    /// Between the last line and the bottom of the video.
    public var bottomMargin: CGFloat

    public init(
      fontName: String? = nil,
      fontSize: CGFloat = 0.05,
      textColor: CGColor = CGColor(gray: 1, alpha: 1),
      outlineColor: CGColor? = CGColor(gray: 0, alpha: 1),
      outlineWidth: CGFloat = 0.08,
      backgroundColor: CGColor? = nil,
      bottomMargin: CGFloat = 0.05
    ) {
      self.fontName = fontName
      self.fontSize = fontSize
      self.textColor = textColor
      self.outlineColor = outlineColor
      self.outlineWidth = outlineWidth
      self.backgroundColor = backgroundColor
      self.bottomMargin = bottomMargin
    }
  }

  /// Nil draws no text subtitles, for drawing ``LycheePlayer/subtitle``
  /// yourself.
  public var subtitleStyle: SubtitleStyle? = SubtitleStyle() {
    didSet { layoutSubtitleText() }
  }

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
  // Text subtitles, in the view's points: the outline drawn under the
  // fill, both over the background.
  private let textLayer = CALayer()
  private let outlineTextLayer = CATextLayer()
  private let fillTextLayer = CATextLayer()
  private var subtitleText: String?
  private var driver: LycheeFrameDriver?
  // Kept while its IOSurface is the layer's contents, so the decoder's pool
  // does not reuse the surface on screen.
  private var shownFrame: CVPixelBuffer?
  /// The presentation time of the frame on screen, in microseconds on the
  /// media's timeline, or nil when none is: for checking what the view
  /// shows against the media.
  public private(set) var shownFramePts: Int64?
  private var rotation = 0
  private var shownHeadroom: CGFloat = 0

  public override init(frame: CGRect) {
    super.init(frame: frame)
    #if os(macOS)
    wantsLayer = true
    layer?.backgroundColor = CGColor.black
    layer?.addSublayer(videoLayer)
    layer?.addSublayer(textLayer)
    layer?.addSublayer(subtitleLayer)
    subtitleLayer.isGeometryFlipped = true
    #else
    backgroundColor = .black
    layer.addSublayer(videoLayer)
    layer.addSublayer(textLayer)
    layer.addSublayer(subtitleLayer)
    #endif
    videoLayer.contentsGravity = .resizeAspect
    for text in [outlineTextLayer, fillTextLayer] {
      text.isWrapped = true
      textLayer.addSublayer(text)
    }
    textLayer.isHidden = true
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
      layoutSubtitleText()
    default:
      break
    }
  }

  private func clear() {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    videoLayer.contents = nil
    shownFrame = nil
    shownFramePts = nil
    setHDR(0)
    showSubtitle(nil)
    subtitleText = nil
    layoutSubtitleText()
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
    shownFramePts = driver?.lastFramePts
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
    guard let transfer = transferFunction(frame) else { return 0 }
    if CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) { return 10000 / 203 }
    if CFEqual(transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG) { return 1000 / 203 }
    return 0
  }

  // CVBufferCopyAttachment is macOS 12, and the older getter warns from
  // there on; macOS 11 shows HDR as SDR.
  private func transferFunction(_ frame: CVPixelBuffer) -> CFTypeRef? {
    guard #available(macOS 12.0, *) else { return nil }
    return CVBufferCopyAttachment(frame, kCVImageBufferTransferFunctionKey, nil)
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
    let pictures = player?.subtitlePictures(width: Int(canvasSize.width), height: Int(canvasSize.height))
    showSubtitle(pictures)
    // Pictures hold the text too.
    subtitleText = pictures == nil ? player?.subtitle : nil
    layoutSubtitleText()
    CATransaction.commit()
  }

  // Where the video is in the view.
  private var videoRect: CGRect {
    let canvas = canvasSize
    guard canvas.width > 0, canvas.height > 0 else { return .zero }
    let scale = min(bounds.width / canvas.width, bounds.height / canvas.height)
    let size = CGSize(width: canvas.width * scale, height: canvas.height * scale)
    return CGRect(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2,
                  width: size.width, height: size.height)
  }

  private func layoutSubtitleText() {
    let video = videoRect
    guard let style = subtitleStyle, let text = subtitleText, !text.isEmpty,
          video.height > 0 else {
      textLayer.isHidden = true
      return
    }
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    defer { CATransaction.commit() }
    let fontSize = style.fontSize * video.height
    let font = style.fontName.map { CTFontCreateWithName($0 as CFString, fontSize, nil) }
      ?? CTFontCreateUIFontForLanguage(.system, fontSize, nil)
      ?? CTFontCreateWithName("Helvetica" as CFString, fontSize, nil)
    var alignment = CTTextAlignment.center
    let paragraph = withUnsafeBytes(of: &alignment) { bytes in
      var setting = CTParagraphStyleSetting(spec: .alignment, valueSize: bytes.count,
                                            value: bytes.baseAddress!)
      return CTParagraphStyleCreate(&setting, 1)
    }
    func string(_ attributes: [CFString: Any]) -> NSAttributedString {
      var all: [CFString: Any] = [kCTFontAttributeName: font, kCTParagraphStyleAttributeName: paragraph]
      all.merge(attributes) { $1 }
      return NSAttributedString(string: text, attributes: Dictionary(uniqueKeysWithValues: all.map {
        (NSAttributedString.Key($0.key as String), $0.value)
      }))
    }
    let fill = string([kCTForegroundColorAttributeName: style.textColor])
    let outline = style.outlineWidth * fontSize
    // Wide enough for the outline around the outermost glyphs.
    let padding = outline / 2 + 1
    let framesetter = CTFramesetterCreateWithAttributedString(fill)
    let size = CTFramesetterSuggestFrameSizeWithConstraints(
      framesetter, CFRange(), nil,
      CGSize(width: video.width * 0.9, height: .greatestFiniteMagnitude), nil)
    let width = ceil(size.width) + 2 * padding
    let height = ceil(size.height) + 2 * padding
    let margin = style.bottomMargin * video.height
    #if os(macOS)
    // y up.
    let y = video.minY + margin - padding
    let scale = window?.backingScaleFactor ?? 2
    #else
    let y = video.maxY - margin + padding - height
    let scale = window?.screen.scale ?? 2
    #endif
    textLayer.frame = CGRect(x: video.midX - width / 2, y: y, width: width, height: height)
    textLayer.backgroundColor = style.backgroundColor
    let inside = CGRect(x: padding, y: padding, width: width - 2 * padding,
                        height: height - 2 * padding)
    fillTextLayer.frame = inside
    fillTextLayer.string = fill
    if let color = style.outlineColor, outline > 0 {
      // A positive width strokes without filling, as a percentage of the
      // font size.
      outlineTextLayer.string = string([kCTStrokeColorAttributeName: color,
                                        kCTStrokeWidthAttributeName: style.outlineWidth * 100])
      outlineTextLayer.frame = inside
      outlineTextLayer.isHidden = false
    } else {
      outlineTextLayer.isHidden = true
    }
    for layer in [outlineTextLayer, fillTextLayer] {
      layer.contentsScale = scale
    }
    textLayer.isHidden = false
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
    layoutSubtitleText()
    CATransaction.commit()
  }
}
