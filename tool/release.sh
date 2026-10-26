#!/usr/bin/env bash
# Builds, pins and checks the prebuilt artifacts of a release. The release
# workflow (.github/workflows/release.yml) runs these steps; each also runs
# here.
#
#   tool/release.sh version VERSION    set flutter/pubspec.yaml's version;
#                                      flutter/CHANGELOG.md must have
#                                      a "## VERSION" section
#   tool/release.sh sources DIR        download the pinned FFmpeg and Mbed TLS
#                                      sources into DIR (release assets too:
#                                      the LGPL asks for the source), and
#                                      libass's with its font
#   tool/release.sh build apple|android|linux|windows SOURCES
#                                      build FFmpeg and libass (platforms
#                                      already in third_party are kept) and the
#                                      xcframeworks or the AAR; linux builds
#                                      only FFmpeg (CI; the plugin still
#                                      downloads BtbN's); windows
#                                      cross-compiles on Linux (mingw-w64)
#   tool/release.sh pack apple|android|windows OUT
#                                      write the release assets into OUT
#   tool/release.sh pin OUT            pin OUT's assets in flutter/prebuilt.json
#                                      and Package.swift
#   tool/release.sh check OUT          build a fresh app against the package
#                                      pub would publish, with OUT served as
#                                      the release (macOS, iOS simulator,
#                                      Android)
#   tool/release.sh verify             download what prebuilt.json pins from
#                                      the release and check it
#   tool/release.sh publish            flutter pub publish --force
#   tool/release.sh notes              print the CHANGELOG section of the
#                                      pubspec's version
#
# The assets: NAME.xcframework.zip for the eight Apple frameworks (what
# SwiftPM's binaryTarget takes; the pod unzips the same files),
# lychee-VERSION.aar and .pom (the Flutter plugin lays them out as a Maven
# repository), ffmpeg-windows-x86_64.zip (the Windows plugin's FFmpeg:
# bin/*.dll, lib/*.lib, include/), and the source archives.
#
# pub packs flutter/ only, and the Windows and Linux plugins compile the
# core: while pub runs, flutter/core is a copy of core/ (gitignored there;
# flutter/.pubignore lets pub take it).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
pubspec="$root/flutter/pubspec.yaml"
manifest="$root/flutter/prebuilt.json"
frameworks=(lychee libavformat libavcodec libswresample libswscale libavutil libass libfribidi)
apple_platforms=(macos-arm64 macos-x86_64 ios-arm64 ios-sim-arm64 ios-sim-x86_64)
android_platforms=(android-arm64-v8a android-x86_64)

ffmpeg_version=9.0.2
ffmpeg_sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
ffmpeg_url=https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz
mbedtls_version=3.6.7
mbedtls_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
mbedtls_url=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2

# libass and what tool/build_libass.sh links into it; FriBidi is LGPL 2.1.
# The Liberation fonts give Android its default font (SIL OFL 1.1).
libass_version=0.17.5
libass_sha256=2dca25c0e0c837ddf00b52011b3f82cac1e4ddd3ad018227806b0c2288864acc
libass_url=https://github.com/libass/libass/releases/download/$libass_version/libass-$libass_version.tar.xz
freetype_version=2.15.0
freetype_sha256=93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5
freetype_url=https://download.savannah.gnu.org/releases/freetype/freetype-$freetype_version.tar.xz
harfbuzz_version=15.0.0
harfbuzz_sha256=968536e9abd782f07f7954044c0c4020199fcdd39b41d221ec42d2a3be4a44df
harfbuzz_url=https://github.com/harfbuzz/harfbuzz/releases/download/$harfbuzz_version/harfbuzz-$harfbuzz_version.tar.xz
fribidi_version=1.0.17
fribidi_sha256=6949dcde27d41cebad1fd741fcafc36d55a1020d2d872d4a6eb3914caabbada2
fribidi_url=https://github.com/fribidi/fribidi/releases/download/v$fribidi_version/fribidi-$fribidi_version.tar.xz
fonts_version=2.1.5
fonts_sha256=7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0
fonts_url=https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-$fonts_version.tar.gz

