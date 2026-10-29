#!/usr/bin/env bash
# Soaks the core: lychee_cli loops a file on the virtual clock (hours of
# playback in minutes), seeking to a random position every few seconds of
# playback and switching to another file every third time, and samples the
# process's resident memory and thread count. Fails if either keeps
# growing: after a warm-up of a fifth of the run, the last fifth's median
# memory may exceed the first's by at most 10% plus 16 MB, and the thread
# count may not exceed the first fifth's highest. On macOS, report and
# exclude the kernel's workqueue and CoreMedia's shared dispatch pool:
# both can add idle workers after warm-up. All other threads, including
# decoder libraries and unknown pthreads, remain checked.
#
#   tool/soak.sh                      # 10 minutes, Debug build in build/
#   MINUTES=60 tool/soak.sh
#   CLI=build-asan/examples/cli/lychee_cli tool/soak.sh
#   tool/soak.sh FILE OTHER_FILE      # default: h264_aac.mp4 and .mkv
#   tool/soak.sh -- COMMAND ARG...    # a platform harness with --soak
#
# macOS, Linux, and Windows in Git Bash (CLI= the regression's build, e.g.
# build-lgpl/examples/cli/lychee_cli.exe, with FFmpeg's DLLs on PATH;
# MEDIA_DIR as C:/...). Run tool/regression.sh (or MEDIA_ONLY=1) first for
# the build and the media.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
media="${MEDIA_DIR:-/tmp/lychee-media}"
cli="${CLI:-$root/build/examples/cli/lychee_cli}"
minutes="${MINUTES:-10}"
file="${1:-$media/h264_aac.mp4}"
other="${2:-$media/h264_aac.mkv}"
interval=5

log="$(mktemp)"
samples="$(mktemp)"
trap 'rm -f "$log" "$samples"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT

if [ "${1:-}" = -- ]; then
  shift
  [ "$#" -gt 0 ] || { echo "soak.sh: expected a harness command after --"; exit 1; }
  "$@" >"$log" 2>&1 &
else
  [ -x "$cli" ] || { echo "no $cli: run tool/regression.sh first"; exit 1; }
  "$cli" --virtual-time --loop --soak 3 "$other" "$file" >"$log" 2>&1 &
fi
pid=$!

# RSS in MB, total threads, system workqueue threads and CoreMedia shared
# workers (the last two on macOS only). Missing columns are zero.
# Nothing once the process is gone.
sample() {
  if [ -r "/proc/$pid/winpid" ]; then
    # A native Windows process: MSYS's /proc knows only its own.
    local winpid
    winpid="$(cat "/proc/$pid/winpid")"
    powershell -NoProfile -Command "\$p = Get-Process -Id $winpid -ErrorAction SilentlyContinue; if (\$p) { '{0} {1}' -f [int](\$p.WorkingSet64 / 1MB), \$p.Threads.Count }" |
      tr -d '\r'
  elif [ -r "/proc/$pid/status" ]; then
    awk '/^VmRSS:/ { rss = int($2 / 1024) } /^Threads:/ { n = $2 }
         END { if (rss) print rss, n }' "/proc/$pid/status"
  else
    python3 - "$pid" <<'PY'
import ctypes
import sys

# Public libproc structs/flavors from <sys/proc_info.h>.
class Task(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in (
        "virtual", "resident", "user", "system", "threads_user", "threads_system"
    )] + [(n, ctypes.c_int32) for n in (
        "policy", "faults", "pageins", "cow_faults", "sent", "received",
        "mach_calls", "unix_calls", "switches", "threads", "running", "priority"
    )]

class Workqueue(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in (
        "threads", "running", "blocked", "state"
    )]

class Thread(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in ("user", "system")] + [
        (n, ctypes.c_int32) for n in (
            "cpu", "policy", "state", "flags", "sleep", "curpri", "priority", "maxpri"
        )] + [("name", ctypes.c_char * 64)]

lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                            ctypes.c_void_p, ctypes.c_int]
lib.proc_pidinfo.restype = ctypes.c_int
pid = int(sys.argv[1])

def query(flavor, result, arg=0):
    return lib.proc_pidinfo(pid, flavor, arg, ctypes.byref(result),
                            ctypes.sizeof(result)) == ctypes.sizeof(result)

