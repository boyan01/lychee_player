#!/usr/bin/env bash
# Soaks the core: lychee_cli loops a file on the virtual clock (hours of
# playback in minutes), seeking to a random position every few seconds of
# playback and switching to another file every third time, and samples the
# process's resident memory and thread count. Fails if either keeps
# growing: after a warm-up of a fifth of the run, the last fifth's median
# memory may exceed the first's by at most 10% plus 16 MB, and the thread
# count may not exceed the first fifth's highest.
#
#   tool/soak.sh                      # 10 minutes, Debug build in build/
#   MINUTES=60 tool/soak.sh
#   CLI=build-asan/examples/cli/lychee_cli tool/soak.sh
#   tool/soak.sh FILE OTHER_FILE      # default: h264_aac.mp4 and .mkv
#
# macOS and Linux. Run tool/regression.sh (or MEDIA_ONLY=1) first for the
# build and the media.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
media="${MEDIA_DIR:-/tmp/lychee-media}"
cli="${CLI:-$root/build/examples/cli/lychee_cli}"
minutes="${MINUTES:-10}"
file="${1:-$media/h264_aac.mp4}"
other="${2:-$media/h264_aac.mkv}"
interval=5

[ -x "$cli" ] || { echo "no $cli: run tool/regression.sh first"; exit 1; }
log="$(mktemp)"
samples="$(mktemp)"
trap 'rm -f "$log" "$samples"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT

"$cli" --virtual-time --loop --soak 3 "$other" "$file" >"$log" 2>&1 &
pid=$!

# Resident memory in MB and the thread count of $pid; nothing once it is gone.
sample() {
  if [ -r "/proc/$pid/status" ]; then
    awk '/^VmRSS:/ { rss = int($2 / 1024) } /^Threads:/ { n = $2 }
         END { if (rss) print rss, n }' "/proc/$pid/status"
  else
    local rss threads
    rss="$(ps -o rss= -p "$pid" 2>/dev/null)" || return
    threads="$(ps -M -p "$pid" 2>/dev/null | tail -n +2 | wc -l)"
    echo "$((rss / 1024)) $((threads))"
  fi
}

end=$(($(date +%s) + minutes * 60))
while [ "$(date +%s)" -lt "$end" ]; do
  sleep "$interval"
  line="$(sample)"
  if [ -z "$line" ]; then
    echo "FAIL soak: lychee_cli exited early"
    tail -20 "$log"
    exit 1
  fi
  echo "$line" >>"$samples"
done

actions="$(grep -c '^soak: ' "$log")"
opens="$(grep -c '^soak: open' "$log")"
errors="$(grep -c 'state=error\|event: error' "$log")"
awk -v actions="$actions" -v opens="$opens" -v errors="$errors" '
  { rss[NR] = $1; threads[NR] = $2 }
  function median(from, to,   n, i, j, t, v) {
    n = 0
    for (i = from; i <= to; i++) v[++n] = rss[i]
    for (i = 2; i <= n; i++)
      for (j = i; j > 1 && v[j - 1] > v[j]; j--) { t = v[j]; v[j] = v[j - 1]; v[j - 1] = t }
    return v[int((n + 1) / 2)]
  }
  END {
    fifth = int(NR / 5)
    if (fifth < 1) { print "FAIL soak: too few samples (" NR ")"; exit 1 }
    first = median(fifth + 1, 2 * fifth)
    last = median(NR - fifth + 1, NR)
    max_first = 0; max_last = 0
    for (i = fifth + 1; i <= 2 * fifth; i++) if (threads[i] > max_first) max_first = threads[i]
    for (i = NR - fifth + 1; i <= NR; i++) if (threads[i] > max_last) max_last = threads[i]
    printf "soak: %d samples, %d actions (%d opens), memory %d -> %d MB, threads %d -> %d, %d errors\n",
           NR, actions, opens, first, last, max_first, max_last, errors
    bad = ""
    if (last > first * 1.1 + 16) bad = bad " memory grew"
    if (max_last > max_first) bad = bad " threads grew"
    if (errors > 0) bad = bad " errors"
    if (actions < 3) bad = bad " too few actions"
    if (bad != "") { print "FAIL soak:" bad; exit 1 }
    print "PASS soak"
  }' "$samples"
