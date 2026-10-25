#!/usr/bin/env python3
"""summarize.py - turn the raw per-run records into a table, a CSV and a JSON file

  summarize.py --raw out/raw.tsv --labels labels.tsv --csv out/results.csv \
               --json out/results.json [--md out/tables.md]

Input records (see measure.py / firstframe.py / pfsbench):

  CLI   <case> <run> <wall_ms> <value> <peak_rss> <footprint> <load1>
  PCT   <case> <run> <metric> <p50> <p95> <min> <max> <n> <unit>
  META  <case> <key> <value>

Output is one row per (case, metric) with median / p95 / min / max across runs,
plus the first run on its own: this machine is warm-cached after run 1, and the
first run is what a player actually experiences, so both are kept.

`--labels` names the unit of each case's primary metric:

  <case>\t<metric label>\t<unit>\t<note>
"""

import argparse
import json
import math
import statistics
import sys


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    rank = int(math.ceil(p / 100.0 * len(sorted_values)))
    rank = max(1, min(rank, len(sorted_values)))
    return sorted_values[rank - 1]


def read_labels(path):
    labels = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                case = parts[0]
                labels[case] = (
                    parts[1] if len(parts) > 1 and parts[1] else case,
                    parts[2] if len(parts) > 2 and parts[2] else "ms",
                    parts[3] if len(parts) > 3 else "",
                )
    except FileNotFoundError:
        pass
    return labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True)
    ap.add_argument("--labels", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--json", required=True)
    ap.add_argument("--md", default="")
    args = ap.parse_args()

    labels = read_labels(args.labels)
    cli = {}     # case -> list of dicts
    pct = {}     # (case, metric) -> list of (run, p50, p95, min, max, n, unit)
    meta = {}    # case -> dict
    order = []   # case order of first appearance

    with open(args.raw, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            kind, case = parts[0], parts[1]
            if case not in order:
                order.append(case)
            if kind == "CLI" and len(parts) >= 8:
                cli.setdefault(case, []).append({
                    "run": int(parts[2]),
                    "wall_ms": float(parts[3]),
                    "value": float(parts[4]),
                    "rss": int(parts[5]),
                    "foot": int(parts[6]),
                    "load": float(parts[7]),
                })
            elif kind == "PCT" and len(parts) >= 10:
                pct.setdefault((case, parts[3]), []).append({
                    "run": int(parts[2]),
                    "p50": float(parts[4]),
                    "p95": float(parts[5]),
                    "min": float(parts[6]),
                    "max": float(parts[7]),
                    "n": int(parts[8]),
                    "unit": parts[9],
                })
            elif kind == "META" and len(parts) >= 4:
                meta.setdefault(case, {}).setdefault(parts[2], []).append(parts[3])

    rows = []

    def row(**kw):
        base = {
            "kind": "", "case": "", "metric": "", "unit": "ms", "n": 0,
            "p50": 0.0, "p95": 0.0, "min": 0.0, "max": 0.0, "cold": 0.0,
            "rss_peak_mib": 0.0, "foot_peak_mib": 0.0, "load_p50": 0.0,
            "note": "", "extra": {},
        }
        base.update(kw)
        rows.append(base)

    for case in order:
        if case in cli:
            runs = sorted(cli[case], key=lambda r: r["run"])
            values = sorted(r["value"] for r in runs)
            walls = sorted(r["wall_ms"] for r in runs)
            warm = values[1:] if len(values) > 1 else values
            label, unit, note = labels.get(case, (case, "ms", ""))
            row(kind="command", case=case, metric=label, unit=unit, n=len(runs),
                p50=percentile(warm, 50), p95=percentile(warm, 95),
                min=values[0], max=values[-1], cold=runs[0]["value"],
                rss_peak_mib=max(r["rss"] for r in runs) / 1048576.0,
                foot_peak_mib=max(r["foot"] for r in runs) / 1048576.0,
                load_p50=percentile(sorted(r["load"] for r in runs), 50),
                note=(meta.get(case, {}).get("exit_stderr") or [""])[-1] or note,
                extra={
                    "wall_p50_ms": percentile(walls[1:] if len(walls) > 1 else walls, 50),
                    "wall_cold_ms": runs[0]["wall_ms"],
                    "exit_code": (meta.get(case, {}).get("exit_code") or ["0"])[-1],
                    "frames": (meta.get(case, {}).get("frames") or [""])[-1],
                })
        # numbers the harnesses reported about themselves, one value per run:
        # throughputs get their own row, and the facts behind the mmap claim
        # (how much of the archive is resident) get one too
        for key, values in sorted(meta.get(case, {}).items()):
            try:
                numbers = sorted(float(v) for v in values)
            except ValueError:
                continue
            mid = percentile(numbers, 50)
            if key.endswith("_mibs") or key.endswith("_per_s"):
                unit = "MiB/s" if key.endswith("_mibs") else "/s"
                row(kind="derived", case=case, metric=key, unit=unit, n=len(numbers),
                    p50=mid, p95=percentile(numbers, 95), min=numbers[0],
                    max=numbers[-1], cold=float(values[0]))
            elif key in ("resident_bytes", "mapping_bytes", "entries_seen", "frames",
                         "footprint_after_open", "rand_content_mean_kib"):
                unit = {"resident_bytes": "B", "mapping_bytes": "B", "entries_seen": "count",
                        "frames": "count", "footprint_after_open": "MiB",
                        "rand_content_mean_kib": "KiB"}[key]
                row(kind="backing", case=case, metric=key, unit=unit, n=len(numbers),
                    p50=mid, p95=percentile(numbers, 95), min=numbers[0],
                    max=numbers[-1], cold=float(values[0]))
        for (cs, metric), samples in sorted(pct.items()):
            if cs != case:
                continue
            samples = sorted(samples, key=lambda s: s["run"])
            warm = samples[1:] if len(samples) > 1 else samples
            row(kind="distribution", case=case, metric=metric,
                unit=samples[0]["unit"], n=max(s["n"] for s in samples),
                p50=percentile(sorted(s["p50"] for s in warm), 50),
                p95=percentile(sorted(s["p95"] for s in warm), 50),
                min=min(s["min"] for s in samples),
                max=max(s["max"] for s in samples),
                cold=samples[0]["p50"],
                extra={"samples_per_run": samples[0]["n"]})

    # ---------------------------------------------------------------- CSV
    with open(args.csv, "w", encoding="utf-8") as f:
        f.write("case,kind,metric,unit,n,p50,p95,min,max,cold_run1,"
                "peak_rss_mib,peak_footprint_mib,load1_p50,note\n")
        for r in rows:
            note = (r["note"] or "").replace(",", ";").replace("\n", " ")[:160]
            f.write("%s,%s,%s,%s,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%s\n" % (
                r["case"], r["kind"], r["metric"], r["unit"], r["n"], r["p50"], r["p95"],
                r["min"], r["max"], r["cold"], r["rss_peak_mib"], r["foot_peak_mib"],
                r["load_p50"], note))

    # ---------------------------------------------------------------- JSON
    env = {}
    with open(args.raw, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4 and parts[0] == "META" and parts[1] == "@env":
                env[parts[2]] = parts[3]
    payload = {"env": env, "rows": rows}
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=1, sort_keys=False)

    # --------------------------------------------------------------- human
    lines = []
    lines.append("%-34s %-22s %10s %10s %10s %10s %9s %9s %6s" % (
        "case", "metric", "p50", "p95", "min", "max", "cold(1)", "rss_MiB", "n"))
    lines.append("-" * 132)
    last_kind = None
    for r in rows:
        if r["kind"] != last_kind:
            title = {"command": "commands", "distribution": "in-process distributions",
                     "derived": "throughput (from the same runs)",
                     "backing": "backing store / what the open touched"}.get(r["kind"], r["kind"])
            lines.append("[%s]" % title)
            last_kind = r["kind"]
        lines.append("%-34s %-22s %10.3f %10.3f %10.3f %10.3f %9.3f %9.1f %6d  %s" % (
            r["case"][:34], ("%s [%s]" % (r["metric"], r["unit"]))[:22], r["p50"], r["p95"],
            r["min"], r["max"], r["cold"], r["rss_peak_mib"], r["n"], r["note"][:40]))
    text = "\n".join(lines)
    print(text)
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write("```\n" + text + "\n```\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
