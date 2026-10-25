#!/bin/bash
# run-demo-game.sh - the second real KiriKiri game, as one command.
#
# testdata/krkrsdl2-demo-game is the official KiriKiri SDL2 / KAG3 demo
# (github.com/krkrsdl2/krkrsdl2-kag3-demo + kag3 system files; license same
# as KAG3, 改変・配布自由).  It is a different game from the ACG test
# project: 640x480, JPEG backgrounds, TLG message frame, crossfade screen
# transitions, a TJS snow plugin driven by a Timer, and a hyperlink menu.
#
# What this checks:
#   - the menu scene loads (first.ks) and the snow plugin loads (snow.ks)
#   - frames are produced and differ from an empty screen
#   - the run is clean: no script exception, no invalidated object
#   - the chapter body is reachable through the click path
#
# Usage: tests/run-demo-game.sh [path/to/krkrrun]

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ENGINES="$(cd "$ROOT/.." && pwd)"
KRKRRUN="${1:-$ROOT/build/krkrrun}"
GAME="$ENGINES/testdata/krkrsdl2-demo-game"
TMP="$(mktemp -d /tmp/krkr-demo.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$KRKRRUN" ] || { echo "FAIL: no krkrrun at $KRKRRUN"; exit 2; }
[ -f "$GAME/startup.tjs" ] || { echo "FAIL: demo game missing at $GAME"; exit 2; }
[ -f "$GAME/scenario/snow.ks" ] || { echo "FAIL: demo scenario missing"; exit 2; }

fail=0
note() { echo "  $*"; }

echo "== KiriKiri second real game (krkrsdl2 KAG3 demo)"

# 1. boot: menu scene + snow plugin, one frame out
(
	cd "$GAME"
	KRKR_LOOP_SECONDS=10 SDL_VIDEODRIVER=dummy \
		"$KRKRRUN" startup.tjs --screenshot "$TMP/menu.bmp" >"$TMP/boot.log" 2>&1
) || { note "FAILED: boot run exited non-zero"; fail=1; }

strings "$TMP/boot.log" | grep -aq "Scenario loaded : first.ks" \
	|| { note "MISSING: first.ks (the menu scene) never loaded"; fail=1; }
strings "$TMP/boot.log" | grep -aq "Scenario loaded : snow.ks" \
	|| { note "MISSING: snow.ks (the TJS plugin scenario) never loaded"; fail=1; }
strings "$TMP/boot.log" | grep -aq "wrote .*bmp" \
	|| { note "MISSING: no frame was written"; fail=1; }
if strings "$TMP/boot.log" | grep -aq "Script exception raised\|already invalidated"; then
	note "FAILED: the boot run raised a script exception"
	strings "$TMP/boot.log" | grep -a "Script exception raised" | head -2
	fail=1
fi

# the frame must not be an empty screen: count its distinct colours
if [ -f "$TMP/menu.bmp" ]; then
	colours=$(python3 - "$TMP/menu.bmp" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', data, 10)[0]
w, h = struct.unpack_from('<ii', data, 18)
bpp = struct.unpack_from('<H', data, 28)[0]
px = (bpp + 7) // 8
seen = set()
for y in range(0, h, 4):
    row = off + y * ((w * px + 3) & ~3)
    for x in range(0, w, 4):
        i = row + x * px
        seen.add(data[i:i + 3])
print(len(seen))
PY
)
	note "menu frame: $(basename "$TMP/menu.bmp"), $colours distinct colours (sampled)"
	[ "${colours:-0}" -ge 8 ] || { note "FAILED: the frame looks empty"; fail=1; }
else
	note "FAILED: no menu frame to inspect"
	fail=1
fi

# 2. click path: the demo says the chapter body is reached through a click
(
	cd "$GAME"
	KRKR_LOOP_SECONDS=20 SDL_VIDEODRIVER=dummy \
		KRKR_DEBUG_INPUT=1 \
		KRKR_AUTO_CLICK="320,160,8000" KRKR_AUTO_CLICK2="320,300,12000" \
		"$KRKRRUN" startup.tjs --screenshot "$TMP/walk.bmp" >"$TMP/walk.log" 2>&1
) || { note "FAILED: click run exited non-zero"; fail=1; }
if strings "$TMP/walk.log" | grep -aq "Script exception raised\|already invalidated"; then
	note "FAILED: the click run raised a script exception"
	fail=1
fi
clicks=$(strings "$TMP/walk.log" | grep -ac "\[input\] click" || true)
note "clicks delivered: ${clicks:-0}"

