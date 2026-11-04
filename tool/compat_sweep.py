#!/usr/bin/env python3
"""Plays every media file under a directory through lychee_cli builds.

For a compatibility sweep over sample collections (FFmpeg's FATE suite,
MPEG's file format conformance files, Chromium's media/test/data):

  python3 -I tool/compat_sweep.py --ffprobe ffprobe OUT.tsv ROOT \\
      noffmpeg=build-noffmpeg/examples/cli/lychee_cli \\
      ffmpeg=build/examples/cli/lychee_cli

ffprobe picks the files (any with an audio or video stream) and names
their format and codecs. Each build plays each file on the virtual clock
for --seconds, seeking to 5 s at 2 s, and gets a status:

  OK       played, no backward jumps, stalls, timeouts or lost decoders
  PROBLEM  played, with one of those
  OPEN     the player ended in an error (often an unsupported format)
  HANG     still running after --timeout s of wall time (killed)
  CRASH    exited with neither 0 nor 1

OUT.tsv gets a row per file and build; a summary per status and, for
the files whose status differs between builds, per format goes to
stdout.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

WARNINGS = re.compile(r'timed out|decoder lost|device lost|removed')
FIELDS = ('state', 'decoder', 'hw', 'rendered', 'dropped', 'glitches',
          'backward_jumps', 'stalls', 'decoded', 'position')


def probe(ffprobe, path):
    """(format, 'v:codec a:codec ...') or None if ffprobe sees no media."""
    try:
        out = subprocess.run(
            [ffprobe, '-v', 'error', '-show_entries',
             'format=format_name:stream=codec_type,codec_name', '-of', 'json',
             path], capture_output=True, timeout=60, check=False).stdout
        info = json.loads(out or b'{}')
    except (subprocess.TimeoutExpired, ValueError):
        return None
    streams = [s for s in info.get('streams', [])
               if s.get('codec_type') in ('video', 'audio')]
    if not streams:
        return None
    codecs = ' '.join('%s:%s' % (s['codec_type'][0], s.get('codec_name', '?'))
                      for s in streams)
    return info.get('format', {}).get('format_name', '?'), codecs


def play(cli, path, seconds, timeout):
    args = [cli, '--virtual-time', '--seconds', str(seconds), '--seek', '5',
            '--seek-after', '2', path]
    start = time.monotonic()
    try:
        done = subprocess.run(args, capture_output=True, timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired:
        return {'status': 'HANG', 'exit': '', 'wall': '%.1f' % timeout}
    wall = time.monotonic() - start
    out = (done.stdout + done.stderr).decode('utf-8', 'replace')
    row = {'exit': str(done.returncode), 'wall': '%.1f' % wall}
    summary = next((l for l in out.splitlines() if l.startswith('summary:')),
                   '')
    for field in FIELDS:
        match = re.search(r' %s=(\S+)' % field, summary)
        row[field] = match.group(1) if match else ''
    errors = [l for l in out.splitlines()
              if l.startswith('error:') or WARNINGS.search(l)]
    row['detail'] = ' | '.join(errors[:2])[:300]
    if done.returncode not in (0, 1) or not summary:
        row['status'] = 'CRASH'
    elif row['state'] == 'error' or done.returncode == 1:
        row['status'] = 'OPEN'
    elif (row['backward_jumps'] not in ('', '0') or
          row['stalls'] not in ('', '0') or WARNINGS.search(out)):
        row['status'] = 'PROBLEM'
    else:
        row['status'] = 'OK'
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('out')
    parser.add_argument('root')
    parser.add_argument('builds', nargs='+', metavar='NAME=CLI')
    parser.add_argument('--ffprobe', default='ffprobe')
    parser.add_argument('--jobs', type=int, default=max(1, os.cpu_count() // 2))
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--timeout', type=float, default=120)
    options = parser.parse_args()
    builds = [b.split('=', 1) for b in options.builds]

    paths = sorted(os.path.join(d, f) for d, dirs, files in os.walk(options.root)
                   if '.git' not in d.split(os.sep) for f in files)
    with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        probes = dict(zip(paths, pool.map(lambda p: probe(options.ffprobe, p),
                                          paths)))
    media = [p for p in paths if probes[p]]
    print('%d files, %d with audio or video' % (len(paths), len(media)),
          flush=True)

    columns = ['set', 'file', 'format', 'codecs', 'build', 'status', 'exit',
               'wall', *FIELDS, 'detail']
    results = collections.defaultdict(dict)
    with open(options.out, 'w', encoding='utf-8', newline='') as out, \
            concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        out.write('\t'.join(columns) + '\n')
        jobs = {pool.submit(play, cli, path, options.seconds,
                            options.timeout): (path, name)
                for path in media for name, cli in builds}
        for count, job in enumerate(concurrent.futures.as_completed(jobs), 1):
            path, name = jobs[job]
            row = job.result()
            relative = os.path.relpath(path, options.root)
            row.update({'set': relative.split(os.sep)[0], 'file': relative,
                        'format': probes[path][0], 'codecs': probes[path][1],
                        'build': name})
            results[path][name] = row['status']
            out.write('\t'.join(str(row.get(c, '')).replace('\t', ' ')
                                for c in columns) + '\n')
            out.flush()
            if count % 200 == 0:
                print('%d/%d runs' % (count, len(jobs)), flush=True)

    for name, _ in builds:
        counts = collections.Counter(r[name] for r in results.values())
        print('%s: %s' % (name, ', '.join('%s %d' % kv
                                          for kv in sorted(counts.items()))))
    differ = collections.Counter()
    for path, by_build in results.items():
        if len(set(by_build.values())) > 1:
            differ[(probes[path][0], ' '.join(
                '%s=%s' % kv for kv in sorted(by_build.items())))] += 1
    if differ:
        print('files whose status differs between builds, by format:')
        for (fmt, statuses), n in differ.most_common():
            print('  %4d  %-30s %s' % (n, fmt, statuses))
    return 0


if __name__ == '__main__':
    sys.exit(main())
