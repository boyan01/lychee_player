#!/usr/bin/env bash
# Builds an LGPL-only FFmpeg as shared libraries for one platform.
#
#   tool/build_ffmpeg.sh PLATFORM FFMPEG_SOURCE_DIR
#
# PLATFORM: ios-arm64, ios-sim-arm64, ios-sim-x86_64, android-arm64-v8a,
# android-x86_64.
# Output: third_party/ffmpeg/PLATFORM/{include,lib}. For Apple platforms,
# tool/package_ffmpeg_apple.sh then wraps the libraries into xcframeworks.
#
# License: no --enable-gpl, no --enable-nonfree, and nothing that would need
# them (x264, x265, fdk-aac, ...). configure's own license summary is
# checked below.
set -euo pipefail

platform="$1"
src="$(cd "$2" && pwd)"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/third_party/ffmpeg/$platform"
work="$root/build-ffmpeg/$platform"
jobs="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

# Demux, decode and resample only: what core/src/backends/ffmpeg uses.
common=(
  --prefix="$out"
  --enable-shared --disable-static --enable-pic
  --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-avfilter
  --disable-encoders --disable-muxers --disable-bsfs --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
  --disable-devices --disable-indevs --disable-outdevs
  --enable-cross-compile
)

case "$platform" in
  ios-arm64|ios-sim-arm64|ios-sim-x86_64)
    case "$platform" in
      ios-arm64) sdk=iphoneos; arch=arm64; min=-miphoneos-version-min=13.0 ;;
      ios-sim-arm64) sdk=iphonesimulator; arch=arm64; min=-mios-simulator-version-min=13.0 ;;
      ios-sim-x86_64) sdk=iphonesimulator; arch=x86_64; min=-mios-simulator-version-min=13.0 ;;
    esac
    sysroot="$(xcrun -sdk $sdk --show-sdk-path)"
    flags=(
      --target-os=darwin --arch=$arch
      --cc="$(xcrun -sdk $sdk -f clang)" --sysroot="$sysroot"
      --extra-cflags="-arch $arch $min"
      --extra-ldflags="-arch $arch $min"
      --enable-videotoolbox --enable-audiotoolbox --enable-securetransport
    )
    # x86_64 assembly needs nasm; the Intel simulator does not need speed.
    [ "$arch" = x86_64 ] && flags+=(--disable-asm)
    ;;
  android-arm64-v8a|android-x86_64)
    ndk="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}"
    toolchain="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*)"
    api=24
    if [ "$platform" = android-arm64-v8a ]; then
      triple=aarch64-linux-android; arch=aarch64
    else
      triple=x86_64-linux-android; arch=x86_64
    fi
    flags=(
      --target-os=android --arch=$arch
      --cc="$toolchain/bin/$triple$api-clang"
      --cxx="$toolchain/bin/$triple$api-clang++"
      --ar="$toolchain/bin/llvm-ar" --nm="$toolchain/bin/llvm-nm"
      --ranlib="$toolchain/bin/llvm-ranlib" --strip="$toolchain/bin/llvm-strip"
      --sysroot="$toolchain/sysroot"
      # Android 15+ devices may use 16 KB pages.
      --extra-ldflags="-Wl,-z,max-page-size=16384"
      --enable-jni --enable-mediacodec
    )
    # x86_64 assembly needs nasm; not worth it for the emulator.
    [ "$platform" = android-x86_64 ] && flags+=(--disable-asm)
    ;;
  *)
    echo "unknown platform $platform" >&2
    exit 1
    ;;
esac

rm -rf "$work" "$out"
mkdir -p "$work"
cd "$work"
"$src/configure" "${common[@]}" "${flags[@]}" >configure.log 2>&1 || {
  tail -20 configure.log
  exit 1
}
grep -E '^License: ' configure.log
grep -qE '^License: LGPL version 2.1 or later' configure.log || {
  echo "not an LGPL build" >&2
  exit 1
}
make -j"$jobs" >make.log 2>&1 || { tail -30 make.log; exit 1; }
make install >/dev/null
echo "installed to $out"
