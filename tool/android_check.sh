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
#    AES-128 TS and fMP4 HLS also seek through HlsDemuxer, without fallback.
#    (--bilinear matches PixelCopy's texture sampling when it scales;
#    swscale's downscaling filter instead averages a wider area;
#    MediaCodec frames (including system software decoding) use --matrix
#    bt601: the emulator's compositor converts them so; README,
#    "Checking frames by pixels").
#    LYCHEE_FFMPEG_EXTENSION=1 loads the optional backend and also checks
#    encrypted HLS through FFmpeg's fallback and its memory data source.
#
# NDK: ANDROID_NDK_HOME, else the newest under ANDROID_HOME (or the
# default SDK location). Needs ffmpeg and ffprobe on PATH for
# tool/frame_check.py.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
media="$(cd "${1:-/tmp/lychee-media}" && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)}"
apk="${LYCHEE_ANDROID_APK:-$root/sdk/android/example/build/outputs/apk/debug/example-debug.apk}"
ffmpeg_extension="${LYCHEE_FFMPEG_EXTENSION:-0}"
dir=/data/local/tmp/lychee-check
port=8765

die() { echo "android_check.sh: $*" >&2; exit 1; }
[ -d "$ndk" ] || die "no NDK; set ANDROID_NDK_HOME"
[ -f "$apk" ] || die "no $apk; run sdk/android/gradlew -p sdk/android :example:assembleDebug"
for f in h264_aac.mp4 rotated.mp4 hls_aes/ts.m3u8 hls_aes/fmp4.m3u8; do
  [ -f "$media/$f" ] || die "no $media/$f; run MEDIA_ONLY=1 tool/regression.sh"
done

abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
case "$abi" in
  armeabi-v7a) triple=arm-linux-androideabi ;;
  arm64-v8a) triple=aarch64-linux-android ;;
  x86_64) triple=x86_64-linux-android ;;
  *) die "unsupported ABI $abi" ;;
esac
ffmpeg="$root/third_party/ffmpeg/android-$abi"
[ -d "$ffmpeg/lib" ] || die "no $ffmpeg; run tool/build_ffmpeg.sh android-$abi"

