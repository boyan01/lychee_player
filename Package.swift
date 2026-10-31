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
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.0-beta.5",
  checksums: [
    "lychee": "b8a7c25f8159e391491f9c6fc0aed76b8f5906a24261fbe877746528051bdf11",
    "libavformat": "01e01db301be907cd9719122e6d0bab562b5ad4c0b5559b0dee328c9e6f15da7",
    "libavcodec": "04c1aeef752a2ecd2b5f50d91d03d1c6b1cf132971e1a9db9b35833746d27fc6",
    "libswresample": "aed86a344b5cb4ca46692e70452588e045047aecd867f9335a4dce9ffe8c93a7",
    "libswscale": "fc2ae1f106ef542d33f12cce7575bc5e056217d29784e9b0fa89c972be03ae49",
    "libavutil": "dc1a4538ed37e09422253adf9687d0d24a2e10b0c6d86a07631426717086f1fb",
    "lychee_noffmpeg": "223ab6065f3abafdd7678c97d964ce0adfc68f0607ab2e153b8ef79406c23128",
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
