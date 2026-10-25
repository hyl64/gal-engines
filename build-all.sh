#!/bin/bash
# build-all.sh - build every engine in this tree and check it
#
# Each engine has its own way of being built (CMake for three of them, a
# hand-written Makefile for OpenBGI) and its own way of saying it works.  This
# script runs all of them in turn and prints one line per engine, so a fresh
# checkout can be brought up with one command and its state read at a glance.
#
#   ./build-all.sh            build everything, then check
#   ./build-all.sh --check    only run the checks

set -u
cd "$(dirname "$0")"
ROOT=$(pwd)
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

ok_count=0
bad_count=0

report() { # name, ok, detail
	if [ "$2" = "1" ]; then
		printf "  \033[32m✓\033[0m %-14s %s\n" "$1" "$3"
		ok_count=$((ok_count + 1))
	else
		printf "  \033[31m✗\033[0m %-14s %s\n" "$1" "$3"
		bad_count=$((bad_count + 1))
	fi
}

build_cmake() { # dir, name
	( cd "$ROOT/$1" && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
		&& cmake --build build -j2 >/dev/null 2>&1 )
}

echo "构建"
if [ $CHECK_ONLY = 0 ]; then
	# A stale executable is not evidence of a successful build.  Preserve logs
	# so a configuration or compiler failure remains diagnosable.
	build_cmake OnscripterYuri >/tmp/gal_build_ns.log 2>&1; ns_built=$?
	report "nscripter" $([ "$ns_built" = 0 ] && [ -x OnscripterYuri/build/onsyuri ] && echo 1 || echo 0) "build/onsyuri (log: /tmp/gal_build_ns.log)"
	( cd "$ROOT/openbgi/openbgi-src" && make PLATFORM=darwin -j2 >/tmp/gal_build_bgi.log 2>&1 ); bgi_built=$?
	report "bgi" $([ "$bgi_built" = 0 ] && [ -x openbgi/openbgi-src/bin/bgi ] && echo 1 || echo 0) "bin/bgi (log: /tmp/gal_build_bgi.log)"
	build_cmake kirikiroid2-mac >/tmp/gal_build_kr.log 2>&1; kr_built=$?
	report "kirikiri" $([ "$kr_built" = 0 ] && [ -x kirikiroid2-mac/build/krkrrun ] && echo 1 || echo 0) "build/krkrrun (log: /tmp/gal_build_kr.log)"
	build_cmake artemis >/tmp/gal_build_artemis.log 2>&1; artemis_built=$?
	report "artemis" $([ "$artemis_built" = 0 ] && [ -x artemis/build/agame ] && echo 1 || echo 0) "build/agame (log: /tmp/gal_build_artemis.log)"
else
	echo "  (跳过，--check)"
fi

echo "自检"

# A check that fails may have been starved of CPU or killed by the host (a
# SIGKILL shows up as an abrupt end, not a wrong answer), so a failing run gets
# one more chance before it counts against the build.
retry_once() {
	local fn="$1"
	"$fn" && return 0
	sleep 2
	"$fn"
}

# Never kill unrelated game processes by name.  Each check owns exactly the
# process it starts, and waits for it so a failed check cannot leak it.
check_nscripter() {
	( cd testdata/lifegame && exec env SDL_VIDEODRIVER=dummy \
		"$ROOT/OnscripterYuri/build/onsyuri" --root . --enc:sjis >/dev/null 2>&1 ) &
	local pid=$!
	sleep 3
	local alive=0
	if kill -0 "$pid" 2>/dev/null; then alive=1; fi
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	[ "$alive" = "1" ]
}
ns=0
if retry_once check_nscripter; then ns=1; fi
report "nscripter" $ns "official demo still running after 3s"

# KAG3 must both load its scenario and finish without a script exception.
kr=0
if ( cd testdata/kag3-sample && KRKR_LOOP_SECONDS=2 SDL_VIDEODRIVER=dummy \
	"$ROOT/kirikiroid2-mac/build/krkrrun" startup.tjs >/tmp/gal_kr.log 2>&1 ) \
	&& grep -q "Scenario loaded : first.ks" /tmp/gal_kr.log \
	&& grep -q "こんにちは。" /tmp/gal_kr.log \
	&& ! grep -q "エラーが発生しました\|Script exception raised\|krkrrun:" /tmp/gal_kr.log; then kr=1; fi
report "kirikiri" $kr "KAG3 displays its first line without script errors"