build="$root/build-android-$abi"
mkdir -p "$build"
if [ "${LYCHEE_SKIP_UNIT:-0}" != 1 ]; then
  echo "== unit tests ($abi)"
  cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=26 -DANDROID_STL=c++_shared \
    -DLYCHEE_WITH_FFMPEG=ON -DLYCHEE_FFMPEG_ROOT="$ffmpeg" >/dev/null
  cmake --build "$build" --target lychee_unittests >/dev/null
  adb shell "rm -rf $dir && mkdir -p $dir"
  adb push "$build/core/test/lychee_unittests" "$build/core/test/liblychee_ffmpeg_mismatch.so" "$ffmpeg"/lib/*.so \
    "$(ls "$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/"$triple"/libc++_shared.so | head -1)" \
    "$dir/" >/dev/null 2>&1
  # adb shell passes the remote command's exit status since Android 7.
  adb shell "cd $dir && LD_LIBRARY_PATH=. ./lychee_unittests" >"$build/unittests.txt" 2>&1 ||
    { cat "$build/unittests.txt"; die "unit tests failed"; }
  tail -1 "$build/unittests.txt"
  adb shell "rm -rf $dir"

fi

echo "== example"
python3 -I "$root/tool/serve_media.py" "$media" "$port" >"$build/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
adb reverse "tcp:$port" "tcp:$port" >/dev/null
adb install -r -t "$apk" >/dev/null
package=tech.soit.lychee.example
files="/data/user/0/$package/files"
file_access="run-as $package"
if [ "${LYCHEE_ANDROID_PUBLIC_FILES:-0}" = 1 ]; then
  files="/sdcard/Android/data/$package/files"
  file_access=""
fi
adb shell "$file_access mkdir -p $files"
snaps="$build/snapshots"
rm -rf "$snaps" && mkdir -p "$snaps"
failed=0
cases=(h264_aac.mp4 rotated.mp4 hls_aes/ts.m3u8 hls_aes/fmp4.m3u8)
if [ "$ffmpeg_extension" = 1 ]; then
  cases+=(hls_aes/ts_fallback.m3u8 hls_aes/fmp4_fallback.m3u8)
fi
for file in "${cases[@]}"; do
  modes="false true"
  seek=()
  reference="$file"
  case "$file" in
    hls_aes/ts.m3u8 | hls_aes/ts_fallback.m3u8) modes=true; seek=(--es seek 1:6); reference=hls/index.m3u8 ;;
    hls_aes/fmp4.m3u8 | hls_aes/fmp4_fallback.m3u8) modes=true; seek=(--es seek 1:6); reference=hls_fmp4/index.m3u8 ;;
  esac
  for hw in $modes; do
    name="${file//\//-}-hw-$hw"
    recreate=()
    if [ "$file" = rotated.mp4 ] && [ "$hw" = false ]; then
      # Recreate after the first capture, with another capture still queued.
      recreate=(--es recreate 2.7)
    fi
    adb shell am force-stop "$package"
    adb logcat -c
    adb shell "$file_access rm -f $files/$name-a.ppm $files/$name-b.ppm"
    extension_args=()
    [ "$ffmpeg_extension" != 1 ] || extension_args=(--es ffmpeg load)
    adb shell am start -n "$package/.MainActivity" ${extension_args[@]+"${extension_args[@]}"} --es url "http://127.0.0.1:$port/$file" \
      --ez hw "$hw" ${seek[@]+"${seek[@]}"} ${recreate[@]+"${recreate[@]}"} --es snapshot "2:$files/$name-a.ppm,3.5:$files/$name-b.ppm" >/dev/null
    log=""
    for _ in $(seq 60); do
      # A recreation can log the same path twice; the last line describes
      # the file now on disk. Wait for both distinct capture paths.
      log="$(adb logcat -d -s lychee-example:I | grep -o 'snapshot .*' |
        awk '{ snapshots[$2] = $0 } END { for (path in snapshots) print snapshots[path] }' | sort || true)"
      [ "$(grep -c . <<<"$log")" -lt 2 ] || break
      sleep 0.5
    done
    # A missing snapshot can also follow an activity restart. Keep its
    # lifecycle and crash diagnostics alongside the player's output.
    adb logcat -d -s lychee-example:I lychee:I ActivityTaskManager:I \
      ActivityManager:I WindowManager:I AndroidRuntime:E >"$snaps/$name.logcat.txt"
    if [ "$file" = rotated.mp4 ] && [ "$hw" = false ] &&
      ! grep -q 'recreate activity' "$snaps/$name.logcat.txt"; then
      echo "activity recreation did not run"
      failed=1
    fi
    adb shell am force-stop "$package"
    for suffix in a b; do
      adb exec-out $file_access cat "$files/$name-$suffix.ppm" >"$snaps/$name-$suffix.ppm"
    done
    if [[ "$file" = hls_aes/* ]]; then
      if [[ "$file" = *_fallback.m3u8 ]]; then
        if ! grep -q 'the fallback opens it' "$snaps/$name.logcat.txt" ||
          ! grep -q 'decrypted an AES-128 segment' "$snaps/$name.logcat.txt"; then
          echo "AES HLS did not use the FFmpeg extension's fallback/decryption"
          failed=1
        fi
      elif grep -q 'the fallback opens it' "$snaps/$name.logcat.txt"; then
        echo "AES HLS unexpectedly used FFmpeg's fallback"
        failed=1
      fi
      if ! grep -q 'seek -> 6.0s' "$snaps/$name.logcat.txt"; then
        echo "AES HLS did not seek"
        failed=1
      fi
    fi
    if [ "$ffmpeg_extension" = 1 ] && [ "$hw" = false ] &&
      ! grep -q '\[I/ffmpeg_video\] ffmpeg decoder opened' "$snaps/$name.logcat.txt"; then
      echo "software playback did not use FFmpeg"
      failed=1
    fi
    matrix=()
    { [ "$hw" = false ] && [ "$ffmpeg_extension" = 1 ]; } || matrix=(--matrix bt601)
    sed "s|/[^ ]*/files/|$snaps/|" <<<"$log" >"$snaps/$name.txt"
    echo "-- $file hw=$hw"
    if [ "$(grep -c ' pts=' <<<"$log")" != 2 ]; then
      echo "${log:-no snapshot}"
      failed=1
    elif ! python3 -I "$root/tool/frame_check.py" "$media/$reference" \
      --log "$snaps/$name.txt" --bilinear ${matrix[@]+"${matrix[@]}"}; then
      failed=1
    fi
  done
done
[ "$failed" = 0 ] || die "snapshots failed"
echo "ok: unit tests and the example's snapshots on $abi"
