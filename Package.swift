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
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.1",
  checksums: [
    "lychee": "01c8b7eaa2649ae6d6190ce446f92d4b0a7eb3602d1d34601461be7acb27040f",
    "libavformat": "928da2f969668ee58f895503426dd8d2992c062a271cac54dd7dfb5c19398e6c",
    "libavcodec": "b896db44d182d0552d9ec395b34fab998e602f1c098c9a80fb7cb2e236547d1e",
    "libswresample": "02153f8782c5464c4287e0e9c454baaa28d4a25ac5a243562dd2305c84eb9d2a",
    "libswscale": "a441fbb131375e7d91a55918474852d7e869fef8a0db33a807209ef69194a780",
    "libavutil": "d7216760acfa3d362976084009455d414b445c2de72e918b8c968bfb12b10294",
    "lychee_noffmpeg": "146d37f36894f877d98fce3c81274d306db5f51eeca23d0186406de1860161d1",
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
