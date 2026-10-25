#!/usr/bin/env bash
# bench.sh — gal-engines 效率基线量具（lead 拥有；跨引擎通用）
#
# 用法:
#   ./bench.sh <binary> <game_dir> [seconds=20] [--cold] [-- 引擎参数透传...]
#
# 契约基线（DESIGN-VIDEO.md）：静态页 CPU 均值 <3%，RSS 峰值 <150MB。
# CPU 用 ps 累计 CPU 时间差分（top -pid 在本机受限环境无输出；ps 生命周期均值
# 对空转主循环会虚高，差分才是瞬时值）。
# 退出码: 0=PASS 1=FAIL 2=用法/环境错误
set -u

BIN="${1:?usage: bench.sh <binary> <game_dir> [seconds] [--cold] [-- passthru]}"; shift
GAME="${1:?}"; shift
SECS=20
if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then SECS="$1"; shift; fi
COLD=0
if [ "${1:-}" = "--cold" ]; then COLD=1; shift; fi
PASS=()
if [ "${1:-}" = "--" ]; then shift; PASS=("$@"); fi

[ -x "$BIN" ] || { echo "not executable: $BIN"; exit 2; }
[ -d "$GAME" ] || { echo "no such dir: $GAME"; exit 2; }
# 相对路径先转绝对（下面要 cd 进游戏目录）
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
GAME=$(cd "$GAME" && pwd)

cd "$GAME" || exit 2
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

# ps TIME 形如 mm:ss.cc（超一小时 h:mm:ss.cc）→ 秒
hms2s() {
  awk -F'[:.]' '{
    if (NF==4) print $1*3600+$2*60+$3+$4/100;
    else print $1*60+$2+$3/100
  }' <<<"$1"
}

T_START=$(python3 -c "import time;print(time.time())")
"$BIN" "${PASS[@]}" > "/tmp/bench.out.$$" 2>&1 &
PID=$!

# 冷启动检查点：0.5s 存活；1.5s 仍存活视为"进主循环"（脚本错误会在此前退出）
sleep 0.5
if ! kill -0 $PID 2>/dev/null; then
  echo "FAIL: died within 0.5s (see /tmp/bench.out.$$)"; tail -5 "/tmp/bench.out.$$"; exit 1
fi
if [ "$COLD" = 1 ]; then
  echo "cold-start: process alive at 0.5s (note: pre-main dyld time not counted)"
fi

# 采样 SECS 秒：每 ~2s 取一段差分
DEADLINE=$(python3 -c "import time;print(time.time()+$SECS)")
TOTAL_CPU=0; TOTAL_WALL=0; RSS_MAX_KB=0; N=0
PREV_S=$(hms2s "$(ps -o time= -p $PID)")
PREV_T=$(python3 -c "import time;print(time.time())")
while :; do
  if ! kill -0 $PID 2>/dev/null; then
    echo "FAIL: died at t=$(( $(python3 -c "import time;print(int(time.time())-$T_START)") ))s"; tail -5 "/tmp/bench.out.$$"; exit 1
  fi
  NOW=$(python3 -c "import time;print(time.time())")
  awk "BEGIN{exit !($NOW>=$DEADLINE)}" && break
  sleep 2
  CUR_S=$(hms2s "$(ps -o time= -p $PID)")
  CUR_T=$(python3 -c "import time;print(time.time())")
  RSS_KB=$(ps -o rss= -p $PID | tr -d ' ')
  TOTAL_CPU=$(python3 -c "print($TOTAL_CPU + max(0,$CUR_S-$PREV_S))")
  TOTAL_WALL=$(python3 -c "print($TOTAL_WALL + ($CUR_T-$PREV_T))")
  PREV_S=$CUR_S; PREV_T=$CUR_T
  [ "${RSS_KB:-0}" -gt "$RSS_MAX_KB" ] && RSS_MAX_KB=$RSS_KB
  N=$((N+1))
done

kill -TERM $PID 2>/dev/null; sleep 0.5; kill -KILL $PID 2>/dev/null
wait $PID 2>/dev/null || true

CPU_AVG=$(python3 -c "print(round(100*$TOTAL_CPU/max($TOTAL_WALL,0.001),2))")
RSS_MB=$(python3 -c "print(round($RSS_MAX_KB/1024,1))")

echo "binary=$BIN game=$GAME samples=$N window=${SECS}s"
echo "cpu_avg_pct=$CPU_AVG rss_peak_mb=$RSS_MB"
STATUS=PASS
python3 -c "import sys; sys.exit(0 if $CPU_AVG < 3.0 else 1)" || { echo "  ! CPU >= 3%"; STATUS=FAIL; }
python3 -c "import sys; sys.exit(0 if $RSS_MB < 150.0 else 1)" || { echo "  ! RSS >= 150MB"; STATUS=FAIL; }
echo "verdict=$STATUS (baseline: static CPU<3%, RSS<150MB)"
[ "$STATUS" = PASS ] || exit 1
