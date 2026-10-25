#!/bin/bash
# run-audio-check.sh - the audio contract, headless.
#
# WaveSoundBuffer is what every KAG game uses for BGM and sound effects; the
# checks here are the ones that broke before: a real pause, a status that
# reflects the mixer, [ws] waiting for a sound to end, a sound effect and a BGM
# at the same time, per-buffer volume, and a fade that completes.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KRKRRUN="${1:-$ROOT/build/krkrrun}"
GAME="$(cd "$ROOT/.." && pwd)/testdata/kag-game/data"
TMP="$(mktemp -d /tmp/krkr-audio.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$KRKRRUN" ] || { echo "FAIL: no krkrrun at $KRKRRUN"; exit 2; }
[ -f "$GAME/sound/boom.wav" ] || { echo "FAIL: test sounds missing"; exit 2; }

cp "$GAME/sound/boom.wav" "$GAME/bgm/song.ogg" "$TMP/" 2>/dev/null
cd "$TMP"
SDL_VIDEODRIVER=dummy KRKR_LOOP_SECONDS=2 "$KRKRRUN" "$HERE/audio-probe.tjs" \
	>"$TMP/run.log" 2>&1
status=$?

fail=0
note() { echo "  $*"; }
want() {
	if strings "$TMP/run.log" | grep -aq "$1"; then
		note "ok: $1"
	else
		note "MISSING: $1"
		fail=1
	fi
}

want "CHECK after-play status=1"
want "CHECK after-pause status=2"
want "CHECK after-resume status=1"
want "CHECK after-stop status=0"
want "CHECK simultaneous bgm=1 se=1"
want "CHECK volumes se=-1200 bgm=-600"
want "CHECK after-close status=0"
want "CHECK reopened status=1"
want "CHECK invalidate-survived"
want "CHECK fade-started"
want "CHECK fade-completed"
want "AUDIO-PROBE-DONE"

# position must have advanced past zero at some point
if strings "$TMP/run.log" | grep -a "CHECK position=" | grep -qv "position=+*0.0*$"; then
	note "ok: position advanced"
else
	note "FAILED: position never advanced"
	fail=1
fi

[ $status -eq 0 ] || { note "FAILED: the run exited $status"; fail=1; }
[ "$fail" = "0" ] && echo "PASS: the audio contract holds" || { echo "FAIL"; exit 1; }
