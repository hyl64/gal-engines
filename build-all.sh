#!/bin/bash
# build-all.sh - build every engine in this tree, then check each one
#
# Five engines, five build systems, five ways of saying "it works":
#
#   nscripter  ONScripter fork, CMake      the official demo reaches its loop
#   kirikiri   TJS2/XP3 port, CMake        KAG3 renders, a chapter plays, and
#                                          save / video / audio / frame rate hold
#   reallive   RLVM revived, scons         260 unit tests, needs no game
#   bgi        OpenBGI, its own Makefile   the trial reaches its engine profile
#   artemis    written from scratch, CMake self tests per layer, 12 frames
#
#   ./build-all.sh                          build everything, then check
#   ./build-all.sh --check                  only run the checks
#   ./build-all.sh --data DIR               take game data from DIR, not ./testdata
#   ./build-all.sh --skip kirikiri          leave a check out (repeatable)
#   ./build-all.sh --help
#
# Game data is not in this repository (docs/DATA.md).  A check that needs a game
# is reported as "skip" when the data is absent, never as a failure: a fresh
# clone must build and say honestly what it could not exercise.

set -u
cd "$(dirname "$0")"
ROOT=$(pwd)

CHECK_ONLY=0
DATA="${GAL_DATA:-$ROOT/testdata}"
SKIP=""

while [ $# -gt 0 ]; do
	case "$1" in
		--check) CHECK_ONLY=1 ;;
		--data) shift; DATA="${1:-}"; [ -n "$DATA" ] || { echo "--data needs a directory" >&2; exit 2; } ;;
		--skip) shift; SKIP="$SKIP ${1:-}" ;;
		--help|-h) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
	esac
	shift
done

DATA=$(cd "$DATA" 2>/dev/null && pwd || echo "$DATA")

# The acceptance scripts read data from ./testdata by convention.  When the data
# lives elsewhere a temporary link makes it look local, so a fresh clone runs the
# very same checks against a data directory outside the repository.
LINKED_DATA=0
if [ "$DATA" != "$ROOT/testdata" ] && [ -d "$DATA" ] && [ ! -e "$ROOT/testdata" ]; then
	ln -s "$DATA" "$ROOT/testdata" 2>/dev/null && LINKED_DATA=1
fi
cleanup_data_link() { [ "$LINKED_DATA" = 1 ] && rm -f "$ROOT/testdata"; }
trap cleanup_data_link EXIT

ok_count=0
bad_count=0
skip_count=0
failed=""

skipped() { case " $SKIP " in *" $1 "*) return 0 ;; esac; return 1; }

report() { # name, ok(1/0/2), detail   (2 = skipped)
	if [ "$2" = "2" ]; then
		printf "  \033[33m·\033[0m %-16s %s\n" "$1" "$3"
		skip_count=$((skip_count + 1))
	elif [ "$2" = "1" ]; then
		printf "  \033[32m✓\033[0m %-16s %s\n" "$1" "$3"
		ok_count=$((ok_count + 1))
	else
		printf "  \033[31m✗\033[0m %-16s %s\n" "$1" "$3"
		bad_count=$((bad_count + 1))
		failed="$failed $1"
	fi
}

# A check is skipped, not failed, when its game is absent or it was asked out.
# $1 check name, $2 data path it needs (empty for none), $3 binary it needs
gate() {
	if skipped "$1"; then echo "skip"; return; fi
	if [ -n "$2" ] && [ ! -e "$DATA/$2" ]; then echo "nodata"; return; fi
	if [ -n "${3:-}" ] && [ ! -x "$3" ]; then echo "unbuilt"; return; fi
	echo "run"
}

build_cmake() { # dir, binary
	# A stale executable is not evidence of a successful build, and swallowed
	# output is a failure nobody can diagnose, so the caller keeps the log.
	( cd "$ROOT/$1" && cmake -B build -DCMAKE_BUILD_TYPE=Release \
		&& cmake --build build -j2 ) && [ -x "$ROOT/$2" ]
}

echo "构建"
if [ $CHECK_ONLY = 1 ]; then
	echo "  (--check: 跳过)"
