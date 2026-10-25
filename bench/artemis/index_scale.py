#!/usr/bin/env python3
"""index_scale.py - does the index cost scale the way the entry count does

Two real archives bracket the question (125 and 1682 entries); the web release's
`archive_index.txt` says a shipped title can list 2891 members.  This builds a
synthetic archive of that size with pfstool's own writer and measures Open() on
it, so the third point on the curve is measured rather than extrapolated.

  index_scale.py --pfstool <pfstool> --pfsbench <pfsbench> \
                 --index testdata/artemis-raspberry-web/archive_index.txt \
                 --scratch <dir>

Prints STAT/META records in the same shape as the other harnesses, plus a short
human-readable note about the real web release's metadata.
"""

import argparse
import os
import re
import subprocess
import sys


def analyse_index(path):
    """What the web release's own index says about itself."""
    rows = 0
    total = 0
    biggest = 0
    ext = {}
    header = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#"):
                    # "# magic=pf8 index_size=154996 file_count=2891 total_bytes=222878531"
                    for token in line.lstrip("# ").split():
                        if "=" in token:
                            k, _, v = token.partition("=")
                            header[k] = v
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3:
                    continue
                try:
                    size = int(parts[2])
                except ValueError:
                    continue
                rows += 1
                total += size
                biggest = max(biggest, size)
                e = parts[3].lower() if len(parts) > 3 and parts[3] else os.path.splitext(parts[0])[1].lower()
                ext[e] = ext.get(e, 0) + 1
    except FileNotFoundError:
        return None
    return {"rows": rows, "total_bytes": total, "biggest": biggest, "ext": ext,
            "header": header}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pfstool", required=True)
    ap.add_argument("--pfsbench", required=True)
    ap.add_argument("--index", default="")
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--entries", type=int, default=2891)
    args = ap.parse_args()

    info = analyse_index(args.index) if args.index else None
    if info:
        for k, v in sorted(info["header"].items()):
            print("META\tindex_header_%s\t%s" % (k, v))
        print("META\tindex_rows\t%d" % info["rows"])
        print("META\tindex_total_bytes\t%d" % info["total_bytes"])
        print("META\tindex_biggest\t%d" % info["biggest"])
        top = sorted(info["ext"].items(), key=lambda kv: -kv[1])[:5]
        print("META\tindex_top_ext\t%s" % " ".join("%s:%d" % (k, v) for k, v in top))
        print("  web release index: %d entries, %.1f MiB of content, largest %d B%s"
              % (info["rows"], info["total_bytes"] / 1048576.0, info["biggest"],
                 ("  (" + " ".join("%s=%d" % (k, v) for k, v in top) + ")") if top else ""))

    # a synthetic archive with as many members as the web release lists: the
    # writer only needs names, and the payload can be one shared 16-byte file
    os.makedirs(args.scratch, exist_ok=True)
    payload = os.path.join(args.scratch, "payload.bin")
    with open(payload, "wb") as f:
        f.write(b"artemis-bench-16")
    archive = os.path.join(args.scratch, "synth.pfs")
    if not os.path.exists(archive):
        pairs = ["dir%04d\\file%04d.ogg=%s" % (i // 100, i, payload)
                 for i in range(args.entries)]
        proc = subprocess.run([args.pfstool, "create", archive, "--magic", "pf6"] + pairs,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            print("META\tsynth_error\t%s" % proc.stderr.decode("utf-8", "replace")[:160])
            return 1

    out = subprocess.run([args.pfsbench, "open", archive, "20"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = out.stdout.decode("utf-8", "replace")
    entries = re.search(r"^META\tentries\t(\d+)$", text, re.M)
    warm = re.search(r"^PCT\topen_warm_ms\t([\d.]+)\t([\d.]+)", text, re.M)
    first = re.search(r"^PCT\topen_first_ms\t([\d.]+)", text, re.M)
    if not entries or not warm:
        print("META\tsynth_error\tpfsbench open produced no numbers")
        return 1
    print("STAT\tsynth_open_first_ms\t%s\tms" % first.group(1))
    print("STAT\tsynth_open_warm_ms\t%s\tms" % warm.group(1))
    print("STAT\tsynth_open_p95_ms\t%s\tms" % warm.group(2))
    print("META\tsynth_entries\t%s" % entries.group(1))
    print("META\tsynth_bytes\t%s" % os.path.getsize(archive))
    print("  synthetic %s-entry archive: Open() warm %s ms (p95 %s), first %s ms"
          % (entries.group(1), warm.group(1), warm.group(2), first.group(1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
