#!/bin/bash
# gal prep selftest — builds throwaway games under /tmp, drives every branch
# of the preprocessing pipeline (happy path, idempotence, the libx264 retry,
# total failure + cleanup, SAR/DAR preservation, deinterlace, map removal on
# a failed re-transcode) and asserts the results.  Never touches testdata/ or
# any real game; `gal remove`s its own library entries at the end.
#
#   cd ~/Documents/gal-engines/gal && ./prep/selftest.sh
#
# exit 0 = all green.  Tunables: GAL_FFMPEG_BIN / GAL_FFPROBE_BIN (shims live
# next to this script), KEEP=1 keeps /tmp/gal-prep-selftest for inspection.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
GAL="$HERE/../gal"
BASE=/tmp/gal-prep-selftest
pass_n=0; fail_n=0

pass() { pass_n=$((pass_n+1)); echo "  ✓ $1"; }
fail() { fail_n=$((fail_n+1)); echo "  ✗ FAIL: $1"; }
check() { # check <desc> <cond-cmd...>
  local d=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi
}

rm -rf "$BASE"; mkdir -p "$BASE"
make_game() { # make_game <dir> ; nscript signature + sample movies
  local g=$1; mkdir -p "$g/sub"
  printf 'stub' > "$g/nscript.dat"; printf 'stub' > "$g/00.txt"
  ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=2" \
    -f lavfi -i "sine=frequency=440:duration=2" \
    -c:v mpeg1video -q:v 5 -c:a mp2 -b:a 128k "$g/movie.mpg"
  ffmpeg -loglevel error -y -f lavfi -i "smptebars=size=320x240:rate=24:duration=1" \
    -f lavfi -i "sine=frequency=660:duration=1" \
    -c:v wmv2 -c:a wmav2 -b:a 96k "$g/intro.avi"
  ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=352x240:rate=29.97:duration=1" \
    -c:v mpeg1video -aspect 4:3 "$g/sub/Parade.MPEG"
  printf '\x00\x1a\x45\xdf not really' > "$g/broken.avi"
}

echo "== 1) gal add 自动 prep（NScripter）"
make_game "$BASE/game"
out=$("$GAL" add "$BASE/game" --name selftest-game 2>&1); rc=$?
[ $rc -eq 0 ] && pass "gal add 返回 0（带 unprobeable 也不影响入库）" || { fail "gal add rc=$rc"; echo "$out"; }
echo "$out" | grep -q "自动触发\|预处理:" && pass "add 输出里跑了预处理" || { fail "add 没跑预处理"; echo "$out"; }
check "_video 建出 3 个 mp4" test "$(ls "$BASE/game/_video"/*.mp4 2>/dev/null | wc -l | tr -d ' ')" -eq 3
check "movie.mp4 = h264+aac 320x240 sar 1:1" bash -c 'ffprobe -v error -show_entries stream=codec_name,width,height,sample_aspect_ratio -of csv=p=0 "$1" | sort | tr "\n" " " | grep -q "320,240,1:1" && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" | grep -q aac' _ "$BASE/game/_video/movie.mp4"
check "Parade 352x240 SAR200:219 -> 320x240 方像素" bash -c 'ffprobe -v error -select_streams v:0 -show_entries stream=width,height,sample_aspect_ratio,display_aspect_ratio -of csv=p=0 "$1" | grep -q "^320,240,1:1,4:3$"' _ "$BASE/game/_video/Parade.mp4"

echo "== 2) video_map.json 逐字对契约（version 1，from/to/codec/source）"
python3 - "$BASE/game/_video/video_map.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["version"] == 1, d
assert set(d) == {"version", "mappings"}, set(d)
m = {e["from"]: e for e in d["mappings"]}
assert set(m) == {"intro.avi", "movie.mpg", "sub/Parade.MPEG"}, m.keys()
for e in d["mappings"]:
    assert set(e) == {"from", "to", "codec", "source"}, e
    assert e["codec"] == "h264" and e["source"] == "loose", e
    assert e["to"].startswith("_video/") and "\\" not in e["to"], e
    assert "\\" not in e["from"], e   # backslash normalised to /
assert m["sub/Parade.MPEG"]["to"] == "_video/Parade.mp4"
print("   map ok:", json.dumps(d, ensure_ascii=False))
PY
[ $? -eq 0 ] && pass "map 结构/字段/归一全部符合契约" || fail "map 不符合契约"

