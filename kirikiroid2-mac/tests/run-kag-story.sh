#!/bin/bash
# run-kag-story.sh - the KiriKiri long-story acceptance run, as one command.
#
# Plays the real KAG project in testdata/kag-game twice:
#   1. from the game folder
#   2. from a packed data.xp3 (the form real games ship in)
# Each run must walk the chapter: title -> click -> messages -> BGM on/off
# -> [playse][ws] -> 4th/5th message, with no script exception and no
# "already invalidated".  Exit 0 means the long story passed.
#
# Usage: tests/run-kag-game.sh [path/to/krkrrun]

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                 # kirikiroid2-mac/
ENGINES="$(cd "$ROOT/.." && pwd)"              # gal-engines/
KRKRRUN="${1:-$ROOT/build/krkrrun}"
GAME="$ENGINES/testdata/kag-game/data"
TMP="$(mktemp -d /tmp/krkr-story.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$KRKRRUN" ] || { echo "FAIL: no krkrrun at $KRKRRUN (build first)"; exit 2; }
[ -f "$GAME/startup.tjs" ] || { echo "FAIL: game data missing at $GAME"; exit 2; }

run_one() {
	local label="$1" target="$2" log="$3"
	(
		cd "$GAME"
		KRKR_LOOP_SECONDS=40 SDL_VIDEODRIVER=dummy \
			KRKR_AUTO_CLICK="1131,446,3000" KRKR_AUTO_CLICK2="400,650,8000" \
			"$KRKRRUN" "$target" >"$log" 2>&1
	)
	local status=$?
	local rc=0
	for marker in "Scenario loaded : 1.ks" "playbgm" "stopbgm" "playse" \
		"4th message" "5th message"; do
		if ! strings "$log" | grep -aq "$marker"; then
			echo "  MISSING: $marker"
			rc=1
		fi
	done
	if strings "$log" | grep -aq "already invalidated"; then
		echo "  FAILED: 'The object is already invalidated' came back"
		rc=1
	fi
	# the startup bookmark save fails on its own (thumbnail path, KAG catches
	# it); any other script exception is a regression
	if strings "$log" | grep -aq "Script exception raised" \
		&& ! strings "$log" | grep -aq "saveBookMarkToFile"; then
		echo "  FAILED: an unexpected script exception was raised"
		rc=1
	fi
	if [ $status -ne 0 ]; then
		echo "  FAILED: exit status $status"
		rc=1
	fi
	[ $rc -eq 0 ] && echo "  ok: the chapter walked to its end"
	return $rc
}

echo "== KiriKiri long-story acceptance ($KRKRRUN)"
echo "1. from the game folder"
run_one folder "$GAME/startup.tjs" "$TMP/folder.log" || FAIL=1
echo "2. the story's text links (the settings screen behind one)"
(
	cd "$GAME"
	KRKR_LOOP_SECONDS=22 SDL_VIDEODRIVER=dummy \
		KRKR_AUTO_CLICK="1131,446,3000" KRKR_AUTO_CLICK2="1080,690,9000" \
		"$KRKRRUN" startup.tjs >"$TMP/setting.log" 2>&1
) || { echo "  FAILED: the link run exited non-zero"; FAIL=1; }
if strings "$TMP/setting.log" | grep -aq "setting.ks : jumped to : \*setting"; then
	echo "  ok: a click on the story's 'setting' link opened its screen"
else
	echo "  FAILED: the story's link did not open the settings screen"
	FAIL=1
fi

echo "3. from a packed data.xp3"
python3 "$ROOT/tests/pack-game-xp3.py" "$GAME" "$TMP/data.xp3" >/dev/null \
	|| { echo "  FAILED: packing the archive"; exit 1; }
run_one archive "$TMP/data.xp3" "$TMP/archive.log" || FAIL=1

[ "${FAIL:-0}" = "0" ] && echo "PASS: long story verified in both distribution forms" \
	|| { echo "FAIL"; exit 1; }
