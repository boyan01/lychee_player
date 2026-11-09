#!/usr/bin/env python3
"""Finds clicks in audio the player output (lychee_cli --audio-dump).

    python3 -I tool/audio_check.py OUT.wav [--threshold X] [--max-gap MS]
                                           [--expect-gap MS]
                                           [--min-peak X] [--max-peak X]
                                           [--samples LOG] [--expect-clicks]

A click is a step in the waveform: a sample far off the line through its
two neighbours. For a sine of amplitude A and angular frequency w per
sample, that distance (the second difference) is at most A * w**2, about
0.0004 for the test media's 440 Hz at 1/8 of full scale (AAC's noise
makes it ~0.002), and a fade of 10 ms adds less than A / 480. Cutting
such a sine to silence, or jumping to another phase of it, moves a
sample by up to A (0.125). The default threshold, 0.005 (-46 dBFS), lies
between the two. Silence is not a click when the audio fades into and
out of it.

A sine cut where it crosses zero makes no step, so a gap of silence can
hide from the steps: --max-gap MS also fails on silence longer than that
between the first and the last sound, for audio that should play on
without one (a loop).

--expect-gap MS fails unless there is such a silence, to see a fade
reach silence.

--min-peak and --max-peak bound the largest sample magnitude, to see a
gain applied and kept within full scale. For a gain, scale --threshold
with it: the steps of a sine grow with its amplitude.

--samples LOG (- for stdin) checks the samples the player says it played
(lychee_cli --audio-samples, its "samples at_us=T frames=N: ..." lines)
against the recording: they must match the N frames, mixed down to mono,
that end at T microseconds into it, give or take a frame. Pulls of only
silence (the first, which starts the recording) are not compared, but
one pull at least must have sound.

Prints PASS or FAIL with the largest step and where the clicks are, in
seconds from the start of the recording, and exits with 1 on FAIL.
--expect-clicks inverts that, for a self-test that a click is found.
Python's standard library only.
"""

import argparse
import array
import struct
import sys

