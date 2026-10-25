#!/bin/bash
# build-all.sh - build every engine in this tree, then check each one
#
# Five engines, five build systems, five ways of saying "it works":
#
#   nscripter  a fork of ONScripter, CMake        - an official demo reaches its loop
#   kirikiri   a TJS2/XP3 port, CMake             - KAG3 renders and a chapter plays
#   reallive   RLVM revived, scons                - 260 unit tests, no data needed
#   bgi        OpenBGI, hand-written Makefile     - a trial reaches its engine profile
#   artemis    written from scratch here, CMake   - self tests, no data needed
#
# Usage:
#   ./build-all.sh                    build everything, then check
#   ./build-all.sh --check            only run the checks
#   ./build-all.sh --data DIR         take game data from DIR instead of ./testdata
#   ./build-all.sh --skip reallive    leave an engine out (repeatable)
#   ./build-all.sh --help
#
# Game data is not in this repository (see docs/DATA.md).  A check that needs it
# is reported as "跳过" when the data is absent, never as a failure: a fresh
# clone must be able to build and see honestly what could not be exercised.

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
		--help|-h) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
	esac
	shift
done

DATA=$(cd "$DATA" 2>/dev/null && pwd || echo "$DATA")

# Some acceptance scripts in the engine trees read data from ./testdata by
# convention.  When the data lives elsewhere, a temporary link makes it look
# local, so a fresh clone can run the same checks against a data directory
# outside the repository without any script knowing the difference.
LINKED_DATA=0
if [ "$DATA" != "$ROOT/testdata" ] && [ -d "$DATA" ] && [ ! -e "$ROOT/testdata" ]; then
	if ln -s "$DATA" "$ROOT/testdata" 2>/dev/null; then LINKED_DATA=1; fi
fi
cleanup_data_link() { [ "$LINKED_DATA" = 1 ] && rm -f "$ROOT/testdata"; }
trap cleanup_data_link EXIT

skipped() { case " $SKIP " in *" $1 "*) return 0 ;; esac; return 1; }

ok_count=0
bad_count=0
skip_count=0
failed=""

pass() { printf "  \033[32m✓\033[0m %-16s %s\n" "$1" "$2"; ok_count=$((ok_count + 1)); }
fail() { printf "  \033[31m✗\033[0m %-16s %s\n" "$1" "$2"; bad_count=$((bad_count + 1)); failed="$failed $1"; }
skip() { printf "  \033[33m·\033[0m %-16s %s\n" "$1" "$2"; skip_count=$((skip_count + 1)); }

# A build counts only if it exits zero and leaves the expected executable: a
# stale binary from an earlier run must not read as a success.
build_cmake() { # dir, binary
	( cd "$ROOT/$1" && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
		&& cmake --build build -j2 >/dev/null 2>&1 ) && [ -x "$ROOT/$2" ]
}

echo "构建"
if [ $CHECK_ONLY = 1 ]; then
	echo "  (--check: 跳过)"
