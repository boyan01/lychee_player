#!/usr/bin/env bash
# Builds the core, runs its unit tests, and plays a fixed media matrix
# through lychee_cli.
# Prints one PASS/FAIL line per case; exits non-zero if any case fails.
#
#   tool/regression.sh            # Debug build in build/
#   TSAN=1 tool/regression.sh     # ThreadSanitizer build in build-tsan/
#   ASAN=1 tool/regression.sh     # AddressSanitizer and UndefinedBehavior-
#                                 # Sanitizer build in build-asan/
#   COVERAGE=1 tool/regression.sh # Line coverage of core/src by the unit
#                                 # tests and cases, in build-coverage/
#                                 # (clang; LLVM_COV, LLVM_PROFDATA)
#   MEDIA_DIR=... tool/regression.sh
#   NO_HW=1 tool/regression.sh    # Software decoding only (CI runners)
#   REALTIME_ADVISORY=1 tool/regression.sh  # The realtime cases' drift,
#                                 # glitches and drops only warn (CI's VMs)
#   ONLY='seek-* rate-2x' tool/regression.sh   # Only cases matching a glob
#   JOBS=1 tool/regression.sh     # Cases one at a time (default: half the
#                                 # cores, a quarter under TSan)
#   FFMPEG_ROOT=third_party/ffmpeg/macos-arm64 tool/regression.sh
#                                 # The shipped FFmpeg (tool/build_ffmpeg.sh)
#                                 # instead of the system's, in build-lgpl/
#   LIBASS_ROOT=third_party/libass/macos-arm64 tool/regression.sh
#                                 # The shipped libass (tool/build_libass.sh)
#   NO_FFMPEG=1 tool/regression.sh  # macOS without FFmpeg, in build-noffmpeg/:
#                                 # only the cases with MP4, Matroska,
#                                 # WebM, MPEG-TS and HLS media, text subtitles
#   MEDIA_ONLY=1 tool/regression.sh # Only generate the media (for the Swift
#                                 # and Rust tests), no build
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
media="${MEDIA_DIR:-/tmp/lychee-media}"
if [ "${TSAN:-0}" = 1 ]; then
  build="$root/build-tsan"
  flags=(-DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread)
elif [ "${ASAN:-0}" = 1 ]; then
  # Every language of the project's own code, so the Objective-C++
  # backends are checked too. FFmpeg and libass are not instrumented.
  build="$root/build-asan"
  asan="-fsanitize=address,undefined -fno-omit-frame-pointer"
  flags=()
  for lang in C CXX OBJCXX; do
    flags+=("-DCMAKE_${lang}_FLAGS=$asan")
  done
  flags+=("-DCMAKE_EXE_LINKER_FLAGS=$asan" "-DCMAKE_SHARED_LINKER_FLAGS=$asan")
elif [ "${COVERAGE:-0}" = 1 ]; then
  build="$root/build-coverage"
  coverage="-fprofile-instr-generate -fcoverage-mapping"
  flags=()
  for lang in C CXX OBJCXX; do
    flags+=("-DCMAKE_${lang}_FLAGS=$coverage")
  done
  flags+=("-DCMAKE_EXE_LINKER_FLAGS=$coverage" "-DCMAKE_SHARED_LINKER_FLAGS=$coverage")
else
  build="$root/build"
  flags=()
fi
if [ -n "${FFMPEG_ROOT:-}" ]; then
  build="$build-lgpl"
  flags+=(-DLYCHEE_FFMPEG_ROOT="$(cd "$FFMPEG_ROOT" && pwd)")
fi
if [ "${NO_FFMPEG:-0}" = 1 ]; then
  build="$build-noffmpeg"
  flags+=(-DLYCHEE_WITH_FFMPEG=OFF)
fi

# Styled subtitles through the libass the apps ship
# (LIBASS_ROOT=third_party/libass/macos-arm64, tool/build_libass.sh), or
# where libass is installed; plain text ASS otherwise. Always passed, so
# CMake's cache does not keep the last root.
flags+=(-DLYCHEE_LIBASS_ROOT="$([ -z "${LIBASS_ROOT:-}" ] || { cd "$LIBASS_ROOT" && pwd; })")
if [ -n "${LIBASS_ROOT:-}" ] || pkg-config --exists libass 2>/dev/null; then
  flags+=(-DLYCHEE_WITH_LIBASS=ON)
  libass=1
else
  flags+=(-DLYCHEE_WITH_LIBASS=OFF)
  libass=0
fi

# Test media, generated once. Synthetic so the expected output is known.
mkdir -p "$media"
gen() {
  local out="$media/$1"; shift
  # A failed run must not leave a partial file for the next run to reuse.
  [ -f "$out" ] || ffmpeg -hide_banner -loglevel error -y "$@" "$out" || { rm -f "$out"; exit 1; }
}
src="testsrc2=size=1920x1080:rate=30"
gen h264_aac.mp4 -f lavfi -i "$src" -f lavfi -i sine=frequency=440:sample_rate=44100 -t 12 \
  -c:v libx264 -preset veryfast -bf 3 -pix_fmt yuv420p -c:a aac -b:a 128k
gen hevc10.mp4 -f lavfi -i testsrc2=size=1920x1080:rate=24 -f lavfi -i sine=frequency=660:sample_rate=48000 -t 10 \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -tag:v hvc1 -x265-params log-level=error -c:a aac
# HDR10: mid gray in PQ, about 92 nits. Tone-mapped to SDR it is light
# gray (~182), not the 50% gray its code values would give as SDR.
gen hdr_pq.mp4 -f lavfi -i color=c=gray:size=1280x720:rate=30 -t 3 -c:v libx265 -preset ultrafast \
  -pix_fmt yuv420p10le -tag:v hvc1 -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
  -x265-params log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:hdr10=1:max-cll=1000,400
# PQ bars of 100, 203 (SDR white), 400, 1000 and 4000 nits, for looking at
# EDR on a display; no case reads it.
gen hdr_pq_bars.mp4 -f lavfi \
  -i "nullsrc=s=1280x720:r=30,format=yuv420p10le,geq=lum='if(lt(X,256),509,if(lt(X,512),573,if(lt(X,768),636,if(lt(X,1024),723,855))))':cb=512:cr=512" \
  -t 3 -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -tag:v hvc1 -color_primaries bt2020 -color_trc smpte2084 \
  -colorspace bt2020nc -x265-params log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:hdr10=1:max-cll=4000,400
gen vp9_opus.webm -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=550:sample_rate=48000 -t 10 \
  -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 2M -c:a libopus
gen av1.mkv -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 10 \
  -c:v libsvtav1 -preset 12 -svtav1-params log-level=0 -c:a aac
gen rotated.mp4 -display_rotation 90 -i "$media/h264_aac.mp4" -c copy
# HD with a BT.601 matrix (in the stream and the container): the tag wins
# over the convention for untagged HD (BT.709).
gen h264_bt601.mp4 -i "$media/h264_aac.mp4" -t 3 -c copy -colorspace smpte170m \
  -bsf:v h264_metadata=matrix_coefficients=6
gen audio_only.mp3 -f lavfi -i sine=frequency=880:sample_rate=44100 -t 8 -c:a libmp3lame
gen video_only.mp4 -f lavfi -i testsrc2=size=1280x720:rate=60 -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p
gen offset.ts -i "$media/h264_aac.mp4" -c copy -output_ts_offset 10 -f mpegts
gen hevc10.ts -i "$media/hevc10.mp4" -c copy -f mpegts
# The index at the front, so a cut file still opens; then cut in half.
gen faststart.mp4 -i "$media/h264_aac.mp4" -c copy -movflags +faststart
[ -f "$media/truncated.mp4" ] || { head -c 4358118 "$media/faststart.mp4" >"$media/truncated.tmp" &&
  mv "$media/truncated.tmp" "$media/truncated.mp4"; } || exit 1
# Two renditions back to back, like an adaptive stream switching.
for size in 1280x720 640x360; do
  gen "rendition_$size.ts" -f lavfi -i "testsrc2=size=$size:rate=30" -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 3 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -f mpegts
done
gen resolution_change.ts -i "concat:$media/rendition_1280x720.ts|$media/rendition_640x360.ts" -c copy
# Live input, as a screen mirroring sender encodes it: no B-frames, audio
# muxed next to its video.
gen live_h264.mp4 -f lavfi -i "$src" -f lavfi -i sine=frequency=440:sample_rate=44100 -t 12 \
  -c:v libx264 -preset veryfast -bf 0 -g 30 -pix_fmt yuv420p -c:a aac -b:a 128k
gen live_hevc.mp4 -f lavfi -i "$src" -f lavfi -i sine=frequency=660:sample_rate=48000 -t 10 \
  -c:v libx265 -preset ultrafast -x265-params bframes=0:keyint=30:log-level=error -tag:v hvc1 -c:a aac
for size in 1280x720 640x360; do
  gen "live_$size.ts" -f lavfi -i "testsrc2=size=$size:rate=30" -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 3 -c:v libx264 -preset veryfast -bf 0 -g 30 -pix_fmt yuv420p -c:a aac -muxdelay 0 -muxpreload 0 -f mpegts
done
gen live_alac.mp4 -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i sine=frequency=440:sample_rate=44100 -t 5 \
  -c:v libx264 -preset veryfast -bf 0 -g 30 -pix_fmt yuv420p -c:a alac
