#!/usr/bin/env bash
# Runs clang-tidy (.clang-tidy) on the C, C++ and Objective-C++ a branch
# changed, and reports findings on the changed lines only, as
# tool/format.sh formats only them: older code is cleaned up when someone
# edits it, not in one sweep.
#
#   tool/tidy.sh          # this branch's changes since it left main,
#                         # uncommitted and untracked files included
#   tool/tidy.sh --all    # every line of every core/src file
#
# Exits 1 when it finds anything. The compile commands come from the
# core's CMake build, configured (not built) twice into build-tidy*: with
# FFmpeg, and without FFmpeg, which compiles
# the FFmpeg-free demuxers and Apple decoders. Files this host does not
# compile (other platforms' backends) are skipped and named. A changed
# header is checked through the file of the same name next to it, when
# that exists. Takes Homebrew's LLVM, else clang-tidy from PATH.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
all=0
[ "${1:-}" = --all ] && all=1

tidy=/opt/homebrew/opt/llvm/bin/clang-tidy
[ -x "$tidy" ] || tidy="$(command -v clang-tidy || true)"
run="$(dirname "$tidy")/run-clang-tidy"
[ -x "$run" ] || run="$(command -v run-clang-tidy || true)"
if [ -z "$tidy" ] || [ -z "$run" ]; then
  echo "tidy.sh: needs clang-tidy and run-clang-tidy (brew install llvm)" >&2
  exit 1
fi

configure() {
  cmake -S "$root" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${@:2}" >/dev/null
}
configure "$root/build-tidy"
configure "$root/build-tidy-noffmpeg" -DLYCHEE_WITH_FFMPEG=OFF
mkdir -p "$root/build-tidy/merged"

if [ "$all" = 1 ]; then
  diff=""
  untracked=""
else
  base="$(git -C "$root" merge-base main HEAD)"
  exts=('*.c' '*.h' '*.cc' '*.cpp' '*.m' '*.mm')
  diff="$(git -C "$root" diff -U0 --no-color --diff-filter=d "$base" -- "${exts[@]}")"
  untracked="$(git -C "$root" ls-files --others --exclude-standard -- "${exts[@]}")"
fi

# Merges the two compile databases (first one wins) and picks the files
# to check and, unless --all, the lines (clang-tidy's -line-filter).
# Prints the line filter, then the files, one per line.
plan="$(python3 -I - "$root" "$all" "$diff" "$untracked" <<'EOF'
import json, os, re, sys

root, all_, diff, untracked = sys.argv[1], sys.argv[2] == "1", sys.argv[3], sys.argv[4]
db, seen = [], set()
for build in ("build-tidy", "build-tidy-noffmpeg"):
    for entry in json.load(open(os.path.join(root, build, "compile_commands.json"))):
        if entry["file"] not in seen:
            seen.add(entry["file"])
            db.append(entry)
with open(os.path.join(root, "build-tidy/merged/compile_commands.json"), "w") as f:
    json.dump(db, f, indent=1)

if all_:
    src = os.path.join(root, "core/src") + os.sep
    print("")
    for path in sorted(p for p in seen if p.startswith(src)):
        print(path)
    sys.exit(0)

# Changed line ranges per file, from the -U0 hunks; [] means every line.
lines = {}
path = None
for line in diff.splitlines():
    if line.startswith("+++ "):
        path = line[6:] if line.startswith("+++ b/") else None
    elif line.startswith("@@") and path:
        m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
        start, count = int(m.group(1)), int(m.group(2) or 1)
        if count:
            lines.setdefault(path, []).append([start, start + count - 1])
for path in untracked.splitlines():
    lines[path] = []

files, skipped = set(), []
for path in sorted(lines):
    full = os.path.join(root, path)
    if full in seen:
        files.add(full)
        continue
    stem, ext = os.path.splitext(full)
    sibling = next((stem + e for e in (".cc", ".mm", ".cpp", ".c")
                    if ext == ".h" and stem + e in seen), None)
    if sibling:
        files.add(sibling)
    else:
        skipped.append(path)
for path in skipped:
    print(f"tidy.sh: skipped {path}: not compiled on this host", file=sys.stderr)
filters = [{"name": p, "lines": r} if r else {"name": p} for p, r in lines.items()]
print(json.dumps(filters))
for full in sorted(files):
    print(full)
EOF
)"
line_filter="$(head -n 1 <<<"$plan")"
files=()
while IFS= read -r f; do
  [ -n "$f" ] && files+=("^$(sed 's/[][\.*^$+?(){}|]/\\&/g' <<<"$f")\$")
done < <(tail -n +2 <<<"$plan")
if [ "${#files[@]}" = 0 ]; then
  echo "tidy.sh: no changed files to check"
  exit 0
fi

args=(-clang-tidy-binary "$tidy" -p "$root/build-tidy/merged" -quiet -hide-progress)
[ -n "$line_filter" ] && args+=(-line-filter "$line_filter")
out="$root/build-tidy/merged/output.txt"
status=0
"$run" "${args[@]}" "${files[@]}" >"$out" 2>&1 || status=$?

# A header's findings come once per file that includes it: print each
# once, then a count per check.
python3 -I - "$status" "$out" <<'EOF'
import re, sys

status = int(sys.argv[1])
raw = open(sys.argv[2]).read()
diag = re.compile(r"^\S+:\d+:\d+: (warning|error): .*?(?: \[([\w.,-]+)\])?$")
blocks, block = {}, None
for line in raw.splitlines():
    if diag.match(line):
        block = blocks.setdefault(line, []) if line not in blocks else None
    elif re.match(r"^\S+:\d+:\d+: note: ", line) or line.startswith(" "):
        if block is not None:
            block.append(line)
    else:
        block = None
counts = {}
for head, body in blocks.items():
    print("\n".join([head] + body))
    check = diag.match(head).group(2) or "error"
    counts[check] = counts.get(check, 0) + 1
if blocks:
    print()
    for check, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"{n:5d} {check}", flush=True)
    print(f"tidy.sh: {len(blocks)} finding(s)", file=sys.stderr)
    sys.exit(1)
if status:
    print(raw, flush=True)
    print(f"tidy.sh: run-clang-tidy failed ({status})", file=sys.stderr)
    sys.exit(status)
print("tidy.sh: no findings")
EOF
