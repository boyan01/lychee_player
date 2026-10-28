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
#                                      the LGPL asks for the source)
#   tool/release.sh build apple|android|linux|windows SOURCES
#                                      build FFmpeg (matching fingerprints
#                                      in third_party are kept) and the
#                                      xcframeworks or the AAR; linux and
#                                      windows build only FFmpeg (the
#                                      plugins compile the core), linux for
#                                      this machine's architecture, windows
#                                      cross-compiled on Linux (mingw-w64)
#   tool/release.sh pack apple|android|linux|windows OUT
#                                      write the release assets into OUT
#   tool/release.sh pin OUT            pin OUT's assets in flutter/prebuilt.json
#                                      and Package.swift
#   tool/release.sh check OUT          build a fresh app against the package
#                                      pub would publish, with OUT served as
#                                      the release: on macOS for macOS and
#                                      the iOS simulator (with CocoaPods and
#                                      with Swift Package Manager) and
#                                      Android; in Git Bash on Windows for
#                                      Windows (OUT needs only
#                                      ffmpeg-windows-x86_64.zip there)
#   tool/release.sh sbom OUT           write OUT/lychee_player-VERSION.spdx.json,
#                                      an SPDX 2.3 SBOM of OUT's assets: what
#                                      each holds (the core, FFmpeg, Mbed TLS,
#                                      zlib and mingw-w64's runtime, as
#                                      THIRD_PARTY_NOTICES lists) and the pins
#                                      of their sources
#   tool/release.sh verify             download what prebuilt.json pins from
#                                      the release and check it
#   tool/release.sh publish            flutter pub publish --force
#   tool/release.sh notes              print the CHANGELOG section of the
#                                      pubspec's version
#
# The assets: NAME.xcframework.zip for the six Apple frameworks (what
# SwiftPM's binaryTarget takes; the pod unzips the same files) and
# lychee_noffmpeg, the core without FFmpeg (the Swift package's NoFFmpeg
# trait),
# lychee-VERSION.aar and .pom (the Flutter plugin lays them out as a Maven
# repository), ffmpeg-windows-x86_64.zip (the Windows plugin's FFmpeg:
# bin/*.dll, lib/*.lib, include/), ffmpeg-linux-x86_64.tar.xz and
# ffmpeg-linux-arm64.tar.xz (the Linux plugin's: lib/*.so*, include/; each
# built on its architecture), and the source archives.
#
# pub packs flutter/ only, and the Windows and Linux plugins compile the
# core: while pub runs, flutter/core is a copy of core/ (gitignored there;
# flutter/.pubignore lets pub take it), and the plugin's Swift package gets
# sdk/apple's sources the same way.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
pubspec="$root/flutter/pubspec.yaml"
manifest="$root/flutter/prebuilt.json"
frameworks=(lychee libavformat libavcodec libswresample libswscale libavutil)
# What the release has besides: the core without FFmpeg, for SwiftPM only.
release_frameworks=("${frameworks[@]}" lychee_noffmpeg)
apple_platforms=(macos-arm64 macos-x86_64 ios-arm64 ios-sim-arm64 ios-sim-x86_64)
android_platforms=(android-arm64-v8a android-x86_64)

ffmpeg_version=9.0.2
ffmpeg_sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
ffmpeg_url=https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz
mbedtls_version=3.6.7
mbedtls_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
mbedtls_url=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2

windows=0
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    windows=1
    # The Microsoft Store's python3.exe only opens the Store.
    python3 -c '' 2>/dev/null || python3() { python "$@"; }
    ;;
esac

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

# with_core COMMAND...: runs COMMAND in flutter/ with the core copied in,
# and sdk/apple's sources for the plugin's Package.swift.
with_core() {
  local sdk="$root/flutter/darwin/lychee_player/Sources/LycheePlayer"
  rm -rf "$root/flutter/core" "$sdk"
  cp -R "$root/core" "$root/flutter/core"
  rm -rf "$root/flutter/core/test"
  cp "$root/THIRD_PARTY_NOTICES" "$root/flutter/core/"
  cp -R "$root/sdk/apple/Sources/LycheePlayer" "$sdk"
  local status=0
  (cd "$root/flutter" && "$@") || status=$?
  rm -rf "$root/flutter/core" "$sdk"
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
}