printf "file 'live_1280x720.ts'\nfile 'live_640x360.ts'\n" >"$media/live_list.txt"
gen live_resolution.ts -f concat -i "$media/live_list.txt" -c copy -muxdelay 0 -muxpreload 0
# Valid headers, then noise where the media data should be.
[ -f "$media/corrupt.mp4" ] || python3 -I -c '
import random, sys
data = bytearray(open(sys.argv[1], "rb").read())
rng = random.Random(1)
data[600000:] = bytes(rng.randrange(256) for _ in range(len(data) - 600000))
open(sys.argv[2], "wb").write(data)' "$media/faststart.mp4" "$media/corrupt.mp4" || exit 1

# Short, for looping and slow playback.
# 5 s of video, 2 s of audio.
gen uneven.mp4 -f lavfi -t 5 -i testsrc2=size=640x360:rate=30 -f lavfi -t 2 -i sine=frequency=440:sample_rate=48000 \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac
gen loop.mp4 -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac
# The video's sample size table says 4 billion samples of one byte.
[ -f "$media/huge_stsz.mp4" ] || python3 -I -c '
import sys
data = bytearray(open(sys.argv[1], "rb").read())
at = data.index(b"stsz")
data[at + 8:at + 16] = (1).to_bytes(4, "big") + (0xffffffff).to_bytes(4, "big")
open(sys.argv[2], "wb").write(data)' "$media/loop.mp4" "$media/huge_stsz.mp4" || exit 1
# One video sample of 1 GiB, far past the end of the file.
[ -f "$media/huge_sample.mp4" ] || python3 -I -c '
import sys
data = bytearray(open(sys.argv[1], "rb").read())
at = data.index(b"stsz") + 16 + 4 * 10
data[at:at + 4] = (1 << 30).to_bytes(4, "big")
open(sys.argv[2], "wb").write(data)' "$media/loop.mp4" "$media/huge_sample.mp4" || exit 1
# Fragmented MP4, a fragment per 1 s keyframe, indexed by mfra; the same
# with the first fragment's samples in the moov; indexed by a sidx only;
# Smooth Streaming's layout (no tfdt, negative composition offsets); and
# without an index, half-second fragments (most without a keyframe) in a
# moof per track.
gen frag.mp4 -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 10 \
  -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p -c:a aac -movflags frag_keyframe+empty_moov+default_base_moof
gen frag_moov.mp4 -i "$media/frag.mp4" -c copy -movflags frag_keyframe
gen frag_ismv.mp4 -i "$media/frag.mp4" -c copy -f ismv
gen frag_scan.mp4 -i "$media/frag.mp4" -c copy -frag_duration 500000 \
  -movflags empty_moov+default_base_moof+separate_moof+skip_trailer
# global_sidx needs the trailer, which ends with mfra: cut it off.
gen frag_sidx.tmp.mp4 -i "$media/frag.mp4" -c copy -movflags frag_keyframe+empty_moov+default_base_moof+global_sidx
[ -f "$media/frag_sidx.mp4" ] || python3 -I -c '
import sys
data = open(sys.argv[1], "rb").read()
at = data.rindex(b"mfra") - 4
open(sys.argv[2], "wb").write(data[:at])' "$media/frag_sidx.tmp.mp4" "$media/frag_sidx.mp4" || exit 1
[ -f "$media/frag_truncated.mp4" ] || { head -c 1700000 "$media/frag.mp4" >"$media/frag_truncated.tmp" &&
  mv "$media/frag_truncated.tmp" "$media/frag_truncated.mp4"; } || exit 1
gen meta.mp4 -i "$media/h264_aac.mp4" -t 4 -c copy -metadata title="Lychee Test" \
  -metadata artist="Lychee Artist" -metadata album="Lychee Album"
# Cover art: an ID3 picture (JPEG) and MP4 cover art (PNG).
gen cover.png -f lavfi -i color=c=orange:s=320x320 -frames:v 1
gen artwork.mp3 -i "$media/audio_only.mp3" -i "$media/cover.png" -map 0 -map 1 -c:a copy -c:v mjpeg \
  -disposition:v attached_pic -metadata:s:v comment="Cover (front)" -metadata title="Lychee Cover"
gen artwork.m4a -i "$media/audio_only.mp3" -i "$media/cover.png" -map 0 -map 1 -c:a aac -c:v copy \
  -disposition:v attached_pic
# Subtitles, with markup that must not reach the text: SRT <i>, ASS
# override blocks and \N, WebVTT <b>.
[ -f "$media/subs.srt" ] || cat >"$media/subs.srt" <<'SRT'
1
00:00:01,000 --> 00:00:02,500
Hello <i>world</i>

2
00:00:03,000 --> 00:00:04,500
Second line
with break

3
00:00:05,000 --> 00:00:06,000
Third
SRT
[ -f "$media/subs_fre.srt" ] || cat >"$media/subs_fre.srt" <<'SRT'
1
00:00:01,000 --> 00:00:02,500
Bonjour le monde

2
00:00:03,000 --> 00:00:04,500
Deuxieme
SRT
[ -f "$media/subs.ass" ] || cat >"$media/subs.ass" <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,{\b1}Bold{\b0} and, plain\Nnext line
ASS
[ -f "$media/subs.vtt" ] || cat >"$media/subs.vtt" <<'VTT'
WEBVTT

00:00:01.000 --> 00:00:02.500
Hello <b>vtt</b>
VTT
subbed() {
  local out="$1" sub="$2" codec="$3"
  gen "$out" -i "$media/h264_aac.mp4" -i "$media/$sub" -map 0 -map 1 -t 7 -c:v copy -c:a copy \
    -c:s "$codec" -disposition:s:0 default
}
subbed sub_srt.mkv subs.srt srt
subbed sub_ass.mkv subs.ass ass
subbed sub_vtt.mkv subs.vtt webvtt
subbed sub_mov_text.mp4 subs.srt mov_text
# One cue from before the keyframe at 8.33 s to the end, in a track that
# is off until switched on.
[ -f "$media/subs_long.srt" ] || printf '1\n00:00:00,500 --> 00:00:11,500\nLong cue\n' >"$media/subs_long.srt"
gen sub_long.mkv -i "$media/h264_aac.mp4" -i "$media/subs_long.srt" -map 0 -map 1 -c:v copy -c:a copy -c:s srt \
  -disposition:s:0 0
# Bitmap subtitles: two boxes, white with a black border (tool/make_pgs.py),
# as PGS and turned into DVB and VobSub.
[ -f "$media/subs.sup" ] || python3 "$root/tool/make_pgs.py" "$media/subs.sup" || exit 1
for codec in pgs:copy dvb:dvbsub vobsub:dvdsub; do
  gen "sub_${codec%%:*}.mkv" -fix_sub_duration -i "$media/subs.sup" -i "$media/h264_aac.mp4" -map 1 -map 0 -t 7 \
    -c:v copy -c:a copy -c:s "${codec#*:}" -disposition:s:0 default
done
# Tracks 0 video, 1-2 audio (eng, fre), 3-4 subtitles (eng, fre).
gen tracks.mkv -i "$media/h264_aac.mp4" -f lavfi -i sine=frequency=880:sample_rate=48000 \
  -i "$media/subs.srt" -i "$media/subs_fre.srt" -map 0:v -map 0:a -map 1:a -map 2 -map 3 -t 8 \
  -c:v copy -c:a:0 copy -c:a:1 aac -c:s srt -metadata:s:a:0 language=eng -metadata:s:a:1 language=fre \
  -metadata:s:a:1 title=French -metadata:s:s:0 language=eng -metadata:s:s:1 language=fre \
  -disposition:s:0 default -disposition:s:1 0
# Tracks 0-1 video (1920x1080, one keyframe; 640x360, one a second), 2 audio.
gen video_tracks.mkv -i "$media/h264_aac.mp4" -map 0:v -map 0:v -map 0:a -t 8 \
  -c:v:0 copy -c:v:1 libx264 -preset veryfast -g 30 -pix_fmt yuv420p -filter:v:1 scale=640:360 -c:a copy

# HLS, 2 s segments, for the http cases.
if [ ! -f "$media/hls/index.m3u8" ]; then
  mkdir -p "$media/hls"
  ffmpeg -hide_banner -loglevel error -y -i "$media/h264_aac.mp4" -c:v libx264 -preset veryfast -g 60 \
    -sc_threshold 0 -c:a copy -f hls -hls_time 2 -hls_playlist_type vod \
    -hls_segment_filename "$media/hls/seg%d.ts" "$media/hls/index.m3u8.tmp" &&
    mv "$media/hls/index.m3u8.tmp" "$media/hls/index.m3u8" || exit 1
fi
# Matroska: the MP4 remuxed, with and without Cues, with clusters of
# unknown size, damaged (tool/mkv_cases.py); FLAC in it, and MP3 in MP4.
gen h264_aac.mkv -i "$media/h264_aac.mp4" -c copy
mkv_case() {
  local out="$media/$1"
  [ -f "$out" ] || python3 -I "$root/tool/mkv_cases.py" "$2" "$media/h264_aac.mkv" "$out" ||
    { rm -f "$out"; exit 1; }
}
for mode in no-cues unknown-sizes huge-tracks huge-block corrupt truncated; do
  mkv_case "mkv_${mode//-/_}.mkv" "$mode"
