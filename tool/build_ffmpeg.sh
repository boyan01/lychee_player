#!/usr/bin/env bash
# Builds an LGPL-only FFmpeg as shared libraries for one platform.
#
#   tool/build_ffmpeg.sh PLATFORM FFMPEG_SOURCE_DIR
#
# PLATFORM: macos-arm64, macos-x86_64, ios-arm64, ios-sim-arm64,
# ios-sim-x86_64, android-arm64-v8a, android-armeabi-v7a, android-x86_64, linux-x86_64,
# linux-arm64 (each built on that architecture),
# windows-x86_64 (cross-compiled on Linux).
# Output: third_party/ffmpeg/PLATFORM/{include,lib}. For Apple platforms,
# tool/package_ffmpeg_apple.sh then wraps the libraries into xcframeworks.
#
# Android's FFmpeg provides cleartext HTTP only. The core's DataSource
# opens HTTPS through HttpURLConnection; AES HLS uses javax.crypto.
# Apple uses SecureTransport and Windows uses Schannel.
#
# DAV1D_SOURCE_DIR (a dav1d release; BSD-2, built with meson and ninja),
# in the environment, adds dav1d on every platform, linked statically
# into libavcodec: it decodes AV1 where the GPU does not (Apple before M3
# and A17 Pro, most Android devices). Releases have it
# (tool/release.sh); without it, AV1 plays only
# where the GPU decodes it.
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
    dav1d_cc="$(xcrun -sdk $sdk -f clang)"
    dav1d_ar="$(xcrun -sdk $sdk -f ar)"
    dav1d_flags="'-arch', '$arch', '$min', '-isysroot', '$sysroot'"
    dav1d_system=darwin
    dav1d_cpu=$arch
    [ "$arch" = arm64 ] && dav1d_cpu=aarch64
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
      ios-sim-x86_64) flags+=(--disable-asm); dav1d_asm=false ;;
      macos-x86_64)
        command -v nasm >/dev/null || {
          echo "nasm not found: building without x86 assembly" >&2
          flags+=(--disable-asm)
          dav1d_asm=false
        } ;;
    esac
    ;;
  android-arm64-v8a|android-armeabi-v7a|android-x86_64)
    ndk="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}"
    toolchain="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*)"
    api=24
    case "$platform" in
      android-arm64-v8a) triple=aarch64-linux-android; arch=aarch64; abi=arm64-v8a ;;
      android-armeabi-v7a) triple=armv7a-linux-androideabi; arch=arm; abi=armeabi-v7a ;;
      android-x86_64) triple=x86_64-linux-android; arch=x86_64; abi=x86_64 ;;
    esac
    dav1d_cc="$toolchain/bin/$triple$api-clang"
    dav1d_ar="$toolchain/bin/llvm-ar"
    dav1d_flags="'-Wl,-z,max-page-size=16384'"
    dav1d_system=android
    dav1d_cpu=$arch
    flags=(
      --target-os=android --arch=$arch
      --cc="$toolchain/bin/$triple$api-clang"
      --cxx="$toolchain/bin/$triple$api-clang++"
      --ar="$toolchain/bin/llvm-ar" --nm="$toolchain/bin/llvm-nm"
      --ranlib="$toolchain/bin/llvm-ranlib" --strip="$toolchain/bin/llvm-strip"
      --sysroot="$toolchain/sysroot"
      # Android 15+ devices may use 16 KB pages.
      --extra-ldflags="-Wl,-z,max-page-size=16384"
      # The system handles all TLS, keeping FFmpeg LGPL 2.1+.
      --disable-protocol=https,tls
      # Homebrew's pkg-config would find the Mac's libraries.
      --pkg-config=false
    )
    if [ "$abi" = armeabi-v7a ]; then
      # Android's 32-bit ARM ABI includes Thumb-2 and NEON, with softfp
      # argument passing even when floating point uses hardware.
      dav1d_flags="'-march=armv7-a', '-mfpu=neon', '-mfloat-abi=softfp', $dav1d_flags"
      flags+=(--cpu=armv7-a --enable-neon
        --extra-cflags="-march=armv7-a -mfpu=neon -mfloat-abi=softfp")
    fi
    # x86_64 assembly needs nasm; not worth it for the emulator.
    [ "$platform" = android-x86_64 ] && { flags+=(--disable-asm); dav1d_asm=false; }
    ;;
  linux-x86_64 | linux-arm64)
    # Built natively on the oldest distribution it should run on (Ubuntu
    # 22.04; GitHub's ubuntu-22.04 and ubuntu-22.04-arm runners):
    # the libraries need its glibc or newer. Nothing is autodetected, so a
    # machine with more -dev packages (a CI runner) builds the same thing.
    # GnuTLS for https: LGPL 2.1, so the build stays LGPL 2.1 (OpenSSL 3
    # is Apache-2.0, which would make it LGPL 3). Links the system's
    # libva, libva-drm and libdrm (VAAPI on a DRM render node, no X11),
    # GnuTLS and zlib dynamically, whose sonames have not changed since
    # Ubuntu 20.04 and Debian 11: apps depend on those packages (Debian
    # and Ubuntu: libva2, libva-drm2, libdrm2, libgnutls30, zlib1g).
    # Needs pkg-config, libva-dev, libdrm-dev, libgnutls28-dev, zlib1g-dev
    # and, on x86_64, nasm.
    arch=x86_64
    if [ "$platform" = linux-arm64 ]; then
      arch=aarch64
    else
      command -v nasm >/dev/null || { echo "nasm not found" >&2; exit 1; }
    fi
    [ "$(uname -m)" = "$arch" ] || { echo "$platform builds on $arch only" >&2; exit 1; }
    dav1d_cc="${CC:-cc}"
    dav1d_ar="${AR:-ar}"
    dav1d_flags=""
    dav1d_system=linux
    dav1d_cpu=$arch
    flags=(
      --target-os=linux --arch="$arch"
      --disable-autodetect
      --enable-pthreads --enable-zlib --enable-gnutls
      --enable-vaapi --enable-libdrm
      --enable-hwaccel=h264_vaapi,hevc_vaapi,vp8_vaapi,vp9_vaapi,av1_vaapi
    )
    ;;
  windows-x86_64)
    # Cross-compiled with mingw-w64 on Linux (Debian/Ubuntu: mingw-w64,
    # libz-mingw-w64-dev, nasm). Nothing is autodetected, so the build is
    # the same on every machine: Windows threads, Schannel for TLS (no
    # OpenSSL, so LGPL 2.1), D3D11VA through the hwdevice API (the core
    # never asks for DXVA2), and zlib, libgcc and the rest of the mingw
    # runtime linked statically: the DLLs import Windows' own DLLs only.
    command -v nasm >/dev/null || { echo "nasm not found" >&2; exit 1; }
    dav1d_cc=x86_64-w64-mingw32-gcc
    dav1d_ar=x86_64-w64-mingw32-ar
    dav1d_flags="'-static'"
    dav1d_system=windows
    dav1d_cpu=x86_64
    flags=(
      --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32-
      --disable-autodetect
      --enable-w32threads --enable-schannel --enable-zlib --enable-d3d11va
      # The core takes D3D11 frames (the *_d3d11va2 hwaccels); FFmpeg's
      # Makefile compiles those only with the older *_d3d11va ones.
      --enable-hwaccel=h264_d3d11va,hevc_d3d11va,vp9_d3d11va,av1_d3d11va
      --enable-hwaccel=h264_d3d11va2,hevc_d3d11va2,vp9_d3d11va2,av1_d3d11va2
      # FFmpeg 9's Schannel code calls into the udp protocol (for DTLS)
      # whether DTLS is enabled or not.
      --enable-protocol=udp
      --extra-ldflags=-static
      --pkg-config=false
    )
    ;;
  *)
    echo "unknown platform $platform" >&2
    exit 1
    ;;
