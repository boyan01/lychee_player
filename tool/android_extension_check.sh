#!/usr/bin/env bash
# Builds both AARs, consumes their Maven publications (not project modules),
# checks the default and loaded frame paths, and exercises explicit loading
# after R8. Requires an already booted device and the regression media.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
out="$root/build-android-extension"
mkdir -p "$out"
gradle() {
  env 'ORG_GRADLE_PROJECT_kotlin.compiler.execution.strategy=in-process' "$root/sdk/android/gradlew" "$@"
}
repo="${LYCHEE_ANDROID_REPO:-$root/sdk/android/build/repo}"
export LYCHEE_ANDROID_PUBLIC_FILES=1

if [ -z "${LYCHEE_ANDROID_REPO:-}" ]; then
  gradle -p "$root/sdk/android" :lychee:publish :ffmpeg:publish >"$out/publish.log" 2>&1 ||
    { tail -50 "$out/publish.log"; exit 1; }
fi
version="$(sed -nE 's/^version: (.+)$/\1/p' "$root/flutter/pubspec.yaml")"
python3 -I - "$repo" "$version" <<'PY'
import pathlib, sys, zipfile, xml.etree.ElementTree as ET
repo, version = pathlib.Path(sys.argv[1]), sys.argv[2]
base = repo / "io/github/boyan01/lychee"
for artifact in ("lychee", "lychee-ffmpeg"):
    path = base / artifact / version
    with zipfile.ZipFile(path / f"{artifact}-{version}.aar") as aar:
        for abi in ("armeabi-v7a", "arm64-v8a", "x86_64"):
            libs = {pathlib.PurePosixPath(n).name for n in aar.namelist()
                    if n.startswith(f"jni/{abi}/") and n.endswith(".so")}
            if artifact == "lychee":
                assert libs == {"liblychee.so", "liblychee_jni.so", "libc++_shared.so"}, (abi, libs)
            else:
                assert libs == {"liblychee_ffmpeg.so", "libavformat.so", "libavcodec.so",
                                "libavutil.so", "libswscale.so", "libswresample.so"}, (abi, libs)
    if artifact == "lychee-ffmpeg":
        ns = {"m": "http://maven.apache.org/POM/4.0.0"}
        pom = ET.parse(path / f"{artifact}-{version}.pom")
        deps = [(d.findtext("m:groupId", namespaces=ns), d.findtext("m:artifactId", namespaces=ns),
                 d.findtext("m:version", namespaces=ns)) for d in pom.findall("m:dependencies/m:dependency", ns)]
        assert ("io.github.boyan01.lychee", "lychee", version) in deps, deps
print("ok: both AARs' ABIs, library ownership and published dependency")
PY

# A fresh consumer has only the example sources: it cannot accidentally
# link either SDK project or reuse native build outputs from this checkout.
consumer="$(mktemp -d "$out/consumer.XXXXXX")"
cp "$root/sdk/android/"{build.gradle.kts,settings.gradle.kts,gradle.properties} "$consumer/"
[ ! -f "$root/sdk/android/local.properties" ] || cp "$root/sdk/android/local.properties" "$consumer/"
mkdir "$consumer/example"
cp "$root/sdk/android/example/build.gradle.kts" "$consumer/example/"
cp -R "$root/sdk/android/example/src" "$consumer/example/"
for extension in false true; do
  gradle -p "$consumer" :example:assembleRelease \
    "-Plychee.example.repo=$repo" "-Plychee.example.ffmpeg=$extension" "-Plychee.example.version=$version" \
    >"$out/example-$extension.log" 2>&1 ||
    { tail -50 "$out/example-$extension.log"; exit 1; }
  cp "$consumer/example/build/outputs/apk/release/example-release.apk" "$out/$extension.apk"
done
if [ "${LYCHEE_ANDROID_BUILD_ONLY:-0}" = 1 ]; then
  echo "ok: fresh Android consumers of both AARs ($out)"
  exit 0
fi

LYCHEE_ANDROID_APK="$out/false.apk" "$root/tool/android_check.sh" "${1:-/tmp/lychee-media}"
abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
rm -rf "$out/base-snapshots"
cp -R "$root/build-android-$abi/snapshots" "$out/base-snapshots"
LYCHEE_ANDROID_APK="$out/true.apk" LYCHEE_SKIP_UNIT=1 LYCHEE_FFMPEG_EXTENSION=1 \
  "$root/tool/android_check.sh" "${1:-/tmp/lychee-media}"
rm -rf "$out/ffmpeg-snapshots"
cp -R "$root/build-android-$abi/snapshots" "$out/ffmpeg-snapshots"

package=tech.soit.lychee.example
check_mode() {
  local mode="$1" expected="$2"
  adb shell am force-stop "$package"
  adb logcat -c
  adb shell am start -n "$package/.MainActivity" --es ffmpeg "$mode" \
    --es url /does-not-exist.mp4 --es seconds 2 >/dev/null
  for _ in $(seq 30); do
    adb logcat -d -s lychee-example:I AndroidRuntime:E >"$out/$mode.log"
    grep -q 'summary:' "$out/$mode.log" && break
    sleep 0.5
  done
  grep -q "$expected" "$out/$mode.log"
  grep -q 'summary:' "$out/$mode.log"
  ! grep -q 'FATAL EXCEPTION' "$out/$mode.log"
  echo "ok: $mode"
}
check_mode twice 'ffmpeg: twice loaded'
check_mode concurrent 'ffmpeg: concurrent loaded'
check_mode late 'ffmpeg: late load rejected'

# Keep the Kotlin entry point, remove its native library, and re-sign. This
# proves the public method reports a missing pack rather than silently using
# the system backend. The signed original APK above also exercises R8 rules.
tools="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
cp "$out/true.apk" "$out/missing-unsigned.apk"
zip -qd "$out/missing-unsigned.apk" 'lib/*/liblychee_ffmpeg.so' 'META-INF/*'
"$tools/zipalign" -P 16 -f 4 "$out/missing-unsigned.apk" "$out/missing.apk"
"$tools/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android "$out/missing.apk"
adb install -r "$out/missing.apk" >/dev/null
check_mode missing 'ffmpeg: missing library rejected'
adb install -r "$out/true.apk" >/dev/null
echo "ok: optional FFmpeg SDK ($out)"
