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
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.3",
  checksums: [
    "lychee": "1e74251b93ec7eabf6ccc9941f0dedbe1a699ffafe7888a26c5a9b44bcb6da3d",
    "libavformat": "1bbf304f2935568741aef27061e0de434da2515acb338db0fa8ef4a4846a3d6f",
    "libavcodec": "f04b4dd171c81eda9465ae36bf5751fc7ac67d74af6ce70e5e4f4b1d1a52a60b",
    "libswresample": "d0a0c4c444e57b21e2e62f4378dc0a964b19e3728ea02cfb9682977e25758b2e",
    "libswscale": "3dbd32b76a33158348314f4aaf2f5d6ddcf08faa9edec1eabeafba0f89b593a8",
    "libavutil": "18550899ed64ec5d5d62437272d5c56ae0875770040b3fe3ad10c274fb6abda4",
    "lychee_noffmpeg": "a091bd228e9b1574e16260ad950e160b82229b2425086f500d3f4abe02ff3c31",
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
