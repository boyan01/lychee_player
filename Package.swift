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
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.0-beta.3",
  checksums: [
    "lychee": "25d2037a296c153c5dee49c4166ee08ff1580850adf5878cbd5dbc49d552c3a0",
    "libavformat": "839e34dde0d34d2d1d6d14a897a88f70715c3c00e41aba64fd664a552932ab91",
    "libavcodec": "b80cb68be9897f80fb742237101277bf2fdf44e8b2714d83e7016f1dd002e5e1",
    "libswresample": "cd58095876970bb8e9426e723ccd849ccaad80e32c3ff57b10d77db1c9546daa",
    "libswscale": "3591f50f3e4dfb8fafbe887cb500ae71de910d081bf2dc82107a6e2efa9a095a",
    "libavutil": "3c089de292f74916a875417a4c8665e2bfa46dde1ce7f8142bc5ace0cc788519",
    "lychee_noffmpeg": "cd0d0f31b201b6b2b41289205d959dec7ce0b9b8000410c26d23b1207881c31e",
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
