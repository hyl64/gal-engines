#!/usr/bin/env python3
"""frames_timeline.py - when did each picture appear, and how long from exec()

The number that decides whether a galgame feels native is not "frames per second
once it is running" but "how long after the click does the first picture appear".
agame has a headless mode that writes one BMP per line of the scenario, so the
first of those files is the first frame.

  firstframe.py --case NAME --runs N --frames DIR -- CMD...

Each run empties DIR, starts CMD under `/usr/bin/time -l` (for peak memory) and
polls DIR every 0.2 ms, recording the moment each file first appears.  That
gives the whole ladder without instrumenting the engine:

  CLI   <case> <run> <total_ms> <first_frame_ms> <peak_rss> <footprint> <load1>
  META  <case> frames <n>
  META  <case> timeline_ms <name=ms,name=ms,...>
  META  <case> intervals_ms <ms,ms,...>     (between consecutive files)

so `value` (the column the summary treats as the metric) is the first-frame
latency and `wall_ms` is the whole run.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

TIME = "/usr/bin/time"
RE_MAXRSS = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.M)
RE_FOOT = re.compile(r"^\s*(\d+)\s+peak memory footprint\s*$", re.M)


def files_now(directory):
    try:
        with os.scandir(directory) as it:
            return sorted(e.name for e in it if e.is_file())
    except FileNotFoundError:
        return []


def first_file(directory):
    names = files_now(directory)
    return names[0] if names else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--frames", required=True)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        ap.error("no command given")

    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")

    for run in range(1, args.runs + 1):
        shutil.rmtree(args.frames, ignore_errors=True)
        os.makedirs(args.frames, exist_ok=True)
        try:
            load = os.getloadavg()[0]
        except OSError:
            load = -1.0

        t0 = time.perf_counter()
        proc = subprocess.Popen([TIME, "-l"] + cmd, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        seen = {}       # file name -> ms after exec
        first_ms = -1.0
        while True:
            now_ms = (time.perf_counter() - t0) * 1000.0
            for name in files_now(args.frames):
                if name not in seen:
                    seen[name] = now_ms
            if seen and first_ms < 0:
                first_ms = min(seen.values())
            if first_ms >= 0 and proc.poll() is not None:
                break
            if proc.poll() is not None and not seen:
                break
            time.sleep(0.0002)
        _, err = proc.communicate()
        total_ms = (time.perf_counter() - t0) * 1000.0

        text = err.decode("utf-8", "replace")
        rss = foot = 0
        m = RE_MAXRSS.search(text)
        if m:
            rss = int(m.group(1))
        m = RE_FOOT.search(text)
        if m:
            foot = int(m.group(1))
        for name in files_now(args.frames):
            seen.setdefault(name, total_ms)
        order = sorted(seen, key=lambda n: seen[n])
        frames = len(order)

        print("CLI\t%s\t%d\t%.4f\t%.4f\t%d\t%d\t%.2f"
              % (args.case, run, total_ms, first_ms, rss, foot, load))
        print("META\t%s\tframes\t%d" % (args.case, frames))
        print("META\t%s\ttimeline_ms\t%s"
              % (args.case, ",".join("%s=%.1f" % (n, seen[n]) for n in order)))
        intervals = [seen[b] - seen[a] for a, b in zip(order, order[1:])]
        if intervals:
            print("META\t%s\tintervals_ms\t%s"
                  % (args.case, ",".join("%.1f" % v for v in intervals)))
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
