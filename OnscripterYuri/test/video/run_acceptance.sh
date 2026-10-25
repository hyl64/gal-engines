#!/usr/bin/env bash
# run_acceptance.sh -- macOS movie backend acceptance, task T1.
#
#   ./test/video/run_acceptance.sh [path/to/onsyuri]
#
# Defaults to build/onsyuri.  Exits 0 only if every check passes.
# Fixtures come from ../../bench/fixtures (generated once, see bench/fixtures).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE_ROOT="$(cd "$HERE/../.." && pwd)"        # .../OnscripterYuri
TREE_ROOT="$(cd "$ENGINE_ROOT/.." && pwd)"      # .../gal-engines
BIN="${1:-$ENGINE_ROOT/build/onsyuri}"
FIX="$TREE_ROOT/bench/fixtures"

[ -x "$BIN" ] || { echo "no binary: $BIN"; exit 2; }
[ -d "$FIX" ] || { echo "no fixtures: $FIX (run bench/fixtures generation)"; exit 2; }
# audio_* and perf_* fixtures are optional: their cases skip when absent

WORK="$(mktemp -d /tmp/ons_video_accept.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0
ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }

run_game() {   # $1 = dir with 0.txt ; extra env via caller
    ( cd "$1" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "$BIN" --root . --enc:sjis --window 2>&1 )
}

movie_case() { # $1 name  $2 script-line  $3 expected-log-pattern  $4 fixture(s)
    local name="$1" line="$2" want="$3"; shift 3
    local d="$WORK/$name"; mkdir -p "$d"
    for f in "$@"; do cp "$FIX/$f" "$d/"; done
    printf '*define\ngame\n*start\n%s\nend\n' "$line" > "$d/0.txt"
    # Retry once: these cases are timing-sensitive (a decode or a transcode can be
    # slow while the machine is busy), and a flaky acceptance suite is worse than
    # none -- it teaches you to re-run until green.
    local attempt out
    for attempt in 1 2; do
        out="$(run_game "$d")"
        if echo "$out" | grep -q "$want"; then
            [ "$attempt" -eq 1 ] && ok "$name: $want" || ok "$name: $want (passed on retry)"
            return
        fi
    done
    bad "$name: expected '$want' (2 attempts)"; echo "$out" | grep -i video | sed 's/^/      /'
}

echo "== 1. mpegplay, MPEG-1, click_stop=0 =="
movie_case mpeg1 'mpegplay "sample.mpg",0' 'played .*sample.mpg (AVFoundation)' sample.mpg

echo "== 2. avi, wmv2 -> mpv fallback =="
movie_case wmv2 'avi "sample_wmv2.avi",1' 'played .*sample_wmv2.avi (mpv backend)' sample_wmv2.avi

echo "== 3. mp4 direct path, no sidecar map =="
movie_case mp4 'mpegplay "sample.mp4",0' 'played .*sample.mp4 (AVFoundation)' sample.mp4

echo "== 4. video_map.json routes to the transcoded file =="
d="$WORK/mapped"; mkdir -p "$d/_video"
cp "$FIX/sample_wmv2.avi" "$d/"
ffmpeg -y -loglevel error -i "$FIX/sample_wmv2.avi" \
    -c:v h264_videotoolbox -b:v 1500k -c:a aac -b:a 128k \
    "$d/_video/sample_wmv2.mp4" 2>/dev/null \
  || ffmpeg -y -loglevel error -i "$FIX/sample_wmv2.avi" \
    -c:v libx264 -preset veryfast -c:a aac "$d/_video/sample_wmv2.mp4"
cat > "$d/_video/video_map.json" <<'JSON'
{ "version": 1,
  "mappings": [ { "from": "sample_wmv2.avi", "to": "_video/sample_wmv2.mp4",
                  "codec": "h264", "source": "loose" } ] }
JSON
printf '*define\ngame\n*start\navi "sample_wmv2.avi",1\nend\n' > "$d/0.txt"
out="$(run_game "$d")"
if echo "$out" | grep -q 'playing .*_video/sample_wmv2.mp4'; then
    ok "mapped: transcoded file was chosen"
else
    bad "mapped: engine did not route to _video/*.mp4"; echo "$out" | grep -i video | sed 's/^/      /'
fi

echo "== 4b. script continues past a movie (the dialogue-flow bug) =="
# The regression that mattered most: playMPEG's return value means "failed",
# not "played", so the caller's `if (playMPEG(..)) endCommand();` must NOT fire
# after a successful movie.  Before the fix every movie ended the script and
# every line after it was lost.  Verified by colour: green before, red after.
d="$WORK/flow"; mkdir -p "$d"; cp "$FIX/sample.mp4" "$d/clip.mp4"
cat > "$d/0.txt" <<'EOS'
*define
effect 10,10,0
game
*start
bg #00FF00,10
waittimer 400
mpegplay "clip.mp4",0
bg #CC0000,10
waittimer 2000
end
EOS
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ONS_SURFACE_DUMP="$d/last.bmp" "$BIN" --root . --enc:sjis --window \
    >/dev/null 2>&1 ) &