# scrolling stops must still be reached and left by the scenario itself
strings "$TMP/walk.log" | grep -aq "first.ks : @s" \
	|| { note "MISSING: the menu's @s stop was never reached"; fail=1; }

# 3. the hyperlink menu: a click on the text must select the link and run the
#    section it points at (this is what province hit areas are for)
(
	cd "$GAME"
	KRKR_LOOP_SECONDS=25 SDL_VIDEODRIVER=dummy KRKR_DEBUG_INPUT=1 \
		KRKR_AUTO_CLICK="320,145,9000" \
		"$KRKRRUN" startup.tjs --screenshot "$TMP/section.bmp" >"$TMP/link.log" 2>&1
) || { note "FAILED: link run exited non-zero"; fail=1; }

if strings "$TMP/link.log" | grep -aq "jumped to : \*about_kirikiri"; then
	note "link: the menu click select the first entry and entered its section"
else
	note "FAILED: a click on the menu text did not enter the linked section"
	fail=1
fi
strings "$TMP/link.log" | grep -aq "changebg_and_clear storage=" \
	|| { note "MISSING: the section did not run its background change"; fail=1; }
if strings "$TMP/link.log" | grep -aq "Script exception raised\|already invalidated"; then
	note "FAILED: the link run raised a script exception"
	fail=1
fi

# 4. the released form: a folder holding data.xp3, which is how a game ships
mkdir -p "$TMP/release"
python3 "$ROOT/tests/pack-game-xp3.py" "$GAME" "$TMP/release/data.xp3" >/dev/null 2>&1
(
	cd "$TMP"
	KRKR_LOOP_SECONDS=8 SDL_VIDEODRIVER=dummy \
		"$KRKRRUN" "$TMP/release" --screenshot "$TMP/release.bmp" >"$TMP/release.log" 2>&1
) || { note "FAILED: the released folder form exited non-zero"; fail=1; }
strings "$TMP/release.log" | grep -aq "Scenario loaded : first.ks" \
	|| { note "MISSING: data.xp3 in a folder did not boot"; fail=1; }
strings "$TMP/release.log" | grep -aq "wrote .*bmp" \
	|| { note "MISSING: the released form produced no frame"; fail=1; }
note "released form: data.xp3 mounted from the game folder"

# 5. patch layering: a patch.xp3 must win over the base archive, which is how
#    a released game ships a fix
mkdir -p "$TMP/relpatch/base" "$TMP/relpatch/patchsrc/scenario"
python3 "$ROOT/tests/pack-game-xp3.py" "$GAME" "$TMP/relpatch/base/data.xp3" >/dev/null 2>&1 \
	|| { note "FAILED: could not pack the base archive"; fail=1; }
cp "$GAME/scenario/first.ks" "$TMP/relpatch/patchsrc/scenario/first.ks" \
	|| { note "FAILED: could not stage the patched scenario"; fail=1; }
python3 - "$TMP/relpatch/patchsrc/scenario/first.ks" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding='utf-8', errors='surrogateescape').read()
t2 = t.replace('\u5409\u91cc\u5409\u91cc\u3068KAG\u306e\u7d39\u4ecb', 'PATCHED-BY-TEST', 1)
if t2 == t:
    sys.stderr.write('patch marker not found in scenario\n')
    sys.exit(1)
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(t2)
PY
[ $? -eq 0 ] || { note "FAILED: could not patch the scenario text"; fail=1; }
python3 "$ROOT/tests/pack-game-xp3.py" "$TMP/relpatch/patchsrc" "$TMP/relpatch/base/patch.xp3" >/dev/null 2>&1 \
	|| { note "FAILED: could not pack the patch archive"; fail=1; }
(
	cd "$TMP"
	KRKR_LOOP_SECONDS=8 SDL_VIDEODRIVER=dummy \
		"$KRKRRUN" "$TMP/relpatch/base" --screenshot "$TMP/patched.bmp" >"$TMP/patched.log" 2>&1
) ; patch_status=$?
if strings "$TMP/patched.log" 2>/dev/null | grep -aq "PATCHED-BY-TEST"; then
	note "patch layering: patch.xp3 overrides data.xp3"
else
	note "FAILED: patch.xp3 did not override the base archive (exit $patch_status)"
	fail=1
fi

[ "$fail" = "0" ] && echo "PASS: the second real game boots, renders and survives the click path" \
	|| { echo "FAIL"; exit 1; }
