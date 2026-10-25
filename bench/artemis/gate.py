#!/usr/bin/env python3
"""gate.py - fail the build when something got slower

    gate.py --baseline bench/artemis/baseline.json --current out/<tag>/results.json \
            [--threshold 20] [--on min|median] [--strict-missing]

Compares every timing in the baseline against the same case in the current run,
prints one line per changed metric, and exits 1 if any of them regressed by more
than `threshold` percent.

Why a noise floor, and why the gate is selective
    This machine is shared: during the measurements that produced this baseline,
    `vm.loadavg` moved between 13 and 64 (iCloud indexing plus other people's
    builds).  The baseline's own seven runs are the honest measure of that noise,
    and they say something uncomfortable: most wall-clock metrics on this machine
    spread by hundreds of percent between runs, while the CPU-bound ones
    (memref, the PNG encoder, Find, the IPT composer) spread by under 20%.

    So a metric is only gated when the baseline's own spread is smaller than the
    threshold we are trying to detect - otherwise a "20% regression" is inside
    the noise and the gate would cry wolf on every run, which teaches everyone to
    ignore it.  The rest are still printed, marked `noisy`, so nothing is hidden.
    On an idle machine (CI), `--include-noisy --spread 1000` gates everything.

    A regression must also be worse than every run the baseline saw (not just
    its minimum), so a flag means "this cannot be the machine".

What is compared
    Only timings.  Rows whose unit is `ms` or `us` must not go up; rows in
    `MiB/s` or `/s` must not go down.  Counts, bytes and resident-memory rows are
    recorded in the baseline but not gated - they describe the machine and the
    data, not the engine's speed (and the resident-memory rows move with the page
    cache, not with the code).

Exit codes
    0  nothing regressed past the threshold (warnings allowed)
    1  at least one metric regressed past the threshold
    2  the baseline cannot be read, or every gated case is missing
"""

import argparse
import json
import sys