# Linux FFmpeg is built natively, for this machine's architecture.
linux_platform() {
  case "$(uname -m)" in
    x86_64) echo linux-x86_64 ;;
    aarch64) echo linux-arm64 ;;
    *) die "no Linux FFmpeg for $(uname -m)" ;;
  esac
}

# The release uses checksum-verified source archives. Reuse only builds
# made with those pins and scripts, not arbitrary local installs.
# build_ffmpeg.sh removes the output directory, so a manual build also
# removes this stamp. Write it only after all build/dependency checks pass.
build_ffmpeg() {
  local platform="$1" fingerprint stamp
  stamp="$root/third_party/ffmpeg/$platform/.release-fingerprint"
  fingerprint="$({
    printf '%s\n' "$platform" "$ffmpeg_sha256"
    [ "${platform#android-}" = "$platform" ] || printf '%s\n' "$mbedtls_sha256"
    cat "$root/tool/build_ffmpeg.sh" "$root/tool/apple_framework.sh"
    # configure also accepts these environment overrides.
    printf '%s\n' "${CC:-}" "${CXX:-}" "${CFLAGS:-}" "${CPPFLAGS:-}" "${LDFLAGS:-}"
  } | shasum -a 256 | cut -d' ' -f1)"
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$fingerprint" ] &&
      [ -f "$root/third_party/ffmpeg/$platform/include/libavcodec/avcodec.h" ]; then
    echo "FFmpeg $platform: reusing matching build"
    return
  fi
  # Intel assembly is needed only when building, not when reusing it.
  [ "$platform" != macos-x86_64 ] || command -v nasm >/dev/null || die "macos-x86_64 needs nasm"
  echo "FFmpeg $platform: rebuilding (missing or changed fingerprint)"
  rm -f "$stamp"
  "$root/tool/build_ffmpeg.sh" "$@"
  printf '%s\n' "$fingerprint" >"$stamp"
}

cmd_build() {
  local target="${1:?apple, android, linux or windows}" sources src p
  cmd_sources "${2:?sources dir}"
  sources="$(cd "$2" && pwd)"
  # Keep extraction tied to the archive too: a changed pin with the same
  # version must not rebuild from the previous extracted source.
  src="$root/build-ffmpeg/src/$ffmpeg_sha256-$mbedtls_sha256"
  mkdir -p "$src"
  [ -d "$src/ffmpeg-$ffmpeg_version" ] || tar xJf "$sources/ffmpeg-$ffmpeg_version.tar.xz" -C "$src"
  case "$target" in
    apple)
      for p in "${apple_platforms[@]}"; do
        build_ffmpeg "$p" "$src/ffmpeg-$ffmpeg_version"
      done
      "$root/tool/package_ffmpeg_apple.sh"
      "$root/tool/build_apple.sh"
      NO_FFMPEG=1 "$root/tool/build_apple.sh"
      ;;
    android)
      [ -d "$src/mbedtls-$mbedtls_version" ] || tar xjf "$sources/mbedtls-$mbedtls_version.tar.bz2" -C "$src"
      for p in "${android_platforms[@]}"; do
        build_ffmpeg "$p" "$src/ffmpeg-$ffmpeg_version" "$src/mbedtls-$mbedtls_version"
      done
      "$root/sdk/android/gradlew" -p "$root/sdk/android" :lychee:publish
      ;;
    linux)
      p="$(linux_platform)"
      build_ffmpeg "$p" "$src/ffmpeg-$ffmpeg_version"
      ;;
    windows)
      build_ffmpeg windows-x86_64 "$src/ffmpeg-$ffmpeg_version"
      ;;
    *) die "unknown target $target" ;;
  esac
}

