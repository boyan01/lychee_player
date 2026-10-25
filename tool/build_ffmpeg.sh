#!/usr/bin/env bash
# Builds an LGPL-only FFmpeg as shared libraries for one platform.
#
#   tool/build_ffmpeg.sh PLATFORM FFMPEG_SOURCE_DIR [MBEDTLS_SOURCE_DIR]
#
# PLATFORM: macos-arm64, macos-x86_64, ios-arm64, ios-sim-arm64,
# ios-sim-x86_64, android-arm64-v8a, android-x86_64, linux-x86_64.
# Output: third_party/ffmpeg/PLATFORM/{include,lib}. For Apple platforms,
# tool/package_ffmpeg_apple.sh then wraps the libraries into xcframeworks.
#
# Android needs MBEDTLS_SOURCE_DIR (an Mbed TLS 3.6 release) for https:
# Apple platforms use the system's TLS, Android's has no C API. mbedTLS is
# Apache-2.0, which LGPL 2.1 cannot take, so Android builds are LGPL 3.
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
license="LGPL version 2.1 or later"

# Demux, decode and resample only what core/src/backends/ffmpeg plays: the
# containers, codecs and protocols below, nothing else. To play a new
# format, add it here and a file for it to tool/regression.sh, and check it
# with FFMPEG_ROOT=third_party/ffmpeg/macos-arm64 tool/regression.sh.
common=(
  --prefix="$out"
  --enable-shared --disable-static --enable-pic
  --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-avfilter
  --disable-everything
  --enable-demuxer=mov,matroska,mpegts,hls,flv,ogg,mp3,aac,ac3,eac3,flac,wav,srt,webvtt,ass
  --enable-protocol=file,data,http,https,httpproxy,tcp,tls,crypto
  # Hardware decoders come first; these are the fallback.
  --enable-decoder=h264,hevc,vp8,vp9,av1
  --enable-decoder=aac,aac_latm,mp3float,opus,vorbis,flac,alac,ac3,eac3,'pcm_*'
  --enable-decoder=subrip,srt,webvtt,movtext,ass,ssa,text,pgssub,dvbsub,dvdsub
  --enable-parser=h264,hevc,vp8,vp9,av1,aac,aac_latm,mpegaudio,opus,vorbis,flac,ac3
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
  --enable-cross-compile
)