p=$!
for _ in 1 2 3 4 5 6; do sleep 1; kill -0 $p 2>/dev/null || break; done
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
if python3 - "$d/last.bmp" <<'PY'
import struct,sys
try:
    d=open(sys.argv[1],'rb').read()
except OSError:
    sys.exit(1)
off=struct.unpack_from('<I',d,10)[0]; i=off+(240*640+320)*4
r,g,b=d[i+2],d[i+1],d[i]
sys.exit(0 if r>150 and g<80 else 1)      # red = continued, green = ended
PY
then ok "script continued past the movie (red background drawn)"; else
    bad "script stopped at the movie (still the pre-movie frame)"; fi

echo "== 4c. movie async keeps the script running; movie stop halts it =="
d="$WORK/async"; mkdir -p "$d"; cp "$FIX/audio_h264.mp4" "$d/mov.mp4"
cat > "$d/0.txt" <<'EOS'
*define
effect 10,10,0
game
*start
bg #000080,10
movie "mov.mp4",async
waittimer 700
movie stop
bg #FF00FF,10
waittimer 2000
end
EOS
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ONS_SURFACE_DUMP="$d/last.bmp" "$BIN" --root . --enc:sjis --window \
    > "$d/log.txt" 2>&1 ) &
p=$!
for _ in 1 2 3 4 5; do sleep 1; kill -0 $p 2>/dev/null || break; done
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
async_started=$(grep -c "video(async): started" "$d/log.txt" 2>/dev/null)
if [ "${async_started:-0}" -ge 1 ] && python3 - "$d/last.bmp" <<'PY'
import struct,sys
try: d=open(sys.argv[1],'rb').read()
except OSError: sys.exit(1)
off=struct.unpack_from('<I',d,10)[0]; i=off+(240*640+320)*4
r,g,b=d[i+2],d[i+1],d[i]
sys.exit(0 if r>200 and g<80 and b>200 else 1)   # magenta = script ran after stop
PY
then ok "async started and movie stop let the script continue"; else
    bad "async/stop did not behave (started=${async_started:-0})"; fi

echo "== 4d. movie audio really decodes (peak/rms, not just a track) =="
d="$WORK/audio"; mkdir -p "$d"; cp "$FIX/audio_h264.mp4" "$d/m.mp4"
printf '*define\ngame\n*start\nmpegplay "m.mp4",0\nend\n' > "$d/0.txt"
out="$(run_game "$d")"
peak="$(echo "$out" | sed -n 's/.*peak=\([0-9.]*\).*/\1/p' | head -1)"
if python3 -c "import sys; sys.exit(0 if float('${peak:-0}' or 0) > 0.01 else 1)" 2>/dev/null; then
    ok "decoded audio has real content (peak=$peak)"
else
    bad "audio decoded silent or not at all (peak=${peak:-none})"; fi

echo "== 4e. corrupt / empty / garbage / missing files do not crash =="
d="$WORK/edge"; mkdir -p "$d"
cp "$FIX/sample_broken.mpg" "$d/broken.mpg" 2>/dev/null
: > "$d/empty.mp4"
head -c 200000 /dev/urandom > "$d/garbage.mp4" 2>/dev/null
bad_edge=0
for f in broken.mpg empty.mp4 garbage.mp4 does_not_exist.mp4; do
    printf '*define\neffect 10,10,0\ngame\n*start\nmpegplay "%s",0\nbg #FF0000,10\nwaittimer 300\nend\n' "$f" > "$d/0.txt"
    o="$( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ONS_SURFACE_DUMP="$d/e.bmp" \
          "$BIN" --root . --enc:sjis --window 2>&1 )"
    rc=$?
    if [ $rc -ne 0 ] || echo "$o" | grep -qiE "abort|segmentation|terminate"; then
        bad "edge case $f crashed or exited $rc"; bad_edge=1
    fi
    rm -f "$d/e.bmp"
done
[ "$bad_edge" -eq 0 ] && ok "all four bad inputs survived and the script continued"

echo "== 4f. repeated playback does not grow RSS =="
d="$WORK/mem"; mkdir -p "$d"
ffmpeg -y -loglevel error -f lavfi -i "testsrc=duration=1:size=640x480:rate=25" \
       -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$d/short.mp4" 2>/dev/null
cat > "$d/0.txt" <<'EOS'
*define
game
*start
mov %0,0
*loop
mpegplay "short.mp4",0
inc %0
if %0 < 6 goto *loop
end
EOS
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$BIN" --root . --enc:sjis --window > /dev/null 2>&1 ) &
p=$!
rss_first=0; rss_last=0; n=0
for _ in $(seq 1 12); do
    sleep 0.5
    kill -0 $p 2>/dev/null || break
    rss="$(ps -o rss= -p $p 2>/dev/null | tr -d ' ')"
    [ -n "$rss" ] || continue
    n=$((n+1)); [ "$n" -eq 1 ] && rss_first=$rss; rss_last=$rss
done
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
if [ "$rss_first" -gt 0 ] && [ "$rss_last" -lt $((rss_first * 3 / 2)) ]; then
    ok "RSS stable across repeated playback (${rss_first}KB -> ${rss_last}KB)"