die() { echo "release.sh: $*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
version() { sed -nE 's/^version: (.+)$/\1/p' "$pubspec"; }

# download URL FILE SHA256
download() {
  [ -f "$2" ] && [ "$(sha256 "$2")" = "$3" ] && return 0
  curl -fsSL --retry 3 -o "$2.part" "$1" || die "cannot download $1"
  [ "$(sha256 "$2.part")" = "$3" ] || die "$1 has SHA-256 $(sha256 "$2.part"), not $3"
  mv "$2.part" "$2"
}

# with_core COMMAND...: runs COMMAND in flutter/ with the core copied in.
with_core() {
  rm -rf "$root/flutter/core"
  cp -R "$root/core" "$root/flutter/core"
  rm -rf "$root/flutter/core/test"
  cp "$root/THIRD_PARTY_NOTICES" "$root/flutter/core/"
  local status=0
  (cd "$root/flutter" && "$@") || status=$?
  rm -rf "$root/flutter/core"
  return $status
}

cmd_version() {
  local v="${1:?version}"
  [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || die "not a version: $v"
  grep -qxF "## $v" "$root/flutter/CHANGELOG.md" ||
    die "flutter/CHANGELOG.md has no '## $v' section"
  sed -i.bak -E "s/^version: .*/version: $v/" "$pubspec"
  rm "$pubspec.bak"
}

cmd_sources() {
  local dir="${1:?dir}"
  mkdir -p "$dir"
  download "$ffmpeg_url" "$dir/ffmpeg-$ffmpeg_version.tar.xz" "$ffmpeg_sha256"
  download "$mbedtls_url" "$dir/mbedtls-$mbedtls_version.tar.bz2" "$mbedtls_sha256"
  download "$libass_url" "$dir/libass-$libass_version.tar.xz" "$libass_sha256"
  download "$freetype_url" "$dir/freetype-$freetype_version.tar.xz" "$freetype_sha256"
  download "$harfbuzz_url" "$dir/harfbuzz-$harfbuzz_version.tar.xz" "$harfbuzz_sha256"
  download "$fribidi_url" "$dir/fribidi-$fribidi_version.tar.xz" "$fribidi_sha256"
  download "$fonts_url" "$dir/liberation-fonts-ttf-$fonts_version.tar.gz" "$fonts_sha256"
}

# build_libass PLATFORM SOURCES: tool/build_libass.sh, unless
# third_party/libass/PLATFORM exists.
build_libass() {
  local src="$root/build-libass/src" f
  [ -d "$root/third_party/libass/$1" ] && return 0
  mkdir -p "$src"
  for f in libass-$libass_version freetype-$freetype_version harfbuzz-$harfbuzz_version fribidi-$fribidi_version; do
    [ -d "$src/$f" ] || tar xJf "$2/$f.tar.xz" -C "$src"
  done
  [ -d "$src/liberation-fonts-ttf-$fonts_version" ] || tar xzf "$2/liberation-fonts-ttf-$fonts_version.tar.gz" -C "$src"
  "$root/tool/build_libass.sh" "$1" "$src/libass-$libass_version" "$src/freetype-$freetype_version" \
    "$src/harfbuzz-$harfbuzz_version" "$src/fribidi-$fribidi_version" "$src/liberation-fonts-ttf-$fonts_version"
}

cmd_build() {
  local target="${1:?apple, android, linux or windows}" sources src p
  cmd_sources "${2:?sources dir}"
  sources="$(cd "$2" && pwd)"
  src="$root/build-ffmpeg/src"
  mkdir -p "$src"
  [ -d "$src/ffmpeg-$ffmpeg_version" ] || tar xJf "$sources/ffmpeg-$ffmpeg_version.tar.xz" -C "$src"
  case "$target" in
    apple)
      for p in "${apple_platforms[@]}"; do
        [ -d "$root/third_party/ffmpeg/$p" ] && continue
        # Intel Macs decode in software through the assembly.
        [ "$p" != macos-x86_64 ] || command -v nasm >/dev/null || die "macos-x86_64 needs nasm"
        "$root/tool/build_ffmpeg.sh" "$p" "$src/ffmpeg-$ffmpeg_version"
      done
      for p in "${apple_platforms[@]}"; do
        build_libass "$p" "$sources"
      done
      "$root/tool/package_ffmpeg_apple.sh"
      "$root/tool/build_apple.sh"
      ;;
    android)
      [ -d "$src/mbedtls-$mbedtls_version" ] || tar xjf "$sources/mbedtls-$mbedtls_version.tar.bz2" -C "$src"
      for p in "${android_platforms[@]}"; do
        [ -d "$root/third_party/ffmpeg/$p" ] && continue
        "$root/tool/build_ffmpeg.sh" "$p" "$src/ffmpeg-$ffmpeg_version" "$src/mbedtls-$mbedtls_version"
      done
      for p in "${android_platforms[@]}"; do
        build_libass "$p" "$sources"
      done
      "$root/sdk/android/gradlew" -p "$root/sdk/android" :lychee:publish
      ;;
    linux)
      [ -d "$root/third_party/ffmpeg/linux-x86_64" ] ||
        "$root/tool/build_ffmpeg.sh" linux-x86_64 "$src/ffmpeg-$ffmpeg_version"
      ;;
    windows)
      [ -d "$root/third_party/ffmpeg/windows-x86_64" ] ||
        "$root/tool/build_ffmpeg.sh" windows-x86_64 "$src/ffmpeg-$ffmpeg_version"
      ;;
    *) die "unknown target $target" ;;
  esac
}

