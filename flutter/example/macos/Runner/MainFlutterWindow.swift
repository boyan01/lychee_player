import Cocoa
import FlutterMacOS
import lychee_player

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    // lib/main.dart reads the command line, without the -NSKey value pairs
    // AppKit and Xcode add.
    let project = FlutterDartProject()
    var arguments: [String] = []
    var skipValue = false
    for argument in CommandLine.arguments.dropFirst() {
      if skipValue {
        skipValue = false
      } else if argument.hasPrefix("-NS") || argument.hasPrefix("-Apple") {
        skipValue = true
      } else {
        arguments.append(argument)
      }
    }
    project.dartEntrypointArguments = arguments
    let flutterViewController = FlutterViewController(project: project)
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    RegisterGeneratedPlugins(registry: flutterViewController)
    registerHostTextureCheck(flutterViewController.registrar(forPlugin: "HostTextureCheck"))

    super.awakeFromNib()
  }
}

// For integration_test: shows players this app creates itself in a
// LycheeHostTexture, as an app with its own live source does, switching
// and destroying them on a worker while the display link runs.
private func registerHostTextureCheck(_ registrar: FlutterPluginRegistrar) {
  let channel = FlutterMethodChannel(
    name: "lychee_player_example/host_texture", binaryMessenger: registrar.messenger)
  channel.setMethodCallHandler { call, result in
    guard call.method == "check", let path = call.arguments as? String else {
      return result(FlutterMethodNotImplemented)
    }
    let texture = LycheeHostTexture(registry: registrar.textures)
    DispatchQueue(label: "host-texture-check").async {
      let report = checkHostTexture(texture, path)
      DispatchQueue.main.async {
        texture.dispose()
        result(report)
      }
    }
  }
}

// The width of each buffer the texture hands to the engine at each step: 1
// is the blank one, 0 none.
private func checkHostTexture(_ texture: LycheeHostTexture, _ path: String) -> [String: Int] {
  // Pulls as the engine's raster thread does.
  func pull() -> Int {
    guard let buffer = (texture as? FlutterTexture)?.copyPixelBuffer()?.takeRetainedValue() else {
      return 0
    }
    return CVPixelBufferGetWidth(buffer)
  }
  func waitForFrame() -> Int {
    for _ in 0..<500 {
      let width = pull()
      if width > 1 { return width }
      usleep(10_000)
    }
    return 0
  }
  func play() -> OpaquePointer {
    var config = lychee_config()
    lychee_config_init(&config, UInt32(MemoryLayout<lychee_config>.size))
    config.video_frame_types = LycheeHostTexture.frameTypes
    let player = lychee_player_create(&config)!
    lychee_player_set_volume(player, 0)
    lychee_player_open(player, path)
    lychee_player_play(player)
    return player
  }

  var report = ["none": pull()]
  let first = play()
  texture.setPlayer(first)
  report["first"] = waitForFrame()
  let second = play()
  texture.setPlayer(second)
  lychee_player_destroy(first)
  report["second"] = waitForFrame()
  texture.setPlayer(nil)
  lychee_player_destroy(second)
  report["cleared"] = pull()
  report["clearedAgain"] = pull()
  return report
}
