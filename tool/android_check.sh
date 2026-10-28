#!/usr/bin/env bash
# Checks the core and the Android SDK on the device adb talks to (an
# emulator, or ANDROID_SERIAL's): CI's Android job runs it, and it runs
# the same by hand.
#
#   tool/android_check.sh [MEDIA_DIR]
#
# 1. The core's unit tests, cross-built with the NDK for the device's ABI
#    into build-android-ABI (FFmpeg from third_party/ffmpeg/android-ABI)
#    and run from adb shell in /data/local/tmp/lychee-check.
# 2. The SDK's example (sdk/android/example, already built with
#    `sdk/android/gradlew -p sdk/android :example:assembleDebug`) plays
#    h264_aac.mp4 and rotated.mp4 from MEDIA_DIR (default
#    /tmp/lychee-media, from MEDIA_ONLY=1 tool/regression.sh) through
#    tool/serve_media.py and `adb reverse`, decoding in software and with
#    MediaCodec, and two snapshots of each pass tool/frame_check.py
#    (hardware frames with --matrix bt601: the emulator's compositor
#    converts them so; README, "Checking frames by pixels").
#
# NDK: ANDROID_NDK_HOME, else the newest under ANDROID_HOME (or the
# default SDK location). Needs ffmpeg and ffprobe on PATH for
# tool/frame_check.py.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
media="$(cd "${1:-/tmp/lychee-media}" && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)}"
apk="$root/sdk/android/example/build/outputs/apk/debug/example-debug.apk"
dir=/data/local/tmp/lychee-check
port=8765

die() { echo "android_check.sh: $*" >&2; exit 1; }
[ -d "$ndk" ] || die "no NDK; set ANDROID_NDK_HOME"
[ -f "$apk" ] || die "no $apk; run sdk/android/gradlew -p sdk/android :example:assembleDebug"
for f in h264_aac.mp4 rotated.mp4; do
  [ -f "$media/$f" ] || die "no $media/$f; run MEDIA_ONLY=1 tool/regression.sh"
done

abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
case "$abi" in
  arm64-v8a) triple=aarch64-linux-android ;;
  x86_64) triple=x86_64-linux-android ;;
  *) die "unsupported ABI $abi" ;;
esac
ffmpeg="$root/third_party/ffmpeg/android-$abi"
[ -d "$ffmpeg/lib" ] || die "no $ffmpeg; run tool/build_ffmpeg.sh android-$abi"

echo "== unit tests ($abi)"
build="$root/build-android-$abi"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$abi" -DANDROID_PLATFORM=26 -DANDROID_STL=c++_shared \
  -DLYCHEE_FFMPEG_ROOT="$ffmpeg" >/dev/null
cmake --build "$build" --target lychee_unittests >/dev/null
adb shell "rm -rf $dir && mkdir -p $dir"
adb push "$build/core/test/lychee_unittests" "$ffmpeg"/lib/*.so \
  "$(ls "$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/"$triple"/libc++_shared.so | head -1)" \
  "$dir/" >/dev/null 2>&1
# adb shell passes the remote command's exit status since Android 7.
adb shell "cd $dir && LD_LIBRARY_PATH=. ./lychee_unittests" >"$build/unittests.txt" 2>&1 ||
  { cat "$build/unittests.txt"; die "unit tests failed"; }
tail -1 "$build/unittests.txt"
adb shell "rm -rf $dir"

echo "== example"
python3 -I "$root/tool/serve_media.py" "$media" "$port" >"$build/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
adb reverse "tcp:$port" "tcp:$port" >/dev/null
adb install -r -t "$apk" >/dev/null
package=tech.soit.lychee.example
snaps="$build/snapshots"
failed=0
for file in h264_aac.mp4 rotated.mp4; do
  for hw in false true; do
    name="${file%.mp4}-hw-$hw"
    adb shell am force-stop "$package"
    adb logcat -c
    adb shell "rm -rf /sdcard/Android/data/$package/files/*"
    adb shell am start -n "$package/.MainActivity" --es url "http://127.0.0.1:$port/$file" \
      --ez hw "$hw" --es snapshot "2:$name-a.ppm,3.5:$name-b.ppm" >/dev/null
    log=""
    for _ in $(seq 60); do
      log="$(adb logcat -d -s lychee-example:I | grep -o 'snapshot .*' || true)"
      [ "$(grep -c . <<<"$log")" -lt 2 ] || break
      sleep 0.5
    done
    adb shell am force-stop "$package"
    rm -rf "$snaps" && mkdir -p "$snaps"
    adb pull "/sdcard/Android/data/$package/files/." "$snaps" >/dev/null 2>&1
    matrix=()
    [ "$hw" = false ] || matrix=(--matrix bt601)
    echo "-- $file hw=$hw"
    if [ "$(grep -c ' pts=' <<<"$log")" != 2 ]; then
      echo "${log:-no snapshot}"
      failed=1
    elif ! sed "s|/[^ ]*/files/|$snaps/|" <<<"$log" |
      python3 -I "$root/tool/frame_check.py" "$media/$file" --log - ${matrix[@]+"${matrix[@]}"}; then
      failed=1
    fi
  done
done
[ "$failed" = 0 ] || die "snapshots failed"
echo "ok: unit tests and the example's snapshots on $abi"
