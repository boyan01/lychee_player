#!/usr/bin/env bash
# Names the areas a change touches, from the files it changes, so that it
# runs only the checks those areas need (AGENTS.md, "Verifying a change";
# CI gates its jobs on the same areas).
#
#   tool/changes.sh               # this branch against main, uncommitted
#                                 # and untracked files included
#   tool/changes.sh BASE [HEAD]   # the files HEAD changed since it left BASE
#   tool/changes.sh --github BASE HEAD   # AREA=true|false lines, for
#                                 # $GITHUB_OUTPUT; every area when BASE
#                                 # is empty, all zeros or not related
#
# Areas:
#   core       the library and its CLI: unit tests, tool/regression.sh
#   threads    the pipeline, the C API, sinks: also TSAN=1
#   parsers    the FFmpeg-free demuxers: also NO_FFMPEG=1 and tool/fuzz.sh
#   apple android windows linux
#              a platform's backends, SDK or plugin side: that platform's
#              native harness (and its CI job where there is one)
#   abi        lychee.h: every binding (rust, apple, android, flutter)
#   rust       the Rust crates
#   flutter    the Flutter plugin or its example
#   packaging  releases and prebuilt pins: tool/release.sh check
#   ci         the CI workflow or this script: every CI job
# Documentation alone touches no area.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
areas=(core threads parsers apple android windows linux abi rust flutter packaging ci)

github=0
if [ "${1:-}" = --github ]; then
  github=1
  shift
fi
base="${1:-}"
head="${2:-HEAD}"

all() {
  local area
  for area in "${areas[@]}"; do echo "$area=true"; done
}

if [ "$github" = 1 ]; then
  if [ -z "$base" ] || [ -z "${base//0/}" ] ||
     ! git -C "$root" merge-base "$base" "$head" >/dev/null 2>&1; then
    all
    exit 0
  fi
fi
# From where HEAD left BASE, so that what BASE gained since is not counted.
if [ -n "$base" ]; then
  files="$(git -C "$root" diff --name-only "$base...$head")"
else
  files="$(git -C "$root" diff --name-only "$(git -C "$root" merge-base main HEAD)";
           git -C "$root" ls-files --others --exclude-standard)"
fi

# Space-separated, so that macOS's bash 3.2 runs this too.
hit=" "
mark() {
  local area
  for area in "$@"; do
    case "$hit" in *" $area "*) ;; *) hit="$hit$area " ;; esac
  done
}
touched() {
  case "$hit" in *" $1 "*) return 0 ;; *) return 1 ;; esac
}
while IFS= read -r f; do
  [ -n "$f" ] || continue
  case "$f" in
    *.md | docs/* | LICENSE | THIRD_PARTY_NOTICES | */LICENSE | .gitignore) ;;
    .github/workflows/ci.yml | tool/changes.sh) mark ci ;;
    .github/workflows/*) mark packaging ;;
    core/include/lychee/lychee.h) mark core threads abi rust apple android flutter ;;
    core/src/pipeline/* | core/src/api/* | core/src/base/*) mark core threads ;;
    core/src/backends/mp4/* | core/src/backends/matroska/* | core/src/backends/mp2t/* | \
      core/src/backends/hls/* | core/src/media/probing_demuxer.* | core/fuzz/* | tool/fuzz.sh)
      mark core parsers ;;
    core/src/backends/apple/*) mark core apple ;;
    core/src/backends/android/*) mark core android ;;
    core/src/backends/windows/* | core/src/backends/media_foundation/*) mark core windows ;;
    core/src/backends/linux/*) mark core linux ;;
    # Every platform's shipped build has FFmpeg and libass but Android's,
    # which has libass only; the factory picks backends per platform.
    core/src/backends/ffmpeg/*) mark core apple windows linux ;;
    core/src/backends/libass/*) mark core apple android ;;
    core/src/media/factory.cc) mark core parsers apple android windows linux ;;
    core/* | CMakeLists.txt | examples/cli/* | tool/regression.sh | tool/*.py) mark core ;;
    examples/glfw/main_win*) mark windows ;;
    examples/glfw/main_linux*) mark linux ;;
    examples/glfw/*) mark apple ;;
    rust/*) mark rust ;;
    sdk/apple/*) mark apple ;;
    sdk/android/*) mark android ;;
    tool/release.sh | flutter/prebuilt.json | Package.swift | flutter/darwin/*.podspec | \
      flutter/android/build.gradle* | flutter/windows/ffmpeg.cmake | flutter/pubspec.yaml)
      mark packaging ;;
    tool/build_apple.sh | tool/apple_framework.sh | tool/package_ffmpeg_apple.sh) mark apple ;;
    tool/build_ffmpeg.sh) mark apple android windows linux ;;
    tool/build_libass.sh) mark apple android ;;
    tool/rust_bindings.sh) mark rust ;;
    flutter/darwin/*) mark flutter apple ;;
    flutter/android/*) mark flutter android ;;
    flutter/windows/*) mark flutter windows ;;
    flutter/linux/*) mark flutter linux ;;
    flutter/*) mark flutter ;;
    # Anything else is new: check the core rather than nothing.
    *) mark core ;;
  esac
done <<<"$files"

if [ "$github" = 1 ]; then
  if touched ci; then
    all
    exit 0
  fi
  for area in "${areas[@]}"; do
    echo "$area=$(touched "$area" && echo true || echo false)"
  done
else
  for area in "${areas[@]}"; do
    ! touched "$area" || echo "$area"
  done
fi
