#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I"$here/fakes" -I"$here/../../main/cpp" \
  "$here/surface_frame_test.cc" -o "$output/surface_frame_test"
"$output/surface_frame_test"
