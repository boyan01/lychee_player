// Plays a file or URL (the argument, else LYCHEE_URL, else
// /tmp/lychee-media/h264_aac.mp4, which the iOS simulator sees too) on a
// loop, with a stats line under the video.
//
//   Example [--seconds N] [--snapshot SEC:FILE]... [--size WxH] [FILE_OR_URL]
//
// --seconds quits after N seconds, printing a summary: line. --snapshot
// writes what the video view shows at SEC seconds as a PPM and prints
//   snapshot FILE pts=US size=WxH video=X,Y,W,H
// for tool/frame_check.py --log, as examples/glfw does. --size sets the
// video view's size in points on macOS (a strong downscale differs from
// the reference at sharp edges: Core Animation filters differently).

import AVFoundation
import Combine
import LycheePlayer
import SwiftUI

#if os(macOS)
import AppKit
import ScreenCaptureKit
#else
import UIKit
#endif

struct Options {
  var url = ProcessInfo.processInfo.environment["LYCHEE_URL"] ?? "/tmp/lychee-media/h264_aac.mp4"
  var seconds: Double?
  var snapshots: [(at: Double, path: String)] = []
  var size: CGSize?

  init(_ arguments: [String]) {
    var args = arguments.makeIterator()
    while let arg = args.next() {
      switch arg {
      case "--seconds":
        seconds = args.next().flatMap(Double.init)
      case "--snapshot":
        // SEC:FILE; the first colon, so FILE may hold one.
        if let value = args.next(), let colon = value.firstIndex(of: ":"),
           let at = Double(value[..<colon]) {
          snapshots.append((at, String(value[value.index(after: colon)...])))
        }
      case "--size":
        let parts = (args.next() ?? "").split(separator: "x").compactMap { Double($0) }
        if parts.count == 2 { size = CGSize(width: parts[0], height: parts[1]) }
      case _ where arg.hasPrefix("-"):
        // The system's own (-NSDocumentRevisionsDebugMode YES, ...).
        _ = args.next()
      default:
        url = arg
      }
    }
  }
}

@main
struct ExampleApp: App {
  var body: some Scene {
    WindowGroup {
      PlayerScreen()
    }
  }
}

struct PlayerScreen: View {
  private static let options = Options(Array(CommandLine.arguments.dropFirst()))
  @StateObject private var model = PlayerModel(options)

  var body: some View {
    VStack(spacing: 0) {
      VideoView(model: model)
        .frame(width: Self.options.size?.width, height: Self.options.size?.height)
      Text(model.status)
        .font(.system(.footnote, design: .monospaced))
        .padding(6)
    }
    #if os(macOS)
    .frame(minWidth: 480, minHeight: 320)
    #endif
  }
}

final class PlayerModel: ObservableObject {
  let player = LycheePlayer()
  weak var view: LycheePlayerView?
  let size: CGSize?
  @Published var status = ""
  private var timer: AnyCancellable?

  init(_ options: Options) {
    size = options.size
    player.isLooping = true
    player.open(options.url)
    player.play()
    timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect().sink { [weak self] _ in
      self?.update()
    }
    for snapshot in options.snapshots {
      DispatchQueue.main.asyncAfter(deadline: .now() + snapshot.at) { [weak self] in
        self?.writeSnapshot(to: snapshot.path)
      }
    }
    if let seconds = options.seconds {
      DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { [weak self] in
        self?.quit()
      }
    }
  }

  private func update() {
    let stats = player.stats
    let size = player.videoSize.map { "\(Int($0.width))x\(Int($0.height))" } ?? "-"
    status = String(
      format: "%@ %.1f/%.1fs %@ %@ rendered=%d dropped=%d", "\(player.state)", player.position,
      player.duration ?? 0, size, stats.hardwareDecoding ? "hw" : "sw", stats.framesRendered,
      stats.framesDropped)
    if let error = player.error {
      status += " error: \(error)"
    }
  }

  private func quit() {
    let stats = player.stats
    print(String(
      format: "summary: state=%@ position=%.3fs hw=%d rendered=%d dropped=%d", "\(player.state)",
      player.position, stats.hardwareDecoding ? 1 : 0, stats.framesRendered, stats.framesDropped))
    fflush(stdout)
    exit(player.error == nil ? 0 : 1)
  }

