#!/bin/bash
# run-perf-check.sh - the frame loop keeps up.
#
# The tools pace themselves against a 60 Hz clock; a flat sleep on top of the
# work used to cost ~15% of the frames.  This measures both real projects and
# fails if either drops below a floor, so a slow path cannot creep back in.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KRKRRUN="${1:-$ROOT/build/krkrrun}"
ENGINES="$(cd "$ROOT/.." && pwd)"
FLOOR=40
fail=0

[ -x "$KRKRRUN" ] || { echo "FAIL: no krkrrun at $KRKRRUN"; exit 2; }

measure() {
	local label="$1" dir="$2" seconds="$3" extra="$4"
	[ -d "$dir" ] || { echo "  (no $label data, skipped)"; return 0; }
	local log
	log="$(mktemp /tmp/krkr-perf.XXXXXX)"
	(
		cd "$dir"
		rm -f ./*.ksd 2>/dev/null
		# env, because an unquoted variable would not be read as an assignment
		env SDL_VIDEODRIVER=dummy KRKR_STATS=1 KRKR_LOOP_SECONDS="$seconds" $extra \
			"$KRKRRUN" startup.tjs >"$log" 2>&1
	)
	local fps
	fps=$(sed -nE 's/.*\[stats\] frames=[0-9]+ seconds=[0-9.]+ fps=([0-9.]+).*/\1/p' "$log" | tail -1)
	if [ -z "$fps" ]; then
		echo "  FAILED: $label produced no statistics"
		fail=1
	elif [ "${fps%.*}" -lt "$FLOOR" ]; then
		echo "  FAILED: $label ran at ${fps} fps (floor ${FLOOR})"
		fail=1
	else
		echo "  ok: $label at ${fps} fps"
	fi
	rm -f "$log"
}

echo "== frame pacing"
measure "the ACG story (1280x720)" "$ENGINES/testdata/kag-game/data" 12 \
	'KRKR_AUTO_CLICK=1131,446,3000'
measure "the official demo (640x480)" "$ENGINES/testdata/krkrsdl2-demo-game" 8 ''

[ "$fail" = "0" ] && echo "PASS: both projects keep a playable frame rate" || { echo "FAIL"; exit 1; }
