#!/usr/bin/env bash
# Builds libass, with FreeType, HarfBuzz and FriBidi, as shared libraries
# for one platform, for the core's styled ASS subtitles.
#
#   tool/build_libass.sh PLATFORM LIBASS FREETYPE HARFBUZZ FRIBIDI [FONTS]
#
# PLATFORM: macos-arm64, macos-x86_64, ios-arm64, ios-sim-arm64,
# ios-sim-x86_64, android-arm64-v8a, android-x86_64 (the names of
# tool/build_ffmpeg.sh). The other arguments are the unpacked source trees
# that tool/release.sh sources pins; FONTS (Android only) is the unpacked
# Liberation Fonts TTF release.
#
# Output: third_party/libass/PLATFORM/{include,lib}: libass and libfribidi.
# For Apple platforms, tool/package_ffmpeg_apple.sh wraps them into
# xcframeworks; on Android the AAR carries them (sdk/android).
#
# License: libass is ISC; FreeType (FTL) and HarfBuzz (MIT) are linked into
# it statically. FriBidi is LGPL 2.1, so it stays a library of its own that
# apps can replace. No fontconfig: libass finds system fonts through Core
# Text on Apple platforms. Android has no font provider; the core embeds
# Liberation Sans Regular and Bold (share/fonts here, SIL OFL 1.1) as the
# default font.
#
# Needs meson, ninja and pkg-config.
set -euo pipefail

platform="$1"
libass="$(cd "$2" && pwd)"
freetype="$(cd "$3" && pwd)"
harfbuzz="$(cd "$4" && pwd)"
fribidi="$(cd "$5" && pwd)"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/third_party/libass/$platform"
work="$root/build-libass/$platform"
deps="$work/prefix"
jobs="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
for tool in meson ninja pkg-config; do
  command -v $tool >/dev/null || { echo "$tool not found" >&2; exit 1; }
done

asm=disabled
link_args=()
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
    system=darwin
    cc="$(xcrun -sdk $sdk -f clang)"
    cxx="$(xcrun -sdk $sdk -f clang++)"
    ar="$(xcrun -sdk $sdk -f ar)"
    strip="$(xcrun -sdk $sdk -f strip)"
    flags=(-arch $arch $min -isysroot "$(xcrun -sdk $sdk --show-sdk-path)")
    coretext=enabled
    # Core Text's fallback for CJK (PingFang UI) is an Apple Hierarchical
    # Variable Font, which FreeType opens only through the system's libhvf
    # (in the SDKs; FreeType checks at run time that the OS has it).
    hvf=enabled
    # x86_64 assembly needs nasm, as for FFmpeg: worth it on Intel Macs.
    case "$platform" in
      *arm64) asm=enabled ;;
      macos-x86_64)
        if command -v nasm >/dev/null; then asm=enabled
        else echo "nasm not found: building libass without x86 assembly" >&2; fi ;;
    esac
    ;;
  android-arm64-v8a|android-x86_64)
    ndk="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}"
    toolchain="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*)"
    api=24
    if [ "$platform" = android-arm64-v8a ]; then
      triple=aarch64-linux-android; arch=aarch64; asm=enabled
    else
      triple=x86_64-linux-android; arch=x86_64
    fi
    fonts="$(cd "${6:?Android needs the Liberation Fonts directory}" && pwd)"
    system=android
    cc="$toolchain/bin/$triple$api-clang"
    cxx="$toolchain/bin/$triple$api-clang++"
    ar="$toolchain/bin/llvm-ar"
    strip="$toolchain/bin/llvm-strip"
    flags=()
    # Android 15+ devices may use 16 KB pages.
    link_args=(-Wl,-z,max-page-size=16384)
    coretext=disabled
    hvf=disabled
    ;;
  *)
    echo "unknown platform $platform" >&2
    exit 1
    ;;
esac
case "$arch" in
  arm64|aarch64) cpu_family=aarch64 ;;
  *) cpu_family=x86_64 ;;
esac

# meson_list ARG...: ['ARG', ...]
meson_list() {
  local out="" a
  for a in "$@"; do out+="${out:+, }'$a'"; done
  echo "[$out]"
}

rm -rf "$work" "$out"
mkdir -p "$work"
# Only these builds' .pc files: Homebrew's pkg-config would find the
# Mac's own FreeType and HarfBuzz.
cat >"$work/cross.ini" <<EOF
[binaries]
c = '$cc'
cpp = '$cxx'
ar = '$ar'
strip = '$strip'
# pkgconfig: meson before 1.3 knows no other name.
pkgconfig = '$(command -v pkg-config)'
$( [ "$asm" = enabled ] && [ "$cpu_family" = x86_64 ] && echo "nasm = '$(command -v nasm)'" )

[built-in options]
c_args = $(meson_list "${flags[@]+"${flags[@]}"}" -ffunction-sections -fdata-sections)
cpp_args = $(meson_list "${flags[@]+"${flags[@]}"}" -ffunction-sections -fdata-sections)
c_link_args = $(meson_list "${flags[@]+"${flags[@]}"}" "${link_args[@]+"${link_args[@]}"}")
cpp_link_args = $(meson_list "${flags[@]+"${flags[@]}"}" "${link_args[@]+"${link_args[@]}"}")

[properties]
pkg_config_libdir = '$deps/lib/pkgconfig'

[host_machine]
system = '$system'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'
EOF

