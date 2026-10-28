#!/usr/bin/env bash
# Fuzzes the FFmpeg-free parsers with libFuzzer under AddressSanitizer and
# UndefinedBehaviorSanitizer, in build-fuzz/. One target per parser
# (core/fuzz/fuzz_targets.h):
#
#   mp4           Mp4Demuxer, with tx3g text
#   matroska      MatroskaDemuxer, with its text subtitles
#   mpegts        TsDemuxer (TsReader, TsParser, the elementary streams)
#   hls_playlist  the HLS playlist parser
#   hls           HlsDemuxer on a playlist and its segments in one input
#
# Needs clang with libFuzzer: Homebrew's LLVM on macOS (brew install llvm;
# Apple's clang has no libFuzzer runtime), clang on Linux (or LLVM=...).
# Seeds are small files made with ffmpeg in the layouts each parser
# reads; the corpus grows in build-fuzz/corpus/TARGET.
#
#   tool/fuzz.sh                    # replay core/fuzz/regressions, then
#                                   # fuzz each target 60 s
#   FUZZ_TARGET='matroska hls' tool/fuzz.sh   # only these targets
#   FUZZ_SECONDS=1800 JOBS=2 tool/fuzz.sh     # longer, in JOBS processes
#   FUZZ_TARGET=mp4 tool/fuzz.sh FILE...      # replay inputs (a crash)
#
# Crashes, leaks, OOMs, timeouts and UB land in build-fuzz/artifacts/TARGET.
# Once fixed, add the input (minimized: the fuzzer with
# -minimize_crash=1 -runs=10000 FILE) to core/fuzz/regressions/TARGET,
# which the unit tests replay.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-fuzz"
all_targets=(mp4 matroska mpegts hls_playlist hls)
read -r -a targets <<<"${FUZZ_TARGET:-${all_targets[*]}}"
for target in "${targets[@]}"; do
  case " ${all_targets[*]} " in *" $target "*) ;; *)
    echo "unknown FUZZ_TARGET $target (${all_targets[*]})"; exit 1 ;;
  esac
done

if [ -n "${LLVM:-}" ]; then
  cc="$LLVM/bin/clang" cxx="$LLVM/bin/clang++"
elif [ "$(uname)" = Darwin ]; then
  cc=/opt/homebrew/opt/llvm/bin/clang cxx=/opt/homebrew/opt/llvm/bin/clang++
else
  cc="$(command -v clang)" cxx="$(command -v clang++)"
fi
if [ ! -x "$cxx" ]; then
  echo "no clang++ with libFuzzer: brew install llvm, apt install clang (or LLVM=...)"
  exit 1
fi
sanitizers="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
# Apple also compiles the http(s) client, Objective-C++.
objcxx=()
mkdir -p "$build"
[ "$(uname)" != Darwin ] ||
  objcxx=(-DCMAKE_OBJCXX_COMPILER="$cxx" -DCMAKE_OBJCXX_FLAGS="-g -O1 $sanitizers")
cmake -S "$root/core/fuzz" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE= \
  -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" ${objcxx[@]+"${objcxx[@]}"} \
  -DCMAKE_CXX_FLAGS="-g -O1 $sanitizers -fsanitize=fuzzer-no-link" \
  -DCMAKE_EXE_LINKER_FLAGS="$sanitizers" -DLYCHEE_FUZZ_LIBFUZZER=ON >"$build/cmake.log" 2>&1 || {
  cat "$build/cmake.log"; exit 1
}
if grep -q "Warning" "$build/cmake.log"; then cat "$build/cmake.log"; exit 1; fi
executables=()
for target in "${targets[@]}"; do executables+=("lychee_${target}_fuzzer"); done
cmake --build "$build" --target "${executables[@]}" >"$build/build.log" 2>&1 || {
  grep -E "error|warning" "$build/build.log" | head -20
  exit 1
}
if grep -q "warning:" "$build/build.log"; then
  grep "warning:" "$build/build.log" | head -20
  exit 1