DEFAULT_THRESHOLD = 0.005
# Steps closer than this belong to one click.
MERGE_SECONDS = 0.005
# Below this is silence; a sine of the test media's amplitude passes
# through it in a few samples at each zero crossing.
SILENCE = 0.002


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV file")
    offset = 12
    fmt = None
    while offset + 8 <= len(data):
        chunk, size = struct.unpack_from("<4sI", data, offset)
        body = data[offset + 8:offset + 8 + size]
        if chunk == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif chunk == b"data":
            if fmt is None:
                raise ValueError(f"{path}: data before fmt")
            format_tag, channels, rate, _, _, bits = fmt
            if format_tag == 3 and bits == 32:
                samples = array.array("f")
                samples.frombytes(body[:len(body) // 4 * 4])
            elif format_tag == 1 and bits == 16:
                samples = array.array("h")
                samples.frombytes(body[:len(body) // 2 * 2])
                samples = array.array("f", (s / 32768 for s in samples))
            else:
                raise ValueError(f"{path}: format {format_tag}/{bits} bits")
            if sys.byteorder != "little":
                samples.byteswap()
            return rate, channels, samples
        offset += 8 + size + (size & 1)
    raise ValueError(f"{path}: no data chunk")


def find_clicks(rate, channels, samples, threshold):
    """Returns the largest step, its time, and [(time, step)] per click."""
    largest, largest_at = 0.0, 0.0
    clicks = []
    frames = len(samples) // channels
    for channel in range(channels):
        x = samples[channel::channels]
        for n in range(1, frames - 1):
            step = abs(x[n + 1] - 2 * x[n] + x[n - 1])
            if step > largest:
                largest, largest_at = step, n / rate
            if step > threshold:
                time = n / rate
                if clicks and time - clicks[-1][0] < MERGE_SECONDS:
                    if step > clicks[-1][1]:
                        clicks[-1] = (clicks[-1][0], step)
                else:
                    clicks.append((time, step))
    clicks.sort()
    merged = []
    for time, step in clicks:
        if merged and time - merged[-1][0] < MERGE_SECONDS:
            merged[-1] = (merged[-1][0], max(step, merged[-1][1]))
        else:
            merged.append((time, step))
    return largest, largest_at, merged


def find_gaps(rate, channels, samples, max_gap):
    """Returns [(time, seconds)] of silence longer than |max_gap| seconds
    between the first and the last sound, in every channel."""
    frames = len(samples) // channels
    loud = [any(abs(samples[i * channels + k]) >= SILENCE
                for k in range(channels)) for i in range(frames)]
    if True not in loud:
        return []
    first = loud.index(True)
    last = frames - 1 - loud[::-1].index(True)
    gaps = []
    start = None
    for i in range(first, last + 1):
        if not loud[i] and start is None:
            start = i
        elif loud[i] and start is not None:
            if (i - start) / rate > max_gap:
                gaps.append((start / rate, (i - start) / rate))
            start = None
    return gaps


def check_samples(rate, channels, samples, log):
    """Returns problems with the pulled samples in |log|, and how many
    with sound it compared."""
    problems = []
    count = 0
    frames = len(samples) // channels
    for line in log:
        if not line.startswith("samples at_us="):
            continue
        head, _, values = line.partition(":")
        fields = dict(f.split("=") for f in head.split()[1:])
        at_us, n = int(fields["at_us"]), int(fields["frames"])
        pulled = [float(v) for v in values.split()]
        if n == 0 or len(pulled) != n:
            problems.append(f"{at_us / 1e6:.3f}s: {n} frames")
            continue
        if not any(pulled):
            continue
        if max(abs(v) for v in pulled) < 0.01:
            problems.append(f"{at_us / 1e6:.3f}s: silent")
            continue
        count += 1
        end = round(at_us * rate / 1e6)
        best = None
        for shift in (0, -1, 1):
            start = end + shift - n
            if start < 0 or start + n > frames:
                continue
            error = max(abs(pulled[i] - sum(samples[(start + i) * channels + k]
                                            for k in range(channels)) / channels)
                        for i in range(n))
            if best is None or error < best[0]:
                best = (error, shift)
        if best is None or best[0] > 1e-4:
            problems.append(f"{at_us / 1e6:.3f}s: differs by "
                            f"{best[0] if best else float('nan'):.4f}")
    return problems, count


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("wav")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    parser.add_argument("--max-gap", type=float, help="milliseconds")
    parser.add_argument("--expect-gap", type=float, help="milliseconds")
    parser.add_argument("--min-peak", type=float)
    parser.add_argument("--max-peak", type=float)
    parser.add_argument("--samples", help="lychee_cli output, - for stdin")
    parser.add_argument("--expect-clicks", action="store_true")
    args = parser.parse_args()

    rate, channels, samples = read_wav(args.wav)
    if not samples or max(abs(s) for s in samples[::97]) < 0.01:
        print(f"FAIL {args.wav}: silent (play with --volume 1)")
        return 1
    largest, largest_at, clicks = find_clicks(rate, channels, samples,
                                              args.threshold)
    gaps = []
    if args.max_gap is not None:
        gaps = find_gaps(rate, channels, samples, args.max_gap / 1000)
    peak = max(abs(s) for s in samples)
    level = []
    if args.expect_gap is not None and not find_gaps(
            rate, channels, samples, args.expect_gap / 1000):
        level.append(f"no silence of {args.expect_gap:g} ms")
    if args.min_peak is not None and peak < args.min_peak:
        level.append(f"peak {peak:.4f} < {args.min_peak}")
    if args.max_peak is not None and peak > args.max_peak:
        level.append(f"peak {peak:.4f} > {args.max_peak}")
    pulled = []
    pulls = 0
    if args.samples:
        if args.samples == "-":
            pulled, pulls = check_samples(rate, channels, samples, sys.stdin)
        else:
            with open(args.samples) as log:
                pulled, pulls = check_samples(rate, channels, samples, log)
        if pulls == 0:
            pulled.append("no pull with sound")
    found = " ".join(f"{t:.3f}s({s:.3f})" for t, s in clicks[:10])
    if len(clicks) > 10:
        found += f" +{len(clicks) - 10}"
    bad = bool(clicks or gaps)
    ok = (bad if args.expect_clicks else not bad) and not level and not pulled
    print(f"{'PASS' if ok else 'FAIL'} {args.wav} clicks={len(clicks)} "
          f"max_step={largest:.4f}@{largest_at:.3f}s peak={peak:.4f}"
          + (f" pulls={pulls}" if args.samples else "")
          + "".join(f" {p};" for p in level + pulled)
          + (f" at: {found}" if clicks else "")
          + (f" gaps={len(gaps)} at: " +
             " ".join(f"{t:.3f}s({d * 1000:.1f}ms)" for t, d in gaps[:10])
             if gaps else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