cmd_pack() {
  local target="${1:?apple, android or windows}" out f v stage
  mkdir -p "${2:?out}"
  out="$(cd "$2" && pwd)"
  case "$target" in
    apple)
      for f in "${frameworks[@]}"; do
        [ -d "$root/flutter/darwin/Frameworks/$f.xcframework" ] || die "no $f.xcframework; run tool/release.sh build apple"
        rm -f "$out/$f.xcframework.zip"
        # -y keeps the symlinks of the macOS framework bundles.
        (cd "$root/flutter/darwin/Frameworks" && zip -qry "$out/$f.xcframework.zip" "$f.xcframework" -x '*.DS_Store')
      done
      ;;
    android)
      v="$(version)"
      for f in "lychee-$v.aar" "lychee-$v.pom"; do
        cp "$root/sdk/android/build/repo/tech/soit/lychee/lychee/$v/$f" "$out/" ||
          die "no $f; run tool/release.sh build android"
      done
      ;;
    windows)
      # What MSVC builds need, in one directory named like the zip.
      f="$root/third_party/ffmpeg/windows-x86_64"
      [ -d "$f/bin" ] || die "no Windows FFmpeg; run tool/release.sh build windows"
      stage="$(mktemp -d)"
      mkdir -p "$stage/ffmpeg-windows-x86_64/lib"
      cp -R "$f/bin" "$f/include" "$stage/ffmpeg-windows-x86_64/"
      cp "$f"/lib/*.lib "$stage/ffmpeg-windows-x86_64/lib/"
      rm -f "$out/ffmpeg-windows-x86_64.zip"
      (cd "$stage" && zip -qr "$out/ffmpeg-windows-x86_64.zip" ffmpeg-windows-x86_64)
      rm -rf "$stage"
      ;;
    *) die "unknown target $target" ;;
  esac
}

cmd_pin() {
  local out v
  out="$(cd "${1:?out}" && pwd)"
  v="$(version)"
  python3 -I - "$out" "v$v" "$manifest" "$root/Package.swift" "${frameworks[@]}" <<'EOF'
import hashlib, json, pathlib, re, sys

out, tag, manifest, package = (pathlib.Path(a) for a in sys.argv[1:5])
tag = str(tag)
frameworks = sys.argv[5:]
version = tag[1:]
names = [f"{f}.xcframework.zip" for f in frameworks] + [
    f"lychee-{version}.aar", f"lychee-{version}.pom", "ffmpeg-windows-x86_64.zip"]
missing = [n for n in names if not (out / n).is_file()]
if missing:
    sys.exit(f"release.sh: missing in {out}: {' '.join(missing)}")
files = {n: hashlib.sha256((out / n).read_bytes()).hexdigest() for n in names}

data = json.loads(manifest.read_text())
data["tag"] = tag
data["files"] = files
manifest.write_text(json.dumps(data, indent=2) + "\n")

checksums = "".join(f'    "{f}": "{files[f + ".xcframework.zip"]}",\n' for f in frameworks)
block = (f'let release = (\n  url: "{data["url"]}/{tag}",\n'
         f'  checksums: [\n{checksums}  ]\n)\n')
text = package.read_text()
text, n = re.subn(r"let release = \(\n.*?\n\)\n", lambda _: block, text, count=1, flags=re.S)
if n != 1:
    sys.exit(f"release.sh: no release block in {package}")
package.write_text(text)
EOF
  echo "pinned $(ls "$out" | wc -l | tr -d ' ') files of v$v"
}

cmd_check() {
  local out v tag tmp pkg app port server f pinned
  out="$(cd "${1:?out}" && pwd)"
  v="$(version)"
  tag="v$v"
  tmp="$(mktemp -d)"
  [ "$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["tag"])' "$manifest")" = "$tag" ] ||
    die "flutter/prebuilt.json does not pin $tag; run tool/release.sh pin $1"

  echo "== package"
  with_core dart pub publish --to-archive="$tmp/package.tar.gz" >"$tmp/pack.log" 2>&1 ||
    { cat "$tmp/pack.log"; die "pub cannot pack the package"; }
  # Uncommitted pins are fine here; the release workflow commits them first.
  ! sed -n '/^Validating package/,$p' "$tmp/pack.log" | grep '^\* ' |
    grep -vE '^\* [0-9]+ checked-in files? (is|are) modified in git' ||
    { sed -n '/^Validating package/,$p' "$tmp/pack.log"; die "the package has warnings"; }
  pkg="$tmp/lychee_player"
  mkdir "$pkg"
  tar xzf "$tmp/package.tar.gz" -C "$pkg"
  [ -f "$pkg/core/CMakeLists.txt" ] || die "the package has no core/"
  [ -f "$pkg/core/THIRD_PARTY_NOTICES" ] || die "the package has no core/THIRD_PARTY_NOTICES"
  [ ! -e "$pkg/darwin/Frameworks" ] || die "the package carries darwin/Frameworks"
  echo "$(du -sh "$tmp/package.tar.gz" | cut -f1) archive"

  for f in "${frameworks[@]}"; do
    pinned="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["files"][sys.argv[2]])' \
      "$manifest" "$f.xcframework.zip")"
    [ "$(cd "$tmp" && swift package compute-checksum "$out/$f.xcframework.zip")" = "$pinned" ] ||
      die "SwiftPM's checksum of $f.xcframework.zip is not the pinned one"
  done

  # The release, served from OUT; a fresh cache, so everything downloads.
  mkdir -p "$tmp/mirror"
  ln -s "$out" "$tmp/mirror/$tag"
  port=$((20000 + RANDOM % 20000))
  python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$tmp/mirror" >"$tmp/server.log" 2>&1 &
  server=$!
  trap "kill $server 2>/dev/null" EXIT
  export LYCHEE_PREBUILT_URL="http://127.0.0.1:$port" XDG_CACHE_HOME="$tmp/cache"

  echo "== fresh app"
  app="$tmp/lychee_check"
  flutter create --org tech.soit --platforms=macos,ios,android "$app" >"$tmp/create.log"
  cd "$app"
  flutter pub add "lychee_player:{path: $pkg}" >"$tmp/add.log"
  cat >lib/main.dart <<'EOF'
import 'package:flutter/material.dart';
import 'package:lychee_player/lychee_player.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final player = await LycheePlayer.create();
  player.open(const String.fromEnvironment('LYCHEE_URL'));
  player.play();
  runApp(MaterialApp(home: Scaffold(body: LycheeVideo(player))));
}
EOF
  # The plugin's minimums (README).
  sed -i.bak -E 's/minSdk = .*/minSdk = 26/' android/app/build.gradle.kts
  perl -0pi -e 's/(\n *)(minSdk = 26)/$1$2$1if (project.findProperty("split-per-abi") != "true") { ndk { abiFilters.clear(); abiFilters += listOf("arm64-v8a", "x86_64") } }/' \
    android/app/build.gradle.kts
  sed -i.bak -E 's/MACOSX_DEPLOYMENT_TARGET = [0-9.]+;/MACOSX_DEPLOYMENT_TARGET = 11.0;/' \
    macos/Runner.xcodeproj/project.pbxproj
  sed -i.bak -E 's/IPHONEOS_DEPLOYMENT_TARGET = [0-9.]+;/IPHONEOS_DEPLOYMENT_TARGET = 16.0;/' \
    ios/Runner.xcodeproj/project.pbxproj
  [ ! -f macos/Podfile ] || sed -i.bak -E "s/^#? *platform :osx, .*/platform :osx, '11.0'/" macos/Podfile
  [ ! -f ios/Podfile ] || sed -i.bak -E "s/^#? *platform :ios, .*/platform :ios, '16.0'/" ios/Podfile

  echo "== macOS"
  LANG=en_US.UTF-8 flutter build macos --debug >"$tmp/macos.log" 2>&1 || { tail -40 "$tmp/macos.log"; die "macOS build failed"; }
  [ -f build/macos/Build/Products/Debug/lychee_check.app/Contents/Frameworks/lychee.framework/lychee ] ||
    die "the macOS app has no lychee.framework"
  echo "== iOS simulator"
  LANG=en_US.UTF-8 flutter build ios --simulator --debug >"$tmp/ios.log" 2>&1 || { tail -40 "$tmp/ios.log"; die "iOS build failed"; }
  [ -f build/ios/iphonesimulator/Runner.app/Frameworks/lychee.framework/lychee ] ||
    die "the iOS app has no lychee.framework"
  echo "== Android"
  flutter build apk --debug >"$tmp/android.log" 2>&1 || { tail -40 "$tmp/android.log"; die "Android build failed"; }
  # Every ABI the APK has Flutter for needs the core too, or the app
  # installs on devices where it cannot play.
  check_apk_abis build/app/outputs/flutter-apk/app-debug.apk
  echo "== Android split APKs"
  flutter build apk --debug --split-per-abi --target-platform=android-arm64,android-x64 \
    >"$tmp/android-split.log" 2>&1 || { tail -40 "$tmp/android-split.log"; die "Android split build failed"; }
  for abi in arm64-v8a x86_64; do
    check_apk_abis "build/app/outputs/flutter-apk/app-$abi-debug.apk" "$abi"
  done
  grep -q "GET /$tag/lychee-$v.aar" "$tmp/server.log" && grep -q "GET /$tag/lychee.xcframework.zip" "$tmp/server.log" ||
    die "the builds did not download from the release"
  echo "ok: a fresh app builds with the package and the release of $tag ($tmp)"
}

