#!/usr/bin/env python3
"""measure.py - run a command N times and record what it cost

Every number in this bench comes through here, so that the units, the repetitions
and the "cold first run" all mean the same thing everywhere.

  measure.py --case NAME --runs N [--stat METRIC]... [--pre CMD] [--env K=V]... -- CMD...

What it records, one tab-separated line per run, on stdout:

  CLI   <case> <run> <wall_ms> <value> <peak_rss> <footprint> <load1>
  PCT   <case> <run> <metric> <p50> <p95> <min> <max> <n> <unit>
  META  <case> <key> <value>

  wall_ms    perf_counter around the child process (fork+exec included)
  value      the metric named by --stat, when given; otherwise wall_ms
  peak_rss   `maximum resident set size` from /usr/bin/time -l, in bytes
  footprint  `peak memory footprint` from the same, in bytes (what Activity
             Monitor calls memory)
  load1      1-minute load average when the run started: this machine runs other
             people's builds, so a number without its load is not a number

`--stat` accepts either a STAT name or a PCT metric (then its p50 is used), so an
in-process benchmark that reports its own percentiles can be driven exactly like
a command line tool.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import time

TIME = "/usr/bin/time"

RE_MAXRSS = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.M)
RE_FOOT = re.compile(r"^\s*(\d+)\s+peak memory footprint\s*$", re.M)
# the block /usr/bin/time -l appends to the child's stderr; it is not the child's words
RE_TIME_BLOCK = re.compile(
    r"^\s*[\d.]+ real\s+[\d.]+ user.*$|"
    r"^\s*\d+\s+(?:maximum resident set size|average shared memory size|"
    r"average unshared data size|average unshared stack size|page reclaims|page faults|"
    r"swaps|block input operations|block output operations|messages sent|"
    r"messages received|signals received|voluntary context switches|"
    r"involuntary context switches|instructions retired|cycles elapsed|"
    r"peak memory footprint)\s*$",
    re.M)


def loadavg():
    try:
        return os.getloadavg()[0]
    except OSError:
        return -1.0


def parse_time_output(text):
    """Pull the two memory numbers out of `/usr/bin/time -l` stderr."""
    rss = foot = 0
    m = RE_MAXRSS.search(text)
    if m:
        rss = int(m.group(1))
    m = RE_FOOT.search(text)
    if m:
        foot = int(m.group(1))
    return rss, foot


def parse_stats(text, wanted):
    """Find the metrics a child reported on stdout (STAT or PCT lines)."""
    found = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        kind, name = parts[0], parts[1]
        if name not in wanted or name in found:
            continue
        try:
            if kind == "STAT":
                found[name] = float(parts[2])
            elif kind == "PCT":
                found[name] = float(parts[2])  # p50
        except (ValueError, IndexError):
            continue
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", required=True)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--stat", action="append", default=[])
    ap.add_argument("--pre", default="")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--allow-exit", type=int, default=0,
                    help="exit status that still counts as a normal run (default 0)")
    ap.add_argument("--discard", action="store_true",
                    help="send the child's stdout to /dev/null (when no --stat is needed)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        ap.error("no command given")

    env = dict(os.environ)
    for pair in args.env:
        k, _, v = pair.partition("=")
        env[k] = v

    out = sys.stdout
    for run in range(1, args.runs + 1):
        if args.pre:
            subprocess.run(args.pre, shell=True, env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        load = loadavg()
        t0 = time.perf_counter()
        sink = subprocess.DEVNULL if args.discard else subprocess.PIPE
        proc = subprocess.run([TIME, "-l"] + cmd, env=env, stdout=sink,
                              stderr=subprocess.PIPE)
        wall_ms = (time.perf_counter() - t0) * 1000.0
        stdout_text = proc.stdout.decode("utf-8", "replace") if proc.stdout else ""
        stderr_text = proc.stderr.decode("utf-8", "replace")
        rss, foot = parse_time_output(stderr_text)

        value = wall_ms
        if args.stat:
            found = parse_stats(stdout_text, set(args.stat))
            for name in args.stat:
                if name in found:
                    value = found[name]
                    break
            for line in stdout_text.splitlines():
                parts = line.split("\t")
                if len(parts) >= 3 and parts[0] == "STAT":
                    out.write("META\t%s\t%s\t%s\n" % (args.case, parts[1], parts[2]))
                elif len(parts) >= 3 and parts[0] == "META":
                    # the child's own account of itself: mmap residency, entry
                    # count, how many bytes it really touched
                    out.write("META\t%s\t%s\t%s\n" % (args.case, parts[1], parts[2]))
                elif len(parts) >= 9 and parts[0] == "PCT":
                    out.write("PCT\t%s\t%d\t%s\n" % (args.case, run, "\t".join(parts[1:])))

        out.write("CLI\t%s\t%d\t%.4f\t%.6f\t%d\t%d\t%.2f\t%s\n"
                  % (args.case, run, wall_ms, value, rss, foot, load,
                     time.strftime("%H:%M:%S")))
        own_stderr = RE_TIME_BLOCK.sub("", stderr_text).strip()
        out.write("META\t%s\texit_code\t%d\n" % (args.case, proc.returncode))
        if proc.returncode not in (0, args.allow_exit):
            snippet = own_stderr.splitlines()[-1:] or [""]
            out.write("META\t%s\texit_stderr\t%s\n" % (args.case, snippet[0][:200]))
        out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