case "$platform" in
  macos-arm64|macos-x86_64|ios-arm64|ios-sim-arm64|ios-sim-x86_64)
    . "$root/tool/apple_framework.sh"
    case "$platform" in
      macos-arm64) sdk=macosx; arch=arm64; min=-mmacosx-version-min=$apple_min_macos ;;
      macos-x86_64) sdk=macosx; arch=x86_64; min=-mmacosx-version-min=$apple_min_macos ;;
      ios-arm64) sdk=iphoneos; arch=arm64; min=-miphoneos-version-min=$apple_min_ios ;;
      ios-sim-arm64) sdk=iphonesimulator; arch=arm64; min=-mios-simulator-version-min=$apple_min_ios ;;
      ios-sim-x86_64) sdk=iphonesimulator; arch=x86_64; min=-mios-simulator-version-min=$apple_min_ios ;;
    esac
    sysroot="$(xcrun -sdk $sdk --show-sdk-path)"
    flags=(
      --target-os=darwin --arch=$arch
      --cc="$(xcrun -sdk $sdk -f clang)" --sysroot="$sysroot"
      --extra-cflags="-arch $arch $min"
      --extra-ldflags="-arch $arch $min"
      --enable-videotoolbox --enable-securetransport
      --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,vp9_videotoolbox,av1_videotoolbox
      # Install names @rpath/libavcodec.63.dylib, not this build's prefix,
      # so whatever links the libraries can ship them next to itself.
      --install-name-dir=@rpath
      # Only what the SDK has. On macOS, Homebrew's pkg-config and
      # sdl2-config would add libxcb, Xlib and SDL2 from /opt/homebrew.
      --pkg-config=false --disable-sdl2
    )
    # x86_64 assembly needs nasm. The Intel simulator does not need speed;
    # Intel Macs do when they decode in software.
    case "$platform" in
      ios-sim-x86_64) flags+=(--disable-asm) ;;
      macos-x86_64)
        command -v nasm >/dev/null ||
          { echo "nasm not found: building without x86 assembly" >&2; flags+=(--disable-asm); } ;;
    esac
    ;;
  android-arm64-v8a|android-x86_64)
    mbedtls="$(cd "${3:?Android needs MBEDTLS_SOURCE_DIR}" && pwd)"
    ndk="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}"
    toolchain="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*)"
    api=24
    if [ "$platform" = android-arm64-v8a ]; then
      triple=aarch64-linux-android; arch=aarch64; abi=arm64-v8a
    else
      triple=x86_64-linux-android; arch=x86_64; abi=x86_64
    fi
    tls="$work/mbedtls"
    flags=(
      --target-os=android --arch=$arch
      --cc="$toolchain/bin/$triple$api-clang"
      --cxx="$toolchain/bin/$triple$api-clang++"
      --ar="$toolchain/bin/llvm-ar" --nm="$toolchain/bin/llvm-nm"
      --ranlib="$toolchain/bin/llvm-ranlib" --strip="$toolchain/bin/llvm-strip"
      --sysroot="$toolchain/sysroot"
      --extra-cflags="-I$tls/include"
      # Android 15+ devices may use 16 KB pages.
      --extra-ldflags="-Wl,-z,max-page-size=16384 -L$tls/lib"
      # Linked statically into libavformat. The core hands it the system's
      # CA certificates (ffmpeg_demuxer.cc).
      --enable-mbedtls --enable-version3
      # Homebrew's pkg-config would find the Mac's libraries.
      --pkg-config=false
    )
    license="LGPL version 3 or later"
    # x86_64 assembly needs nasm; not worth it for the emulator.
    [ "$platform" = android-x86_64 ] && flags+=(--disable-asm)
    ;;
  linux-x86_64)
    # Built on the oldest distribution it should run on (Ubuntu 22.04):
    # the libraries need its glibc or newer. Links the system's libva,
    # libva-drm, libdrm and OpenSSL 3 dynamically; apps depend on those
    # packages. OpenSSL 3 is Apache-2.0, so the build is LGPL 3, as BtbN's.
    command -v nasm >/dev/null || { echo "nasm not found" >&2; exit 1; }
    flags=(
      --target-os=linux --arch=x86_64
      --enable-vaapi --enable-libdrm
      --enable-hwaccel=h264_vaapi,hevc_vaapi,vp8_vaapi,vp9_vaapi,av1_vaapi
      --enable-openssl --enable-version3
      # VAAPI on a DRM render node only, so no libva-x11 or Xlib.
      --disable-xlib --disable-libxcb --disable-sdl2
    )
    license="LGPL version 3 or later"
    ;;
  *)
    echo "unknown platform $platform" >&2
    exit 1
    ;;
esac

rm -rf "$work" "$out"
mkdir -p "$work"
cd "$work"
if [ -n "${tls:-}" ]; then
  cmake -S "$mbedtls" -B "$work/mbedtls-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$abi -DANDROID_PLATFORM=$api \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$tls" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF >mbedtls.log 2>&1 &&
    cmake --build "$work/mbedtls-build" --target install >>mbedtls.log 2>&1 ||
    { tail -20 mbedtls.log; exit 1; }
fi
"$src/configure" "${common[@]}" "${flags[@]}" >configure.log 2>&1 || {
  tail -20 configure.log
  exit 1
}
grep -E '^License: ' configure.log
grep -qE "^License: $license" configure.log || {
  echo "not an $license build" >&2
  exit 1
}
make -j"$jobs" >make.log 2>&1 || { tail -30 make.log; exit 1; }
make install >/dev/null
# Apple builds may link only the system and each other, through @rpath.
case "$platform" in
  macos-*|ios-*)
    foreign="$(otool -L "$out"/lib/*.dylib | awk '/^\t/ { print $1 }' | sort -u |
      grep -vE "^(/System/|/usr/lib/|@rpath/lib(avutil|swresample|swscale|avcodec|avformat)\.[0-9]+\.dylib$)" || true)"
    [ -z "$foreign" ] || { printf 'links outside the SDK:\n%s\n' "$foreign" >&2; exit 1; }
    ;;
  linux-*)
    foreign="$(for lib in "$out"/lib/*.so.*[0-9]; do readelf -d "$lib"; done |
      sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u |
      grep -vE '^(libc|libm|libz|libva|libva-drm|libdrm|libssl|libcrypto|libav(util|codec|format)|libsw(resample|scale))\.so\.[0-9]+$' || true)"
    [ -z "$foreign" ] || { printf 'unexpected dependencies:\n%s\n' "$foreign" >&2; exit 1; }
    ;;
esac
echo "installed to $out"
