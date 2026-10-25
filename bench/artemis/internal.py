#!/usr/bin/env python3
"""internal.py - take a number out of a tool's own report

Some layers time themselves and say so: `fxtool sfade` prints `apply 0.656 ms,
4154.8 MiB/s`, `audiotool probe` prints `decode time: 0.036 s wall`,
`audiotool play` prints `rendered ... in 0.045 s wall`.  Those numbers are worth
much more than the tool's wall clock, because they leave out process start, PNG
decoding and file writing - the things a per-frame budget does not include.

  internal.py --case NAME --runs N --regex 'apply\\s+([\\d.]+) ms' [--scale 1] \
              [--unit ms] -- CMD...

Emits the same CLI records measure.py does, so summarize.py and gate.py treat
these exactly like any other measurement:

  CLI  <case> <run> <wall_ms> <value> <peak_rss> <footprint> <load1> <hh:mm:ss>
"""

import argparse
import os
import re
import subprocess
import sys
import time

TIME = "/usr/bin/time"
RE_MAXRSS = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.M)
RE_FOOT = re.compile(r"^\s*(\d+)\s+peak memory footprint\s*$", re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--regex", required=True)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--unit", default="ms")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        ap.error("no command given")
    pattern = re.compile(args.regex)

    for run in range(1, args.runs + 1):
        try:
            load = os.getloadavg()[0]
        except OSError:
            load = -1.0
        t0 = time.perf_counter()
        proc = subprocess.run([TIME, "-l"] + cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        wall_ms = (time.perf_counter() - t0) * 1000.0
        text = proc.stdout.decode("utf-8", "replace") + proc.stderr.decode("utf-8", "replace")
        match = pattern.search(text)
        if match is None:
            print("internal: %s: pattern not found in the tool's output" % args.case,
                  file=sys.stderr)
            print("META\t%s\texit_code\t%d" % (args.case, proc.returncode))
            continue
        value = float(match.group(1)) * args.scale
        rss = foot = 0
        m = RE_MAXRSS.search(text)
        if m:
            rss = int(m.group(1))
        m = RE_FOOT.search(text)
        if m:
            foot = int(m.group(1))
        print("CLI\t%s\t%d\t%.4f\t%.6f\t%d\t%d\t%.2f\t%s"
              % (args.case, run, wall_ms, value, rss, foot, load,
                 time.strftime("%H:%M:%S")))
        print("META\t%s\tunit_internal\t%s" % (args.case, args.unit))
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