done
gen flac.mkv -i "$media/h264_aac.mp4" -f lavfi -i sine=frequency=440:sample_rate=44100 -map 0:v -map 1:a -t 5 \
  -c:v copy -c:a flac
gen mp3.mp4 -i "$media/h264_aac.mp4" -f lavfi -i sine=frequency=440:sample_rate=44100 -map 0:v -map 1:a -t 5 \
  -c:v copy -c:a libmp3lame
# A second rendition at 640x360, and a master playlist over both plus a
# VP9 variant (the same media, but no FFmpeg-free decoder) with the
# highest bandwidth.
if [ ! -f "$media/hls_low/index.m3u8" ]; then
  mkdir -p "$media/hls_low"
  ffmpeg -hide_banner -loglevel error -y -i "$media/h264_aac.mp4" -vf scale=640:360 -c:v libx264 -preset veryfast \
    -g 60 -sc_threshold 0 -c:a copy -f hls -hls_time 2 -hls_playlist_type vod \
    -hls_segment_filename "$media/hls_low/seg%d.ts" "$media/hls_low/index.m3u8.tmp" &&
    mv "$media/hls_low/index.m3u8.tmp" "$media/hls_low/index.m3u8" || exit 1
fi
[ -f "$media/hls/master.m3u8" ] || printf '%s\n' '#EXTM3U' \
  '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.64001e,mp4a.40.2"' ../hls_low/index.m3u8 \
  '#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS="avc1.640028,mp4a.40.2"' index.m3u8 \
  '#EXT-X-STREAM-INF:BANDWIDTH=9000000,RESOLUTION=1920x1080,CODECS="vp09.00.40.08,mp4a.40.2"' index.m3u8 \
  >"$media/hls/master.m3u8"
# The two renditions after each other, each with timestamps from ~1.4 s.
[ -f "$media/hls/discontinuity.m3u8" ] || printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:3' \
  '#EXT-X-PLAYLIST-TYPE:VOD' '#EXTINF:3.0,' ../rendition_1280x720.ts '#EXT-X-DISCONTINUITY' '#EXTINF:3.0,' \
  ../rendition_640x360.ts '#EXT-X-ENDLIST' >"$media/hls/discontinuity.m3u8"
# The same discontinuity after the 12 s of index.m3u8, beyond the read-ahead
# of a seek soon after the open.
[ -f "$media/hls/discontinuity_late.m3u8" ] || printf '%s\n' '#EXTM3U' '#EXT-X-TARGETDURATION:3' \
  '#EXT-X-PLAYLIST-TYPE:VOD' '#EXTINF:2.0,' seg0.ts '#EXTINF:2.0,' seg1.ts '#EXTINF:2.0,' seg2.ts \
  '#EXTINF:2.0,' seg3.ts '#EXTINF:2.0,' seg4.ts '#EXTINF:2.0,' seg5.ts '#EXT-X-DISCONTINUITY' \
  '#EXTINF:3.0,' ../rendition_640x360.ts '#EXT-X-ENDLIST' >"$media/hls/discontinuity_late.m3u8"
# MPEG-TS that lost one packet in a thousand (continuity errors), and one
# that turns into noise after 600 KB.
[ -f "$media/ts_lost_packets.ts" ] || python3 -I -c '
import sys
data = open(sys.argv[1], "rb").read()
out = bytearray()
for i in range(0, len(data) - 187, 188):
    if i // 188 % 1000 != 999:
        out += data[i:i + 188]
open(sys.argv[2], "wb").write(out)' "$media/offset.ts" "$media/ts_lost_packets.ts" || exit 1
[ -f "$media/corrupt.ts" ] || python3 -I -c '
import random, sys
data = bytearray(open(sys.argv[1], "rb").read())
rng = random.Random(1)
data[600000:] = bytes(rng.randrange(256) for _ in range(len(data) - 600000))
open(sys.argv[2], "wb").write(data)' "$media/offset.ts" "$media/corrupt.ts" || exit 1

[ "${MEDIA_ONLY:-0}" = 1 ] && exit 0

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug ${flags[@]+"${flags[@]}"} >/dev/null || exit 1
build_out="$(cmake --build "$build" 2>&1)"
build_status=$?
if [ "$build_status" -ne 0 ] || grep -q "warning:" <<<"$build_out"; then
  echo "FAIL build (exit $build_status) has errors or warnings"
  grep -E "error|warning:|FAILED|Undefined" <<<"$build_out" | head -40
  exit 1
fi
cli="$build/examples/cli/lychee_cli"

# Sanitizers report and carry on where they can, so a case lists every
# report rather than the first; the patterns below fail it.
export TSAN_OPTIONS=halt_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1
sanitizer_reports="WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:"
# The start of the first report in $1, if any.
print_reports() {
  grep -Eq "$sanitizer_reports" <<<"$1" || return 0
  grep -E -A20 "$sanitizer_reports" <<<"$1" | head -30 | sed 's/^/     /'
}

# Each process writes its own profile; merged at the end.
if [ "${COVERAGE:-0}" = 1 ]; then
  profiles="$build/profiles"
  rm -rf "$profiles"
  mkdir -p "$profiles"
  export LLVM_PROFILE_FILE="$profiles/%p.profraw"
fi

failures=0

# Unit tests first: they pinpoint a failure the playback cases only show.
unit_out="$("$build/core/test/lychee_unittests" 2>&1)"
if [ $? -eq 0 ] && ! grep -Eq "$sanitizer_reports" <<<"$unit_out"; then
  echo "PASS unittests ($(tail -1 <<<"$unit_out"))"
else
  echo "FAIL unittests"
  grep -v '^PASS' <<<"$unit_out" | sed 's/^/  /'
  failures=$((failures + 1))
fi

# Cases run on the library's virtual clock (lychee_cli --virtual-time): as
# fast as decoding allows, and unaffected by load, so timing checks are
# exact. realtime=1 before a case plays it on the real clock and audio
# device instead. Every case also checks the CLI's exit code: 0, or the
# one exits=N before the case names (1: the player ended in an error).
#
# On a loaded machine (CI's hosted VMs) a realtime case's drift, glitches
# and drops measure the machine, not the player: REALTIME_ADVISORY=1 makes
# those checks warn instead of fail there, and keeps the rest.

# timing PROBLEM: a timing check that failed.
timing() {
  if [ "${realtime:-0}" = 1 ] && [ "${REALTIME_ADVISORY:-0}" = 1 ]; then
    advisory+="$1"
  else
    problems+="$1"
  fi
}
# pass NAME: the PASS line, and what timing did not hold under
# REALTIME_ADVISORY=1 (also as a GitHub annotation).
pass() {
  echo "PASS $1 ($((SECONDS - start))s)${advisory:+, timing advisory:$advisory}"
  [ -n "$advisory" ] || return 0
  grep -E '^summary' <<<"$out" | sed 's/^/     /'
  if [ "${GITHUB_ACTIONS:-}" = true ]; then
    echo "::warning title=$1::realtime timing:$advisory"
  fi
}
#
# check NAME EXPECT_HW EXPECT_SEEK_PTS -- CLI_ARGS...
# EXPECT_HW: 1, 0, or - (no video). EXPECT_SEEK_PTS: seconds, or - for none.
check() {
  local name="$1" hw="$2" seek_pts="$3"; shift 4
  if [ "${NO_HW:-0}" = 1 ] && [ "$hw" = 1 ]; then
    hw=0
    set -- --no-hw "$@"
  fi
  [ "${realtime:-0}" = 1 ] || set -- --virtual-time "$@"
  local out status summary problems="" advisory="" start=$SECONDS
  out="$("$cli" "$@" 2>&1)"
  status=$?
  [ "$status" = "${exits:-0}" ] || problems+=" exit=$status"
  summary="$(grep '^summary:' <<<"$out")"
  grep -Eq "$sanitizer_reports" <<<"$out" && problems+=" sanitizer-report"
  grep -q "state=ended" <<<"$summary" || problems+=" not-ended"
  grep -q "backward_jumps=0" <<<"$summary" || problems+=" backward-jumps"
  grep -q "stalls=0" <<<"$summary" || problems+=" stalls"
  if [ "$hw" != - ]; then
    grep -q " hw=$hw " <<<"$summary" || problems+=" hw!=$hw"
    local dropped
    dropped="$(sed -E 's/.*dropped=([0-9]+).*/\1/' <<<"$summary")"
    [ "${dropped:-99}" -le 3 ] || timing " dropped=$dropped"
    # Frames held for the wrong number of vsyncs (see the CLI).
    local glitches
    glitches="$(sed -E 's/.*glitches=([0-9]+).*/\1/' <<<"$summary")"
    [ "${glitches:-99}" -le 3 ] || timing " glitches=$glitches"
  fi
  if [ "$seek_pts" != - ]; then
    grep -q "first_pts_after_seek=$seek_pts" <<<"$summary" || problems+=" seek-landed-elsewhere"
  fi
  if [ -z "$problems" ]; then
    pass "$name"
  else
    echo "FAIL $name ($((SECONDS - start))s):$problems"
    grep -E '^(summary|error)' <<<"$out" | sed 's/^/     /'
    print_reports "$out"
  fi
}