fi
export ASAN_OPTIONS="detect_leaks=1:allocator_may_return_null=1"
# libFuzzer's own threads outlive the fork-mode parent: not ours (the
# inputs run in child processes, whose stacks have neither frame).
printf 'leak:fuzzer::StartRssThread\nleak:fuzzer::FuzzWithFork\n' >"$build/lsan.supp"
export LSAN_OPTIONS="suppressions=$build/lsan.supp"
export UBSAN_OPTIONS="print_stacktrace=1"
# Older sanitizer runtimes (clang 14, 15) crash at random on kernels with
# 32 bits of mmap randomness (WSL2's 6.18): run without ASLR on Linux.
run=()
[ "$(uname)" != Linux ] || ! command -v setarch >/dev/null || run=(setarch "$(uname -m)" -R)

if [ $# -gt 0 ]; then
  if [ "${#targets[@]}" != 1 ]; then
    echo "replaying needs one FUZZ_TARGET (${all_targets[*]})"
    exit 1
  fi
  exec ${run[@]+"${run[@]}"} "$build/lychee_${targets[0]}_fuzzer" -runs=1 "$@"
fi

# Seeds: a second of tiny video and audio in each layout.
seeds="$build/seeds"
media="$seeds/media"
mkdir -p "$media"
command -v ffmpeg >/dev/null || echo "no ffmpeg: fuzzing without seeds"
tiny=(-f lavfi -i testsrc2=size=64x48:rate=10 -f lavfi -i sine=frequency=440:sample_rate=8000 -t 1)
tiny48k=(-f lavfi -i testsrc2=size=64x64:rate=10 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1)
h264=(-c:v libx264 -preset ultrafast -g 5 -pix_fmt yuv420p)
hevc=(-c:v libx265 -preset ultrafast -x265-params log-level=error -g 5)
# seed TARGET NAME FFMPEG-ARGS...: seeds/TARGET/NAME, if it can be made.
seed() {
  local out="$seeds/$1/$2"; shift 2
  mkdir -p "$(dirname "$out")"
  [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y "$@" "$out" 2>/dev/null || rm -f "$out"
}
[ -f "$media/subs.srt" ] ||
  printf '1\n00:00:00,100 --> 00:00:00,600\nHi <b>there</b>\n\n2\n00:00:00,700 --> 00:00:00,900\n{\\an8}Up\n' \
    >"$media/subs.srt"

seeds_mp4() {
  seed mp4 plain.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac
  seed mp4 faststart.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags +faststart
  seed mp4 frag.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags frag_keyframe+empty_moov+default_base_moof
  seed mp4 frag_moov.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags frag_keyframe
  seed mp4 frag_sidx.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac \
    -movflags frag_keyframe+empty_moov+default_base_moof+global_sidx
  seed mp4 frag_dash.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac \
    -movflags frag_keyframe+empty_moov+default_base_moof+dash+skip_trailer
  seed mp4 frag_scan.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -frag_duration 300000 \
    -movflags empty_moov+default_base_moof+separate_moof+skip_trailer
  seed mp4 ismv.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -f ismv
  seed mp4 negative_cts.mp4 "${tiny[@]}" "${h264[@]}" -c:a aac -movflags negative_cts_offsets
  seed mp4 rotated.mp4 -display_rotation 90 -i "$seeds/mp4/plain.mp4" -c copy
  seed mp4 hevc.mp4 "${tiny[@]}" "${hevc[@]}" -tag:v hvc1 -c:a alac
  seed mp4 meta.m4a -f lavfi -i sine=frequency=440:sample_rate=8000 -t 1 -c:a aac -metadata title=Seed
  seed mp4 mov_text.mp4 -i "$seeds/mp4/plain.mp4" -i "$media/subs.srt" -map 0 -map 1 -c:v copy -c:a copy \
    -c:s mov_text
  seed mp4 mov_text_frag.mp4 -i "$seeds/mp4/mov_text.mp4" -map 0 -c copy -movflags frag_keyframe+empty_moov
}

seeds_matroska() {
  seed matroska h264_aac.mkv "${tiny[@]}" "${h264[@]}" -c:a aac -metadata title=Seed
  seed matroska hevc_flac.mkv "${tiny[@]}" "${hevc[@]}" -c:a flac
  seed matroska h264_mp3.mkv "${tiny[@]}" "${h264[@]}" -c:a libmp3lame
  seed matroska vp9_opus.webm "${tiny48k[@]}" -c:v libvpx-vp9 -deadline realtime -g 5 -c:a libopus
  seed matroska av1_opus.mkv "${tiny48k[@]}" -c:v libsvtav1 -preset 12 -g 5 -c:a libopus
  seed matroska live.webm "${tiny48k[@]}" -c:v libvpx-vp9 -deadline realtime -g 5 -c:a libopus -live 1
  seed matroska clusters.mkv "${tiny[@]}" "${h264[@]}" -c:a aac -cluster_time_limit 100
  seed matroska audio.mka -f lavfi -i sine=frequency=440:sample_rate=8000 -t 1 -c:a aac
  local codec
  for codec in srt ass webvtt; do
    seed matroska "sub_$codec.mkv" -i "$seeds/matroska/h264_aac.mkv" -i "$media/subs.srt" -map 0 -map 1 \
      -c:v copy -c:a copy -c:s "$codec"
  done
  # A Tags element and an attachment (the subtitles' "font").
  seed matroska attachment.mkv -i "$seeds/matroska/sub_ass.mkv" -map 0 -c copy -attach "$media/subs.srt" \
    -metadata:s:t mimetype=font/ttf -metadata:s:t filename=font.ttf -metadata artist=Seed
  # The layouts tool/mkv_cases.py makes for tool/regression.sh.
  local mode
  for mode in no-cues unknown-sizes huge-tracks huge-block corrupt truncated; do
    local out="$seeds/matroska/mkv_${mode//-/_}.mkv"
    [ -f "$out" ] || [ ! -f "$seeds/matroska/clusters.mkv" ] ||
      python3 -I "$root/tool/mkv_cases.py" "$mode" "$seeds/matroska/clusters.mkv" "$out" >/dev/null 2>&1 ||
      rm -f "$out"
  done
}

seeds_mpegts() {
  seed mpegts h264_aac.ts "${tiny[@]}" "${h264[@]}" -c:a aac
  seed mpegts hevc_aac.ts "${tiny[@]}" "${hevc[@]}" -c:a aac
  seed mpegts h264_mp3.ts "${tiny[@]}" "${h264[@]}" -c:a libmp3lame
  seed mpegts ac3.ts -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:a ac3 -f mpegts
  seed mpegts eac3.ts -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:a eac3 -f mpegts
  seed mpegts m2ts.m2ts "${tiny[@]}" "${h264[@]}" -c:a aac -f mpegts -mpegts_m2ts_mode 1
  # The 33-bit timestamps wrap half a second in.
  seed mpegts wrap.ts "${tiny[@]}" "${h264[@]}" -c:a aac -output_ts_offset 95443.2
  seed mpegts resend.ts "${tiny[@]}" "${h264[@]}" -c:a aac -mpegts_flags resend_headers -pat_period 0.1
  # A second program version on other PIDs, as a rendition switch.
  seed mpegts other_pids.ts "${tiny[@]}" "${h264[@]}" -c:a aac -mpegts_start_pid 0x300 \
    -mpegts_pmt_start_pid 0x1100
  local out="$seeds/mpegts/switch.ts"
  [ -f "$out" ] || cat "$seeds/mpegts/h264_aac.ts" "$seeds/mpegts/other_pids.ts" >"$out" 2>/dev/null ||
    rm -f "$out"
}

# Playlists, for both HLS targets.
playlists() {
  local dir="$seeds/hls_playlist"
  mkdir -p "$dir"
  printf '%s\n' '#EXTM3U' '#EXT-X-VERSION:3' '#EXT-X-TARGETDURATION:1' '#EXT-X-MEDIA-SEQUENCE:0' \
    '#EXTINF:0.5,' seg0.ts '#EXTINF:0.5,' seg1.ts '#EXT-X-ENDLIST' >"$dir/media.m3u8"
  printf '%s\n' '#EXTM3U' \
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="en",URI="audio.m3u8"' \
    '#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=1920x1080,CODECS="vp09.00.40.08,mp4a.40.2"' vp9.m3u8 \
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.64001e,mp4a.40.2",AUDIO="a"' \
    low/index.m3u8 \
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=64x48' media.m3u8 >"$dir/master.m3u8"
  printf '%s\r\n' $'\xEF\xBB\xBF#EXTM3U' '#EXT-X-PLAYLIST-TYPE:VOD' '#EXT-X-TARGETDURATION:6' \
    '#EXT-X-MEDIA-SEQUENCE:100' '#EXT-X-DISCONTINUITY-SEQUENCE:7' '#EXTINF:5.005,title, with comma' \
    '#EXT-X-BYTERANGE:1000@200' ../media.ts '#EXTINF:4.5,' '#EXT-X-BYTERANGE:500' ../media.ts \
    '#EXT-X-DISCONTINUITY' '#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x1' '#EXTINF:6,' \
    '//cdn.example/next.ts?x=1' '#EXT-X-KEY:METHOD=NONE' '#EXT-X-MAP:URI="init.mp4"' '#EXTINF:6.,' \
    'https://o.example/a/./b/../last.ts#f' >"$dir/tags.m3u8"
}

# seeds/hls/NAME: the playlist and its resources (fuzz_targets.h).
hls_seed() {
  local out="$seeds/hls/$1"; shift
  mkdir -p "$seeds/hls"
  local first=1 part
  : >"$out"
  for part in "$@"; do
    [ -f "$part" ] || { rm -f "$out"; return; }
    [ "$first" = 1 ] || printf '\n#PART\n' >>"$out"
    cat "$part" >>"$out"
    first=0
  done
}

seeds_hls() {
  playlists
  local segs="$media/hls"
  mkdir -p "$segs"
  [ -f "$segs/seg1.ts" ] || ffmpeg -hide_banner -loglevel error -y "${tiny[@]}" "${h264[@]}" -c:a aac \
    -f segment -segment_time 0.5 -segment_format mpegts "$segs/seg%d.ts" 2>/dev/null
  [ -f "$segs/later.ts" ] || ffmpeg -hide_banner -loglevel error -y "${tiny[@]}" "${h264[@]}" -c:a aac \
    -t 0.5 -output_ts_offset 100 -f mpegts "$segs/later.ts" 2>/dev/null
  local pl="$seeds/hls_playlist"
  hls_seed media "$pl/media.m3u8" "$segs/seg0.ts" "$segs/seg1.ts"
  # The master: its H.264 variant, then that playlist's segments.
  printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXTINF:0.5,' a.ts '#EXTINF:0.5,' b.ts \
    >"$media/variant.m3u8"
  hls_seed master "$pl/master.m3u8" "$media/variant.m3u8" "$segs/seg0.ts" "$segs/seg1.ts"
  if [ -f "$segs/seg1.ts" ]; then
    cat "$segs/seg0.ts" "$segs/seg1.ts" >"$media/both.ts"
    local size0
    size0="$(wc -c <"$segs/seg0.ts" | tr -d ' ')"
    printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXT-X-PLAYLIST-TYPE:VOD' '#EXTINF:0.5,' \
      "#EXT-X-BYTERANGE:$size0@0" both.ts '#EXTINF:0.5,' \
      "#EXT-X-BYTERANGE:$(wc -c <"$segs/seg1.ts" | tr -d ' ')" both.ts >"$media/byterange.m3u8"
    hls_seed byterange "$media/byterange.m3u8" "$media/both.ts"
  fi
  printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXT-X-MEDIA-SEQUENCE:5' '#EXTINF:0.5,' seg0.ts \
    '#EXT-X-DISCONTINUITY' '#EXTINF:0.5,' later.ts '#EXTINF:0.5,' seg1.ts >"$media/discontinuity.m3u8"
  hls_seed discontinuity "$media/discontinuity.m3u8" "$segs/seg0.ts" "$segs/later.ts" "$segs/seg1.ts"
  # Live: the playlist, its segment, the reload with one more and the end.
  printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXT-X-MEDIA-SEQUENCE:0' '#EXTINF:0.5,' seg0.ts \
    >"$media/live0.m3u8"
  { cat "$media/live0.m3u8"; printf '%s\n' '#EXTINF:0.5,' seg1.ts '#EXT-X-ENDLIST'; } >"$media/live1.m3u8"
  hls_seed live "$media/live0.m3u8" "$segs/seg0.ts" "$media/live1.m3u8" "$segs/seg1.ts"
  # Fragmented MP4: the map before the first segment.
  [ -f "$segs/frag1.m4s" ] || ffmpeg -hide_banner -loglevel error -y "${tiny[@]}" "${h264[@]}" -c:a aac \
    -f hls -hls_time 0.5 -hls_playlist_type vod -hls_segment_type fmp4 -hls_fmp4_init_filename init.mp4 \
    -hls_segment_filename "$segs/frag%d.m4s" "$segs/fmp4.m3u8" 2>/dev/null
  hls_seed fmp4 "$segs/fmp4.m3u8" "$segs/init.mp4" "$segs/frag0.m4s" "$segs/frag1.m4s"
  # AES-128 with the sequence number as the IV: a segment, then its key.
  if [ -f "$segs/seg1.ts" ] && command -v openssl >/dev/null; then
    printf '0123456789abcdef' >"$media/key.bin"
    local i
    for i in 0 1; do
      openssl aes-128-cbc -K 30313233343536373839616263646566 -iv "$(printf '%032x' "$i")" \
        -in "$segs/seg$i.ts" -out "$media/aes$i.ts"
    done
    printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXT-X-KEY:METHOD=AES-128,URI="key.bin"' \
      '#EXTINF:0.5,' aes0.ts '#EXTINF:0.5,' aes1.ts '#EXT-X-ENDLIST' >"$media/aes.m3u8"
    hls_seed aes "$media/aes.m3u8" "$media/aes0.ts" "$media/key.bin" "$media/aes1.ts"
  fi
  # An audio rendition: the variant's playlist and segment, the
  # rendition's playlist and segment.
  [ -f "$segs/audio.ts" ] || ffmpeg -hide_banner -loglevel error -y "${tiny[@]}" -vn -c:a aac -t 0.5 \
    -f mpegts "$segs/audio.ts" 2>/dev/null
  printf '%s\n' '#EXTM3U' '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="en",DEFAULT=YES,URI="audio.m3u8"' \
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS="avc1.64001e,mp4a.40.2",AUDIO="a"' video.m3u8 \
    '#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=64x48' low.m3u8 >"$media/renditions.m3u8"
  printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:1' '#EXTINF:0.5,' a.ts '#EXT-X-ENDLIST' \
    >"$media/one.m3u8"
  hls_seed renditions "$media/renditions.m3u8" "$media/one.m3u8" "$segs/seg0.ts" "$media/one.m3u8" \
    "$segs/audio.ts"
}

if command -v ffmpeg >/dev/null; then
  for target in "${targets[@]}"; do
    case "$target" in
      hls_playlist) playlists ;;
      *) "seeds_$target" ;;
    esac
  done