else
	if skipped nscripter; then report nscripter 2 "--skip"
	elif build_cmake OnscripterYuri OnscripterYuri/build/onsyuri >/tmp/gal_build_ns.log 2>&1; then
		report nscripter 1 "build/onsyuri"
	else report nscripter 0 "见 /tmp/gal_build_ns.log"; fi

	if skipped bgi; then report bgi 2 "--skip"
	elif ( cd "$ROOT/openbgi/openbgi-src" && make PLATFORM=darwin -j2 >/tmp/gal_build_bgi.log 2>&1 ) \
		&& [ -x "$ROOT/openbgi/openbgi-src/bin/bgi" ]; then
		report bgi 1 "openbgi-src/bin/bgi"
	else report bgi 0 "见 /tmp/gal_build_bgi.log"; fi

	if skipped kirikiri; then report kirikiri 2 "--skip"
	elif build_cmake kirikiroid2-mac kirikiroid2-mac/build/krkrrun >/tmp/gal_build_kr.log 2>&1; then
		report kirikiri 1 "build/krkrrun"
	else report kirikiri 0 "见 /tmp/gal_build_kr.log"; fi

	if skipped artemis; then report artemis 2 "--skip"
	elif build_cmake artemis artemis/build/agame >/tmp/gal_build_artemis.log 2>&1; then
		report artemis 1 "build/agame"
	else report artemis 0 "见 /tmp/gal_build_artemis.log"; fi

	# --no-bundle: assembling rlvm.app rewrites signatures inside Homebrew's
	# dylibs, which Xcode 15's install_name_tool refuses.  The executable and the
	# tests are what a build check needs; the .app is a packaging step.
	if skipped reallive; then report reallive 2 "--skip"
	elif ( cd "$ROOT/rlvm" && scons --release --no-bundle -j2 >/tmp/gal_build_rlvm.log 2>&1 ) \
		&& [ -x "$ROOT/rlvm/build/release/rlvm-static" ]; then
		report reallive 1 "build/release/rlvm-static"
	else report reallive 0 "见 /tmp/gal_build_rlvm.log"; fi
fi

echo "自检"

# A check that fails may have been starved of CPU or killed by the host (a SIGKILL
# shows up as an abrupt end, not a wrong answer), so a failing run gets one more
# chance before it counts against the build.
retry_once() {
	local fn="$1"
	"$fn" && return 0
	sleep 2
	"$fn"
}

# Never kill unrelated game processes by name.  Each check owns exactly the
# process it starts, and waits for it so a failed check cannot leak it.

# --- nscripter: the official demo must still be alive after a few seconds -----
check_nscripter() {
	( cd "$DATA/lifegame" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/OnscripterYuri/build/onsyuri" --root . --enc:sjis >/tmp/gal_ns.log 2>&1 ) &
	local pid=$!
	local alive=0
	for _ in 1 2 3 4 5 6; do
		sleep 1
		kill -0 "$pid" 2>/dev/null || break
		alive=$((alive + 1))
	done
	if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid" 2>/dev/null; fi
	wait "$pid" 2>/dev/null
	[ "$alive" -ge 3 ]
}
case $(gate nscripter lifegame "$ROOT/OnscripterYuri/build/onsyuri") in
	skip) report nscripter 2 "--skip" ;;
	nodata) report nscripter 2 "需要 $DATA/lifegame（docs/DATA.md）" ;;
	unbuilt) report nscripter 2 "未构建" ;;
	*) if retry_once check_nscripter; then report nscripter 1 "官方 demo 保持运行"
	   else report nscripter 0 "提前退出，见 /tmp/gal_ns.log"; fi ;;
esac

# --- kirikiri: KAG3 loads its scenario and prints its first line --------------
check_kirikiri() {
	( cd "$DATA/kag3-sample" && KRKR_LOOP_SECONDS=2 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/kirikiroid2-mac/build/krkrrun" startup.tjs >/tmp/gal_kr.log 2>&1 )
	grep -q "Scenario loaded : first.ks" /tmp/gal_kr.log \
		&& grep -q "こんにちは。" /tmp/gal_kr.log \
		&& ! grep -q "エラーが発生しました\|Script exception raised\|krkrrun:" /tmp/gal_kr.log
}
case $(gate kirikiri kag3-sample "$ROOT/kirikiroid2-mac/build/krkrrun") in
	skip) report kirikiri 2 "--skip" ;;
	nodata) report kirikiri 2 "需要 $DATA/kag3-sample" ;;
	unbuilt) report kirikiri 2 "未构建" ;;
	*) if retry_once check_kirikiri; then report kirikiri 1 "KAG3 载入剧本并显示首行台词"
	   else report kirikiri 0 "见 /tmp/gal_kr.log"; fi ;;
esac

