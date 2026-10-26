#!/usr/bin/env bash
# Formats the C, C++ and Objective-C++ lines a branch changed with
# clang-format (.clang-format: Chromium style). Only changed lines, so
# that older code is reformatted when someone edits it, not in one sweep.
#
#   tool/format.sh                 # format this branch's changes since it
#                                  # left main, uncommitted ones included
#   tool/format.sh --check [BASE [HEAD]]
#                                  # print what it would change and fail,
#                                  # without touching files (CI: the
#                                  # commits HEAD made since it left BASE)
#
# clang-format's output differs between major versions; CI pins the one
# in CLANG_FORMAT_VERSION below (pip's clang-format). Locally it takes
# git-clang-format from PATH, or Homebrew's LLVM.
set -euo pipefail

CLANG_FORMAT_VERSION=23

root="$(cd "$(dirname "$0")/.." && pwd)"
check=0
if [ "${1:-}" = --check ]; then
  check=1
  shift
fi
base="${1:-}"
head="${2:-}"

tool="$(command -v git-clang-format || true)"
if [ -z "$tool" ] && [ -x /opt/homebrew/opt/llvm/bin/git-clang-format ]; then
  tool=/opt/homebrew/opt/llvm/bin/git-clang-format
fi
if [ -z "$tool" ]; then
  echo "format.sh: needs git-clang-format (brew install llvm, or pip install clang-format)" >&2
  exit 1
fi
binary="$(dirname "$tool")/clang-format"
[ -x "$binary" ] || binary="$(command -v clang-format)"
version="$("$binary" --version | sed -E 's/.*version ([0-9]+).*/\1/')"
if [ "$version" != "$CLANG_FORMAT_VERSION" ]; then
  echo "format.sh: clang-format $version, CI uses $CLANG_FORMAT_VERSION; results may differ" >&2
fi

if [ -z "$base" ]; then
  base="$(git -C "$root" merge-base main HEAD)"
fi
args=(--binary "$binary" --extensions c,h,cc,cpp,m,mm)
if [ -n "$head" ]; then
  # Two commits: only --diff is possible. From where HEAD left BASE, so
  # that what BASE gained since is not counted.
  base="$(git -C "$root" merge-base "$base" "$head")"
  out="$(cd "$root" && "$tool" "${args[@]}" --diff "$base" "$head" -- . || true)"
elif [ "$check" = 1 ]; then
  out="$(cd "$root" && "$tool" "${args[@]}" --diff "$base" -- . || true)"
else
  # --force: the files may have unstaged changes, which it formats too.
  # It exits 1 when it changed a file, which is not a failure here.
  cd "$root" && { "$tool" "${args[@]}" --force "$base" -- . || true; }
  exit 0
fi
if grep -q '^diff ' <<<"$out"; then
  echo "$out"
  echo "format.sh: run tool/format.sh to apply the changes above" >&2
  exit 1
fi
echo "format.sh: changed lines are formatted"