cmd_pack() {
  local target="${1:?apple, android, linux or windows}" out f v stage
  mkdir -p "${2:?out}"
  out="$(cd "$2" && pwd)"
  case "$target" in
    apple)
      for f in "${release_frameworks[@]}"; do
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
    linux)
      # The libraries with their symlinks (lib*.so for linking, lib*.so.N
      # for the loader) and the headers, in one directory named like the
      # archive. Not lib/pkgconfig: it names this machine's prefix.
      v="$(linux_platform)"
      f="$root/third_party/ffmpeg/$v"
      [ -d "$f/lib" ] || die "no Linux FFmpeg; run tool/release.sh build linux"
      v="ffmpeg-$v"
      stage="$(mktemp -d)"
      mkdir -p "$stage/$v/lib"
      cp -R "$f/include" "$stage/$v/"
      cp -P "$f"/lib/lib*.so* "$stage/$v/lib/"
      rm -f "$out/$v.tar.xz"
      tar cJf "$out/$v.tar.xz" -C "$stage" "$v"
      rm -rf "$stage"
      ;;
    *) die "unknown target $target" ;;
  esac
}

cmd_pin() {
  local out v
  out="$(cd "${1:?out}" && pwd)"
  v="$(version)"
  python3 -I - "$out" "v$v" "$manifest" "$root/Package.swift" "${release_frameworks[@]}" <<'EOF'
import hashlib, json, pathlib, re, sys

out, tag, manifest, package = (pathlib.Path(a) for a in sys.argv[1:5])
tag = str(tag)
frameworks = sys.argv[5:]
version = tag[1:]
names = [f"{f}.xcframework.zip" for f in frameworks] + [
    f"lychee-{version}.aar", f"lychee-{version}.pom", "ffmpeg-windows-x86_64.zip",
    "ffmpeg-linux-x86_64.tar.xz", "ffmpeg-linux-arm64.tar.xz"]
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
  # Flutter and CMake on Windows take C:/... paths, which Git Bash reads too.
  [ "$windows" = 0 ] || tmp="$(cd "$tmp" && pwd -W)"
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
  # From stdin: GNU tar takes C:/... for a remote host.
  tar xzf - -C "$pkg" <"$tmp/package.tar.gz"
  [ -f "$pkg/core/CMakeLists.txt" ] || die "the package has no core/"
  [ -f "$pkg/core/THIRD_PARTY_NOTICES" ] || die "the package has no core/THIRD_PARTY_NOTICES"
  [ ! -e "$pkg/darwin/Frameworks" ] || die "the package carries darwin/Frameworks"
  [ ! -e "$pkg/darwin/lychee_player/Frameworks" ] || die "the package carries darwin/lychee_player/Frameworks"
  [ -f "$pkg/darwin/lychee_player/Sources/LycheePlayer/LycheeFrameDriver.swift" ] ||
    die "the package has no copy of sdk/apple's sources"
  echo "$(du -sh "$tmp/package.tar.gz" | cut -f1) archive"

  # The release, served from OUT; a fresh cache, so everything downloads.
  mkdir -p "$tmp/mirror"
  ln -s "$out" "$tmp/mirror/$tag"
  port=$((20000 + RANDOM % 20000))
  python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$tmp/mirror" >"$tmp/server.log" 2>&1 &
  server=$!
  # On Windows $server may be the subshell that runs python3() (above); a
  # signal stops it but not the Python it started.
  trap "{ [ ! -r /proc/$server/winpid ] || taskkill //F //T //PID \$(cat /proc/$server/winpid); kill $server; } >/dev/null 2>&1 || true" EXIT
  export LYCHEE_PREBUILT_URL="http://127.0.0.1:$port" XDG_CACHE_HOME="$tmp/cache"
  app="$tmp/lychee_check"

  if [ "$windows" = 1 ]; then
    check_windows
    return
  fi

  for f in "${release_frameworks[@]}"; do
    pinned="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["files"][sys.argv[2]])' \
      "$manifest" "$f.xcframework.zip")"
    [ "$(cd "$tmp" && swift package compute-checksum "$out/$f.xcframework.zip")" = "$pinned" ] ||
      die "SwiftPM's checksum of $f.xcframework.zip is not the pinned one"
  done

  create_app macos,ios,android
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

  # The pod (Swift Package Manager off for this app), from the local server.
  swift_package_manager false
  check_apple CocoaPods
  # The plugin's Package.swift with the copy of sdk/apple. SwiftPM
  # downloads only over https, so the release is unzipped into the package
  # instead, which the manifest takes.
  for f in "${frameworks[@]}"; do
    unzip -q "$out/$f.xcframework.zip" -d "$pkg/darwin/lychee_player/Frameworks"
  done
  swift_package_manager true
  check_apple "Swift Package Manager"
  # Debug builds keep the app's code in NAME.debug.dylib. nm fails on the
  # files that are not Mach-O, which pipefail would take for no match.
  for f in build/macos/Build/Products/Debug/lychee_check.app/Contents/MacOS build/ios/iphonesimulator/Runner.app; do
    { nm "$f"/* 2>/dev/null || true; } | grep -q LycheeFrameDriver || die "$f does not use sdk/apple's LycheeFrameDriver"
  done
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

# create_app PLATFORMS: a fresh app at $app on the package at $pkg, which
# plays LYCHEE_URL; the current directory becomes the app's.
create_app() {
  echo "== fresh app"
  flutter create --org tech.soit --platforms="$1" "$app" >"$tmp/create.log"
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
}

# The Windows app: the plugin compiles the core from the package's copy
# and downloads the release's FFmpeg, both of which ship next to the app.
check_windows() {
  local dir=build/windows/x64/runner/Debug lib
  create_app windows
  echo "== Windows"
  # LYCHEE_FFMPEG_ROOT would skip the download this checks.
  env -u LYCHEE_FFMPEG_ROOT flutter build windows --debug >"$tmp/windows.log" 2>&1 ||
    { tail -40 "$tmp/windows.log"; die "Windows build failed"; }
  [ -f "$dir/lychee_player.dll" ] || die "the Windows app has no lychee_player.dll"
  for lib in avformat avcodec swresample swscale avutil; do
    compgen -G "$dir/$lib-*.dll" >/dev/null || die "the Windows app has no $lib DLL"
  done
  grep -q "GET /$tag/ffmpeg-windows-x86_64.zip" "$tmp/server.log" ||
    die "the build did not download from the release"
  echo "ok: a fresh Windows app builds with the package and the release of $tag ($tmp)"
}

# swift_package_manager true|false: Flutter's Swift Package Manager
# integration for the app in the current directory.
swift_package_manager() {
  perl -0pi -e 's/\n  config:\n    enable-swift-package-manager: \w+\n/\n/; s/^flutter:\n/flutter:\n  config:\n    enable-swift-package-manager: '"$1"'\n/m' pubspec.yaml
}

# check_apple LABEL: builds the app in the current directory for macOS and
# the iOS simulator and checks that both carry the core.
check_apple() {
  local log="$tmp/$(tr -cd 'a-zA-Z' <<<"$1")"
  echo "== macOS ($1)"
  LANG=en_US.UTF-8 flutter build macos --debug >"$log-macos.log" 2>&1 || { tail -40 "$log-macos.log"; die "macOS build failed"; }
  [ -f build/macos/Build/Products/Debug/lychee_check.app/Contents/Frameworks/lychee.framework/lychee ] ||
    die "the macOS app has no lychee.framework"
  echo "== iOS simulator ($1)"
  LANG=en_US.UTF-8 flutter build ios --simulator --debug >"$log-ios.log" 2>&1 || { tail -40 "$log-ios.log"; die "iOS build failed"; }
  [ -f build/ios/iphonesimulator/Runner.app/Frameworks/lychee.framework/lychee ] ||
    die "the iOS app has no lychee.framework"
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

cmd_sbom() {
  local out v
  out="$(cd "${1:?out}" && pwd)"
  v="$(version)"
  rm -f "$out/lychee_player-$v.spdx.json"
  python3 -I - "$out" "$v" "$manifest" "$ffmpeg_version" "$ffmpeg_url" "$ffmpeg_sha256" \
    "$mbedtls_version" "$mbedtls_url" "$mbedtls_sha256" <<'EOF'
import datetime, hashlib, json, pathlib, sys, uuid

out = pathlib.Path(sys.argv[1])
version, manifest = sys.argv[2], json.load(open(sys.argv[3]))
ffmpeg_version, ffmpeg_url, ffmpeg_sha256 = sys.argv[4:7]
mbedtls_version, mbedtls_url, mbedtls_sha256 = sys.argv[7:10]
repo = manifest["url"].removesuffix("/releases/download")

def package(id_, name, version, url, license_, sha256=None):
    p = {"SPDXID": f"SPDXRef-Package-{id_}", "name": name, "versionInfo": version,
         "downloadLocation": url, "filesAnalyzed": False,
         "licenseConcluded": license_, "licenseDeclared": license_,
         "copyrightText": "NOASSERTION"}
    if sha256:
        p["checksums"] = [{"algorithm": "SHA256", "checksumValue": sha256}]
    return p

# THIRD_PARTY_NOTICES: the core carries code ported from Chromium (BSD-3);
# the AAR's FFmpeg is LGPL 3 because it links Mbed TLS (Apache 2.0); the
# Windows FFmpeg DLLs carry zlib and parts of mingw-w64's runtime.
packages = [
    package("lychee", "lychee_player", version, f"git+{repo}@v{version}", "MIT AND BSD-3-Clause"),
    package("FFmpeg", "FFmpeg", ffmpeg_version, ffmpeg_url, "LGPL-2.1-or-later", ffmpeg_sha256),
    package("MbedTLS", "Mbed TLS", mbedtls_version, mbedtls_url, "Apache-2.0", mbedtls_sha256),
    package("zlib", "zlib", "NOASSERTION", "NOASSERTION", "Zlib"),
    package("mingw-w64", "mingw-w64 runtime", "NOASSERTION", "NOASSERTION", "ZPL-2.1"),
]

def sources(name):
    if name.startswith("lychee-") and name.endswith(".aar"):
        return ["lychee", "FFmpeg", "MbedTLS"]
    if name.startswith("lychee-") and name.endswith(".pom"):
        return []
    if name.startswith(("lychee.", "lychee_noffmpeg.")):
        return ["lychee"]
    if name.startswith(("libav", "libsw", "ffmpeg-linux-")):
        return ["FFmpeg"]
    if name == "ffmpeg-windows-x86_64.zip":
        return ["FFmpeg", "zlib", "mingw-w64"]
    return []  # the source archives, pinned in the packages above

files, relationships = [], [{"spdxElementId": "SPDXRef-DOCUMENT",
                             "relationshipType": "DESCRIBES",
                             "relatedSpdxElement": "SPDXRef-Package-lychee"}]
for i, path in enumerate(sorted(p for p in out.iterdir() if p.is_file())):
    id_ = f"SPDXRef-File-{i}"
    data = path.read_bytes()
    # SPDX 2.3 wants SHA-1 for every file; SHA-256 is what the pins use.
    files.append({"SPDXID": id_, "fileName": f"./{path.name}",
                  "checksums": [{"algorithm": a.upper(), "checksumValue": hashlib.new(a, data).hexdigest()}
                                for a in ("sha1", "sha256")],
                  "licenseConcluded": "NOASSERTION", "copyrightText": "NOASSERTION"})
    relationships.append({"spdxElementId": "SPDXRef-DOCUMENT",
                          "relationshipType": "DESCRIBES", "relatedSpdxElement": id_})
    for source in sources(path.name):
        relationships.append({"spdxElementId": id_, "relationshipType": "GENERATED_FROM",
                              "relatedSpdxElement": f"SPDXRef-Package-{source}"})

doc = {
    "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
    "name": f"lychee_player-{version}",
    "documentNamespace": f"{repo}/releases/tag/v{version}/spdx/{uuid.uuid4()}",
    "creationInfo": {"created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                     "creators": ["Tool: lychee_player tool/release.sh"]},
    "packages": packages, "files": files, "relationships": relationships,
}
target = out / f"lychee_player-{version}.spdx.json"
target.write_text(json.dumps(doc, indent=1) + "\n")
print(f"wrote {target.name}: {len(files)} files")
EOF
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
  version|sources|build|pack|pin|check|sbom|verify|publish|notes) "cmd_$cmd" "$@" ;;
  *) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
