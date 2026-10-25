#!/usr/bin/env bash
# long_run_check.sh -- C3: does the real game survive ten minutes without crashing
# or growing?
#
#   ./test/video/long_run_check.sh [minutes=10] [path/to/onsyuri]
#
# Drives the trial with timed clicks (the title needs a button press to leave) and
# samples RSS + CPU once a minute.  Reports:
#   - whether the process is still alive at the end
#   - RSS at the first and last sample, and the peak
#   - any crash in the log
# Exit 0 = survived and RSS did not grow beyond 1.5x.
set -u

MINUTES="${1:-10}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE_ROOT="$(cd "$HERE/../.." && pwd)"
TREE="$(cd "$ENGINE_ROOT/.." && pwd)"
BIN="${2:-$ENGINE_ROOT/build/onsyuri}"
GAME="$TREE/testdata/ns-07th-trt/game/trt_trial"

[ -x "$BIN" ] || { echo "no binary: $BIN"; exit 2; }
[ -d "$GAME" ] || { echo "no game data: $GAME"; exit 2; }

OUT="$(mktemp -d /tmp/ons_longrun.XXXXXX)"
echo "work: $OUT"

( cd "$GAME" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ONS_AUTOCLICK_MS=1500 ONS_AUTOCLICK_X=305 ONS_AUTOCLICK_Y=424 \
    exec "$BIN" --root . --enc:sjis --window > "$OUT/run.log" 2>&1 ) &
PID=$!
echo "pid: $PID"
sleep 3

first=""; last=""; peak=0; crashes=0
for m in $(seq 1 "$MINUTES"); do
    if ! kill -0 $PID 2>/dev/null; then
        echo "DIED after about $((m-1)) minute(s)"
        tail -5 "$OUT/run.log" | sed 's/^/    /'
        crashes=1
        break
    fi
    rss=$(ps -o rss= -p $PID 2>/dev/null | tr -d ' ')
    [ -z "$rss" ] && rss=0
    [ -z "$first" ] && first=$rss
    last=$rss
    [ "$rss" -gt "$peak" ] && peak=$rss
    printf "  minute %2d  rss=%6d KB\n" "$m" "$rss"
    sleep 60
done

kill -TERM $PID 2>/dev/null; sleep 2; kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null

if [ "$crashes" -eq 0 ] && ! grep -qiE "segmentation|abort trap|signal [0-9]+," "$OUT/run.log"; then
    echo "no crash in the log"
else
    crashes=1
    echo "crash indicators found in the log"
fi

echo
echo "first=${first}KB last=${last}KB peak=${peak}KB"
if [ -z "$first" ] || [ "$first" -eq 0 ]; then
    echo "RESULT: FAIL - never sampled the process"
    exit 1
fi
limit=$(( ${first} * 3 / 2 ))
if [ "$crashes" -eq 0 ] && [ "$peak" -lt "$limit" ]; then
    echo "RESULT: PASS - survived ${MINUTES} minutes, peak RSS under 1.5x of the first sample"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
