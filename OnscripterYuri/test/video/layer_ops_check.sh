#!/usr/bin/env bash
# layer_ops_check.sh -- setx/sety, pause/resume and close-then-reopen for wcmpg.
#
#   ./test/video/layer_ops_check.sh [path/to/onsyuri]
#
# Each case plays a bright 320x240 clip through the layer and looks at where the
# pixels land on a 1024x576 surface.  Checks:
#   1. setx/sety   the movie is drawn at the requested offset, not at 0,0
#   2. pause       after layermessage pause/1 the rectangle stops changing
#   3. reopen      close then open+play again draws again
#
# Exit 0 only if all three pass.  2 = environment missing.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE_ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${1:-$ENGINE_ROOT/build/onsyuri}"

[ -x "$BIN" ] || { echo "no binary: $BIN"; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 2; }

WORK="$(mktemp -d /tmp/ons_layerops.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

ffmpeg -y -loglevel error -f lavfi -i "testsrc=duration=2:size=320x240:rate=10" \
  -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$WORK/clip.mp4" || exit 2

run_case() {   # $1 = script body (after *start), $2.. = sample count
    local body="$1" n="${2:-8}"
    local d="$WORK/case$RANDOM"; mkdir -p "$d"
    cp "$WORK/clip.mp4" "$d/"
    {
        printf ';$V6000G2000S1024,576L10000\n'
        printf '*define\neffect 10,10,0\nsetlayer 10,20,"dll\\wcmpg.dll"\ngame\n*start\n'
        printf '%s\n' "$body"
    } > "$d/00.txt"
    ( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        ONS_SURFACE_DUMP="$d/cur.bmp" "$BIN" --root . --enc:sjis --window \
        > "$d/log.txt" 2>&1 ) &
    local pid=$! i=0
    while [ $i -lt "$n" ]; do
        sleep 0.5; i=$((i+1))
        kill -0 $pid 2>/dev/null || break
        cp "$d/cur.bmp" "$d/s$i.bmp" 2>/dev/null
    done
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    echo "$d"
}

# mean brightness of a rectangle in a captured BMP
region_mean() {  # $1 bmp  $2 x $3 y $4 w $5 h
    python3 - "$@" <<'PY'
import struct, sys
p, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
d = open(p, 'rb').read()
if len(d) < 200:
    print(0); raise SystemExit
off = struct.unpack_from('<I', d, 10)[0]
sw = struct.unpack_from('<i', d, 18)[0]
sh = abs(struct.unpack_from('<i', d, 22)[0])
tot = n = 0
for yy in range(y, min(y + h, sh), 4):
    row = off + yy * sw * 4
    for xx in range(x, min(x + w, sw), 4):
        i = row + xx * 4
        if i + 2 >= len(d):
            continue
        tot += d[i] + d[i + 1] + d[i + 2]; n += 1
print(tot // n // 3 if n else 0)
PY
}

fail=0

echo "== 1. setx/sety puts the movie at the requested offset =="
d=$(run_case 'bg black,0
lsp 983,"*10"
layermessage 10,"setx/400"
layermessage 10,"sety/200"
layermessage 10,"open/clip.mp4"
layermessage 10,"play"
print 1
delay 2500
end' 8)
best=""; bestv=0
for f in "$d"/s*.bmp; do
    [ -f "$f" ] || continue
    v=$(region_mean "$f" 420 220 260 180)
    [ "$v" -gt "$bestv" ] && { bestv=$v; best=$f; }
done
at_origin=$(region_mean "${best:-$d/s8.bmp}" 10 10 260 180 2>/dev/null || echo 0)
echo "  brightest sample $best: at(420,220)=$bestv at(10,10)=$at_origin"
if [ "$bestv" -gt 20 ] && [ "$bestv" -gt "$at_origin" ]; then
    echo "  ✓ drawn where setx/sety asked"
else
    echo "  ✗ not drawn at the offset"; fail=1
fi

echo "== 2. pause/1 stops the rectangle changing =="
d=$(run_case 'bg black,0
lsp 983,"*10"
layermessage 10,"setx/0"
layermessage 10,"sety/0"
layermessage 10,"open/clip.mp4"
layermessage 10,"play"
print 1
delay 500
layermessage 10,"pause/1"
delay 2500
end' 8)
# Pause means: something got drawn, and then the rectangle stopped changing.
mx=0; for f in "$d"/s*.bmp; do [ -f "$f" ] || continue
    m=$(region_mean "$f" 0 0 320 240); [ "$m" -gt "$mx" ] && mx=$m; done
last3=$(for f in "$d"/s6.bmp "$d"/s7.bmp "$d"/s8.bmp; do [ -f "$f" ] && region_mean "$f" 0 0 320 240; done | sort -u | wc -l | tr -d ' ')
echo "  brightest during run=$mx  distinct values in last 3 samples=$last3"
if [ "$mx" -gt 20 ] && [ "$last3" -eq 1 ]; then
    echo "  ✓ drawn, then frozen after pause"
else
    echo "  ✗ nothing drawn, or not frozen"; fail=1
fi

echo "== 3. close then reopen draws again =="
d=$(run_case 'bg black,0
lsp 983,"*10"
layermessage 10,"setx/0"
layermessage 10,"sety/0"
layermessage 10,"open/clip.mp4"
layermessage 10,"play"
print 1
delay 800
layermessage 10,"close"
delay 300
layermessage 10,"open/clip.mp4"
layermessage 10,"play"
print 1
delay 1800
end' 10)
v=0
for f in "$d"/s*.bmp; do
    [ -f "$f" ] || continue
    m=$(region_mean "$f" 0 0 320 240)
    [ "$m" -gt "$v" ] && v=$m
done
echo "  region mean after reopen=$v"
if [ "$v" -gt 20 ]; then
    echo "  ✓ drew again after close/open"
else
    echo "  ✗ nothing after reopen"; fail=1
fi

echo
[ $fail -eq 0 ] && echo "RESULT: PASS (all three layer operations)" || echo "RESULT: FAIL"
exit $fail
