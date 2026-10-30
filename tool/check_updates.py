#!/usr/bin/env python3
"""Lists newer releases of the sources the repository pins by hand.

  python3 -I tool/check_updates.py            a table
  python3 -I tool/check_updates.py --github   also a GitHub notice per update

Dependabot reads none of these pins:
  FFmpeg    ffmpeg_version in tool/release.sh (what every platform's
            FFmpeg builds from, and the source the releases carry)
  GLFW      the URL in examples/glfw/CMakeLists.txt (the Windows harness;
            macOS and Linux take the system's GLFW)

For each it prints the pinned version, the newest release with the same
major and minor version (a patch to take as it is) and the newest release
overall (a new FFmpeg major changes the API the core follows).

Updating a pin means its version and SHA-256
together. Only reads: it opens nothing and
always exits 0 unless a source cannot be read. GITHUB_TOKEN, if set, lifts
GitHub's rate limit for unauthenticated requests.
"""

import argparse
import json
import os
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent


def fetch(url):
    headers = {"User-Agent": "lychee_player-check-updates"}
    if "api.github.com" in url and os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = f"Bearer {os.environ['GITHUB_TOKEN']}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
        return r.read().decode()


def key(version):
    return tuple(int(p) for p in version.split("."))


def github_releases(repo, tag_pattern):
    """Versions of the repository's published, non-prerelease releases."""
    versions = []
    for page in range(1, 4):
        releases = json.loads(fetch(f"https://api.github.com/repos/{repo}/releases?per_page=100&page={page}"))
        for release in releases:
            m = re.fullmatch(tag_pattern, release["tag_name"])
            if m and not release["draft"] and not release["prerelease"]:
                versions.append(m.group(1))
        if len(releases) < 100:
            break
    return versions


def ffmpeg_releases():
    # The release archives ffmpeg.org lists; snapshots and RCs do not match.
    return sorted(set(re.findall(r'href="ffmpeg-(\d+(?:\.\d+)+)\.tar\.xz"', fetch("https://ffmpeg.org/releases/"))),
                  key=key)


def pins():
    release = (ROOT / "tool/release.sh").read_text()
    glfw = (ROOT / "examples/glfw/CMakeLists.txt").read_text()
    return {
        "FFmpeg": (re.search(r"^ffmpeg_version=(\S+)", release, re.M).group(1), "tool/release.sh",
                   ffmpeg_releases),
        "GLFW": (re.search(r"glfw/releases/download/([\d.]+)/", glfw).group(1), "examples/glfw/CMakeLists.txt",
                 lambda: github_releases("glfw/glfw", r"(\d+\.\d+(?:\.\d+)?)")),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--github", action="store_true")
    args = parser.parse_args()

    rows, updates, failed = [], [], False
    for name, (pinned, where, releases) in pins().items():
        try:
            versions = releases()
        except Exception as e:  # noqa: BLE001 - report every source
            rows.append((name, pinned, "?", "?", where, f"cannot read: {e}"))
            failed = True
            continue
        series = key(pinned)[:2]
        patch = max((v for v in versions if key(v)[:2] == series), key=key, default=pinned)
        newest = max(versions, key=key, default=pinned)
        notes = []
        if key(patch) > key(pinned):
            notes.append(f"patch {patch}")
        if key(newest)[:2] > series:
            notes.append(f"new release {newest}")
        rows.append((name, pinned, patch, newest, where, ", ".join(notes) or "up to date"))
        updates += [f"{name} {pinned} in {where}: {n}" for n in notes]

    print("| source | pinned | newest patch | newest | pinned in | |")
    print("|---|---|---|---|---|---|")
    for row in rows:
        print("| " + " | ".join(row) + " |")
    if args.github:
        for line in updates:
            print(f"::notice title=Pinned source update::{line}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
