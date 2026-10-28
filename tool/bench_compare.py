#!/usr/bin/env python3
"""Compares the last commit in a tool/bench.sh table with the one before.

  bench_compare.py [TSV]            default build-release/bench.tsv
  bench_compare.py TSV --github     also a GitHub warning per regression

Prints a Markdown table per case: fps, open and seek latency and peak
memory of the newest commit's rows, and the change from the newest rows
of the case for an earlier commit. A case regresses when fps drops by
more than --fps (default 10 %) or open_ms or seek_ms grow by more than
--latency (default 25 %) and 5 ms; only the rows of one machine compare.
Always exits 0: a benchmark on one machine warns, it does not gate.
"""

import argparse
import csv
import os
import sys


def number(value):
    try:
        return float(value)
    except ValueError:
        return None


def change(new, old):
    if new is None or old is None or old == 0:
        return None
    return (new - old) / old


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv", nargs="?", default="build-release/bench.tsv")
    parser.add_argument("--github", action="store_true")
    parser.add_argument("--fps", type=float, default=0.10)
    parser.add_argument("--latency", type=float, default=0.25)
    args = parser.parse_args()

    with open(args.tsv, newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    if not rows:
        print("no rows")
        return
    commit = rows[-1]["commit"]
    # The newest row per case: this commit's, and an earlier commit's.
    current, previous = {}, {}
    for row in rows:
        if row["commit"] == commit:
            current[row["case"]] = row
        else:
            previous[row["case"]] = row

    print(f"Benchmark of {commit}\n")
    print("| case | decoder | fps | open_ms | seek_ms | rss_mb | against |")
    print("|---|---|---:|---:|---:|---:|---|")
    regressions = []
    for case, row in current.items():
        old = previous.get(case)
        cells = []
        for key in ("fps", "open_ms", "seek_ms", "rss_mb"):
            new_value = number(row[key])
            delta = change(new_value, number(old[key])) if old else None
            cells.append(row[key] + (f" ({delta:+.0%})" if delta is not None else ""))
        print(f"| {case} | {row['decoder']} | " + " | ".join(cells) +
              f" | {old['commit'] if old else '-'} |")
        if not old:
            continue
        fps = change(number(row["fps"]), number(old["fps"]))
        if fps is not None and fps < -args.fps:
            regressions.append(f"{case}: fps {old['fps']} -> {row['fps']} ({fps:+.0%})")
        for key in ("open_ms", "seek_ms"):
            new_value, old_value = number(row[key]), number(old[key])
            delta = change(new_value, old_value)
            if delta is not None and delta > args.latency and new_value - old_value > 5:
                regressions.append(f"{case}: {key} {old[key]} -> {row[key]} ({delta:+.0%})")
    print()
    for line in regressions:
        print(f"- regression: {line}")
        if args.github:
            print(f"::warning title=Benchmark::{line} against {previous[line.split(':')[0]]['commit']}",
                  file=sys.stderr)
    if not regressions:
        print("no regressions")


if __name__ == "__main__":
    sys.exit(main())
