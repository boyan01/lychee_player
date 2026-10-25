#!/usr/bin/env bash
# Wraps the FFmpeg libraries from tool/build_ffmpeg.sh into one xcframework
# per library. The headers also go to Frameworks/include: CocoaPods copies
# xcframework slices only after the pod's sources compile.
#
#   tool/package_ffmpeg_apple.sh
#
# Packages whichever of ios-arm64, ios-sim-arm64 and ios-sim-x86_64 were
# built; the simulator ones become one universal slice. Apps build the
# simulator for both architectures, and CocoaPods copies nothing from a
# slice that lacks one of them.
#
# Output: flutter/darwin/Frameworks/LIB.xcframework, which the podspec
# vendors. Dynamic frameworks keep FFmpeg replaceable, as the LGPL requires.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
builds="$root/third_party/ffmpeg"
dest="$root/flutter/darwin/Frameworks"
staging="$root/build-ffmpeg/frameworks"
libs=(libavutil libswresample libswscale libavcodec libavformat)

slices=()
[ -d "$builds/ios-arm64" ] && slices+=(ios)
{ [ -d "$builds/ios-sim-arm64" ] || [ -d "$builds/ios-sim-x86_64" ]; } && slices+=(ios-simulator)
[ ${#slices[@]} -gt 0 ] || { echo "no iOS FFmpeg build in ${builds#$root/}" >&2; exit 1; }

rm -rf "$staging"
for slice in "${slices[@]}"; do
  if [ "$slice" = ios ]; then
    platforms=(ios-arm64); supported=iPhoneOS
  else
    platforms=(); supported=iPhoneSimulator
    for p in ios-sim-arm64 ios-sim-x86_64; do
      [ -d "$builds/$p" ] && platforms+=("$p")
    done
  fi
  prefix="$builds/${platforms[0]}"
  version="$(sed -nE 's/.*FFMPEG_VERSION "n?([0-9.]+).*/\1/p' "$prefix/include/libavutil/ffversion.h")"
  for lib in "${libs[@]}"; do
    framework="$staging/$slice/$lib.framework"
    mkdir -p "$framework/Headers"
    # libavcodec.dylib is a symlink to libavcodec.63.dylib. One binary per
    # architecture, merged.
    binaries=()
    for p in "${platforms[@]}"; do
      binaries+=("$(realpath "$builds/$p/lib/$lib.dylib")")
    done
    lipo -create "${binaries[@]}" -output "$framework/$lib"
    cp -R "$prefix/include/$lib/" "$framework/Headers/"
    install_name_tool -id "@rpath/$lib.framework/$lib" "$framework/$lib"
    # Point the references between the libraries at the frameworks.
    otool -L "$framework/$lib" | awk 'NR > 1 { print $1 }' | sort -u | while read -r dep; do
      for other in "${libs[@]}"; do
        if [[ "$(basename "$dep")" == "$other".* ]]; then
          install_name_tool -change "$dep" "@rpath/$other.framework/$other" "$framework/$lib"
        fi
      done
    done
    cat >"$framework/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$lib</string>
  <key>CFBundleIdentifier</key><string>org.ffmpeg.$lib</string>
  <key>CFBundleName</key><string>$lib</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$supported</string></array>
  <key>MinimumOSVersion</key><string>13.0</string>
</dict>
</plist>
EOF
    codesign --force --sign - "$framework/$lib" 2>/dev/null
  done
done

mkdir -p "$dest"
for lib in "${libs[@]}"; do
  args=()
  for slice in "${slices[@]}"; do
    args+=(-framework "$staging/$slice/$lib.framework")
  done
  rm -rf "$dest/$lib.xcframework"
  xcodebuild -create-xcframework "${args[@]}" -output "$dest/$lib.xcframework" >/dev/null
done
rm -rf "$dest/include"
mkdir -p "$dest/include"
for lib in "${libs[@]}"; do
  cp -R "$prefix/include/$lib" "$dest/include/"
done
echo "wrote ${libs[*]/%/.xcframework} and include/ to ${dest#$root/}"