# The real KAG project must walk its whole chapter - title, click, messages,
# BGM on/off, [playse][ws] - both from the folder and from a packed data.xp3.
# This is the regression that caught the click dead-end and the invalidated
# objects; keep it in the acceptance, not only in notes.
check_story() {
	"$ROOT/kirikiroid2-mac/tests/run-kag-story.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_story.log 2>&1
}
krs=0
if retry_once check_story; then krs=1; fi
report "kirikiri-story" $krs "the real KAG chapter plays through, folder and archive"

# The second real game: the official KiriKiri SDL2 / KAG3 demo.  Different
# resolution, JPEG + TLG assets, crossfades, a TJS snow plugin on a Timer and
# a hyperlink menu - a build that breaks one project rarely breaks both.
check_demo() {
	"$ROOT/kirikiroid2-mac/tests/run-demo-game.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_demo.log 2>&1
}
krd=0
if retry_once check_demo; then krd=1; fi
report "kirikiri-demo" $krd "the official KAG3 demo boots, renders and takes clicks"

# Saving is what makes a game a game: the bookmark path is a thumbnail image
# followed by a structured write at an offset, and that offset write used to
# fail with "Write error" on every save.
check_save() {
	"$ROOT/kirikiroid2-mac/tests/run-save-check.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_save.log 2>&1
}
krsv=0
if retry_once check_save; then krsv=1; fi
report "kirikiri-save" $krsv "bookmark save: thumbnail, offset struct, read-back"

# Movies: KAG plays openings and cutscenes through VideoOverlay, which used to
# be a state recorder.  A two-colour clip proves it decodes and advances.
check_video() {
	"$ROOT/kirikiroid2-mac/tests/run-video-check.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_video.log 2>&1
}
krv=0
if retry_once check_video; then krv=1; fi
report "kirikiri-video" $krv "a movie decodes, paints and advances"

# The sound contract a KAG game depends on: pause, status, [ws] waiting for a
# clip to end, a sound effect and a BGM together, per-buffer volume, fades.
kra=0
check_audio() {
	"$ROOT/kirikiroid2-mac/tests/run-audio-check.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_audio.log 2>&1
}
if retry_once check_audio; then kra=1; fi
report "kirikiri-audio" $kra "the audio contract holds (pause, [ws], mixing, fades)"

# Frame pacing: a flat sleep on top of the work cost ~15% of the frames before
# the loop was put on a clock.  Both projects must stay playable.
krp=0
check_perf() {
	"$ROOT/kirikiroid2-mac/tests/run-perf-check.sh" \
		"$ROOT/kirikiroid2-mac/build/krkrrun" >/tmp/gal_kr_perf.log 2>&1
}
if retry_once check_perf; then krp=1; fi
report "kirikiri-perf" $krp "both projects keep a playable frame rate"

# The trial must reach its known engine profile, not merely leave a process alive.
# Do not pkill an unrelated game: if it holds the engine lock, report the failure.
( cd testdata/bgi-trial && exec env SDL_VIDEODRIVER=dummy \
	"$ROOT/openbgi/openbgi-src/bin/bgi" >/tmp/gal_bg.log 2>&1 ) &
bg_pid=$!
bg=0
for unused in 1 2 3 4 5; do
	sleep 5
	if ! kill -0 "$bg_pid" 2>/dev/null; then break; fi
	if grep -q "profile 1.669" /tmp/gal_bg.log; then bg=1; break; fi
done
if kill -0 "$bg_pid" 2>/dev/null; then kill -9 "$bg_pid" 2>/dev/null; fi
wait "$bg_pid" 2>/dev/null
report "bgi" $bg "the trial reaches profile 1.669"

# Artemis: its own self tests, one per layer
ok=1
for t in "pfstool selftest" "asttool selftest" "arun --selftest"; do
	( cd "$ROOT/artemis" && ./build/$t >/dev/null 2>&1 ) || ok=0
done
frames_dir=$(mktemp -d /tmp/gal-artemis.XXXXXX)
if ! ( cd "$ROOT/artemis" && SDL_VIDEODRIVER=dummy ./build/agame tests/sample-scene.ast \
	--frames "$frames_dir" >/tmp/gal_artemis.log 2>&1 ); then ok=0; fi
frame_count=$(find "$frames_dir" -maxdepth 1 -type f -name 'frame*.bmp' | wc -l | tr -d ' ')
if [ "$frame_count" != 12 ]; then ok=0; fi
report "artemis" $ok "pfs, ast, runtime and 12 rendered fixture frames (not a real game)"

echo
echo "通过 ${ok_count}，失败 ${bad_count}"
[ "$bad_count" = 0 ]