# --- the KiriKiri acceptance suite: a real KAG project in both distribution
#     forms, saving, movies, audio and frame pacing ---------------------------
kcheck() { # name, data dir, script, log, description
	local name="$1" data="$2" script="$3" log="$4" desc="$5"
	case $(gate "$name" "$data" "$ROOT/kirikiroid2-mac/build/krkrrun") in
		skip) report "$name" 2 "--skip" ;;
		nodata) report "$name" 2 "需要 $DATA/$data" ;;
		unbuilt) report "$name" 2 "未构建" ;;
		*)
			run_kcheck() { "$ROOT/kirikiroid2-mac/tests/$script" \
				"$ROOT/kirikiroid2-mac/build/krkrrun" >"$log" 2>&1; }
			if retry_once run_kcheck; then report "$name" 1 "$desc"
			else report "$name" 0 "见 $log"; fi ;;
	esac
}
# A real KAG chapter, from a folder and from a packed data.xp3: the regression
# that caught the click dead-end and the invalidated objects.
kcheck kirikiri-story  kag-game            run-kag-story.sh   /tmp/gal_kr_story.log "真实章节：目录与 data.xp3 两种形态"
# The second real game: different resolution, JPEG + TLG, crossfades, a TJS snow
# plugin on a Timer, a hyperlink menu.
kcheck kirikiri-demo   krkrsdl2-demo-game  run-demo-game.sh   /tmp/gal_kr_demo.log  "官方 KAG3 demo 启动、渲染、响应点击"
# Saving is what makes a game a game: thumbnail, offset write, read-back.
kcheck kirikiri-save   kag-game            run-save-check.sh  /tmp/gal_kr_save.log  "书签存档：缩略图 + 偏移写入 + 读回"
# Movies through VideoOverlay; the clip lives in the tests directory.
kcheck kirikiri-video  ""                  run-video-check.sh /tmp/gal_kr_video.log "影片解码、上屏并推进"
# The audio contract: pause, status, [ws] waiting, per-buffer volume, fades.
kcheck kirikiri-audio  kag-game            run-audio-check.sh /tmp/gal_kr_audio.log "音频契约（暂停/[ws]/混音/淡入淡出）"
# Frame pacing: both projects must stay playable.
kcheck kirikiri-perf   kag-game            run-perf-check.sh  /tmp/gal_kr_perf.log  "两个真实工程保持可玩帧率"

# --- bgi: the trial reaches the profile it is known to need ------------------
check_bgi() {
	( cd "$DATA/bgi-trial" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/openbgi/openbgi-src/bin/bgi" >/tmp/gal_bg.log 2>&1 ) &
	local pid=$!
	local seen=0
	for _ in 1 2 3 4 5; do
		sleep 5
		kill -0 "$pid" 2>/dev/null || break
		if grep -q "profile 1.669" /tmp/gal_bg.log; then seen=1; break; fi
	done
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	[ "$seen" = 1 ]
}
case $(gate bgi bgi-trial "$ROOT/openbgi/openbgi-src/bin/bgi") in
	skip) report bgi 2 "--skip" ;;
	nodata) report bgi 2 "需要 $DATA/bgi-trial" ;;
	unbuilt) report bgi 2 "未构建" ;;
	*) if retry_once check_bgi; then report bgi 1 "识别出体验版的 profile 1.669"
	   else report bgi 0 "见 /tmp/gal_bg.log"; fi ;;
esac

# --- reallive: 260 unit tests, and nothing here needs a game -----------------
case $(gate reallive "" "$ROOT/rlvm/build/rlvm_unittests") in
	skip) report reallive 2 "--skip" ;;
	unbuilt) report reallive 2 "未构建" ;;
	*) if ( cd "$ROOT/rlvm" && ./build/rlvm_unittests >/tmp/gal_rlvm_ut.log 2>&1 ); then
		report reallive 1 "$(LC_ALL=C grep -c '^\[       OK \]' /tmp/gal_rlvm_ut.log) 个单元测试通过"
	   else report reallive 0 "见 /tmp/gal_rlvm_ut.log"; fi ;;
esac

# --- artemis: its own self tests, one per layer, plus twelve rendered frames --
check_artemis() {
	local ok=1 t frames n
	for t in "pfstool selftest" "asttool selftest" "arun --selftest"; do
		( cd "$ROOT/artemis" && ./build/$t >/dev/null 2>&1 ) || ok=0
	done
	frames=$(mktemp -d /tmp/gal-artemis.XXXXXX)
	( cd "$ROOT/artemis" && SDL_VIDEODRIVER=dummy ./build/agame tests/sample-scene.ast \
		--frames "$frames" >/tmp/gal_artemis.log 2>&1 ) || ok=0
	n=$(find "$frames" -maxdepth 1 -type f -name 'frame*.bmp' | wc -l | tr -d ' ')
	rm -rf "$frames"
	[ "$n" = 12 ] || ok=0
	[ "$ok" = 1 ]
}
case $(gate artemis "" "$ROOT/artemis/build/agame") in
	skip) report artemis 2 "--skip" ;;
	unbuilt) report artemis 2 "未构建" ;;
	*) if check_artemis; then report artemis 1 "pfs/ast/runtime 自检 + 12 帧渲染"
	   else report artemis 0 "见 /tmp/gal_artemis.log"; fi ;;
esac

echo
echo "通过 ${ok_count}，失败 ${bad_count}，跳过 ${skip_count}"
if [ "$bad_count" != 0 ]; then
	echo "失败项:${failed}"
	exit 1
fi
if [ "$skip_count" != 0 ] && [ $CHECK_ONLY = 0 ]; then
	echo "跳过的检查需要游戏数据：docs/DATA.md（--data DIR 可指向别处）"
fi
exit 0
