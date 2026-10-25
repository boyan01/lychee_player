// Plays LYCHEE_URL (default /tmp/lychee-media/h264_aac.mp4, which the iOS
// simulator sees too) on a loop, with a stats line under the video.

import Combine
import LycheePlayer
import SwiftUI

@main
struct ExampleApp: App {
  var body: some Scene {
    WindowGroup {
      PlayerScreen()
    }
  }
}

struct PlayerScreen: View {
  @StateObject private var model = PlayerModel()

  var body: some View {
    VStack(spacing: 0) {
      VideoView(player: model.player)
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
  @Published var status = ""
  private var timer: AnyCancellable?

  init() {
    let url = ProcessInfo.processInfo.environment["LYCHEE_URL"] ?? "/tmp/lychee-media/h264_aac.mp4"
    player.isLooping = true
    player.open(url)
    player.play()
    timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect().sink { [weak self] _ in
      self?.update()
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
}

#if os(macOS)
struct VideoView: NSViewRepresentable {
  let player: LycheePlayer

  func makeNSView(context: Context) -> LycheePlayerView {
    let view = LycheePlayerView(frame: .zero)
    view.player = player
    return view
  }

  func updateNSView(_ view: LycheePlayerView, context: Context) {}
}
#else
struct VideoView: UIViewRepresentable {
  let player: LycheePlayer

  func makeUIView(context: Context) -> LycheePlayerView {
    let view = LycheePlayerView(frame: .zero)
    view.player = player
    return view
  }

  func updateUIView(_ view: LycheePlayerView, context: Context) {}
}
#endif
