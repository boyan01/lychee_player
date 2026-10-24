#!/usr/bin/env bash
# Builds the core and plays a fixed media matrix through lychee_cli.
# Prints one PASS/FAIL line per case; exits non-zero if any case fails.
#
#   tool/regression.sh            # Debug build in build/
#   TSAN=1 tool/regression.sh     # ThreadSanitizer build in build-tsan/
#   MEDIA_DIR=... tool/regression.sh
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
media="${MEDIA_DIR:-/tmp/lychee-media}"
if [ "${TSAN:-0}" = 1 ]; then
  build="$root/build-tsan"
  flags=(-DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread)
else
  build="$root/build"
  flags=()
fi

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug ${flags[@]+"${flags[@]}"} >/dev/null || exit 1
cmake --build "$build" 2>&1 | grep -E "error|warning:" && { echo "FAIL build has errors or warnings"; exit 1; }
cli="$build/examples/cli/lychee_cli"

# Test media, generated once. Synthetic so the expected output is known.
mkdir -p "$media"
gen() {
  local out="$media/$1"; shift
  [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y "$@" "$out" || exit 1
}
src="testsrc2=size=1920x1080:rate=30"
gen h264_aac.mp4 -f lavfi -i "$src" -f lavfi -i sine=frequency=440:sample_rate=44100 -t 12 \
  -c:v libx264 -preset veryfast -bf 3 -pix_fmt yuv420p -c:a aac -b:a 128k
gen hevc10.mp4 -f lavfi -i testsrc2=size=1920x1080:rate=24 -f lavfi -i sine=frequency=660:sample_rate=48000 -t 10 \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -tag:v hvc1 -x265-params log-level=error -c:a aac
gen vp9_opus.webm -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=550:sample_rate=48000 -t 10 \
  -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 2M -c:a libopus
gen av1.mkv -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 10 \
  -c:v libsvtav1 -preset 12 -svtav1-params log-level=0 -c:a aac
gen rotated.mp4 -display_rotation 90 -i "$media/h264_aac.mp4" -c copy
gen audio_only.mp3 -f lavfi -i sine=frequency=880:sample_rate=44100 -t 8 -c:a libmp3lame
gen video_only.mp4 -f lavfi -i testsrc2=size=1280x720:rate=60 -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p
gen offset.ts -i "$media/h264_aac.mp4" -c copy -output_ts_offset 10 -f mpegts
# The index at the front, so a cut file still opens; then cut in half.
gen faststart.mp4 -i "$media/h264_aac.mp4" -c copy -movflags +faststart
[ -f "$media/truncated.mp4" ] || head -c 4358118 "$media/faststart.mp4" >"$media/truncated.mp4"
# Valid headers, then noise where the media data should be.
[ -f "$media/corrupt.mp4" ] || python3 -I -c '
import random, sys
data = bytearray(open(sys.argv[1], "rb").read())
rng = random.Random(1)
data[600000:] = bytes(rng.randrange(256) for _ in range(len(data) - 600000))
open(sys.argv[2], "wb").write(data)' "$media/faststart.mp4" "$media/corrupt.mp4"

failures=0
# check NAME EXPECT_HW EXPECT_SEEK_PTS -- CLI_ARGS...
# EXPECT_HW: 1, 0, or - (no video). EXPECT_SEEK_PTS: seconds, or - for none.
check() {
  local name="$1" hw="$2" seek_pts="$3"; shift 4
  local out summary problems=""
  out="$(TSAN_OPTIONS=halt_on_error=0 "$cli" "$@" 2>&1)"
  summary="$(grep '^summary:' <<<"$out")"
  grep -q "WARNING: ThreadSanitizer" <<<"$out" && problems+=" tsan-report"
  grep -q "state=ended" <<<"$summary" || problems+=" not-ended"
  grep -q "backward_jumps=0" <<<"$summary" || problems+=" backward-jumps"
  if [ "$hw" != - ]; then
    grep -q " hw=$hw " <<<"$summary" || problems+=" hw!=$hw"
    local dropped
    dropped="$(sed -E 's/.*dropped=([0-9]+).*/\1/' <<<"$summary")"
    [ "${dropped:-99}" -le 3 ] || problems+=" dropped=$dropped"
  fi
  if [ "$seek_pts" != - ]; then
    grep -q "first_pts_after_seek=$seek_pts" <<<"$summary" || problems+=" seek-landed-elsewhere"
  fi
  if [ -z "$problems" ]; then
    echo "PASS $name"
  else
    echo "FAIL $name:$problems"
    echo "     $summary"
    failures=$((failures + 1))
  fi
}

check h264          1 - -- "$media/h264_aac.mp4"
check hevc10        1 - -- "$media/hevc10.mp4"
check vp9           1 - -- "$media/vp9_opus.webm"
check av1-software  0 - -- "$media/av1.mkv"
check rotated       1 - -- "$media/rotated.mp4"
check audio-only    - - -- "$media/audio_only.mp3"
check video-only    1 - -- "$media/video_only.mp4"
check ts-offset     1 - -- "$media/offset.ts"
check no-hw-cpu     0 - -- --no-hw --cpu "$media/h264_aac.mp4"
check seek-forward  1 7.500 -- --seek 7.5 --seek-after 2 "$media/h264_aac.mp4"
check seek-backward 1 1.000 -- --seek 1 --seek-after 5 "$media/hevc10.mp4"
check seek-near-end 1 11.900 -- --seek 11.9 --seek-after 1 "$media/h264_aac.mp4"
check seek-past-end 1 - -- --seek 30 --seek-after 1 "$media/offset.ts"
check seek-audio    - - -- --seek 3 --seek-after 1 "$media/audio_only.mp3"

# expect NAME PATTERN... -- CLI_ARGS: passes if the CLI output matches every
# extended regex PATTERN.
expect() {
  local name="$1"; shift
  local patterns=() out problems=""
  while [ "$1" != -- ]; do patterns+=("$1"); shift; done
  shift
  out="$(TSAN_OPTIONS=halt_on_error=0 "$cli" "$@" 2>&1)"
  grep -q "WARNING: ThreadSanitizer" <<<"$out" && problems+=" tsan-report"
  for pattern in "${patterns[@]}"; do
    grep -Eq "$pattern" <<<"$out" || problems+=" missing:'$pattern'"
  done
  if [ -z "$problems" ]; then
    echo "PASS $name"
  else
    echo "FAIL $name:$problems"
    grep -E '^(summary|error|seek took)' <<<"$out" | sed 's/^/     /'
    failures=$((failures + 1))
  fi
}

# Network cases against a local server that injects faults. Each starts its
# own server so a fault fires once.
server=""
stop_server() {
  [ -n "$server" ] || return 0
  kill "$server" 2>/dev/null
  wait "$server" 2>/dev/null
  server=""
}
serve() {
  stop_server
  local log="$build/media_server.log"
  python3 -I "$root/tool/media_server.py" "$media" "$@" >"$log" 2>&1 &
  server=$!
  port=""
  for _ in $(seq 50); do
    port="$(awk '/^port/ {print $2}' "$log")"
    [ -n "$port" ] && break
    sleep 0.1
  done
  url="http://127.0.0.1:$port/faststart.mp4"
}
trap stop_server EXIT

expect truncated-file "state=error" "error: input ended at" -- "$media/truncated.mp4"
expect corrupt-file "state=error" "error: (audio|video) decoding failed" -- "$media/corrupt.mp4"
serve
check http          1 - -- "$url"
serve --drop-at 3000000
check http-reconnect 1 - -- "$url"
serve --drop-at 3000000 --refuse-after-drop
expect http-lost "state=error" "error: input ended at" -- "$url"
# The read stalls before playback starts; the seek must not wait for it.
serve --stall-at 3000000 --stall-for 10
expect http-stall-seek "seek took [0-9]{1,3}\." "first_pts_after_seek=0.500s" -- \
  --seek 0.5 --seek-after 1 --seconds 3 "$url"
stop_server

echo "$failures failure(s)"
[ "$failures" = 0 ]
