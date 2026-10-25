#!/bin/bash
# run-save-check.sh - the KAG bookmark save path, as one command.
#
# KAG saves a bookmark as "thumbnail image, then Dictionary.saveStruct at an
# offset".  That offset write used to fail with "Write error", which broke
# saving in every game.  This runs the same sequence headlessly.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KRKRRUN="${1:-$ROOT/build/krkrrun}"
TMP="$(mktemp -d /tmp/krkr-save.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$KRKRRUN" ] || { echo "FAIL: no krkrrun at $KRKRRUN"; exit 2; }

cd "$TMP"
SDL_VIDEODRIVER=dummy "$KRKRRUN" "$ROOT/tests/save-roundtrip.tjs" >"$TMP/save.log" 2>&1
status=$?
rc=0
strings "$TMP/save.log" | grep -aq "thumbnail: written" || { echo "  MISSING: thumbnail save"; rc=1; }
strings "$TMP/save.log" | grep -aq "saveStruct at offset: ok" || { echo "  MISSING: offset save"; rc=1; }
strings "$TMP/save.log" | grep -aq "SAVE-ROUNDTRIP-OK" || { echo "  MISSING: read-back"; rc=1; }
if strings "$TMP/save.log" | grep -aq "Write error"; then
	echo "  FAILED: 'Write error' came back"; rc=1
fi
[ $status -eq 0 ] || { echo "  FAILED: exit status $status"; rc=1; }

[ $rc -eq 0 ] && echo "PASS: bookmark save (thumbnail + offset struct + read-back)" || { echo "FAIL"; exit 1; }