  // The view's pixels as drawn, and where the video is in them. Paused
  // meanwhile, since the capture takes a few frames and the pts must be the
  // one on screen while it runs.
  private func writeSnapshot(to path: String) {
    guard let view, view.shownFramePts != nil, player.videoSize != nil else {
      print("snapshot \(path) failed: no frame shown by then")
      return
    }
    player.pause()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
      let before = view.shownFramePts
      render(view) { image in
        guard let self else { return }
        self.player.play()
        guard let image, let pts = before, view.shownFramePts == pts else {
          print("snapshot \(path) failed: cannot capture the window")
          return
        }
        self.writeSnapshot(image, pts: pts, to: path)
      }
    }
  }

  private func writeSnapshot(_ image: CGImage, pts: Int64, to path: String) {
    guard let size = player.videoSize else { return }
    let rotated = player.rotation % 180 != 0 ? CGSize(width: size.height, height: size.width) : size
    let bounds = CGRect(x: 0, y: 0, width: CGFloat(image.width), height: CGFloat(image.height))
    // Centered, so the same from the top as from the bottom.
    let video = AVMakeRect(aspectRatio: rotated, insideRect: bounds).integral
    guard writePPM(image, to: path) else {
      print("snapshot \(path) failed: cannot write")
      return
    }
    print("snapshot \(path) pts=\(pts) size=\(image.width)x\(image.height) "
      + "video=\(Int(video.minX)),\(Int(video.minY)),\(Int(video.width)),\(Int(video.height))")
    fflush(stdout)
  }
}

// What the window server composited for the view, on the main queue. On
// macOS the view's own drawing (cacheDisplay) leaves IOSurface contents
// out, so this captures the window with ScreenCaptureKit (macOS 14; the
// Screen Recording permission of whoever launched the app) and crops it.
private func render(_ view: LycheePlayerView, done: @escaping (CGImage?) -> Void) {
  #if os(macOS)
  guard #available(macOS 14.0, *), let window = view.window else { return done(nil) }
  let windowID = CGWindowID(window.windowNumber)
  let scale = window.backingScaleFactor
  let frame = window.frame
  // Window coordinates start at the bottom left of its frame.
  let rect = view.convert(view.bounds, to: nil)
  Task {
    var image: CGImage?
    if let content = try? await SCShareableContent.excludingDesktopWindows(
      false, onScreenWindowsOnly: true),
      let scWindow = content.windows.first(where: { $0.windowID == windowID }) {
      let configuration = SCStreamConfiguration()
      configuration.width = Int(frame.width * scale)
      configuration.height = Int(frame.height * scale)
      configuration.showsCursor = false
      // The space of untagged and BT.709 video, so that Core Animation's
      // color matching comes out as no change, as the frame check's
      // reference has it (sRGB would differ by the transfer function).
      configuration.colorSpaceName = CGColorSpace.itur_709
      let filter = SCContentFilter(desktopIndependentWindow: scWindow)
      let whole = try? await SCScreenshotManager.captureImage(
        contentFilter: filter, configuration: configuration)
      image = whole?.cropping(to: CGRect(
        x: rect.minX * scale, y: (frame.height - rect.maxY) * scale,
        width: rect.width * scale, height: rect.height * scale).integral)
    }
    DispatchQueue.main.async { done(image) }
  }
  #else
  let format = UIGraphicsImageRendererFormat()
  format.scale = view.window?.screen.scale ?? 1
  done(UIGraphicsImageRenderer(bounds: view.bounds, format: format).image { _ in
    view.drawHierarchy(in: view.bounds, afterScreenUpdates: false)
  }.cgImage)
  #endif
}

// Binary PPM (P6), 8-bit RGB.
private func writePPM(_ image: CGImage, to path: String) -> Bool {
  let width = image.width
  let height = image.height
  var rgba = [UInt8](repeating: 0, count: width * height * 4)
  // The capture's own space keeps its values as they are; sRGB where that
  // takes no 8-bit context (the extended range UIKit renders in).
  let spaces = [image.colorSpace, CGColorSpace(name: CGColorSpace.sRGB)].compactMap { $0 }
  guard let context = spaces.lazy.compactMap({ space in
    CGContext(
      data: &rgba, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
      space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
  }).first else { return false }
  context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
  var data = Data("P6\n\(width) \(height)\n255\n".utf8)
  data.reserveCapacity(data.count + width * height * 3)
  for i in stride(from: 0, to: rgba.count, by: 4) {
    data.append(contentsOf: rgba[i..<i + 3])
  }
  return FileManager.default.createFile(atPath: path, contents: data)
}

#if os(macOS)
struct VideoView: NSViewRepresentable {
  let model: PlayerModel

  func makeNSView(context: Context) -> LycheePlayerView {
    let view = LycheePlayerView(frame: .zero)
    view.player = model.player
    model.view = view
    if let size = model.size {
      // The window keeps the size it was last closed with otherwise. The
      // content view runs under the title bar, which SwiftUI keeps clear;
      // the stats line goes under the video.
      DispatchQueue.main.async {
        guard let window = view.window else { return }
        let titleBar = window.frame.height - window.contentLayoutRect.height
        window.setContentSize(CGSize(width: size.width, height: size.height + 40 + titleBar))
      }
    }
    return view
  }

  func updateNSView(_ view: LycheePlayerView, context: Context) {}
}
#else
struct VideoView: UIViewRepresentable {
  let model: PlayerModel

  func makeUIView(context: Context) -> LycheePlayerView {
    let view = LycheePlayerView(frame: .zero)
    view.player = model.player
    model.view = view
    return view
  }

  func updateUIView(_ view: LycheePlayerView, context: Context) {}
}
#endif
