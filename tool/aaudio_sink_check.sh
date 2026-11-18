#!/usr/bin/env bash
# Runs the real AAudio sink with NDK declarations and fake AAudio calls.
# Usage: tool/aaudio_sink_check.sh <Android NDK directory>
set -euo pipefail

if [ "$#" != 1 ] || [ ! -d "$1" ]; then
  echo "usage: tool/aaudio_sink_check.sh <Android NDK directory>" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
ndk="$(cd "$1" && pwd)"
headers=("$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/include/aaudio)
if [ ! -f "${headers[0]}/AAudio.h" ]; then
  echo "AAudio.h not found in $ndk" >&2
  exit 2
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
# Use the host C/C++ headers and put only the AAudio declarations on the
# include path. The NDK's API availability annotation is Android-specific.
ln -s "${headers[0]}" "$scratch/aaudio"
cd "$root"
"${CXX:-clang++}" -std=c++20 -pthread -Wall -Wextra -Werror \
  '-D__INTRODUCED_IN(x)=' -isystem "$scratch" -Icore/src \
  core/test/main.cc core/test/aaudio_sink_test.cc \
  core/src/backends/android/aaudio_sink.cc core/src/base/{log,time}.cc \
  -o "$scratch/aaudio_sink_test"
"$scratch/aaudio_sink_test"
