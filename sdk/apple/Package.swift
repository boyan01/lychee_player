// swift-tools-version:5.9
//
// LycheePlayer for macOS and iOS: a Swift player over the C ABI
// (lychee/lychee.h), a display-link frame driver and a view.
//
// The core, FFmpeg and libass come prebuilt, as the xcframeworks the Flutter
// plugin vendors from flutter/darwin/Frameworks: lychee.xcframework
// (tool/build_apple.sh), the LGPL-only FFmpeg ones (tool/build_ffmpeg.sh)
// and libass with FriBidi (tool/build_libass.sh), packaged by
// tool/package_ffmpeg_apple.sh.

import PackageDescription

let frameworks = [
  "lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil", "libass", "libfribidi",
]

let package = Package(
  name: "LycheePlayer",
  // The same as in tool/apple_framework.sh.
  platforms: [.macOS(.v11), .iOS(.v16)],
  products: [
    .library(name: "LycheePlayer", targets: ["LycheePlayer"]),
  ],
  targets: [
    .target(
      name: "LycheePlayer",
      dependencies: frameworks.map { .target(name: $0) }
    ),
    .testTarget(name: "LycheePlayerTests", dependencies: ["LycheePlayer"]),
  ] + frameworks.map {
    .binaryTarget(name: $0, path: "../../flutter/darwin/Frameworks/\($0).xcframework")
  }
)
