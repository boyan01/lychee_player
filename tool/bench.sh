#!/usr/bin/env bash
# Measures the core's throughput and latencies per resolution and codec,
# on the virtual clock (lychee_cli --virtual-time), so the numbers depend
# on the machine but not on its load from the media's own pace: each case
# plays 10 s as fast as the pipeline decodes and converts it.
#
#   tool/bench.sh                  # Release build in build-release/
#   NO_FFMPEG=1 tool/bench.sh      # without FFmpeg, in build-release-noffmpeg/
#   ONLY='1080p-*' tool/bench.sh   # only cases matching a glob
#   FFMPEG_ROOT=third_party/ffmpeg/macos-arm64 tool/bench.sh
#                                  # the shipped FFmpeg (tool/build_ffmpeg.sh)
#                                  # instead of the system's, in
#                                  # build-release-lgpl/
#   BENCH_TSV=FILE tool/bench.sh   # append to FILE instead (CI keeps one
#                                  # table per machine there)
#
# Per case it prints and appends to build-release*/bench.tsv (one row per
# case and commit, "+" after the commit when the tree was dirty):
#   fps      frames rendered per wall-clock second (decode, plus the
#            conversion to BGRA in the -cpu cases)
#   open_ms  from the open to the first frame pulled
#   seek_ms  from a seek to 5 s until the first frame after it, which the
#            virtual clock lets through only once the pipeline has settled
#   rss_mb   the process's peak resident memory
# Compare rows of one machine; run it on an idle one (it runs cases one at
# a time). Media goes to /tmp/lychee-bench.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
media="${MEDIA_DIR:-/tmp/lychee-bench}"
build="$root/build-release"
flags=()
if [ "${NO_FFMPEG:-0}" = 1 ]; then
  build="$build-noffmpeg"
  flags+=(-DLYCHEE_WITH_FFMPEG=OFF)
elif [ -n "${FFMPEG_ROOT:-}" ]; then
  build="$build-lgpl"
  flags+=(-DLYCHEE_FFMPEG_ROOT="$(cd "$FFMPEG_ROOT" && pwd)")
fi

mkdir -p "$media"
gen() {
  local out="$media/$1"
  shift
  [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y "$@" "$out" || { rm -f "$out"; exit 1; }
}
# 10 s at 30 fps with a keyframe every 2 s, and AAC, as typical files have.
for size in 1280x720 1920x1080 3840x2160; do
  for codec in h264 hevc; do
    case $codec in
      h264) encoder=(-c:v libx264 -preset veryfast) ;;
      hevc) encoder=(-c:v libx265 -preset veryfast -tag:v hvc1 -x265-params log-level=error) ;;
    esac
    gen "${codec}_${size#*x}p.mp4" -f lavfi -i "testsrc2=size=$size:rate=30:duration=10" \
      -f lavfi -i "sine=frequency=440:duration=10" "${encoder[@]}" -g 60 -pix_fmt yuv420p \
      -c:a aac -b:a 128k
  done
done

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release ${flags[@]+"${flags[@]}"} >/dev/null || exit 1
cmake --build "$build" --target lychee_cli >/dev/null || exit 1
cli="$build/examples/cli/lychee_cli"

commit="$(git -C "$root" rev-parse --short=12 HEAD)"
[ -z "$(git -C "$root" status --porcelain -- core examples)" ] || commit="$commit+"
results="${BENCH_TSV:-$build/bench.tsv}"
[ -f "$results" ] || printf 'date\tcommit\tcase\tdecoder\tfps\topen_ms\tseek_ms\trss_mb\n' >"$results"

# Peak RSS in MB from time's report, on macOS (-l, bytes) or Linux (-v, KB).
time_args=(-l)
[ "$(uname)" = Darwin ] || time_args=(-v)
rss_mb() {
  awk '/maximum resident set size/ { print int($1 / 1048576) }
       /Maximum resident set size/ { print int($NF / 1024) }' "$1"
}
selected() {
  [ -z "${ONLY:-}" ] && return 0
  local glob
  for glob in $ONLY; do
    # shellcheck disable=SC2053
    [[ $1 == $glob ]] && return 0
  done
  return 1
}
field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<<"$2"; }

printf '%-18s %-16s %7s %8s %8s %7s\n' case decoder fps open_ms seek_ms rss_mb
for file in "$media"/*.mp4; do
  name="$(basename "$file" .mp4)"
  for mode in hw sw cpu; do
    case $mode in
      hw) args=() ;;
      sw) args=(--no-hw) ;;
      cpu) args=(--cpu) ;;
    esac
    label="${name#*_}-${name%%_*}-$mode"
    selected "$label" || continue
    log="$(mktemp)"
    summary="$(/usr/bin/time "${time_args[@]}" "$cli" --virtual-time --seek 5 --seek-after 2 \
      ${args[@]+"${args[@]}"} "$file" 2>"$log" | grep '^summary:')"
    rendered="$(field rendered "$summary")"
    wall="$(field wall "$summary")"
    wall="${wall%s}"
    decoder="$(field decoder "$summary")"
    open_ms="$(field open_ms "$summary")"
    seek_ms="$(field seek_ms "$summary")"
    rss="$(rss_mb "$log")"
    rm -f "$log"
    fps="$(awk -v n="${rendered:-0}" -v t="${wall:-0}" 'BEGIN { printf "%.0f", (t > 0 ? n / t : 0) }')"
    printf '%-18s %-16s %7s %8s %8s %7s\n' "$label" "${decoder:--}" "$fps" "${open_ms:--}" "${seek_ms:--}" "${rss:--}"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%MZ)" "$commit" "$label" \
      "${decoder:--}" "$fps" "${open_ms:--}" "${seek_ms:--}" "${rss:--}" >>"$results"
  done
done
echo "appended to $results"