# build NAME SOURCE OPTION...
build() {
  local name="$1" src="$2"
  shift 2
  meson setup "$work/$name" "$src" --cross-file "$work/cross.ini" \
    --prefix "$deps" --libdir lib -Db_ndebug=true \
    --wrap-mode nodownload "$@" >"$work/$name.log" 2>&1 &&
    ninja -C "$work/$name" -j "$jobs" install >>"$work/$name.log" 2>&1 ||
    { tail -30 "$work/$name.log"; exit 1; }
}

# FreeType and HarfBuzz go into libass, with nothing they could take from
# the system: no zlib, PNG or Brotli fonts, no ICU, no Core Text shaping.
# Shaping and glyph loading are a small part of rendering a subtitle:
# optimized for size, they make libass a quarter smaller.
build freetype "$freetype" --buildtype minsize -Ddefault_library=static \
  -Dbrotli=disabled -Dbzip2=disabled -Dharfbuzz=disabled -Dpng=disabled \
  -Dzlib=disabled -Dhvf=$hvf -Dtests=disabled
build harfbuzz "$harfbuzz" --buildtype minsize -Ddefault_library=static \
  -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled \
  -Dpng=disabled -Dzlib=disabled -Dicu=disabled -Dgraphite=disabled \
  -Dfreetype=disabled -Dfontations=disabled -Dgdi=disabled \
  -Ddirectwrite=disabled -Dcoretext=disabled -Dharfrust=disabled \
  -Dkbts=disabled -Dwasm=disabled -Draster=disabled -Dvector=disabled \
  -Dgpu=disabled -Dsubset=disabled -Dtests=disabled \
  -Dintrospection=disabled -Ddocs=disabled -Dutilities=disabled \
  -Dbenchmark=disabled
build fribidi "$fribidi" --buildtype release -Ddefault_library=shared \
  -Ddocs=false -Dbin=false -Dtests=false
if [ "$system" = darwin ]; then
  # @rpath/libfribidi.0.dylib, not this build's prefix, as for FFmpeg.
  strip -x "$deps/lib/libfribidi.0.dylib"
  install_name_tool -id @rpath/libfribidi.0.dylib "$deps/lib/libfribidi.0.dylib" 2>/dev/null
  codesign --force --sign - "$deps/lib/libfribidi.0.dylib" 2>/dev/null
fi
build libass "$libass" --buildtype release -Ddefault_library=static \
  -Dfontconfig=disabled -Ddirectwrite=disabled -Dcoretext=$coretext \
  -Dlibunibreak=disabled -Drequire-system-font-provider=false -Dasm=$asm \
  -Dtest=disabled -Dcompare=disabled -Dprofile=disabled -Dfuzz=disabled \
  -Dcheckasm=disabled
grep -E "^ *(Font providers|ASM optimizations)" "$work/libass.log" || true

# The shared libass, linked here rather than by its meson build, which
# would export FreeType and HarfBuzz too: only libass's API (libass.sym),
# so the linker drops what libass does not call. Unversioned names: APKs
# carry lib*.so only.
mkdir -p "$out/include" "$out/lib"
cp -R "$deps/include/ass" "$deps/include/fribidi" "$out/include/"
libs=($(PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig" pkg-config --static --libs libass | tr ' ' '\n' | grep -vx -- -lass))
case "$system" in
  darwin)
    sed 's/^/_/' "$libass/libass/libass.sym" >"$work/exports.txt"
    "$cc" "${flags[@]}" -dynamiclib -o "$out/lib/libass.dylib" \
      -install_name @rpath/libass.dylib \
      -Wl,-force_load,"$deps/lib/libass.a" "${libs[@]}" \
      -Wl,-exported_symbols_list,"$work/exports.txt" -Wl,-dead_strip
    strip -x "$out/lib/libass.dylib"
    codesign --force --sign - "$out/lib/libass.dylib" 2>/dev/null
    cp "$deps/lib/libfribidi.0.dylib" "$out/lib/"
    ln -s libfribidi.0.dylib "$out/lib/libfribidi.dylib"
    # Only the system and each other, through @rpath.
    foreign="$(otool -L "$out"/lib/*.dylib | awk '/^\t/ { print $1 }' | sort -u |
      grep -vE "^(/System/.*|/usr/lib/.*|@rpath/libass\.dylib|@rpath/libfribidi\.0\.dylib)$" || true)"
    ;;
  android)
    { echo '{ global:'; sed 's/$/;/' "$libass/libass/libass.sym"; echo 'local: *; };'; } >"$work/exports.map"
    "$cc" -shared -o "$out/lib/libass.so" -Wl,-soname,libass.so "${link_args[@]}" \
      -Wl,--whole-archive "$deps/lib/libass.a" -Wl,--no-whole-archive "${libs[@]}" \
      -Wl,--version-script,"$work/exports.map" -Wl,--gc-sections -Wl,--no-undefined
    "$strip" --strip-unneeded "$out/lib/libass.so"
    cp "$deps/lib/libfribidi.so" "$out/lib/"
    "$strip" --strip-unneeded "$out/lib/libfribidi.so"
    foreign="$(for lib in "$out"/lib/*.so; do "$toolchain/bin/llvm-readelf" -d "$lib"; done |
      sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u |
      grep -vE '^(libc|libm|libdl|libfribidi)\.so$' || true)"
    mkdir -p "$out/share/fonts"
    cp "$fonts/LiberationSans-Regular.ttf" "$fonts/LiberationSans-Bold.ttf" "$fonts/LICENSE" "$out/share/fonts/"
    ;;
esac
[ -z "$foreign" ] || { printf 'unexpected dependencies:\n%s\n' "$foreign" >&2; exit 1; }
ls -lL "$out"/lib/ | awk 'NR > 1 { print $5, $9 }'
echo "installed to $out"