else
	# logs are kept so a failure stays diagnosable
	if skipped nscripter; then skip nscripter "--skip"; else
		if build_cmake OnscripterYuri OnscripterYuri/build/onsyuri >/tmp/gal_build_ns.log 2>&1; then
			pass nscripter "build/onsyuri"
		else
			fail nscripter "见 /tmp/gal_build_ns.log"
		fi
	fi

	if skipped bgi; then skip bgi "--skip"; else
		if ( cd "$ROOT/openbgi/openbgi-src" && make PLATFORM=darwin -j2 >/tmp/gal_build_bgi.log 2>&1 ) \
			&& [ -x "$ROOT/openbgi/openbgi-src/bin/bgi" ]; then
			pass bgi "openbgi-src/bin/bgi"
		else
			fail bgi "见 /tmp/gal_build_bgi.log"
		fi
	fi

	if skipped kirikiri; then skip kirikiri "--skip"; else
		if build_cmake kirikiroid2-mac kirikiroid2-mac/build/krkrrun >/tmp/gal_build_kr.log 2>&1; then
			pass kirikiri "build/krkrrun"
		else
			fail kirikiri "见 /tmp/gal_build_kr.log"
		fi
	fi

	if skipped artemis; then skip artemis "--skip"; else
		if build_cmake artemis artemis/build/agame >/tmp/gal_build_artemis.log 2>&1; then
			pass artemis "build/agame"
		else
			fail artemis "见 /tmp/gal_build_artemis.log"
		fi
	fi

	if skipped reallive; then skip reallive "--skip"; else
		if ( cd "$ROOT/rlvm" && scons --release -j2 >/tmp/gal_build_rlvm.log 2>&1 ) \
			&& [ -x "$ROOT/rlvm/build/rlvm.app/Contents/MacOS/rlvm" ]; then
			pass reallive "build/rlvm.app"
		else
			fail reallive "见 /tmp/gal_build_rlvm.log"
		fi
	fi
fi

echo "自检"
# Every check owns exactly the process it starts and waits for it, so a failed
# check cannot leak a process that would poison the next run.

