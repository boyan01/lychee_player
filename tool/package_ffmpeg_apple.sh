#!/usr/bin/env bash
# Wraps the FFmpeg libraries from tool/build_ffmpeg.sh into one xcframework
# per library.
#
#   tool/package_ffmpeg_apple.sh
#
# Packages whichever of macos-arm64, macos-x86_64, ios-arm64, ios-sim-arm64
# and ios-sim-x86_64 were built. The two macOS ones become one universal
# slice, and so do the two simulator ones: apps build the simulator for both
# architectures, and CocoaPods copies nothing from a slice that lacks one of
# them.
#
# Output: flutter/darwin/Frameworks/LIB.xcframework, which the podspec
# vendors and tool/build_apple.sh links the core against. Dynamic frameworks
# keep FFmpeg replaceable, as the LGPL requires.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
. "$root/tool/apple_framework.sh"
dest="$root/flutter/darwin/Frameworks"
staging="$root/build-ffmpeg/frameworks"
ffmpeg_libs=(libavutil libswresample libswscale libavcodec libavformat)

# The builds in BUILDS that go into a slice, the ones that exist.
slice_platforms() {
  local builds="$1" p
  case "$2" in
    macos) set -- macos-arm64 macos-x86_64 ;;
    ios) set -- ios-arm64 ;;
    ios-simulator) set -- ios-sim-arm64 ios-sim-x86_64 ;;
  esac
  for p in "$@"; do
    [ -d "$builds/$p" ] && echo "$p"
  done
  return 0
}

#   package BUILDS LIB...
#
# One xcframework per LIB from the builds in BUILDS (include/, lib/), with
# the references between the LIBs pointed at their frameworks.
package() {
  local builds="$1" slices=() slice platforms prefix p lib binaries binary dep other
  local headers id version
  shift
  for slice in macos ios ios-simulator; do
    [ -n "$(slice_platforms "$builds" $slice)" ] && slices+=("$slice")
  done
  [ ${#slices[@]} -gt 0 ] || { echo "no Apple build in ${builds#$root/}" >&2; exit 1; }

  for slice in "${slices[@]}"; do
    platforms=($(slice_platforms "$builds" "$slice"))
    prefix="$builds/${platforms[0]}"
    mkdir -p "$staging/$slice/bin"
    for lib in "$@"; do
      # libavcodec.dylib is a symlink to libavcodec.63.dylib. One binary per
      # architecture, merged.
      binaries=()
      for p in "${platforms[@]}"; do
        binaries+=("$(realpath "$builds/$p/lib/$lib.dylib")")
      done
      binary="$staging/$slice/bin/$lib"
      lipo -create "${binaries[@]}" -output "$binary"
      install_name_tool -id "@rpath/$lib.framework/$lib" "$binary" 2>/dev/null
      otool -L "$binary" | awk 'NR > 1 { print $1 }' | sort -u | while read -r dep; do
        for other in "$@"; do
          if [[ "$(basename "$dep")" == "$other".* ]]; then
            install_name_tool -change "$dep" "@rpath/$other.framework/$other" "$binary" 2>/dev/null
          fi
        done
      done
      headers="$prefix/include/$lib"; id="org.ffmpeg.$lib"
      version="$(sed -nE 's/.*FFMPEG_VERSION "n?([0-9.]+).*/\1/p' "$prefix/include/libavutil/ffversion.h")"
      make_framework "$slice" "$staging/$slice/$lib.framework" "$binary" "$headers" "$id" "$version"
    done
  done

  for lib in "$@"; do
    args=()
    for slice in "${slices[@]}"; do
      args+=(-framework "$staging/$slice/$lib.framework")
    done
    rm -rf "$dest/$lib.xcframework"
    xcodebuild -create-xcframework "${args[@]}" -output "$dest/$lib.xcframework" >/dev/null
  done
  echo "wrote ${*/%/.xcframework} (${slices[*]}) to ${dest#$root/}"
}

rm -rf "$staging"
# Built here: the pod no longer replaces Frameworks/ with a release.
mkdir -p "$dest"
rm -f "$dest/.prebuilt"
package "$root/third_party/ffmpeg" "${ffmpeg_libs[@]}"