echo "== 3) 幂等：第二次 prep 全跳过、产物 mtime 不变"
mt1=$(stat -f %m "$BASE/game/_video/movie.mp4")
out=$("$GAL" prep selftest-game 2>&1)
echo "$out" | grep -q "跳过(幂等)" && pass "重跑显示 跳过(幂等)" || { fail "重跑没走幂等"; echo "$out"; }
mt2=$(stat -f %m "$BASE/game/_video/movie.mp4")
[ "$mt1" = "$mt2" ] && pass "产物未被重写" || fail "产物 mtime 变了"

echo "== 4) 源更新 -> 重转；产物损坏 -> 重转"
touch "$BASE/game/movie.mpg"
"$GAL" prep selftest-game >/dev/null 2>&1
mt3=$(stat -f %m "$BASE/game/_video/movie.mp4")
[ "$mt3" != "$mt2" ] && pass "源 touch 后重转" || fail "源更新没触发重转"
printf 'corrupt' > "$BASE/game/_video/intro.mp4"
"$GAL" prep selftest-game >/dev/null 2>&1
check "坏产物被重转修复" bash -c 'ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$1" | grep -q h264' _ "$BASE/game/_video/intro.mp4"

echo "== 5) 硬编失败 -> libx264 veryfast 重试路径"
mkdir -p "$BASE/retry"; printf x > "$BASE/retry/nscript.dat"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=1" \
  -c:v mpeg1video "$BASE/retry/clip.mpg"
GAL_FFMPEG_BIN="$HERE/shim-ffmpeg-no-hw" "$GAL" prep "$BASE/retry" --report json \
  > "$BASE/retry/hw-fail.json" 2>/dev/null
python3 - "$BASE/retry/hw-fail.json" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))["videos"][0]
a = {t["encoder"]: t["ok"] for t in v["attempts"]}
assert v["status"] == "transcoded" and v["encoder"] == "libx264", v
assert a == {"h264_videotoolbox": False, "libx264": True}, a
PY
[ $? -eq 0 ] && pass "videotoolbox 失败后 libx264 成功，attempts 两条齐全" || fail "重试路径不对"
check "回退产物是 h264 High profile" bash -c 'ffprobe -v error -select_streams v:0 -show_entries stream=profile -of csv=p=0 "$1" | grep -q High' _ "$BASE/retry/_video/clip.mp4"

echo "== 6) 双编码器全失败 -> failed、清 .part、映射不落"
mkdir -p "$BASE/fail"; printf x > "$BASE/fail/nscript.dat"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=1" \
  -c:v mpeg1video "$BASE/fail/fail.mpg"
GAL_FFMPEG_BIN="$HERE/shim-ffmpeg-all-fail" "$GAL" prep "$BASE/fail" >/dev/null 2>&1
[ $? -eq 1 ] && pass "全失败时 gal prep 返回 1" || fail "返回码不是 1"
check "map 为空" python3 -c 'import json,sys;assert json.load(open(sys.argv[1]))["mappings"]==[]' "$BASE/fail/_video/video_map.json"
check "没有残留 .part/产物" bash -c '! ls "$1"/_video | grep -qE "\.mp4$|\.part"' _ "$BASE/fail"

echo "== 7) 成功后改坏 -> 映射移除，恢复后映射回来"
cp "$BASE/retry/clip.mpg" "$BASE/retry/clip.bak"; touch "$BASE/retry/clip.mpg"
GAL_FFMPEG_BIN="$HERE/shim-ffmpeg-all-fail" "$GAL" prep "$BASE/retry" >/dev/null 2>&1
check "重转失败后该条从映射移除" python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));assert not [m for m in d["mappings"] if m["from"]=="clip.mpg"]' "$BASE/retry/_video/video_map.json"
cp "$BASE/retry/clip.bak" "$BASE/retry/clip.mpg"; touch "$BASE/retry/clip.mpg"
"$GAL" prep "$BASE/retry" >/dev/null 2>&1
check "源恢复后映射回来" python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));assert [m for m in d["mappings"] if m["from"]=="clip.mpg"]' "$BASE/retry/_video/video_map.json"