# Fails unless every ABI with libflutter.so in APK also has liblychee.so,
# and the requested ABI (arm64-v8a by default) is among them.
check_apk_abis() {
  local libs abi required="${2:-arm64-v8a}"
  libs="$(unzip -Z1 "$1" 'lib/*')" || die "cannot list $1"
  grep -qx "lib/$required/liblychee.so" <<<"$libs" || die "$1 has no $required liblychee.so"
  for abi in $(sed -nE 's|^lib/([^/]+)/libflutter\.so$|\1|p' <<<"$libs"); do
    grep -qx "lib/$abi/liblychee.so" <<<"$libs" ||
      die "$1 has libflutter.so but no liblychee.so for $abi: set abiFilters (README)"
  done
}

cmd_verify() {
  local tmp
  tmp="$(mktemp -d)"
  python3 -I -c '
import json, os, sys
m = json.load(open(sys.argv[1]))
url = os.environ.get("LYCHEE_PREBUILT_URL") or m["url"]
tag = m["tag"]
for name, sha in m["files"].items():
    print(f"{url}/{tag}/{name} {sha} {name}")
' "$manifest" | while read -r url sha name; do
    download "$url" "$tmp/$name" "$sha"
    echo "ok $name"
  done
}

cmd_publish() {
  with_core flutter pub publish --force
}

cmd_notes() {
  awk -v h="## $(version)" '$0 == h { f = 1; next } /^## / { f = 0 } f' "$root/flutter/CHANGELOG.md"
}

cmd="${1:-}"
shift || true
case "$cmd" in
  version|sources|build|pack|pin|check|verify|publish|notes) "cmd_$cmd" "$@" ;;
  *) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