# expect NAME PATTERN... -- CLI_ARGS: passes if the CLI output matches every
# extended regex PATTERN. "<=N PATTERN" instead allows at most N matching lines.
# "~PATTERN" is a timing check (see timing).
expect() {
  local name="$1"; shift
  local patterns=() out status problems="" advisory="" start=$SECONDS
  while [ "$1" != -- ]; do patterns+=("$1"); shift; done
  shift
  [ "${NO_HW:-0}" = 1 ] && set -- --no-hw "$@"
  [ "${realtime:-0}" = 1 ] || set -- --virtual-time "$@"
  out="$("$cli" "$@" 2>&1)"
  status=$?
  [ "$status" = "${exits:-0}" ] || problems+=" exit=$status"
  grep -Eq "$sanitizer_reports" <<<"$out" && problems+=" sanitizer-report"
  for pattern in "${patterns[@]}"; do
    if [[ "$pattern" =~ ^\<=([0-9]+)\ (.*)$ ]]; then
      local count
      count="$(grep -Ec "${BASH_REMATCH[2]}" <<<"$out")"
      [ "$count" -le "${BASH_REMATCH[1]}" ] || problems+=" ${count}x:'${BASH_REMATCH[2]}'"
    elif [ "${pattern:0:1}" = "~" ]; then
      grep -Eq "${pattern:1}" <<<"$out" || timing " missing:'${pattern:1}'"
    else
      grep -Eq "$pattern" <<<"$out" || problems+=" missing:'$pattern'"
    fi
  done
  if [ -z "$problems" ]; then
    pass "$name"
  else
    echo "FAIL $name ($((SECONDS - start))s):$problems"
    grep -E '^(summary|error|seek took)' <<<"$out" | sed 's/^/     /'
    print_reports "$out"
  fi
}

# frames NAME MEDIA SEC... -- CLI_ARGS: plays MEDIA, writes the frame on
# screen at each SEC (--frame-ppm, into $build/frames/NAME) and passes if
# tool/frame_check.py finds each to be MEDIA's frame at its pts, by
# pixels. self_test=1 also checks that wrong frames fail.
frames() {
  local name="$1" file="$2"; shift 2
  local dir="$build/frames/$name" snapshots=() out status checked problems="" start=$SECONDS
  mkdir -p "$dir"
  while [ "$1" != -- ]; do snapshots+=(--frame-ppm "$1:$dir/$1.ppm"); shift; done
  shift
  [ "${NO_HW:-0}" = 1 ] && set -- --no-hw "$@"
  [ "${realtime:-0}" = 1 ] || set -- --virtual-time "$@"
  out="$("$cli" "$@" "${snapshots[@]}" "$file" 2>&1)"
  status=$?
  [ "$status" = 0 ] || problems+=" exit=$status"
  grep -Eq "$sanitizer_reports" <<<"$out" && problems+=" sanitizer-report"
  checked="$(grep '^snapshot ' <<<"$out" |
    python3 -I "$root/tool/frame_check.py" "$file" --log - ${self_test:+--self-test} 2>&1)" ||
    problems+=" pixels"
  if [ -z "$problems" ]; then
    echo "PASS $name ($((SECONDS - start))s)"
  else
    echo "FAIL $name ($((SECONDS - start))s):$problems"
    grep -E '^(summary|error)' <<<"$out" | sed 's/^/     /'
    sed 's/^/     /' <<<"$checked"
    print_reports "$out"
  fi
}

# Most cases spend their time waiting for media to play in real time, using a
# few percent of a core, so they run JOBS at a time. Results print in order.
# `par` cases may overlap; `seq` cases wait for the running ones and run
# alone (shared network server, or timing that other load would disturb).
cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
if [ "${TSAN:-0}" = 1 ]; then
  max_jobs="${JOBS:-$((cores / 4))}"
else
  max_jobs="${JOBS:-$((cores / 2))}"