def is_coremedia_worker(name):
    # sample(1) identifies these as _dispatch_worker_thread pthreads,
    # distinct from the kernel's _pthread_wqthread pool reported above.
    # Never exempt arbitrary decoder or unnamed pthreads.
    prefix = b"com.apple.coremedia.sharedRootQueue."
    return name.startswith(prefix) and name[len(prefix):].isdigit()

def coremedia_workers():
    task = Task()
    if not query(4, task):
        return set()  # Unavailable: conservatively check every thread.
    ids = (ctypes.c_uint64 * (task.threads + 32))()
    # Unlike the fd-list flavor, PROC_PIDLISTTHREADS needs a buffer even
    # when asking how many entries are present.
    size = lib.proc_pidinfo(pid, 6, 0, ctypes.byref(ids), ctypes.sizeof(ids))
    if size <= 0 or size >= ctypes.sizeof(ids):
        return None  # The process changed while reading its thread list.
    workers = set()
    for thread_id in ids[:size // 8]:
        thread = Thread()
        if not query(5, thread, thread_id):  # PROC_PIDTHREADINFO
            return None
        if is_coremedia_worker(thread.name):
            workers.add(thread_id)
    return workers

# Read the task between matching system-pool snapshots to avoid
# subtracting a newly created worker from an older total.
for _ in range(3):
    before, task, after = Workqueue(), Task(), Workqueue()
    # A task need not have a system workqueue. If its query is unavailable,
    # keep zero and conservatively check every thread.
    query(12, before)
    media_before = coremedia_workers()
    if not query(4, task):
        sys.exit(1)
    media_after = coremedia_workers()
    query(12, after)
    if (before.threads == after.threads and media_before is not None
            and media_before == media_after):
        media_count = len(media_after)
        assert task.threads >= after.threads + media_count
        print(task.resident // (1024 * 1024), task.threads, after.threads, media_count)
        break
else:
    sys.exit("soak: system worker pools changed while sampling")
PY
  fi
}

end=$(($(date +%s) + minutes * 60))
while [ "$(date +%s)" -lt "$end" ]; do
  sleep "$interval"
  line="$(sample)"
  if [ -z "$line" ]; then
    echo "FAIL soak: player exited early"
    tail -20 "$log"
    exit 1
  fi
  echo "$line" >>"$samples"
done

actions="$(grep -c '^soak: ' "$log")"
opens="$(grep -c '^soak: open' "$log")"
errors="$(grep -c 'state=error\|event: error' "$log")"
awk -v actions="$actions" -v opens="$opens" -v errors="$errors" '
  {
    rss[NR] = $1
    total[NR] = $2
    workqueue[NR] = $3 + 0
    coremedia[NR] = $4 + 0
    threads[NR] = total[NR] - workqueue[NR] - coremedia[NR]
    if (workqueue[NR] || coremedia[NR]) has_system = 1
  }
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
    printf "soak: %d samples, %d actions (%d opens), memory %d -> %d MB, threads %d -> %d",
           NR, actions, opens, first, last, max_first, max_last
    if (has_system) {
      for (i = fifth + 1; i <= 2 * fifth; i++) {
        if (total[i] > raw_first) raw_first = total[i]
        if (workqueue[i] > work_first) work_first = workqueue[i]
        if (coremedia[i] > media_first) media_first = coremedia[i]
      }
      for (i = NR - fifth + 1; i <= NR; i++) {
        if (total[i] > raw_last) raw_last = total[i]
        if (workqueue[i] > work_last) work_last = workqueue[i]
        if (coremedia[i] > media_last) media_last = coremedia[i]
      }
      printf " (total %d -> %d, system workqueue %d -> %d, CoreMedia shared %d -> %d)",
             raw_first, raw_last, work_first, work_last, media_first, media_last
    }
    printf ", %d errors\n", errors
    bad = ""
    if (last > first * 1.1 + 16) bad = bad " memory grew"
    if (max_last > max_first) bad = bad " threads grew"
    if (errors > 0) bad = bad " errors"
    if (actions < 3) bad = bad " too few actions"
    if (bad != "") { print "FAIL soak:" bad; exit 1 }
    print "PASS soak"
  }' "$samples"
