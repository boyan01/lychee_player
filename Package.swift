// swift-tools-version:6.1
//
// LycheePlayer (sdk/apple) for apps that add this repository by URL:
//
//   .package(url: "https://github.com/boyan01/lychee_player", from: "1.0.0")
//
// The same package as sdk/apple/Package.swift, which builds against the
// xcframeworks in flutter/darwin/Frameworks; this one takes them from the
// release below. tool/release.sh writes `release` when it tags a version;
// until the first release, the local frameworks.
//
// Its NoFFmpeg trait takes the core without FFmpeg, as in
// sdk/apple/Package.swift, from releases that have it:
//
//   .package(url: "https://github.com/boyan01/lychee_player", from: "1.0.0",
//            traits: ["NoFFmpeg"])

import Foundation
import PackageDescription

let release = (
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.2",
  checksums: [
    "lychee": "7a73f5095a224378c2afd7371cc4b4cd2bce4123abb32ac9484f4bd6afc0b480",
    "libavformat": "5988a49e3f4a6ec062ce9ae9d49215cc4d07c366f351d0eb9ec010b71e138c2f",
    "libavcodec": "4a9c37a85fcf085e94936461f719cc8ae1806b3e4a01b1b0e8f8b8c9c251a26b",
    "libswresample": "f45b01636beafac144e3d575bc2bdd9708de0edf82a990451aa81676b56166d3",
    "libswscale": "aff4a1fe8ac5f0a1905eb074e578bd1427591152701a43e6e2aaba660572f9f2",
    "libavutil": "a13edaedeadfb9ee187718b472d9d149aa9e9ed834896e60f0cd3d1553c46c1d",
    "lychee_noffmpeg": "ffb5956670bfe48ef3389e34fe825e38e46a7a9bf527aa6a4367193034308e5b",
  ]
)

// A release's frameworks are the ones it pins (1.0.0-beta.1 had no
// lychee_noffmpeg).
let frameworks = release.url.isEmpty
  ? ["lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil"]
    + (FileManager.default.fileExists(
      atPath: URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .appendingPathComponent("flutter/darwin/Frameworks/lychee_noffmpeg.xcframework").path)
      ? ["lychee_noffmpeg"] : [])
  : release.checksums.keys.sorted()
let noFFmpeg = ["lychee_noffmpeg"].filter(frameworks.contains)

let package = Package(
  name: "LycheePlayer",
  // The same as in tool/apple_framework.sh.
  platforms: [.macOS(.v11), .iOS(.v16)],
  products: [
    .library(name: "LycheePlayer", targets: ["LycheePlayer"]),
  ],
  traits: [
    .trait(name: "FFmpeg", description: "The core with FFmpeg (LGPL)"),
    .trait(name: "NoFFmpeg", description: "The core without FFmpeg"),
    .default(enabledTraits: ["FFmpeg"]),
  ],
  targets: [
    .target(
      name: "LycheePlayer",
      dependencies: frameworks.map {
        .target(name: $0, condition: .when(traits: [noFFmpeg.contains($0) ? "NoFFmpeg" : "FFmpeg"]))
      },
      path: "sdk/apple/Sources/LycheePlayer"
    ),
  ] + frameworks.map { name in
    release.url.isEmpty
      ? .binaryTarget(name: name, path: "flutter/darwin/Frameworks/\(name).xcframework")
      : .binaryTarget(
        name: name, url: "\(release.url)/\(name).xcframework.zip", checksum: release.checksums[name]!)
  },
  swiftLanguageModes: [.v5]
)