esac

rm -rf "$work" "$out"
mkdir -p "$work"
cd "$work"
# FFmpeg 9.0.2's libavformat/hls.c loses a subtitle playlist's unused
# 32 KB read buffer and overwrites the AVIO probe/previous segment buffer,
# and refuses https URLs when FFmpeg has no https protocol (Android's
# build, whose https goes through the core's io_open to the platform).
# Compile a patched out-of-tree copy; never change the caller's source
# tree, which may be shared by other platform builds.
mkdir -p libavformat
cp "$src/libavformat/hls.c" libavformat/hls.c
patch -f -s -p1 <"$root/tool/patches/ffmpeg-hls-subtitle-buffers.patch"
patch -f -s -p1 <"$root/tool/patches/ffmpeg-hls-https-io-open.patch"
flags+=(--extra-cflags="-iquote$src/libavformat")
if [ -n "${dav1d_cc:-}" ] && [ -z "${DAV1D_SOURCE_DIR:-}" ]; then
  echo "no DAV1D_SOURCE_DIR: AV1 only in hardware" >&2
elif [ -n "${dav1d_cc:-}" ]; then
  dav1d_src="$(cd "$DAV1D_SOURCE_DIR" && pwd)"
  dav1d="$work/dav1d"
  cat >dav1d-cross.txt <<CROSS
