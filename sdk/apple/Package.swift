// swift-tools-version:6.1
//
// LycheePlayer for macOS and iOS: a Swift player over the C ABI
// (lychee/lychee.h), a display-link frame driver and a view.
//
// The core, FFmpeg and libass come prebuilt, as the xcframeworks the Flutter
// plugin vendors from flutter/darwin/Frameworks: lychee.xcframework
// (tool/build_apple.sh), the LGPL-only FFmpeg ones (tool/build_ffmpeg.sh)
// and libass with FriBidi (tool/build_libass.sh), packaged by
// tool/package_ffmpeg_apple.sh.
//
// The NoFFmpeg trait takes the core without FFmpeg and libass instead
// (lychee_noffmpeg.xcframework, NO_FFMPEG=1 tool/build_apple.sh): MP4,
// Matroska and WebM, MPEG-TS and HLS through the core's own demuxers,
// VideoToolbox and AudioToolbox, ASS as plain text, and nothing under the
// LGPL. An app enables one of the two traits, e.g.
// `.package(path: "sdk/apple", traits: ["NoFFmpeg"])`.

import Foundation
import PackageDescription

let ffmpeg = [
  "lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil", "libass", "libfribidi",
]
// Only once built: SwiftPM refuses a binary target without its xcframework.
let noFFmpeg = FileManager.default.fileExists(
  atPath: URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../flutter/darwin/Frameworks/lychee_noffmpeg.xcframework").path)
  ? ["lychee_noffmpeg"] : []

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
      dependencies: ffmpeg.map { .target(name: $0, condition: .when(traits: ["FFmpeg"])) }
        + noFFmpeg.map { .target(name: $0, condition: .when(traits: ["NoFFmpeg"])) }
    ),
    .testTarget(name: "LycheePlayerTests", dependencies: ["LycheePlayer"]),
  ] + (ffmpeg + noFFmpeg).map {
    .binaryTarget(name: $0, path: "../../flutter/darwin/Frameworks/\($0).xcframework")
  },
  swiftLanguageModes: [.v5]
)
