// swift-tools-version:5.9
//
// LycheePlayer (sdk/apple) for apps that add this repository by URL:
//
//   .package(url: "https://github.com/boyan01/lychee_player", from: "1.0.0")
//
// The same package as sdk/apple/Package.swift, which builds against the
// xcframeworks in flutter/darwin/Frameworks; this one takes them from the
// release below. tool/release.sh writes `release` when it tags a version;
// until the first release, the local frameworks.

import PackageDescription

let release = (
  url: "https://github.com/boyan01/lychee_player/releases/download/v1.0.0-beta.1",
  checksums: [
    "lychee": "1783d66b0c8c05783cd9ed0fdbadb5e61e50499c42f446fa35bd6d6591454f94",
    "libavformat": "14517224e269b3808aa1c58f054e570267a3608d5d828974d7c1218799a4028e",
    "libavcodec": "3282349880200880a5d5f7e53cef81b518053718700c93956e72db86dfe381ec",
    "libswresample": "0a2b5933c8e88697922c3d358938ced123a99453d85790dc8f73671fd75652b6",
    "libswscale": "1dfd394d2f02c79cde524925eb2153619ca7766aff7e7d80e03a1aac9240e9a3",
    "libavutil": "be70ec8636187086fff68553c626653c1538963b05c03b6b79f17841c5b67335",
  ]
)

let frameworks = ["lychee", "libavformat", "libavcodec", "libswresample", "libswscale", "libavutil"]

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
      dependencies: frameworks.map { .target(name: $0) },
      path: "sdk/apple/Sources/LycheePlayer"
    ),
  ] + frameworks.map { name in
    release.url.isEmpty
      ? .binaryTarget(name: name, path: "flutter/darwin/Frameworks/\(name).xcframework")
      : .binaryTarget(
        name: name, url: "\(release.url)/\(name).xcframework.zip", checksum: release.checksums[name]!)
  }
)