# units that mean "time": lower is better; "MiB/s" and "/s": higher is better
LOWER_IS_BETTER = {"ms", "us"}
HIGHER_IS_BETTER = {"MiB/s", "/s"}


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def rows_by_case(payload):
    out = {}
    for row in payload.get("rows", []):
        if row.get("kind") not in ("command", "derived"):
            continue
        out[(row["case"], row["metric"])] = row
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--threshold", type=float, default=20.0)
    ap.add_argument("--on", choices=["min", "median"], default="min")
    ap.add_argument("--strict-missing", action="store_true")
    ap.add_argument("--spread", type=float, default=25.0,
                    help="max baseline spread (max/min, %%) for a metric to be gated")
    ap.add_argument("--floor", type=float, default=1.0,
                    help="baseline minimum below which a metric is too small to gate")
    ap.add_argument("--load-margin", type=float, default=5.0,
                    help="absolute load1 headroom over the baseline before gating stops")
    ap.add_argument("--max-load-factor", type=float, default=1.5,
                    help="if this run's load is this much higher than the baseline's, "
                         "do not gate (the comparison is not meaningful)")
    ap.add_argument("--skip-prefix", default="memref_,applykey_,alloc_",
                    help="case prefixes that are reference ceilings, not measured paths")
    ap.add_argument("--include-noisy", action="store_true",
                    help="gate every metric, even those the baseline shows to be noisy")
    ap.add_argument("--warn-only", action="store_true")
    args = ap.parse_args()

    try:
        base_payload = load(args.baseline)
        cur_payload = load(args.current)
    except (OSError, json.JSONDecodeError) as e:
        print("gate: cannot read %s" % e, file=sys.stderr)
        return 2

    base = rows_by_case(base_payload)
    cur = rows_by_case(cur_payload)

    # Does this run even have the right to judge?  A benchmark gate that ignores
    # the load it ran under just relays the machine's mood.  Measured facts: the
    # same unmodified build scored 19 "regressions" on a run at load 140 that it
    # did not score at load 30.
    def median_load(payload):
        loads = [r.get("load_p50") or 0 for r in payload.get("rows", [])
                 if r.get("kind") == "command"]
        loads = sorted(v for v in loads if v > 0)
        return loads[len(loads) // 2] if loads else 0.0

    base_load = median_load(base_payload)
    cur_load = median_load(cur_payload)
    limit = max(base_load * args.max_load_factor, base_load + args.load_margin)
    too_busy = base_load > 0 and cur_load > limit
    print("gate: load1 median %.1f (baseline) vs %.1f (now), limit %.1f%s"
          % (base_load, cur_load, limit, "  -> NOT gating" if too_busy else ""))
    if too_busy:
        print()
        print("gate: SKIPPED - the machine is busier than the baseline's conditions")
        print("      (load1 %.1f -> %.1f, limit %.1f).  Timings from such a run are not"
              % (base_load, cur_load, limit))
        print("      comparable.  Rerun when it is quiet, or --max-load-factor 0")
        print("      --load-margin 0 to gate anyway (CI on a dedicated machine).")
        return 0
    if not base:
        print("gate: baseline has no rows", file=sys.stderr)
        return 2

    key = "min" if args.on == "min" else "p50"
    field = "min" if args.on == "min" else "median"

    regressed = []
    improved = []
    missing = []
    compared = 0

    print("gate: baseline %s" % base_payload.get("env", {}).get("tag", "?"))
    print("      %-34s %-24s %12s %12s %9s  %s"
          % ("case", "metric", "baseline", "now", "change", "verdict"))
    print("      " + "-" * 100)

    noisy = []
    for (case, metric), brow in sorted(base.items()):
        unit = brow.get("unit", "")
        if any(case.startswith(pfx) for pfx in args.skip_prefix.split(",") if pfx):
            # a CPU ceiling (memcpy, the XOR loops, the allocator) is a scale
            # reference, not a code path: on a shared machine it drifts with
            # memory contention by 30% while the real paths move by <2%, so it is
            # recorded and printed but never gated.
            noisy.append((case, metric, -1.0, 0.0))
            print("      %-34s %-24s %12s %12s %8s  ceiling (not gated)"
                  % (case[:34], ("%s [%s]" % (metric, unit))[:24], "-", "-", "-"))
            continue
        if unit in LOWER_IS_BETTER:
            direction = "lower"
        elif unit in HIGHER_IS_BETTER:
            direction = "higher"
        else:
            continue  # informational row: recorded, not gated
        crows = cur.get((case, metric))
        if crows is None:
            missing.append("%s / %s" % (case, metric))
            continue
        try:
            b = float(brow.get(key) or 0)
            c = float(crows.get(key) or 0)
            bmin = float(brow.get("min") or 0)
            bmax = float(brow.get("max") or 0)
        except (TypeError, ValueError):
            continue
        if b <= 0:
            continue
        # positive change = worse
        change = (c - b) / b * 100.0
        worse = change if direction == "lower" else -change

        # the same precondition, per case: a run can start quiet and get busy, and
        # a case measured at load 60 cannot be compared to one measured at 15
        bload = float(brow.get("load_p50") or 0)
        cload = float(crows.get("load_p50") or 0)
        row_limit = max(bload * args.max_load_factor, bload + args.load_margin)
        if bload > 0 and cload > row_limit:
            noisy.append((case, metric, -2.0, 0.0))
            print("      %-34s %-24s %12.3f %12.3f %8.1f%%  load %.0f->%.0f (not gated)"
                  % (case[:34], ("%s [%s]" % (metric, unit))[:24], b, c, change, bload, cload))
            continue

        spread = (bmax - bmin) / bmin * 100.0 if bmin > 0 else 1e9
        too_small = bmin < args.floor
        if (spread > args.spread or too_small) and not args.include_noisy:
            reason = "noisy(%.0f%% spread)" % spread if spread > args.spread else "too small"
            noisy.append((case, metric, spread, bmin))
            print("      %-34s %-24s %12.3f %12.3f %8.1f%%  %s"
                  % (case[:34], ("%s [%s]" % (metric, unit))[:24], b, c, change, reason))
            continue

        compared += 1
        # the current run must also be outside everything the baseline saw
        beyond_range = c > bmax if direction == "lower" else c < bmin
        if worse > args.threshold and beyond_range:
            verdict = "REGRESSED %+.0f%%" % worse
            regressed.append((case, metric, unit, b, c, worse))
        elif worse < -args.threshold:
            verdict = "faster %+.0f%%" % worse
            improved.append((case, metric, worse))
        else:
            verdict = "ok"
        print("      %-34s %-24s %12.3f %12.3f %8.1f%%  %s"
              % (case[:34], ("%s [%s]" % (metric, unit))[:24], b, c, change, verdict))

    print()
    print("gate: %d metric(s) gated, %d regressed, %d faster, %d too noisy to gate, %d missing"
          % (compared, len(regressed), len(improved), len(noisy), len(missing)))
    if missing:
        for m in missing:
            print("      missing: %s" % m)
    if regressed:
        print()
        print("gate: REGRESSION - these got slower than %.0f%%:" % args.threshold)
        for case, metric, unit, b, c, worse in regressed:
            print("      %-34s %-10s %.3f -> %.3f %s  (%+.0f%%)"
                  % (case, metric, b, c, unit, worse))
    if compared == 0:
        print("gate: no comparable metric - check the case names", file=sys.stderr)
        return 2
    if missing and args.strict_missing:
        return 1
    if regressed and not args.warn_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