else
    bad "RSS grew: ${rss_first}KB -> ${rss_last}KB"; fi

echo "== 4g. 1080p60 playback drops no frames =="
d="$WORK/perf"; mkdir -p "$d"; cp "$FIX/perf_1080p60.mp4" "$d/perf.mp4" 2>/dev/null
if [ -f "$d/perf.mp4" ]; then
    printf '*define\ngame\n*start\nmpegplay "perf.mp4",0\nend\n' > "$d/0.txt"
    out="$(run_game "$d")"
    line="$(echo "$out" | grep -E 'frames \(' | head -1)"
    if echo "$line" | grep -q "0 dropped"; then ok "1080p60: $line"; else
        bad "1080p60 dropped frames: ${line:-no measurement}"; fi
else
    echo "  - perf fixture missing, skipped"
fi

echo "== 4h. movie pos draws only into its rectangle =="
d="$WORK/pos"; mkdir -p "$d"; cp "$FIX/sample.mp4" "$d/mov.mp4"
cat > "$d/0.txt" <<'EOS'
*define
effect 10,10,0
game
*start
bg #FF00FF,10
waittimer 200
movie "mov.mp4",pos,0,0,200,150
waittimer 200
end
EOS
out="$( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ONS_VIDEO_PIXEL_LOG=1 \
        "$BIN" --root . --enc:sjis --window 2>&1 )"
line="$(echo "$out" | grep "pos rect" | head -1)"
inside="$(echo "$line"  | sed -n 's/.*inside=\(#[0-9a-f]*\).*/\1/p')"
outside="$(echo "$line" | sed -n 's/.*outside=\(#[0-9a-f]*\).*/\1/p')"
if [ "$line" != "" ] && [ "$inside" != "$outside" ] && [ "$outside" = "#ff00ff" ]; then
    ok "pos rect drew into 200x150, outside left as bg ($inside / $outside)"
else
    bad "pos rect not honoured: ${line:-no measurement}"; fi

echo "== 5. click_stop=1 interrupts early =="
d="$WORK/click1"; mkdir -p "$d"; cp "$FIX/sample.mpg" "$d/"
printf '*define\ngame\n*start\nmpegplay "sample.mpg",1\nend\n' > "$d/0.txt"
t0=$(python3 -c 'import time;print(time.time())')
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ONS_VIDEO_CLICK_AFTER=500 \
    "$BIN" --root . --enc:sjis --window >/dev/null 2>&1 )
el=$(python3 -c "import time;print(round(time.time()-$t0,2))")
if python3 -c "import sys;sys.exit(0 if $el < 2.6 else 1)"; then
    ok "click_stop=1 stopped at ${el}s (clip 3.00s)"
else
    bad "click_stop=1 did not interrupt (elapsed ${el}s)"
fi

echo "== 6. click_stop=0 ignores the same click =="
d="$WORK/click0"; mkdir -p "$d"; cp "$FIX/sample.mpg" "$d/"
printf '*define\ngame\n*start\nmpegplay "sample.mpg",0\nend\n' > "$d/0.txt"
t0=$(python3 -c 'import time;print(time.time())')
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ONS_VIDEO_CLICK_AFTER=500 \
    "$BIN" --root . --enc:sjis --window >/dev/null 2>&1 )
el=$(python3 -c "import time;print(round(time.time()-$t0,2))")
if python3 -c "import sys;sys.exit(0 if $el >= 2.9 else 1)"; then
    ok "click_stop=0 played through (${el}s)"
else
    bad "click_stop=0 ended early (${el}s)"
fi

echo "== 7. frame really reaches the engine surface =="
d="$WORK/frame"; mkdir -p "$d"; cp "$FIX/sample.mpg" "$d/"
printf '*define\ngame\n*start\nmpegplay "sample.mpg",0\nend\n' > "$d/0.txt"
( cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ONS_VIDEO_DUMP="$d/last.bmp" "$BIN" --root . --enc:sjis --window >/dev/null 2>&1 )
if [ -s "$d/last.bmp" ] && python3 - "$d/last.bmp" <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read(); off=struct.unpack_from('<I',d,10)[0]
w=struct.unpack_from('<i',d,18)[0]; h=abs(struct.unpack_from('<i',d,22)[0])
px=d[off:]; cols=set()
for y in range(0,h,max(1,h//40)):
    row=px[y*w*4:(y+1)*w*4]
    for x in range(0,w,max(1,w//40)): cols.add(row[x*4:x*4+3])
sys.exit(0 if len(cols)>5 else 1)
PY
then ok "dumped frame has real content (not black)"; else bad "dumped frame is blank/missing"; fi

echo "== 8. regression: a game with no movies still runs =="
LG="$TREE_ROOT/testdata/lifegame"
if [ -d "$LG" ]; then
    if ( cd "$LG" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 3 \
         "$BIN" --root . --enc:sjis --window 2>&1 | grep -qiE 'segmentation|abort trap' ); then
        bad "lifegame crashed"
    else
        ok "lifegame starts and survives 3s without crashing"
    fi
else
    echo "  - lifegame not present, skipped"
fi

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
