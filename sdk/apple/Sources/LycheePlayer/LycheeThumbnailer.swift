import CoreGraphics
import Foundation
import lychee

/// Pictures of a file or URL at given positions, for seek bar previews:
/// lychee_thumbnailer, decoded apart from any player with a demuxer and a
/// decoder of its own. The input opens with the first request and stays
/// open for the next.
///
/// Thread-safe; requests run one at a time. ``image(at:exact:)`` blocks, so
/// call it off the main thread.
public final class LycheeThumbnailer: @unchecked Sendable {
  private let handle: OpaquePointer

  /// Thumbnails of `url`, whose http(s) requests carry `headers` as for
  /// ``LycheePlayer/open(_:headers:start:)``, fitted to `maxWidth` x
  /// `maxHeight` pixels (0: no limit) keeping their aspect ratio, never
  /// enlarged. Opens nothing yet. Nil if a header name or value holds a
  /// line break.
  public init?(
    url: String, headers: [String: String] = [:], maxWidth: Int = 0, maxHeight: Int = 0
  ) {
    guard let lines = LycheePlayer.headerLines(headers),
      let handle = LycheePlayer.withOpenOptions(lines, [], {
        lychee_thumbnailer_create(
          url, $0, Int32(clamping: maxWidth), Int32(clamping: maxHeight))
      })
    else { return nil }
    self.handle = handle
  }

  deinit {
    lychee_thumbnailer_destroy(handle)
  }

  /// The picture at `position` (seconds), upright. Without `exact`, the
  /// keyframe at or before it, which is quick; with it, the frame showing
  /// there. Nil if the input cannot be opened, has no video, cannot seek
  /// there or be decoded (the log says why).
  public func image(at position: TimeInterval, exact: Bool = false) -> CGImage? {
    guard
      let frame = lychee_thumbnailer_get(
        handle, Int64((max(position, 0) * 1e6).rounded()), exact ? 1 : 0)
    else { return nil }
    defer { lychee_frame_release(frame) }
    var stride: Int32 = 0
    let width = Int(lychee_frame_get_width(frame))
    let height = Int(lychee_frame_get_height(frame))
    guard lychee_frame_get_type(frame) == Int32(LYCHEE_FRAME_CPU_RGBA.rawValue),
      let data = lychee_frame_get_data(frame, &stride)
    else { return nil }
    // Copied: the pixels go with the frame.
    let bytes = Data(bytes: data, count: Int(stride) * height)
    guard let provider = CGDataProvider(data: bytes as CFData) else { return nil }
    return CGImage(
      width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
      bytesPerRow: Int(stride), space: CGColorSpace(name: CGColorSpace.sRGB)!,
      bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
      provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
  }
}
