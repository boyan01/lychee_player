#!/usr/bin/env bash
# Builds the core as lychee.xcframework: one dynamic framework per slice,
# linked against the FFmpeg xcframeworks, carrying only lychee/lychee.h.
#
#   tool/build_apple.sh
#
# Needs the FFmpeg builds from tool/build_ffmpeg.sh (for their headers) and
# the xcframeworks from tool/package_ffmpeg_apple.sh (what it links), and
# builds the slices those have: macOS (arm64 + x86_64), iOS (arm64) and the
# iOS simulator (arm64 + x86_64 in one slice, see package_ffmpeg_apple.sh).
#
# Output: flutter/darwin/Frameworks/lychee.xcframework, which the podspec
# vendors next to FFmpeg.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
. "$root/tool/apple_framework.sh"
frameworks="$root/flutter/darwin/Frameworks"
work="$root/build-apple"
ffmpeg_libs=(avformat avcodec swresample swscale avutil)
version="$(sed -nE 's/^version: ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/flutter/pubspec.yaml")"

rm -rf "$work"
mkdir -p "$work"
# So Swift (sdk/apple) can import lychee.
cat >"$work/module.modulemap" <<EOF
framework module lychee {
  umbrella header "lychee.h"
  export *
}
EOF
args=()
for slice in macos ios ios-simulator; do
  case "$slice" in
    macos)
      dir=macos-arm64_x86_64; headers=macos-arm64
      cmake_args=(-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=$apple_min_macos) ;;
    ios)
      dir=ios-arm64; headers=ios-arm64
      cmake_args=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$apple_min_ios) ;;
    ios-simulator)
      dir=ios-arm64_x86_64-simulator; headers=ios-sim-arm64
      cmake_args=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=$apple_min_ios) ;;
  esac
  [ -d "$frameworks/libavcodec.xcframework/$dir" ] || continue

  # An FFmpeg root as core/CMakeLists.txt expects it (include/, lib/), whose
  # libraries are the framework binaries, so the core records their
  # @rpath/LIB.framework/LIB install names.
  ffmpeg="$work/$slice/ffmpeg"
  mkdir -p "$ffmpeg/lib"
  ln -s "$root/third_party/ffmpeg/$headers/include" "$ffmpeg/include"
  for lib in "${ffmpeg_libs[@]}"; do
    ln -s "$frameworks/lib$lib.xcframework/$dir/lib$lib.framework/lib$lib" "$ffmpeg/lib/lib$lib.dylib"
  done

  build="$work/$slice/build"
  cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DLYCHEE_BUILD_EXAMPLES=OFF -DLYCHEE_BUILD_TESTS=OFF \
    -DLYCHEE_FFMPEG_ROOT="$ffmpeg" -DCMAKE_SKIP_BUILD_RPATH=ON \
    "${cmake_args[@]}" >"$work/$slice.log"
  cmake --build "$build" >>"$work/$slice.log" || { tail -30 "$work/$slice.log"; exit 1; }

  binary="$work/$slice/lychee"
  cp "$build/core/liblychee.dylib" "$binary"
  install_name_tool -id @rpath/lychee.framework/lychee "$binary" 2>/dev/null
  mkdir -p "$work/$slice/headers"
  cp "$root/core/include/lychee/lychee.h" "$work/$slice/headers/"
  make_framework "$slice" "$work/$slice/lychee.framework" "$binary" \
    "$work/$slice/headers" tech.soit.lychee "$version" "$work/module.modulemap"
  args+=(-framework "$work/$slice/lychee.framework")
done
[ ${#args[@]} -gt 0 ] || { echo "no FFmpeg xcframeworks; run tool/package_ffmpeg_apple.sh" >&2; exit 1; }

rm -rf "$frameworks/lychee.xcframework"
xcodebuild -create-xcframework "${args[@]}" -output "$frameworks/lychee.xcframework" >/dev/null
echo "wrote ${frameworks#$root/}/lychee.xcframework"