fi

jobs="${JOBS:-1}"
seconds="${FUZZ_SECONDS:-60}"
failed=0
for target in "${targets[@]}"; do
  fuzzer="$build/lychee_${target}_fuzzer"
  artifacts="$build/artifacts/$target"
  regressions="$root/core/fuzz/regressions/$target"
  mkdir -p "$build/corpus/$target" "$artifacts" "$seeds/$target"
  dirs=("$build/corpus/$target" "$seeds/$target")
  # The inputs that crashed once first.
  inputs=("$regressions"/*)
  if [ -e "${inputs[0]}" ]; then
    dirs+=("$regressions")
    echo "$target: replaying ${#inputs[@]} regression inputs"
    ${run[@]+"${run[@]}"} "$fuzzer" -runs=1 "${inputs[@]}" >"$build/regressions-$target.log" 2>&1 || {
      echo "FAIL $target: a regression input:"; tail -30 "$build/regressions-$target.log"
      failed=1
      continue
    }
  fi
  args=(-max_len=65536 -timeout=10 -rss_limit_mb=2048 -max_total_time="$seconds"
    -artifact_prefix="$artifacts/" -print_final_stats=1)
  case "$target" in hls*) args+=(-dict="$root/core/fuzz/hls.dict") ;; esac
  if [ "$jobs" -gt 1 ]; then
    # Fork mode keeps going after a crash, collecting every artifact.
    args+=(-fork="$jobs" -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1)
  fi
  echo "$target: fuzzing for ${seconds} s in $jobs process(es); artifacts in $artifacts"
  ${run[@]+"${run[@]}"} "$fuzzer" "${args[@]}" "${dirs[@]}" >"$build/fuzz-$target.log" 2>&1
  status=$?
  grep -E "^(#[0-9]+.*(DONE|INITED)|stat::|INFO: fuzzed for|==[0-9]+==ERROR|SUMMARY)" \
    "$build/fuzz-$target.log" | tail -15
  found="$(find "$artifacts" -type f | wc -l | tr -d ' ')"
  echo "$target: fuzzer exit $status, $found artifact(s)"
  [ "$status" = 0 ] && [ "$found" = 0 ] || failed=1
done
exit "$failed"