echo "== 8) 隔行 -> yadif；无音频 -> -an；大写扩展名已覆盖(Parade.MPEG)"
mkdir -p "$BASE/ilf"; printf x > "$BASE/ilf/nscript.dat"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=50:duration=1" \
  -c:v mpeg2video -flags +ilme+ildct -field_order tt "$BASE/ilf/inter.mpg"
"$GAL" prep "$BASE/ilf" --report json 2>/dev/null | grep -q "yadif=0:-1:0" \
  && pass "隔行源滤镜含 yadif=0:-1:0" || fail "没加 yadif"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=176x144:rate=25:duration=1" \
  -an -c:v mpeg1video "$BASE/ilf/mute.mpg"
"$GAL" prep "$BASE/ilf" >/dev/null 2>&1
check "无音频产物只有 video 流" bash -c '[ "$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$1" | wc -l | tr -d " ")" = 1 ]' _ "$BASE/ilf/_video/mute.mp4"

echo "== 9) --no-prep 与非 NScripter 不自动 prep"
make_game "$BASE/nop" >/dev/null 2>&1; rm -rf "$BASE/nop/_video"
"$GAL" add "$BASE/nop" --name selftest-nop --no-prep >/dev/null 2>&1
check "--no-prep 不建 _video" test ! -d "$BASE/nop/_video"
mkdir -p "$BASE/krk"; : > "$BASE/krk/data.xp3"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=160x120:rate=25:duration=1" -c:v mpeg1video "$BASE/krk/a.mpg"
"$GAL" add "$BASE/krk" --name selftest-krk >/dev/null 2>&1
check "KiriKiri 不自动 prep" test ! -d "$BASE/krk/_video"

echo "== 10) savetest：harness 存在则转发，缺失才报待接入(2)"
# the real harness now lives in ../harness/run_savetest.sh, so the default path
# forwards; point it at a missing script to exercise the "待接入" branch.
out=$(GAL_SAVETEST_SH=/nonexistent/run_savetest.sh "$GAL" savetest selftest-game 2>&1)
rc=$?
[ $rc -eq 2 ] && echo "$out" | grep -q "待接入" \
  && pass "缺 harness 时返回 2 并报待接入" || { fail "savetest 缺失分支不对 (rc=$rc)"; echo "$out"; }
if [ -x "$HERE/../harness/run_savetest.sh" ]; then
  out=$(GAL_SAVETEST_SH="$HERE/../harness/run_savetest.sh" GAL_ENGINE=/bin/true \
        "$GAL" savetest selftest-game 2>&1)
  rc=$?
  [ $rc -eq 2 ] && pass "harness 存在时被转发（/bin/true 不是引擎，故报环境缺失 2）" \
    || { fail "savetest 转发分支不对 (rc=$rc)"; echo "$out"; }
fi
printf '#!/bin/sh\necho "SAVETEST-GOT $*"\n' > "$BASE/fake-savetest.sh"; chmod +x "$BASE/fake-savetest.sh"
out=$(GAL_SAVETEST_SH="$BASE/fake-savetest.sh" "$GAL" savetest selftest-game -- --extra 2>&1)
echo "$out" | grep -q "SAVETEST-GOT $BASE/game" && echo "$out" | grep -q -- "--extra" \
  && pass "GAL_SAVETEST_SH 转发游戏目录与 -- 后参数" || { fail "转发不对"; echo "$out"; }

echo "== 11) 库里没有且不是目录 -> 1"
"$GAL" prep definitely-not-here-$$ >/dev/null 2>&1
[ $? -eq 1 ] && pass "未知目标返回 1" || fail "未知目标返回码不对"

echo "== 12) 源坏但旧产物仍有效 -> 映射保留"
mkdir -p "$BASE/keep"; printf x > "$BASE/keep/nscript.dat"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=1" \
  -c:v mpeg1video "$BASE/keep/good.mpg"
"$GAL" prep "$BASE/keep" >/dev/null 2>&1
printf 'broken bytes' > "$BASE/keep/good.mpg"
# 源坏，但 mtime 拨回转码产物之前（幂等判据仍成立 -> 旧映射保留）
python3 -c 'import os,sys;s,d=sys.argv[1:];t=os.path.getmtime(d)-10;os.utime(s,(t,t))' \
  "$BASE/keep/good.mpg" "$BASE/keep/_video/good.mp4"
