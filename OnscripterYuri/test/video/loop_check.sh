#!/usr/bin/env bash
# loop_check.sh -- does `setloop/1` actually loop the wcmpg movie layer?
#
#   ./test/video/loop_check.sh [path/to/onsyuri]
#
# How it decides: it plays a 1-second clip with setloop/1 for 10 seconds and
# samples the engine surface twice a second.  A looping clip replays the same
# frame sequence, so two samples ~6 s apart come out byte-identical; a clip that
# plays once and stops leaves a constant frame and produces no such repeat.
#
# Why not just use the real game: from cold start it needs 40-55 s to reach the
# title, which makes the capture window unreliable.  This check answers the same
# question in 10 seconds.
#
# Exit: 0 = looping, 1 = not looping, 2 = environment missing.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE_ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${1:-$ENGINE_ROOT/build/onsyuri}"

[ -x "$BIN" ] || { echo "no binary: $BIN"; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 2; }

WORK="$(mktemp -d /tmp/ons_loopcheck.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# A 1-second, high-contrast clip: any repeat is unambiguous.
ffmpeg -y -loglevel error -f lavfi -i "testsrc=duration=1:size=640x480:rate=10" \
  -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$WORK/bright.mp4" || exit 2

cat > "$WORK/00.txt" <<'EOS'
;$V6000G2000S1024,576L10000
*define
effect 10,10,0
setlayer 10,20,"dll\wcmpg.dll"
game
*start
bg black,0
lsp 983,"*10"
layermessage 10,"setx/0"
layermessage 10,"sety/0"
layermessage 10,"setloop/1"
layermessage 10,"open/bright.mp4"
layermessage 10,"play"
print 1
delay 10000
layermessage 10,"close"
end
EOS

( cd "$WORK" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ONS_SURFACE_DUMP="$WORK/cur.bmp" "$BIN" --root . --enc:sjis --window \
    > "$WORK/log.txt" 2>&1 ) &
pid=$!
n=0
while [ $n -lt 20 ]; do
    sleep 0.5
    n=$((n+1))
    kill -0 $pid 2>/dev/null || break
    cp "$WORK/cur.bmp" "$WORK/s$n.bmp" 2>/dev/null
done
kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null

python3 - "$WORK" <<'PY'
import glob, os, struct, sys

work = sys.argv[1]

def movie_mean(path):
    """Mean RGB over the movie's own rectangle.

    The clip is 640x480 drawn at (0,0) on a 1024x576 screen, so sampling the whole
    surface dilutes it: roughly half the samples land on untouched background and a
    frame full of video reads as 'no content'.  That mistake cost four rounds.
    """
    d = open(path, 'rb').read()
    if len(d) < 200:
        return None
    off = struct.unpack_from('<I', d, 10)[0]
    w = struct.unpack_from('<i', d, 18)[0]
    h = abs(struct.unpack_from('<i', d, 22)[0])
    rw, rh = min(640, w), min(480, h)
    sr = sg = sb = n = 0
    for y in range(0, rh, 4):
        row = off + y * w * 4
        for x in range(0, rw, 4):
            i = row + x * 4
            if i + 2 >= len(d):
                continue
            sb += d[i]; sg += d[i + 1]; sr += d[i + 2]; n += 1
    if not n:
        return None
    return (sr // n, sg // n, sb // n)

seq = []
for f in sorted(glob.glob(os.path.join(work, 's*.bmp')),
                key=lambda p: int(os.path.basename(p)[1:-4])):
    m = movie_mean(f)
    if m is not None:
        seq.append((os.path.basename(f), m))

print("samples:", len(seq))
for name, m in seq:
    print(f"  {name} movie-mean rgb{m}")

if not seq:
    print("RESULT: FAIL - no frames captured")
    sys.exit(1)

vals = [m for _, m in seq]
print("distinct movie means:", len(set(vals)))

if len(set(vals)) < 2:
    print("RESULT: FAIL - the movie rectangle never changed (static)")
    sys.exit(1)

cyc = None
for i in range(len(vals)):
    for j in range(i + 2, len(vals)):
        if vals[j] == vals[i] and any(vals[k] != vals[i] for k in range(i + 1, j)):
            cyc = (i, j)
            break
    if cyc:
        break

if cyc:
    print(f"RESULT: PASS - movie mean at sample {cyc[0]} reappears at {cyc[1]} "
          f"after different content => setloop loops")
    sys.exit(0)

print("RESULT: INCONCLUSIVE - the rectangle changed but never repeated")
print("        (a long clip sampled for too short a window looks like this)")
sys.exit(1)
PY