[binaries]
c = '$dav1d_cc'
ar = '$dav1d_ar'

[built-in options]
c_args = [$dav1d_flags]
c_link_args = [$dav1d_flags]

[host_machine]
system = '$dav1d_system'
cpu_family = '$dav1d_cpu'
cpu = '$dav1d_cpu'
endian = 'little'
CROSS
  meson setup "$work/dav1d-build" "$dav1d_src" --cross-file=dav1d-cross.txt \
    --prefix="$dav1d" --libdir=lib --buildtype=release --default-library=static \
    -Denable_tools=false -Denable_tests=false -Denable_asm="${dav1d_asm:-true}" \
    >dav1d.log 2>&1 &&
    ninja -C "$work/dav1d-build" install >>dav1d.log 2>&1 ||
    { tail -20 dav1d.log; exit 1; }
  if [ "$dav1d_system" = linux ]; then
    # Native builds still need the system's GnuTLS, libva and libdrm.
    export PKG_CONFIG_PATH="$dav1d/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # dav1d is static already. --static here would also ask pkg-config
    # for GnuTLS's private dependencies, though GnuTLS stays shared.
  else
    # Cross builds must not find libraries from the build host.
    export PKG_CONFIG_LIBDIR="$dav1d/lib/pkgconfig" PKG_CONFIG_PATH=
    flags+=(--pkg-config-flags=--static)
  fi
  flags+=(--pkg-config=pkg-config
    --enable-libdav1d --enable-decoder=libdav1d)
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
    # Only the system libraries named above, and glibc's dynamic loader,
    # which aarch64 builds link for symbols glibc keeps there; OpenSSL
    # would make it LGPL 3.
    foreign="$(for lib in "$out"/lib/*.so.*[0-9]; do readelf -d "$lib"; done |
      sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u |
      grep -vE '^(ld-linux-aarch64|libc|libm|libz|libva|libva-drm|libdrm|libgnutls|libav(util|codec|format)|libsw(resample|scale))\.so\.[0-9]+$' || true)"
    [ -z "$foreign" ] || { printf 'unexpected dependencies:\n%s\n' "$foreign" >&2; exit 1; }
    ;;
  windows-*)
    # MSVC links the import libraries (avcodec.lib, from dlltool), which
    # FFmpeg installs next to the DLLs; lib/ is where CMake looks.
    mv "$out"/bin/*.lib "$out/lib/"
    foreign="$(for dll in "$out"/bin/*.dll; do x86_64-w64-mingw32-objdump -p "$dll"; done |
      sed -n 's/^[[:space:]]*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u |
      grep -vE '^(kernel32|msvcrt|bcrypt|crypt32|ncrypt|secur32|ws2_32|(avutil|swresample|swscale|avcodec|avformat)-[0-9]+)\.dll$' || true)"
    [ -z "$foreign" ] || { printf 'imports more than Windows DLLs:\n%s\n' "$foreign" >&2; exit 1; }
    sed -n '/^Enabled hwaccels:/,/^$/p' configure.log | grep -qw av1_d3d11va2 ||
      echo "warning: no AV1 D3D11VA: these mingw-w64 headers lack DXVA_PicParams_AV1 (Ubuntu 22.04's do)" >&2
    ;;
esac
echo "installed to $out"
