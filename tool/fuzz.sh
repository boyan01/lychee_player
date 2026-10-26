#!/usr/bin/env bash
# Fuzzes the FFmpeg-free MP4 parser (Mp4Demuxer, tx3g text) with libFuzzer
# under AddressSanitizer and UndefinedBehaviorSanitizer, in build-fuzz/.
# Needs Homebrew's LLVM (brew install llvm): Apple's clang has no libFuzzer
# runtime. Seeds are small files made with ffmpeg in every layout the
# demuxer reads; the corpus grows in build-fuzz/corpus.
#
#   tool/fuzz.sh                    # replay core/fuzz/regressions, fuzz 60 s
#   FUZZ_SECONDS=1800 JOBS=2 tool/fuzz.sh   # longer, in JOBS processes
#   tool/fuzz.sh FILE...            # replay inputs (a crash) once each
#
# Crashes, leaks and UB land in build-fuzz/artifacts/. Once fixed, add the
# input (minimized: the fuzzer with -minimize_crash=1 -runs=10000 FILE) to
# core/fuzz/regressions, which the unit tests replay.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
llvm="${LLVM:-/opt/homebrew/opt/llvm}"
build="$root/build-fuzz"
if [ ! -x "$llvm/bin/clang++" ]; then
  echo "no $llvm/bin/clang++: brew install llvm (or LLVM=...)"
  exit 1
fi
sanitizers="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE= \
  -DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++" \
  -DCMAKE_OBJCXX_COMPILER="$llvm/bin/clang++" \
  -DCMAKE_CXX_FLAGS="-g -O1 $sanitizers -fsanitize=fuzzer-no-link" \
  -DCMAKE_OBJCXX_FLAGS="-g -O1 $sanitizers" \
  -DCMAKE_EXE_LINKER_FLAGS="$sanitizers" \
  -DLYCHEE_WITH_FFMPEG=OFF -DLYCHEE_BUILD_EXAMPLES=OFF -DLYCHEE_BUILD_TESTS=OFF \
  -DLYCHEE_BUILD_FUZZERS=ON -DLYCHEE_FUZZ_LIBFUZZER=ON >/dev/null || exit 1
cmake --build "$build" --target lychee_mp4_fuzzer >/dev/null || {
  cmake --build "$build" --target lychee_mp4_fuzzer 2>&1 | grep -E "error|warning" | head -20
  exit 1
}
fuzzer="$build/core/fuzz/lychee_mp4_fuzzer"
export ASAN_OPTIONS="detect_leaks=1:allocator_may_return_null=1"
# libFuzzer's own threads outlive the fork-mode parent: not ours (the
# inputs run in child processes, whose stacks have neither frame).
printf 'leak:fuzzer::StartRssThread\nleak:fuzzer::FuzzWithFork\n' >"$build/lsan.supp"
export LSAN_OPTIONS="suppressions=$build/lsan.supp"
export UBSAN_OPTIONS="print_stacktrace=1"

if [ $# -gt 0 ]; then
  exec "$fuzzer" -runs=1 "$@"
fi

# Seeds: a second of tiny video and audio in each layout.
seeds="$build/seeds"
mkdir -p "$seeds" "$build/corpus" "$build/artifacts"
tiny=(-f lavfi -i testsrc2=size=64x48:rate=10 -f lavfi -i sine=frequency=440:sample_rate=8000 -t 1)
seed() {
  local out="$seeds/$1"; shift
  [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y "$@" "$out" || rm -f "$out"
}
h264=(-c:v libx264 -preset ultrafast -g 5 -pix_fmt yuv420p)
seed plain.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac
seed faststart.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags +faststart
seed frag.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags frag_keyframe+empty_moov+default_base_moof
seed frag_moov.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags frag_keyframe
seed frag_sidx.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac \
  -movflags frag_keyframe+empty_moov+default_base_moof+global_sidx
seed frag_dash.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac \
  -movflags frag_keyframe+empty_moov+default_base_moof+dash+skip_trailer
seed frag_scan.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -frag_duration 300000 \
  -movflags empty_moov+default_base_moof+separate_moof+skip_trailer
seed ismv.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -f ismv
seed negative_cts.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags negative_cts_offsets
seed rotated.mp4 -display_rotation 90 -i "$seeds/plain.mp4" -c copy
seed hevc.mp4 "${tiny[@]}" -c:v libx265 -preset ultrafast -x265-params log-level=error -tag:v hvc1 \
  -c:a alac
seed meta.m4a -f lavfi -i sine=frequency=440:sample_rate=8000 -t 1 -c:a aac -metadata title=Seed
[ -f "$seeds/subs.srt" ] || printf '1\n00:00:00,100 --> 00:00:00,600\nHi <b>there</b>\n' >"$seeds/subs.srt"
seed mov_text.mp4 -i "$seeds/plain.mp4" -i "$seeds/subs.srt" -map 0 -map 1 -c:v copy -c:a copy -c:s mov_text
seed mov_text_frag.mp4 -i "$seeds/mov_text.mp4" -map 0 -c copy -movflags frag_keyframe+empty_moov

# The inputs that crashed once first, then fuzz.
regressions=("$root/core/fuzz/regressions"/*)
if [ -e "${regressions[0]}" ]; then
  echo "replaying ${#regressions[@]} regression inputs"
  "$fuzzer" -runs=1 "${regressions[@]}" >"$build/regressions.log" 2>&1 || {
    echo "FAIL a regression input:"; tail -30 "$build/regressions.log"; exit 1
  }
fi
jobs="${JOBS:-1}"
seconds="${FUZZ_SECONDS:-60}"
args=(-max_len=65536 -timeout=10 -rss_limit_mb=2048 -max_total_time="$seconds"
  -artifact_prefix="$build/artifacts/" -print_final_stats=1)
if [ "$jobs" -gt 1 ]; then
  # Fork mode keeps going after a crash, collecting every artifact.
  args+=(-fork="$jobs" -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1)
fi
echo "fuzzing for ${seconds} s in $jobs process(es); artifacts in $build/artifacts"
"$fuzzer" "${args[@]}" "$build/corpus" "$seeds" "$root/core/fuzz/regressions" >"$build/fuzz.log" 2>&1
status=$?
grep -E "^(#[0-9]+.*(DONE|INITED)|stat::|INFO: fuzzed for|==[0-9]+==ERROR|SUMMARY)" "$build/fuzz.log" | tail -15
artifacts="$(ls "$build/artifacts" | wc -l | tr -d ' ')"
echo "fuzzer exit $status, $artifacts artifact(s)"
[ "$status" = 0 ] && [ "$artifacts" = 0 ]