# --- nscripter: the official demo must still be alive after three seconds ----
if skipped nscripter; then skip nscripter "--skip"
elif [ ! -d "$DATA/lifegame" ]; then skip nscripter "需要 $DATA/lifegame（见 docs/DATA.md）"
elif [ ! -x "$ROOT/OnscripterYuri/build/onsyuri" ]; then skip nscripter "未构建"
else
	( cd "$DATA/lifegame" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/OnscripterYuri/build/onsyuri" --root . --enc:sjis >/tmp/gal_ns.log 2>&1 ) &
	pid=$!
	sleep 3
	if kill -0 "$pid" 2>/dev/null; then pass nscripter "官方 demo 3 秒后仍在主循环"; else fail nscripter "提前退出，见 /tmp/gal_ns.log"; fi
	kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
fi

# --- kirikiri: KAG3 loads its scenario and prints its first line --------------
if skipped kirikiri; then skip kirikiri "--skip"
elif [ ! -d "$DATA/kag3-sample" ]; then skip kirikiri "需要 $DATA/kag3-sample"
elif [ ! -x "$ROOT/kirikiroid2-mac/build/krkrrun" ]; then skip kirikiri "未构建"
else
	( cd "$DATA/kag3-sample" && KRKR_LOOP_SECONDS=3 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/kirikiroid2-mac/build/krkrrun" startup.tjs >/tmp/gal_kr.log 2>&1 )
	if grep -q "Scenario loaded : first.ks" /tmp/gal_kr.log \
		&& grep -q "こんにちは。" /tmp/gal_kr.log \
		&& ! grep -q "エラーが発生しました\|Script exception raised\|krkrrun:" /tmp/gal_kr.log; then
		pass kirikiri "KAG3 载入剧本并显示首行台词"
	else
		fail kirikiri "见 /tmp/gal_kr.log"
	fi
fi

# --- kirikiri-story: a whole chapter, from a folder and from data.xp3 ---------
if skipped kirikiri; then skip kirikiri-story "--skip"
elif [ ! -d "$DATA/kag-game/data" ]; then skip kirikiri-story "需要 $DATA/kag-game"
elif [ ! -x "$ROOT/kirikiroid2-mac/build/krkrrun" ]; then skip kirikiri-story "未构建"
else
	if "$ROOT/kirikiroid2-mac/tests/run-kag-story.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_story.log 2>&1; then
		pass kirikiri-story "真实章节在目录与 data.xp3 两种形态下走完"
	else
		fail kirikiri-story "见 /tmp/gal_kr_story.log"
	fi
fi

# --- kirikiri-demo: the official KiriKiri SDL2 / KAG3 demo --------------------
if skipped kirikiri; then skip kirikiri-demo "--skip"
elif [ ! -d "$DATA/krkrsdl2-demo-game" ]; then skip kirikiri-demo "需要 $DATA/krkrsdl2-demo-game"
elif [ ! -x "$ROOT/kirikiroid2-mac/build/krkrrun" ]; then skip kirikiri-demo "未构建"
else
	if "$ROOT/kirikiroid2-mac/tests/run-demo-game.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_demo.log 2>&1; then
		pass kirikiri-demo "官方 KAG3 demo 启动、渲染并响应点击"
	else
		fail kirikiri-demo "见 /tmp/gal_kr_demo.log"
	fi
fi

# --- bgi: the trial reaches the profile it is known to need ------------------
if skipped bgi; then skip bgi "--skip"
elif [ ! -d "$DATA/bgi-trial" ]; then skip bgi "需要 $DATA/bgi-trial"
elif [ ! -x "$ROOT/openbgi/openbgi-src/bin/bgi" ]; then skip bgi "未构建"
else
	( cd "$DATA/bgi-trial" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		"$ROOT/openbgi/openbgi-src/bin/bgi" >/tmp/gal_bg.log 2>&1 ) &
	pid=$!
	ok=0
	for _ in 1 2 3 4 5; do
		sleep 5
		kill -0 "$pid" 2>/dev/null || break
		if grep -q "profile 1.669" /tmp/gal_bg.log; then ok=1; break; fi
	done
	kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
	if [ "$ok" = 1 ]; then pass bgi "识别出体验版的 profile 1.669"; else fail bgi "见 /tmp/gal_bg.log"; fi
fi

# --- reallive: 260 unit tests, and nothing here needs a game -----------------
if skipped reallive; then skip reallive "--skip"
elif [ ! -x "$ROOT/rlvm/build/rlvm_unittests" ]; then skip reallive "未构建"
else
	# the tests load their fixtures by relative path, so they run from their own tree
	if ( cd "$ROOT/rlvm" && ./build/rlvm_unittests >/tmp/gal_rlvm_ut.log 2>&1 ); then
		pass reallive "$(LC_ALL=C grep -c '^\[       OK \]' /tmp/gal_rlvm_ut.log) 个单元测试通过"
	else
		fail reallive "见 /tmp/gal_rlvm_ut.log"
	fi
fi

# --- artemis: its own self tests, one per layer, plus twelve rendered frames --
if skipped artemis; then skip artemis "--skip"
elif [ ! -x "$ROOT/artemis/build/agame" ]; then skip artemis "未构建"
else
	ok=1
	for t in "pfstool selftest" "asttool selftest" "arun --selftest"; do
		( cd "$ROOT/artemis" && ./build/$t >/dev/null 2>&1 ) || ok=0
	done
	frames=$(mktemp -d /tmp/gal-artemis.XXXXXX)
	( cd "$ROOT/artemis" && SDL_VIDEODRIVER=dummy ./build/agame tests/sample-scene.ast \
		--frames "$frames" >/tmp/gal_artemis.log 2>&1 ) || ok=0
	n=$(find "$frames" -maxdepth 1 -type f -name 'frame*.bmp' | wc -l | tr -d ' ')
	rm -rf "$frames"
	[ "$n" = 12 ] || ok=0
	if [ "$ok" = 1 ]; then pass artemis "pfs/ast/runtime 自检 + 12 帧渲染"; else fail artemis "见 /tmp/gal_artemis.log"; fi
fi

echo
echo "通过 ${ok_count}，失败 ${bad_count}，跳过 ${skip_count}"
if [ "$bad_count" != 0 ]; then
	echo "失败项:${failed}"
	exit 1
fi
if [ "$skip_count" != 0 ] && [ $CHECK_ONLY = 0 ]; then
	echo "跳过项需要游戏数据：见 docs/DATA.md（--data DIR 可指向别处）"
fi
exit 0
