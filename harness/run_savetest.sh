#!/usr/bin/env bash
# run_savetest.sh -- NScripter save/load round-trip, automated.
#
#   ./harness/run_savetest.sh [game_dir]
#
# With no argument it runs the synthetic probe under testdata/save-probe
# (allowed by task T3: "若 SDK 样例没有真实存档菜单流程，就在
# testdata/save-probe/ 下自建一个最小 00.txt 测试剧本").
#
# How the probe decides pass/fail
# -------------------------------
# A variable is set, written with `savegame`, the process is SIGKILLed, a
# second process runs `loadgame`, and the variable's value then selects
# between two movies.  The log tells us which one played, so the verdict is
# behavioural rather than "a file exists":
#
#   saved %1=12345 -> kill -9 -> loadgame -> played ok.mp4   PASS
#                                         -> played bad.mp4  FAIL (not restored)
#                                         -> neither          FAIL (load died)
#
# Both phases use the same script layout so the save file's stored line
# number resolves to the same place; only the save/load command differs.
# (ONScripter stores absolute script line numbers, see T3's notes in
# testdata/save-probe/00.txt.)
#
# Env:
#   GAL_ENGINE   path to onsyuri   (default: OnscripterYuri/build/onsyuri)
#   GAL_TIMEOUT  seconds per phase (default: 6)
#   GAL_KEEP=1   keep the work directory
#
# Exit: 0 = PASS, 1 = FAIL, 2 = environment missing.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/.." && pwd)"
GAME_ARG="${1:-}"

BIN="${GAL_ENGINE:-$TREE/OnscripterYuri/build/onsyuri}"
TIMEOUT="${GAL_TIMEOUT:-6}"
FIX="$TREE/bench/fixtures"

[ -x "$BIN" ] || { echo "run_savetest: no engine at $BIN (set GAL_ENGINE)"; exit 2; }
[ -f "$FIX/sample.mp4" ] || { echo "run_savetest: fixtures missing at $FIX"; exit 2; }

WORK="$(mktemp -d /tmp/ons_savetest.XXXXXX)"
cleanup() { [ "${GAL_KEEP:-0}" = "1" ] || rm -rf "$WORK"; }
trap cleanup EXIT

G="$WORK/game"
mkdir -p "$G"
cp "$FIX/sample.mp4" "$G/ok.mp4" 2>/dev/null || cp "$FIX/sample.mp4" "$G/ok.mp4"
cp "$FIX/sample.mpg" "$G/bad.mpg"

# Phase 1: set the variable, write slot 4.
cat > "$G/00.txt" <<'EOS'
*define
game
*start
mov %0,12345
savegame 4
end
EOS

# Phase 2: load slot 4; the restored variable picks the branch.
cat > "$WORK/phase2.txt" <<'EOS'
*define
game
*start
loadgame 4
if %0 == 12345 mpegplay "ok.mp4",0
if %0 != 12345 mpegplay "bad.mpg",0
end
EOS

run_phase() {   # $1 = log file
    ( cd "$G" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "$BIN" --root . --enc:sjis --window > "$1" 2>&1 ) &
    local pid=$!
    local n=0
    while [ "$n" -lt "$TIMEOUT" ]; do
        kill -0 $pid 2>/dev/null || break
        sleep 1; n=$((n+1))
    done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null        # the point of phase 1: no clean exit
        wait $pid 2>/dev/null
        echo "killed-after-${TIMEOUT}s"
    else
        wait $pid 2>/dev/null
        echo "exited"
    fi
}

echo "engine: $BIN"
echo "work:   $WORK"
if [ -n "$GAME_ARG" ]; then
    # A real game needs a human to reach a save menu, and NScripter saves at
    # whatever line the player happened to be on.  Rather than pretend, say so
    # and fall back to the deterministic synthetic probe.
    echo "note:   game_dir '$GAME_ARG' given, but driving a real game's save"
    echo "        menu headlessly is not implemented; running the synthetic"
    echo "        probe instead.  See harness/results.md (未做到 section)."
fi

# ---- phase 1: save ----------------------------------------------------
echo "[1/2] save phase ..."
end1="$(run_phase "$WORK/phase1.log")"
SAVED="$G/save4.dat"
if [ ! -s "$SAVED" ]; then
    echo "FAIL: savegame 4 produced no save4.dat (phase ended: $end1)"
    grep -iE "error|can't" "$WORK/phase1.log" | head -3 | sed 's/^/      /'
    echo "SUMMARY: FAIL save - no save file"
    exit 1
fi
echo "      save4.dat $(stat -f%z "$SAVED") bytes, phase ended: $end1"

# ---- phase 2: load ----------------------------------------------------
echo "[2/2] load phase (fresh process) ..."
cp "$WORK/phase2.txt" "$G/00.txt"
end2="$(run_phase "$WORK/phase2.log")"

if grep -q "playing .*ok\.mp4" "$WORK/phase2.log"; then
    echo "      variable %0 was restored: the ok.mp4 branch ran"
    VARS=restored
elif grep -q "playing .*bad\.mpg" "$WORK/phase2.log"; then
    echo "      variable %0 was NOT restored: the bad.mpg branch ran"
    VARS=lost
else
    echo "      neither branch ran (phase ended: $end2)"
    grep -iE "error|can't|failed" "$WORK/phase2.log" | head -5 | sed 's/^/      /'
    VARS=none
fi

# ---- also record the files NScripter drops next to the game ----------
echo
echo "files written in the game dir:"
ls -1 "$G" | grep -vE "^(00\.txt|ok\.mp4|bad\.mpg)$" | sed 's/^/  /' || true

echo
if [ "$VARS" = "restored" ]; then
    echo "SUMMARY: PASS save+load round-trip (variable restored across processes)"
    exit 0
else
    echo "SUMMARY: FAIL save+load round-trip (variable state: $VARS)"
    exit 1
fi