"$GAL" prep "$BASE/keep" >/dev/null 2>&1
check "旧映射保留且产物大小照报" python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));assert [m for m in d["mappings"] if m["from"]=="good.mpg"]' "$BASE/keep/_video/video_map.json"

echo "== 13) verify 分级管道（error/warn 互斥；warn 不计败；MARKERS 即 T3 接入点）"
python3 - "$GAL" <<'PY'
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader("galmod", sys.argv[1])
m = importlib.util.module_from_spec(importlib.util.spec_from_loader("galmod", loader))
loader.exec_module(m)
import re
# the whitelist is no longer a blank placeholder: it must cover the optional
# resources OnscripterYuri skips, and must NOT contain the over-broad words
# that made 228eb58 mis-kill the official demos.
assert m.VERIFY_WARN_MARKERS, "warn 集不应再为空"
assert m.VERIFY_WARN_PATTERNS, "warn 正则集不应为空"
for banned in ("invalid", "no decoder", "can't find file"):
    assert banned not in m.VERIFY_WARN_MARKERS, f"过宽的词不该进白名单: {banned}"
    assert banned not in m.VERIFY_ERROR_MARKERS, f"过宽的词不该进 error: {banned}"
warn_res = [re.compile(p, re.I) for p in m.VERIFY_WARN_PATTERNS]
lines = [" *** can't find file [cursor0.bmp] ***",
         " *** can't find file [uoncur.bmp] ***",
         " *** can't find file [doffcur.bmp] ***",
         "cannot open system.lua",
         "some error here", "plain output"]
low = [(l, l.lower()) for l in lines]
err = [l for l, k in low if any(x in k for x in m.VERIFY_ERROR_MARKERS)]
warn = [l for l, k in low
        if (any(x in k for x in m.VERIFY_WARN_MARKERS)
            or any(r.search(k) for r in warn_res))
        and not any(x in k for x in m.VERIFY_ERROR_MARKERS)]
assert err == ["some error here"], err
assert len(warn) == 4, warn       # 3 cursor lines + system.lua
# and a genuinely required file must still fail the run
req = [" *** can't find file [scenario.dat] ***"]
assert not any(any(r.search(x.lower()) for r in warn_res) for x in req), \
    "必需文件缺失不该被白名单吞掉"
PY
[ $? -eq 0 ] && pass "分级互斥正确，VERIFY_*_MARKERS 即接入点" || fail "分级管道不对"

echo "== 14) nsa 归档内视频：解出 -> 转码 -> source=nsa: 映射"
N="$BASE/nsa"; mkdir -p "$N"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=1" \
  -c:v mpeg4 -q:v 5 "$BASE/inside.avi"
printf 'stub' > "$N/nscript.dat"
python3 - "$BASE/inside.avi" "$N/arc.nsa" <<'PY'
import struct, sys
payload = open(sys.argv[1], 'rb').read()
name = b'opening.avi\x00'
entry = len(name) + 1 + 4 + 4 + 4
base = 6 + entry
hdr = struct.pack('>H I', 1, base)
hdr += name + struct.pack('>B I I I', 0, 0, len(payload), len(payload))
open(sys.argv[2], 'wb').write(hdr + payload)
PY
"$GAL" add "$N" --name selftest-nsa >/dev/null 2>&1
check "归档内视频被转码" test "$(ls "$N/_video"/*.mp4 2>/dev/null | wc -l | tr -d ' ')" -eq 1
check "映射 source 用 nsa: 形态" python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
m=d["mappings"][0]
assert m["source"].startswith("nsa:arc.nsa#"), m["source"]
assert m["from"]=="opening.avi", m["from"]
' "$N/_video/video_map.json"
check "计数把归档条目算进 found" bash -c '"$1" prep "$2" | grep -q "共 1 个影片"' _ "$GAL" "$N"

"$GAL" remove selftest-game >/dev/null 2>&1
"$GAL" remove selftest-nop  >/dev/null 2>&1
"$GAL" remove selftest-krk  >/dev/null 2>&1
"$GAL" remove selftest-nsa  >/dev/null 2>&1
[ "${KEEP:-}" = "1" ] || rm -rf "$BASE"
echo
echo "selftest: $pass_n pass, $fail_n fail"
[ $fail_n -eq 0 ]