fi
[ "$max_jobs" -ge 1 ] || max_jobs=1
results="$(mktemp -d)"
case_count=0
next_print=1
pids=()
print_done() {
  local file
  while :; do
    file="$results/$(printf %03d "$next_print")"
    [ -f "$file" ] || break
    cat "$file"
    next_print=$((next_print + 1))
  done
}
running_cases() {
  local pid alive=()
  for pid in ${pids[@]+"${pids[@]}"}; do
    kill -0 "$pid" 2>/dev/null && alive+=("$pid")
  done
  pids=(${alive[@]+"${alive[@]}"})
  echo "${#pids[@]}"
}
wait_cases() {
  [ "${#pids[@]}" -gt 0 ] && wait "${pids[@]}"
  pids=()
  print_done
}
selected() {
  [ -z "${ONLY:-}" ] && return 0
  local pattern patterns
  # Split on spaces without expanding the globs against the working
  # directory.
  read -ra patterns <<<"$ONLY"
  for pattern in "${patterns[@]}"; do
    # shellcheck disable=SC2053
    [[ "$1" == $pattern ]] && return 0
  done
  return 1
}
# Without FFmpeg: MP4, Matroska, WebM, MPEG-TS and HLS, with text subtitles.
plays_without_ffmpeg() {
  case "$1" in
    # Bitmap subtitles have no decoder.
    subtitle-pgs | subtitle-dvb | subtitle-vobsub) return 1 ;;
    # No software AV1 decoder: av1-videotoolbox instead.
    av1-software) return 1 ;;
  esac
  local arg
  for arg in "$@"; do
    [[ "$arg" == "$media"/* ]] || continue
    case "$arg" in
      *.mp4 | *.m4a | *.mkv | *.webm | *.ts | *.m3u8) ;;
      *) return 1 ;;
    esac
    # VideoToolbox decodes VP9 only in hardware.
    [ "${NO_HW:-0}" = 1 ] && [[ "$arg" == *vp9* ]] && return 1
  done
  return 0
}
# par|seq check|expect NAME ...
run_case() {
  local mode="$1"; shift
  selected "$2" || return 0
  if [ "${NO_FFMPEG:-0}" = 1 ] && ! plays_without_ffmpeg "${@:2}"; then
    return 0
  fi
  case_count=$((case_count + 1))
  local file
  file="$results/$(printf %03d "$case_count")"
  if [ "$mode" = par ] && [ "$max_jobs" -gt 1 ]; then
    while [ "$(running_cases)" -ge "$max_jobs" ]; do
      print_done
      sleep 0.1
    done
    ("$@" >"$file.tmp" 2>&1; mv "$file.tmp" "$file") &
    pids+=($!)
  else
    wait_cases
    "$@" >"$file.tmp" 2>&1
    mv "$file.tmp" "$file"
  fi
  print_done
}
par() { run_case par "$@"; }
seq_() { run_case seq "$@"; }

par check h264          1 - -- "$media/h264_aac.mp4"
par check hevc10        1 - -- "$media/hevc10.mp4"
# VideoToolbox's VP9 hangs on the Intel Mac these run on: the player
# gives up on it after 4 s and decodes in software.
vp9_hw=1
[ "$(uname -sm)" = "Darwin x86_64" ] && vp9_hw=0
par check vp9           $vp9_hw - -- "$media/vp9_opus.webm"
# The shipped FFmpeg decodes AV1 only in hardware: no dav1d (roadmap).
[ -n "${FFMPEG_ROOT:-}" ] ||
  par check av1-software  0 - -- "$media/av1.mkv"
# Without FFmpeg AV1 needs a hardware decoder (M3, A17 Pro): elsewhere the
# video track is off and the audio plays to the end.
[ "${NO_FFMPEG:-0}" = 1 ] &&
  par expect av1-videotoolbox "track 0: video av1 " "summary: state=ended position=10\.0" -- "$media/av1.mkv"
par check flac-mkv      1 - -- "$media/flac.mkv"
par check mp3-mp4       1 - -- "$media/mp3.mp4"
par check rotated       1 - -- "$media/rotated.mp4"
# -display_rotation is counter-clockwise; the library reports clockwise.
par expect rotation "track 0: video h264 1920x1080 rotation=270 " -- --seconds 1 "$media/rotated.mp4"
par check audio-only    - - -- "$media/audio_only.mp3"
par check video-only    1 - -- "$media/video_only.mp4"
par check ts-offset     1 - -- "$media/offset.ts"
par check ts-hevc10     1 - -- "$media/hevc10.ts"
par check no-hw-cpu     0 - -- --no-hw --cpu "$media/h264_aac.mp4"
# 8-bit frames from HDR are tone-mapped, by VideoToolbox and by swscale.
par expect hdr-tonemap "center pixel: 1(7[5-9]|8[0-9]),1(7[5-9]|8[0-9]),1(7[5-9]|8[0-9])$" -- --cpu --seconds 1 \
  "$media/hdr_pq.mp4"
par expect hdr-tonemap-sw "hw=0 " "center pixel: 1(7[5-9]|8[0-9]),1(7[5-9]|8[0-9]),1(7[5-9]|8[0-9])$" -- --cpu \
  --no-hw --seconds 1 "$media/hdr_pq.mp4"
# Apple: the decoder's own buffers, 10-bit and tagged PQ for HDR10; BGRA
# from software decoding.
if [ "$(uname)" = Darwin ]; then
  native_hdr="native frame: x420 transfer=SMPTE_ST_2084_PQ$"
  [ "${NO_HW:-0}" = 1 ] && native_hdr="native frame: BGRA "
  par expect native-hdr "$native_hdr" "frame_type=64 " -- --native --seconds 1 "$media/hdr_pq.mp4"
fi
# Frames by pixels, against FFmpeg's decode of the same pts: each frame
# path (CVPixelBuffer BGRA on Apple, RGBA elsewhere; --cpu; software
# decoding; Apple's --native buffers converted by VideoToolbox), 10-bit,
# rotation and HDR tone mapping (a flat PQ gray: VideoToolbox maps bright
# colors differently from swscale, which the reference uses). The test
# media has no color tags, so these also check the library's convention:
# BT.709 above 576 lines, BT.601 at or below.
par frames frame-h264 "$media/h264_aac.mp4" 0.5 1.5 -- --seconds 1.6
par frames frame-h264-cpu "$media/h264_aac.mp4" 1 -- --cpu --seconds 1.1
par frames frame-h264-sw "$media/h264_aac.mp4" 1 -- --no-hw --cpu --seconds 1.1
par frames frame-hevc10 "$media/hevc10.mp4" 1 -- --seconds 1.1
par frames frame-rotated "$media/rotated.mp4" 1 -- --seconds 1.1
par frames frame-hdr "$media/hdr_pq.mp4" 1 -- --seconds 1.1
par frames frame-sd "$media/rendition_640x360.ts" 1 -- --seconds 1.1
par frames frame-sd-sw "$media/rendition_640x360.ts" 1 -- --no-hw --cpu --seconds 1.1
par frames frame-bt601 "$media/h264_bt601.mp4" 1 -- --seconds 1.1
par frames frame-bt601-sw "$media/h264_bt601.mp4" 1 -- --no-hw --cpu --seconds 1.1
if [ "$(uname)" = Darwin ]; then
  par frames frame-h264-native "$media/h264_aac.mp4" 1 -- --native --seconds 1.1
  par frames frame-hevc10-native "$media/hevc10.mp4" 1 -- --native --seconds 1.1
  par frames frame-hdr-native "$media/hdr_pq.mp4" 1 -- --native --seconds 1.1
fi
# The thresholds still tell the frame from the one before and after, a
# wrong turn or size, black, and BT.601 read as BT.709.
self_test=1 par frames frame-check-self-test "$media/rotated.mp4" 1 -- --seconds 1.1
# Reading hardware frames back into CPU memory costs ~50 ms a frame on macOS
# (far more under TSan); a second of video covers the conversion.
par expect rgba "frame_type=4 " "rendered=2[0-9] dropped=0 " -- --rgba --seconds 1 "$media/hevc10.mp4"
par expect no-hw-rgba "frame_type=4 " "state=ended" -- --no-hw --rgba "$media/h264_aac.mp4"
# Cadence: 30, 24 and 60 fps at 60 Hz with jittered vsync timestamps (callers
# that only know when they woke up), and refresh rates other than 60 Hz.
par check cadence-30fps 1 - -- --vsync-jitter 4000 "$media/h264_aac.mp4"
par check cadence-24fps 1 - -- --vsync-jitter 4000 "$media/hevc10.mp4"
par check cadence-60fps 1 - -- --vsync-jitter 4000 "$media/video_only.mp4"
par check cadence-50hz  1 - -- --refresh 50 --vsync-jitter 1000 "$media/hevc10.mp4"
par check cadence-144hz 1 - -- --refresh 144 --vsync-jitter 1000 "$media/video_only.mp4"
par check seek-forward  1 7.500 -- --seek 7.5 --seek-after 2 "$media/h264_aac.mp4"
par check seek-backward 1 1.000 -- --seek 1 --seek-after 5 "$media/hevc10.mp4"
par check seek-near-end 1 11.900 -- --seek 11.9 --seek-after 1 "$media/h264_aac.mp4"
par check seek-past-end 1 - -- --seek 30 --seek-after 1 "$media/offset.ts"
par check seek-vp9      $vp9_hw 7.500 -- --seek 7.5 --seek-after 2 "$media/vp9_opus.webm"
# Matroska seeks through the Cues, by scanning the clusters without them,
# and in clusters of unknown size.
par check seek-mkv      1 7.500 -- --seek 7.5 --seek-after 2 "$media/h264_aac.mkv"
par check seek-mkv-no-cues 1 1.000 -- --seek 1 --seek-after 5 "$media/mkv_no_cues.mkv"
par check seek-mkv-unknown-sizes 1 7.500 -- --seek 7.5 --seek-after 2 "$media/mkv_unknown_sizes.mkv"
# MPEG-TS seeks to the keyframe before the target (8.33 s apart here) and
# lands on the target frame (10 s offset + start 1.377 s + 7.5 s): by
# bisection without FFmpeg, by reading on from before it with FFmpeg,
# whose own seek lands on a packet, not a keyframe.
par check seek-ts           1 18.867 -- --seek 7.5 --seek-after 2 "$media/offset.ts"
par check seek-ts-backward  1 12.367 -- --seek 1 --seek-after 5 "$media/offset.ts"
par check seek-ts-hevc10    1 7.442 -- --seek 6 --seek-after 2 "$media/hevc10.ts"
# Into the second rendition.
par check seek-ts-rendition 1 5.433 -- --seek 4 --seek-after 1 "$media/resolution_change.ts"
par expect loop-ts "video size 640x360" "loops=1" "summary:.*backward_jumps=0 " "<=0 event: state (ended|buffering)" -- \
  --loop --seconds 9 "$media/resolution_change.ts"
par check seek-audio    - - -- --seek 3 --seek-after 1 "$media/audio_only.mp3"

# Playback rate. Drift (position vs. wall clock times the rate) stays small
# only if both the clock and the WSOLA output follow the rate.
small_drift="max_drift=([0-9]|[1-9][0-9]|1[0-4][0-9])\.[0-9]ms"
# Under 20 ms: no stall on top of what the clock already does.
no_drift="max_drift=([0-9]|1[0-9])\.[0-9]ms"
few_glitches="summary:.*glitches=[0-3] "
if [ "${TSAN:-0}" = 1 ]; then
  # An unoptimized ThreadSanitizer build cannot run WSOLA in real time:
  # audio falls behind, so only races and the end state are checked.
  small_drift="max_drift="
  no_drift="max_drift="
  few_glitches="glitches="
fi
par expect rate-2x "summary: state=ended" "summary:.*dropped=[0-3] " "$few_glitches" "$small_drift" -- \
  --rate 2 "$media/h264_aac.mp4"
par expect rate-half "summary: state=ended" "summary:.*dropped=[0-3] " "$few_glitches" "$small_drift" -- \
  --rate 0.5 "$media/loop.mp4"
par expect rate-change "rate -> 1.50" "summary: state=ended" "summary:.*glitches=[0-5] " "$small_drift" -- \
  --rate 1.5 --rate-after 2 "$media/h264_aac.mp4"
par expect rate-audio-only "summary: state=ended" "$small_drift" -- --rate 2 "$media/audio_only.mp3"
par expect rate-video-only "summary: state=ended" "$few_glitches" "$small_drift" -- \
  --rate 2 "$media/video_only.mp4"
# Looping: back to the start without ENDED or BUFFERING, and without
# stopping: a frame held too long at the jump is a glitch.
par expect loop "summary: state=ready" "loops=2" "summary:.*backward_jumps=0 " "summary:.*glitches=0 " \
  "<=0 event: state (ended|buffering)" -- --loop --seconds 8 "$media/loop.mp4"
# Looping off after the read has wrapped: ends with the pass playing, with
# none of the next one shown (90 frames).
par expect loop-off "summary: state=ended position=3\.000s .*pulled=90 " "summary:.*backward_jumps=0 " \
  "summary:.*loops=0 " -- --loop --loop-off-after 2 --seconds 8 "$media/loop.mp4"
# A second open on the same player, while playing and while still loading.
par expect reopen "open -> " "<=0 event: state error" "video size 1280x720" "summary: state=ended position=8.000s" -- \
  --open-after 2 "$media/video_only.mp4" "$media/h264_aac.mp4"
par expect reopen-loading "open -> " "<=0 event: state error" "summary: state=ended position=8.000s" -- \
  --open-after 0 "$media/video_only.mp4" "$media/hevc10.mp4"
# Track selection keeps the position.
par expect track-audio "select audio 2 at [0-9.]+s: accepted" "track 2: audio .* lang=fre title=French selected=1" \
  "track switch: resumed [+-]0\.[0-2][0-9]{2}s" "summary: state=ended.* backward_jumps=0 " -- --select audio:2 "$media/tracks.mkv"
# Switching back before the first switch ran cancels it, and playback
# goes on.
par expect track-cancel "select audio 1 at [0-9.]+s: accepted" "track 1: audio .* selected=1" \
  "summary: state=ended position=8\.[0-9]{3}s" -- --select audio:2 --select audio:1 "$media/tracks.mkv"
par expect track-no-video "select video -1 at [0-9.]+s: accepted" "video size 0x0" "track switch: resumed [+-]0\.[0-2]" \
  "summary: state=ended position=8\.[0-9]{3}s decoder=none" -- --select video:-1 "$media/tracks.mkv"
par expect track-subtitle "select subtitle 4 .*: accepted" "select video 1 .*: rejected" \
  "subtitle at [0-9.]+s: Bonjour le monde" "<=0 subtitle at [0-9.]+s: Hello world" -- \
  --select subtitle:4 --select video:1 --select-after 0.5 "$media/tracks.mkv"
# Audio only (video_frame_types 0): the video track is listed but never
# decoded or selectable.
# Without the longer track the file ends with the shorter one, and is not
# cut short.
par expect audio-only-shorter "<=0 error" "summary: state=ended" -- \
  --audio-only "$media/uneven.mp4"
par expect track-off-longer "<=0 error" "summary: state=ended" -- \
  --select video:-1 --select-after 0.5 "$media/uneven.mp4"
par expect audio-only-config "track 0: video .* selected=0" "select video 0 .*: rejected" \
  "summary: state=ended position=12\.000s decoder=none .*pulled=0 .*frame_type=0 " -- \
  --audio-only --select video:0 --select-after 1 "$media/h264_aac.mp4"
# A hardware decoder failing midway (a GPU reset) is rebuilt at the
# position: in hardware the first time, in software after that.
hw_first=1
[ "${NO_HW:-0}" = 1 ] && hw_first=0
par expect decoder-lost "video rebuilt: resumed [+-]0\.[0-2][0-9]{2}s" "<=0 event: state error" \
  "summary: state=ended .* hw=$hw_first .*dropped=[0-3] glitches=[0-3] backward_jumps=0 " -- \
  --lose-decoder-after 2 "$media/h264_aac.mp4"
par expect decoder-lost-twice "<=0 event: state error" \
  "summary: state=ended .*decoder=(ffmpeg|videotoolbox-sw) hw=0 .*backward_jumps=0 " -- \
  --lose-decoder-after 2 --lose-decoder-after 5 "$media/h264_aac.mp4"
# A hardware decoder that never returns (a driver hang) is given up after
# 4 s without frames and rebuilt in software at the position. Destroying
# the player meanwhile waits for it no longer than that.
par expect decoder-hung "hang video decoder at" "video rebuilt: resumed [+-]0\.[0-2][0-9]{2}s" \
  "<=0 event: state error" "summary: state=ended .* hw=0 .*backward_jumps=0 .*stalls=0" -- \
  --hang-decoder-after 2 "$media/h264_aac.mp4"
realtime=1 par expect decoder-hung-destroy "hang video decoder at" "destroyed in [0-4]\." -- \
  --hang-decoder-after 1 --seconds 2 "$media/h264_aac.mp4"
# An audio device that never pulls (it failed to start, as on a closed
# laptop lid) is pulled for in real time after 2 s, and playback goes on
# silently to the end; one that stops midway too, and it takes over again
# when it pulls. --seconds: a stall fails the case rather than hangs it.
par expect audio-device-dead "audio device pulled nothing for 20[0-9]{2} ms" \
  "summary: state=ended position=12\.000s .*backward_jumps=0 .*stalls=0" -- \
  --stall-audio-after 0 --seconds 20 "$media/h264_aac.mp4"
par expect audio-device-stalled "stall audio device at 2\." "audio device pulled nothing for 20[0-9]{2} ms" \
  "audio device pulls again" "summary: state=ended position=12\.000s .*glitches=[0-3] backward_jumps=0 .*stalls=0" -- \
  --stall-audio-after 2 --stall-audio-for 4 --seconds 20 "$media/h264_aac.mp4"
# The same with the platform's audio device pulling on its own thread.
realtime=1 par expect audio-device-stalled-realtime "audio device pulled nothing for 20[0-9]{2} ms" \
  "audio device pulls again" "summary: state=ready position=[34]\.[0-9]{3}s .*backward_jumps=0 " -- \
  --stall-audio-after 1 --stall-audio-for 3 --seconds 6 "$media/h264_aac.mp4"
# Nobody pulls frames for 3 s, as in the background: frames that went by
# meanwhile are not counted as dropped, and playback resumes at the clock.
par expect background "show at " "summary: state=ended .*dropped=0 glitches=[0-3] backward_jumps=0 " -- \
  --hide-after 2 --hide-for 3 "$media/h264_aac.mp4"
# A track whose tables cannot be real is skipped, not allocated
# (FFmpeg-free MP4 parser).
[ "${NO_FFMPEG:-0}" = 1 ] &&
  par expect huge-sample-table "track 0: .* skipped" "summary: state=ended .*decoder=none " -- "$media/huge_stsz.mp4"
# A sample larger than any real one is an error, not an allocation.
[ "${NO_FFMPEG:-0}" = 1 ] &&
  exits=1 par expect huge-sample "state=error" "error: sample at 0\.300 s .* too large" -- "$media/huge_sample.mp4"
# Damaged Matroska (FFmpeg-free demuxer): sizes no file has are errors,
# not allocations; noise past the last cluster and a cut file are errors.
if [ "${NO_FFMPEG:-0}" = 1 ]; then
  exits=1 par expect mkv-huge-tracks "state=error" "error: element 1654AE6B too large" -- "$media/mkv_huge_tracks.mkv"
  exits=1 par expect mkv-huge-block "state=error" "error: block at 5\.[0-9]{3} s .* too large" -- \
    "$media/mkv_huge_block.mkv"
  exits=1 par expect mkv-corrupt "state=error" "error: corrupt data at 6\." -- "$media/mkv_corrupt.mkv"
  exits=1 par expect mkv-truncated "state=error" "error: input ended at 7\." -- "$media/mkv_truncated.mkv"
fi
# Fragmented MP4: seeks through mfra, into the moov's own samples,
# through a sidx, by reading the moofs (to a keyframe in an earlier
# fragment), and without tfdt; loops, and (FFmpeg-free: FFmpeg ends it
# quietly, not knowing the duration) a file cut in a fragment.
par check frag             1 - -- "$media/frag.mp4"
par check frag-seek        1 7.500 -- --seek 7.5 --seek-after 2 "$media/frag.mp4"
par check frag-moov-seek   1 0.500 -- --seek 0.5 --seek-after 3 "$media/frag_moov.mp4"
par check frag-sidx-seek   1 6.200 -- --seek 6.2 --seek-after 1 "$media/frag_sidx.mp4"
par check frag-scan        1 - -- "$media/frag_scan.mp4"
par check frag-scan-seek   1 8.700 -- --seek 8.7 --seek-after 1 "$media/frag_scan.mp4"
par check frag-scan-back   1 1.500 -- --seek 1.5 --seek-after 6 "$media/frag_scan.mp4"
par check frag-ismv-seek   1 4.400 -- --seek 4.4 --seek-after 1 "$media/frag_ismv.mp4"
par expect frag-loop "summary: state=ready" "loops=2" "summary:.*backward_jumps=0 " "summary:.*glitches=[0-3] " \
  "<=0 event: state (ended|buffering)" -- --loop --seconds 25 "$media/frag.mp4"
[ "${NO_FFMPEG:-0}" = 1 ] &&
  exits=1 par expect frag-truncated "state=error" "error: input ended" -- "$media/frag_truncated.mp4"
# Hidden until the end: the stream still ends, with or without audio.
par expect background-end "<=0 show at" "summary: state=ended position=3\.[0-9]{3}s" -- \
  --hide-after 0 --hide-for 30 "$media/loop.mp4"
par expect background-end-video "<=0 show at" "summary: state=ended position=8\.[0-9]{3}s" -- \
  --hide-after 0 --hide-for 30 "$media/video_only.mp4"
# Live input (lychee_player_open_live): the CLI pushes a file's packets
# when a sender would send them. A live input never ends or buffers.
# live_vbuf is how far ahead decoded video reaches once the last packet is
# in: the latency, 80 ms unless set, less up to one frame.
live_ok="<=0 event: state (buffering|ended|error)"
live_latency="live_vbuf=0\.(0[4-9]|1[0-2])[0-9]s"
# Live input plays on the real clock, which an unoptimized ThreadSanitizer
# build does not keep up with: there, glitches are not counted.
tsan_glitches=
if [ "${TSAN:-0}" = 1 ]; then
  tsan_glitches="glitches=[0-9]+ "
fi
par expect live-h264 "$live_ok" "^seekable=0$" "summary: state=ready .*rendered=360 dropped=0 ${tsan_glitches:-glitches=[0-3] }backward_jumps=0 .*stalls=0 $live_latency" -- \
  --live "$media/live_h264.mp4"
par expect live-hevc "$live_ok" "summary: state=ready .*rendered=300 dropped=0 ${tsan_glitches:-glitches=[0-3] }backward_jumps=0 .*stalls=0 $live_latency" -- \
  --live "$media/live_hevc.mp4"
par expect live-latency "$live_ok" "summary: .*dropped=0 .*live_vbuf=0\.(2[5-9]|30)[0-9]s" -- \
  --live --latency 300 "$media/live_h264.mp4"
par expect live-audio-only "$live_ok" "summary: state=ready position=1[23]\.[0-9]{3}s decoder=none .*live_abuf=0\.0[4-9][0-9]s" -- \
  --live --audio-only "$media/live_h264.mp4"
# ALAC, as AirPlay sends: the track's config is its magic cookie.
par expect live-alac "$live_ok" "track 1: audio alac 44100 Hz" "summary: .*dropped=0 .*live_abuf=0\.(0[1-9]|1[0-9])[0-9]s" -- \
  --live "$media/live_alac.mp4"
# Arrival jitter within the latency costs nothing.
par expect live-jitter "$live_ok" "summary: .*rendered=360 dropped=0 ${tsan_glitches:-glitches=[0-3] }backward_jumps=0 " -- \
  --live --live-jitter 30 "$media/live_h264.mp4"
# Half a second without packets, then all at once: what arrived too late is
# dropped, and the latency comes back.
par expect live-stall "$live_ok" "summary: .*dropped=([0-9]|1[0-9]) .*backward_jumps=0 .*$live_latency" -- \
  --live --live-stall 4 0.5 "$media/live_h264.mp4"
# A flush (the sender restarted) starts over at the next keyframe.
par expect live-flush "$live_ok" "live flush at" "summary: .*rendered=3[3-5][0-9] dropped=0 ${tsan_glitches:-glitches=[0-5] }backward_jumps=0 .*$live_latency" -- \
  --live --live-flush 5 "$media/live_h264.mp4"
# No video for 3 s (a still screen), audio on: video resumes at a keyframe.
par expect live-gap "$live_ok" "live video resumes" "summary: .*rendered=2[6-7][0-9] dropped=0 ${tsan_glitches:-glitches=[0-3] }backward_jumps=0 .*$live_latency" -- \
  --live --live-gap 4 3 "$media/live_h264.mp4"
# New parameter sets in the stream, as when the sender rotates. The
# decoder restarts, which can hold a frame a vsync longer.
par expect live-resolution "$live_ok" "video size 1280x720" "video size 640x360" \
  "summary: .*rendered=180 dropped=0 ${tsan_glitches:-glitches=[0-5] }backward_jumps=0 " -- --live "$media/live_resolution.ts"
# Text subtitles: cues as plain text at their times.
par expect subtitle-srt "subtitle at [01]\.[0-9]{3}s: Hello world$" "subtitle at 2\.[0-9]{3}s: \(none\)" \
  "subtitle at [34]\.[0-9]{3}s: Second line \| with break$" "summary: state=ended" -- "$media/sub_srt.mkv"
par expect subtitle-ass "subtitle at [01]\.[0-9]{3}s: Bold and, plain \| next line$" -- "$media/sub_ass.mkv"
par expect subtitle-webvtt "subtitle at [01]\.[0-9]{3}s: Hello vtt$" -- "$media/sub_vtt.mkv"
par expect subtitle-mov-text "subtitle at [01]\.[0-9]{3}s: Hello world$" -- "$media/sub_mov_text.mp4"
# Switched on in the middle of a cue that started before the last
# keyframe: shown at once, and without a seek.
par expect subtitle-mid-cue "select subtitle 2 at 9\.[0-9]{3}s: accepted" "subtitle at 9\.[0-9]{3}s: Long cue" \
  "<=0 seek to" "summary: state=ended .*glitches=[0-3] " -- --select subtitle:2 --select-after 9 "$media/sub_long.mkv"
# The position can read a few ms before the seek target: the audio frame
# that contains it starts earlier.
# Bitmap subtitles: pictures where they are on the 1920x1080 canvas, in
# their colours (83% of each box is white), and gone when they end.
for kind in pgs dvb vobsub; do
  par expect "subtitle-$kind" "subtitle images: 1 \| 600x80 at 660,900 opaque=100% rgb=21[0-4],21[0-4],21[0-4]$" \
    "subtitle at 2\.[5-6][0-9]{2}s: \(none\)" "subtitle images: 1 \| 200x60 at 860,100 opaque=100% rgb=19[0-3]," \
    "summary: state=ended" -- "$media/sub_$kind.mkv"
done
# Styled ASS through libass: one picture of the two lines near the bottom.
if [ "$libass" = 1 ]; then
  par expect subtitle-ass-libass "subtitle images: 1 \| [0-9]+x[0-9]+ at [0-9]+,9[0-9]{2} " -- "$media/sub_ass.mkv"
fi
par expect subtitle-seek "subtitle at 3\.[4-9][0-9]{2}s: Second line" -- --seek 3.5 --seek-after 0.5 "$media/sub_srt.mkv"
par expect subtitle-mov-text-seek "subtitle at 3\.[4-9][0-9]{2}s: Second line \| with break$" "<=0 Hello world" -- \
  --seek 3.5 --seek-after 0.5 "$media/sub_mov_text.mp4"
par expect metadata "metadata: title=Lychee Test artist=Lychee Artist album=Lychee Album" "summary: state=ended" -- \
  "$media/meta.mp4"
par expect artwork-id3 "metadata: title=Lychee Cover" "artwork: [0-9]+ bytes jpeg" "<=0 track 1" \
  "summary: state=ended" -- "$media/artwork.mp3"
par expect artwork-mp4 "artwork: [0-9]+ bytes png" "summary: state=ended" -- "$media/artwork.m4a"
par expect no-artwork "<=0 artwork:" -- --seconds 1 "$media/meta.mp4"
par expect stats "vbuf=[1-9]\.[0-9]{2}s abuf=[1-9]\.[0-9]{2}s vdec=([1-9][0-9]*\.[0-9]{2}|0\.[0-9][1-9]|0\.[1-9][0-9])ms" -- \
  --seconds 3 "$media/h264_aac.mp4"

# On the real clock and audio device: the path apps take. Alone, since
# load on the machine shows up as drift and glitches (advisory on CI:
# REALTIME_ADVISORY, above).
realtime=1 seq_ check realtime-h264 1 - -- "$media/meta.mp4"
realtime=1 seq_ check realtime-seek 1 3.000 -- --seek 3 --seek-after 1 "$media/meta.mp4"
realtime=1 seq_ expect realtime-rate "summary: state=ended" "~$few_glitches" "~$small_drift" -- \
  --rate 2 "$media/meta.mp4"
# Drift is measured afresh from each jump back: a loop that stops the audio
# device and prerolls again shows up as ~60 ms behind.
realtime=1 seq_ expect realtime-loop "loops=2" "~$no_drift" "<=0 event: state (ended|buffering)" -- \
  --loop --seconds 8 "$media/loop.mp4"

# Network cases against a local server that injects faults. Each starts its
# own server so a fault fires once. Faults at a byte offset work on virtual
# time; a slow or stalled server needs the real clock.
server=""
stop_server() {
  [ -n "$server" ] || return 0
  kill "$server" 2>/dev/null
  wait "$server" 2>/dev/null
  server=""
}
serve() {
  stop_server
  local log="$build/media_server.log"
  python3 -I "$root/tool/media_server.py" "$media" "$@" >"$log" 2>&1 &
  server=$!
  port=""
  for _ in $(seq 50); do
    port="$(awk '/^port/ {print $2}' "$log")"
    [ -n "$port" ] && break
    sleep 0.1
  done
  if [ -z "$port" ]; then
    local status=exited
    kill -0 "$server" 2>/dev/null && status="still starting"
    echo "media server reported no port in 5 s ($status):"
    sed 's/^/     /' "$log"
  fi
  url="http://127.0.0.1:$port/faststart.mp4"
}
trap 'stop_server; rm -rf "$results"' EXIT

exits=1 par expect truncated-file "state=error" "error: input ended at" -- "$media/truncated.mp4"
par expect resolution-change "video size 1280x720" "video size 640x360" "state=ended" -- \
  "$media/resolution_change.ts"
# Damaged MPEG-TS: continuity errors pass the damaged data on (audio
# resyncs on its ADTS headers), and noise ends the input where it starts
# (600 KB in: ~0.9 s, a frame or two either way with another x264 build).
par expect ts-lost-packets "summary: state=ended position=12\.0" -- --audio-only "$media/ts_lost_packets.ts"
par expect ts-corrupt "summary: state=ended position=0\.[89]" -- "$media/corrupt.ts"
exits=1 par expect corrupt-file "state=error" "error: (audio|video) decoding failed" -- "$media/corrupt.mp4"
serve
seq_ check http          1 - -- "$url"
# lychee_player_is_seekable(), known with the media info.
seq_ expect http-seekable "^seekable=1$" -- --seconds 1 "$url"
serve --drop-at 3000000
seq_ check http-reconnect 1 - -- "$url"
# A server without range requests: FFmpeg cannot seek, so the seek is
# ignored, not reported, and playback goes on from where it was to the end.
# (The FFmpeg-free source reads from the start and discards up to the
# offset: a real seek.)
if [ "${NO_FFMPEG:-0}" != 1 ]; then
  serve --no-ranges
  seq_ expect http-no-ranges "^seekable=0$" "seek ignored" "<=0 seek completed" "<=0 seek took" \
    "summary: state=ended .*backward_jumps=0 .*stalls=0" -- --seek 8 --seek-after 1 "$url"
  # A track switch or a decoder rebuild on it cannot seek: the renderers
  # start over from the packets kept since the keyframe before the
  # position, so playback resumes there and no frame is skipped (8 s at
  # 30 fps: 242 frames), then the read goes on from where it was. Also
  # after the input was read to its end.
  seq_ expect http-no-ranges-track "select audio 2 at [0-9.]+s: accepted" "track 2: audio .* selected=1" \
    "track switch: resumed [+-]0\.0[0-9]{2}s" "<=0 seek failed" "<=0 input failed" \
    "summary: state=ended position=8\.[0-9]{3}s .*pulled=24[0-2] .*backward_jumps=0 " -- \
    --select audio:2 "http://127.0.0.1:$port/tracks.mkv"
  seq_ expect http-no-ranges-track-end "track switch: resumed [+-]0\.0[0-9]{2}s" "<=0 input failed" \
    "summary: state=ended position=8\.[0-9]{3}s .*pulled=24[0-2] .*backward_jumps=0 " -- \
    --select audio:2 --select-after 5 "http://127.0.0.1:$port/tracks.mkv"
  # Every video stream is read too, so another video track, or video
  # turned off and on again (track 0 has one keyframe, at 0), starts at the
  # position as from the local file: 240 frames, and 183 with video off from
  # 1 s to 3 s. Read from the read position, they were 92 and 32.
  seq_ expect http-no-ranges-track-video "select video 1 at [0-9.]+s: accepted" "track 1: video .* selected=1" \
    "track switch: resumed [+-]0\.0[0-9]{2}s" "<=0 input failed" \
    "summary: state=ended position=8\.[0-9]{3}s .*pulled=2(3[5-9]|4[0-2]) .*backward_jumps=0 " -- \
    --select video:1 "http://127.0.0.1:$port/video_tracks.mkv"
  seq_ expect http-no-ranges-video-off-on "select video -1 at [0-9.]+s: accepted" \
    "select video 0 at [0-9.]+s: accepted" "track 0: video .* selected=1" "<=0 input failed" \
    "summary: state=ended position=8\.[0-9]{3}s .*pulled=1(7[5-9]|8[0-9]) .*backward_jumps=0 " -- \
    --select video:-1@1 --select video:0@3 "http://127.0.0.1:$port/video_tracks.mkv"
  # A rebuild may skip a frame, as decoder-lost allows (the new renderer
  # goes on after the frame on screen), not the read-ahead's ~120. The
  # CLI's "resumed" line needs it to see the buffering in between, which
  # it missed twice on CI's loaded VMs; the frame count says the same.
  seq_ expect http-no-ranges-decoder-lost "video rebuilt" "<=0 event: state error" \
    "summary: state=ended position=12\.000s .*pulled=3(5[5-9]|6[0-9]) .*backward_jumps=0 " -- \
    --lose-decoder-after 2 "$url"
fi
serve --drop-at 3000000
seq_ check http-mkv-reconnect 1 - -- "http://127.0.0.1:$port/h264_aac.mkv"
# Fragmented over http: the moofs read one by one (frag_scan has no
# index, so opening reads them all), and a seek through mfra, which is
# read from the end.
serve
seq_ check http-frag 1 - -- "http://127.0.0.1:$port/frag_scan.mp4"
seq_ check http-frag-seek 1 6.200 -- --seek 6.2 --seek-after 1 "http://127.0.0.1:$port/frag.mp4"
# The TS probe opens three ranges in both builds (the start, the end for
# the duration, then back). Later requests fail, like an expired URL: a
# failed seek cannot safely resume the demuxer's old read position. (A
# seek to near the start may read what the last range still holds.)
serve --refuse-after-requests 3
exits=1 seq_ expect http-seek-failed "403 [Ff]orbidden" "error: seek failed" \
  "summary: state=error" "<=0 seek completed" "<=0 event: state ended" -- \
  --seek 6 --seek-after 5 "http://127.0.0.1:$port/offset.ts"
# A failed read-ahead loop is an input error after the buffered pass,
# not an EOF that silently schedules another seek on the control thread.
serve --refuse-after-requests 3
exits=1 seq_ expect http-loop-seek-failed "403 [Ff]orbidden" "error: loop seek failed" \
  "summary: state=error" "<=0 loop to" "<=0 seek completed" \
  "<=0 event: state ended" -- --loop "http://127.0.0.1:$port/offset.ts"
serve
seq_ check http-ts 1 - -- "http://127.0.0.1:$port/offset.ts"
# HLS reads its playlist and segments through the same http client (the
# FFmpeg build through io_open). Behind a redirect, relative segment URLs
# resolve against where the playlist came from.
serve --redirect /moved.m3u8 /hls/index.m3u8
seq_ check http-hls 1 - -- "http://127.0.0.1:$port/hls/index.m3u8"
seq_ check http-hls-redirect 1 - -- "http://127.0.0.1:$port/moved.m3u8"
# Into the fourth segment (6-8 s): its first frame is 1.443 s + 7 s.
seq_ check http-hls-seek 1 8.433 -- --seek 7 --seek-after 2 "http://127.0.0.1:$port/hls/index.m3u8"
# A master playlist plays one variant: FFmpeg's build the first, the
# FFmpeg-free one the highest bandwidth it can decode (not the VP9 one).
hls_variant="video size (640x360|1920x1080)"
[ "${NO_FFMPEG:-0}" = 1 ] && hls_variant="variant 1 of 3: .* 1920x1080"
seq_ expect http-hls-master "$hls_variant" "summary: state=ended position=12\.000s" -- \
  "http://127.0.0.1:$port/hls/master.m3u8"
# Timestamps that start over at a discontinuity go on from where the
# previous segment ended, also after a seek into the later segment (which
# the FFmpeg build reads on to from before the discontinuity).
seq_ expect http-hls-discontinuity "video size 1280x720" "video size 640x360" \
  "summary: state=ended position=6\.000s .*backward_jumps=0 " -- "http://127.0.0.1:$port/hls/discontinuity.m3u8"
seq_ check http-hls-discontinuity-seek 1 5.433 -- --seek 4 --seek-after 1 \
  "http://127.0.0.1:$port/hls/discontinuity.m3u8"
# A seek past a discontinuity not read yet: FFmpeg's HLS seek finds nothing
# there, and the FFmpeg build reads on from the start. The FFmpeg-free
# build places the segment by the playlist's times, the FFmpeg build where
# the video before it ended, 2 ms later.
seq_ check http-hls-discontinuity-late-seek 1 '14\.43[13]' -- --seek 13 --seek-after 0.2 \
  "http://127.0.0.1:$port/hls/discontinuity_late.m3u8"
# Live: segments appear in real time; the playlist reloads until
# EXT-X-ENDLIST. With no duration the position at the end is where the
# clock was when the end was seen, a timing check: CI's ASan VM once saw
# it at 12.106 s.
serve --live /hls/live.m3u8 /hls/index.m3u8
realtime=1 seq_ expect http-hls-live "^seekable=0$" "<=0 event: state (error|buffering)" \
  "summary: state=ended .*backward_jumps=0 " "~summary: state=ended position=12\.0[0-9]{2}s " \
  "~summary:.*dropped=0 " -- \
  "http://127.0.0.1:$port/hls/live.m3u8"
# At 2 MB/s, three times the file's bitrate, the drop comes 1.5 s in,
# after the open: the Apple http source reads ahead as fast as the server
# sends, and twice on CI the open itself got the 503 (taken to be a
# request it made after the drop; not reproduced here).
serve --drop-at 3000000 --refuse-after-drop --rate 2000
exits=1 seq_ expect http-lost "state=error" "error: input ended at" -- "$url"
# Slower than the bitrate: rebuffer in long stretches, not every few frames.
serve --rate 550
realtime=1 seq_ expect http-slow "state=ended" "<=2 event: state buffering" -- "$url"
# The read stalls before playback starts; the seek must not wait for it.
# Interrupting that read must not turn the end of the file into an error.
serve --stall-at 3000000 --stall-for 10
realtime=1 seq_ expect http-stall-seek "seek took [0-9]{1,3}\." "first_pts_after_seek=0.500s" \
  "summary: state=ended" -- --seek 0.5 --seek-after 1 "$url"
wait_cases
stop_server

failures=$((failures + $(cat "$results"/[0-9][0-9][0-9] 2>/dev/null | grep -c '^FAIL ')))
echo "$failures failure(s)"

if [ "${COVERAGE:-0}" = 1 ]; then
  # Xcode's LLVM tools on macOS, matching its clang.
  llvm() {
    if command -v xcrun >/dev/null 2>&1; then xcrun "$@"; else "$@"; fi
  }
  llvm "${LLVM_PROFDATA:-llvm-profdata}" merge -sparse "$profiles"/*.profraw \
    -o "$build/coverage.profdata" || exit 1
  cov_args=("$cli" -object "$build/core/test/lychee_unittests"
            -instr-profile="$build/coverage.profdata" "$root/core/src")
  # Its warnings about functions whose data differ between the two
  # programs (inline functions built twice) do not matter here.
  llvm "${LLVM_COV:-llvm-cov}" report "${cov_args[@]}" >"$build/coverage.txt" 2>/dev/null
  llvm "${LLVM_COV:-llvm-cov}" show "${cov_args[@]}" -format=html \
    -output-dir="$build/coverage" >/dev/null 2>&1
  echo "coverage: $(grep '^TOTAL' "$build/coverage.txt" |
    awk '{print "lines " $(NF-3) ", functions " $(NF-6) ", branches " $NF}')"
  echo "  per file: $build/coverage.txt, $build/coverage/index.html"
fi
[ "$failures" = 0 ]
