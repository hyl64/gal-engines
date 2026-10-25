#!/bin/sh
# smoke-scenarios.sh: start every scenario of a game, briefly, and report the
# ones that fail to come up.
#
# A compatibility layer should not fall over on scene 187. Parsing every
# scenario proves the reader agrees with the file; this proves the machine
# survives being pointed at each one, which is a different question and has
# caught a crash of its own.
#
#   usage: smoke-scenarios.sh <rlvm-binary> <game-directory> [settle-seconds]
#
# The scenario numbers come from the archive's own table of contents, read
# with seen-dump. Requires a build of tools/seen-dump next to the binary.

set -eu

BIN="${1:?usage: smoke-scenarios.sh <rlvm-binary> <game-dir> [settle-seconds]}"
GAME="${2:?usage: smoke-scenarios.sh <rlvm-binary> <game-dir> [settle-seconds]}"
SETTLE="${3:-1.6}"
SEEN_DUMP="$(dirname "$BIN")/seen-dump"
SEEN="$GAME/Seen.txt"

if [ ! -x "$BIN" ]; then
  echo "smoke: $BIN is not executable" >&2
  exit 2
fi
if [ ! -x "$SEEN_DUMP" ]; then
  echo "smoke: need seen-dump beside the binary ($SEEN_DUMP)" >&2
  exit 2
fi
if [ ! -f "$SEEN" ]; then
  SEEN="$GAME/SEEN.TXT"
fi

REGNAME="$(awk 'tolower($0) ~ /^#regname/ {gsub(/.*= *"?/,""); gsub(/".*/,""); print}' \
  "$GAME/Gameexe.ini" 2>/dev/null || true)"
[ -n "$REGNAME" ] || REGNAME="$(awk 'tolower($0) ~ /^#regname/ {gsub(/.*= *"?/,""); gsub(/".*/,""); print}' "$GAME/GAMEEXE.INI" 2>/dev/null || true)"

scenes="$("$SEEN_DUMP" --all ${REGNAME:+--regname "$REGNAME"} "$SEEN" 2>/dev/null |
  sed -n 's/^s \([0-9][0-9]*\) .*/\1/p')"

if [ -z "$scenes" ]; then
  echo "smoke: could not list scenarios in $SEEN" >&2
  exit 2
fi

total=0
started=0
for scene in $scenes; do
  total=$((total + 1))
  log="/tmp/rlvm-smoke-$scene.log"
  RLVM_SEEN_START="$scene" "$BIN" "$GAME" > "$log" 2>&1 &
  pid=$!
  sleep "$SETTLE"
  if kill -0 "$pid" 2>/dev/null; then
    started=$((started + 1))
    kill "$pid" 2>/dev/null || true
  else
    echo "FAILED scene $scene: $(grep -iE 'fatal|error' "$log" | head -1)"
  fi
  wait "$pid" 2>/dev/null || true
done

echo "smoke: $started of $total scenarios came up"
[ "$started" -eq "$total" ] || exit 1
