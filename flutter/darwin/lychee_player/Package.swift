// swift-tools-version:6.1
//
// The plugin for Flutter's Swift Package Manager integration
// (`flutter config --enable-swift-package-manager`); lychee_player.podspec
// builds the same sources with CocoaPods.
//
// The core and FFmpeg come as the xcframeworks the pod vendors, in
// one of three ways:
//
// - A checkout with all of them built in flutter/darwin/Frameworks (no
//   `.prebuilt` stamp, the pod's mark of a download): sdk/apple, by its
//   absolute path, brings them, and the plugin's texture uses its
//   LycheeFrameDriver. Flutter links this package through a symlink in the
//   app, and SwiftPM resolves relative paths against the symlink and
//   refuses targets and binaries outside the package, hence the absolute
//   path from the symlink's target.
// - The pub package: tool/release.sh copies sdk/apple's sources into
//   Sources/LycheePlayer while pub packs, and the frameworks are the
//   release's that ../../prebuilt.json pins, or the same unzipped into
//   Frameworks/ here (tool/release.sh check: SwiftPM downloads only over
//   https, so not from its local server). Not a dependency on the root
//   Package.swift by URL: that clones the whole repository, and resolves
//   only once the release exists.
// - A checkout without local builds: the pinned frameworks, which may be
//   older than sdk/apple's sources, so the texture keeps its own copy of
//   the driver, as with CocoaPods.
//
// LYCHEE_PREBUILT_URL replaces the release URL with a mirror's, as for the
// pod, if it is https and the environment reaches the manifest (`flutter
// build` and xcodebuild from a shell; not Xcode started from the Dock).

import Foundation
import PackageDescription

// Select the backend in the ordinary Flutter/Xcode build process environment.
let backend = ProcessInfo.processInfo.environment["LYCHEE_WITH_FFMPEG", default: "OFF"].uppercased()
precondition(["ON", "OFF"].contains(backend), "LYCHEE_WITH_FFMPEG must be ON or OFF")
let withFFmpeg = backend == "ON"
let frameworks = withFFmpeg
  ? ["lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil"]
  : ["lychee_noffmpeg"]

let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent().resolvingSymlinksInPath()
func exists(_ path: String) -> Bool {
  FileManager.default.fileExists(atPath: here.appendingPathComponent(path).path)
}

let sdk = here.appendingPathComponent("../../../sdk/apple").standardized.path
let localBuild = FileManager.default.fileExists(atPath: "\(sdk)/Package.swift")
  && !exists("../Frameworks/.prebuilt")
  && frameworks.allSatisfy { exists("../Frameworks/\($0).xcframework") }
let bundledSDK = exists("Sources/LycheePlayer")

// The release's xcframeworks, as ../../prebuilt.json pins them: unzipped in
// Frameworks/ if they are there, else downloaded.
func pinnedFrameworks() -> [Target] {
  struct Manifest: Decodable {
    let url: String
    let tag: String
    let files: [String: String]
  }
  let data = try! Data(contentsOf: here.appendingPathComponent("../../prebuilt.json"))
  let manifest = try! JSONDecoder().decode(Manifest.self, from: data)
  let mirror = ProcessInfo.processInfo.environment["LYCHEE_PREBUILT_URL"] ?? ""
  let url = mirror.isEmpty ? manifest.url : mirror
  let names = frameworks.map { "\($0).xcframework" }
  precondition(
    names.allSatisfy { manifest.files["\($0).zip"] != nil },
    "The pinned release has no requested Apple backend")
  if names.allSatisfy({ exists("Frameworks/\($0)") }) {
    return names.map { .binaryTarget(name: String($0.dropLast(".xcframework".count)), path: "Frameworks/\($0)") }
  }
  return names.map { name in
    .binaryTarget(
      name: String(name.dropLast(".xcframework".count)),
      url: "\(url)/\(manifest.tag)/\(name).zip",
      checksum: manifest.files["\(name).zip"]!)
  }
}

var dependencies: [Package.Dependency] = [
  .package(name: "FlutterFramework", path: "../FlutterFramework"),
]
var pluginDependencies: [Target.Dependency] = [
  .product(name: "FlutterFramework", package: "FlutterFramework"),
]
var targets: [Target] = []
var cSettings: [CSetting] = []

if localBuild {
  dependencies.append(.package(path: sdk, traits: [withFFmpeg ? "FFmpeg" : "NoFFmpeg"]))
  pluginDependencies.append(.product(name: "LycheePlayer", package: "apple"))
  cSettings.append(.define("LYCHEE_SDK_DRIVER", to: "1"))
} else {
  let binaries = pinnedFrameworks()
  targets += binaries
  if bundledSDK {
    targets.append(.target(
      name: "LycheePlayer",
      dependencies: binaries.map { .target(name: $0.name) },
      path: "Sources/LycheePlayer"))
    pluginDependencies.append(.target(name: "LycheePlayer"))
    cSettings.append(.define("LYCHEE_SDK_DRIVER", to: "1"))
  } else {
    pluginDependencies += binaries.map { .target(name: $0.name) }
  }
}

let package = Package(
  name: "lychee_player",
  // The same as in tool/apple_framework.sh.
  platforms: [.macOS("11.0"), .iOS("16.0")],
  products: [
    .library(name: "lychee-player", targets: ["lychee_player"]),
  ],
  dependencies: dependencies,
  targets: targets + [
    .target(
      name: "lychee_player",
      dependencies: pluginDependencies,
      cSettings: cSettings,
      linkerSettings: [
        .linkedFramework("CoreVideo"),
        .linkedFramework("QuartzCore"),
        .linkedFramework("AVFAudio", .when(platforms: [.iOS])),
      ]
    ),
  ],
  swiftLanguageModes: [.v5]
)
