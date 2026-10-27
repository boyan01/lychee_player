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
// Its NoFFmpeg trait takes the core without FFmpeg and libass, as in
// sdk/apple/Package.swift, from releases that have it:
//
//   .package(url: "https://github.com/boyan01/lychee_player", from: "1.0.0",
//            traits: ["NoFFmpeg"])

import Foundation
import PackageDescription

let release = (
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.0-beta.2",
  checksums: [
    "lychee": "97ed155260ce2d8d9d069f27d63dbe0953558b22863876552431c38ff3b74a93",
    "libavformat": "6c045f97c8156f52efcdcd9f249cbc25638d6f56a37aecebd0605a29f0ae7ec1",
    "libavcodec": "3a514d40ff18eba6c0adfed7e01b361336c77825400cb6906d6c157a78817113",
    "libswresample": "f71b8fae68ce95d11c288a1159b8eba8426620f9106c069ffd6c25c7b159b82f",
    "libswscale": "0ea53a7e085f391ecfeb15d5361737fa06e1c9f530bbc2b242c32cce21eda0ac",
    "libavutil": "00efd43ffda033fc6952eaa6812512013935630a8ba600638ca49c6167d667d8",
    "libass": "a90e081fde253984314a3d7abdd694a31626a716e84b34be7d8066005aeb33df",
    "libfribidi": "19de91cfe92246e8fc6786b09f54465a422b116b1f31c08a998b20e2929f5c66",
    "lychee_noffmpeg": "928a1f3bec122647ffac74ace49bc54b494212e9b373f71842829a5194ced4a2",
  ]
)

// A release's frameworks are the ones it pins (1.0.0-beta.1 had neither
// libass nor lychee_noffmpeg).
let frameworks = release.url.isEmpty
  ? ["lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil", "libass", "libfribidi"]
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
    .trait(name: "FFmpeg", description: "The core with FFmpeg and libass (LGPL)"),
    .trait(name: "NoFFmpeg", description: "The core without FFmpeg and libass"),
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
