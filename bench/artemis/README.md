# bench/artemis - what the container layer actually costs

Real data, real binaries, reproducible in one command:

```bash
./bench/artemis/bench.sh                  # ~5 minutes, 7 runs per case, whole stack
TAG=after ./bench/artemis/bench.sh        # name a run, for before/after tables
./bench/artemis/bench.sh --baseline       # re-take the regression baseline
./bench/artemis/bench.sh --check          # exit 1 if a timing regressed >20%
```

Results, the bottleneck analysis and the recommendations are in **[RESULTS.md](RESULTS.md)**.
Start there.

## What is here

| file | role |
|---|---|
| `bench.sh` | orchestrates everything; waits for `artemis/build` to stop changing, then measures |
| `measure.py` | runs a command N times, records wall clock, peak RSS and `peak memory footprint` from `/usr/bin/time -l`, plus whatever the child reports as `STAT`/`PCT` |
| `firstframe.py` | polls a frame directory every 0.2 ms, so "exec to first picture" and per-frame intervals are measured from outside the engine |
| `index_scale.py` | the 2891-entry point on the index-cost curve, from the web release's own metadata |
| `summarize.py` | median / p95 / min / max / cold-first-run across runs -> `results.csv`, `results.json`, `report.txt` |
| `src/pfsbench.cpp` | in-process: `open`, `rand`, `seq`, `one`, `find`, `types`, `hold`, `memref`, `alloc` - links `libartemis.a`, so it times the library the engine uses |
| `src/pngbench.cpp` | image layer: decode 73 real PNGs, or time `WritePng`/`CountColors` on a real frame |
| `src/applykey-check.cpp` | proof harness for R1: the old and the proposed XOR loop, byte-for-byte over 368,734 cases, then timed |
| `src/sniff-equivalence.cpp` | proof harness for R2: `Sample(512)` must be the prefix of `Content()` and sniff identically, over all 1807 real members |
| `gate.py` | the regression gate: noise-aware, load-aware, exits 1 on a real regression |
| `baseline.json` | the numbers the gate compares against (`--baseline` rewrites it) |
| `internal.py` | pulls a layer's own timer out of a tool's report (`fxtool sfade` 0.658 ms, `audiotool` decode 28 ms) |
| `src/find-equivalence.cpp` | proof harness for R3: 3,058 lookup answers must be identical before/after |
| `patches/R2-bounded-sniff.patch` | R2, ready to `git apply` from `artemis/` |
| `patches/R3-find-precomputed-name.patch` | R3, applied 17:21 (`1e3ae02`) |

`out/<tag>/` holds each run: `env.txt` (machine, data mtimes, every binary's mtime and sha256,
`git rev-parse HEAD`), `raw.tsv` (every single run, unaggregated), `results.csv`,
`results.json`, `report.txt`, and the raw output of the harnesses.

## Two things worth knowing before reading numbers

1. **The machine is shared.** `vm.loadavg` was 13-64 during these runs (iCloud indexing plus
   other people's builds). Every table carries both median and min; `min` is the least
   contended sample and the closest thing to the CPU's real cost.
2. **A gate needs its own preconditions.** The baseline's own seven runs spread by
   hundreds of percent on wall-clock metrics, so only 30 CPU-bound metrics are gated;
   and a run is skipped outright when the machine is busier than the baseline's
   conditions. See RESULTS.md §0.2 - it is the most useful thing in this directory.
3. **In-process beats command line for comparability.** `pfsbench`/`pngbench` link
   `libartemis.a` at the moment `bench.sh` builds them, so their numbers are locked to one
   library version even if a teammate relinks mid-run. Command-line cases include process
   startup (~8 ms) and can straddle a rebuild; `bench.sh` prints a warning if that happens.

Nothing here writes to `artemis/` or `testdata/`. Everything goes to `out/` and `/tmp`.
