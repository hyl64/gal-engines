#!/bin/bash
# bench/artemis/bench.sh - every Artemis efficiency number, from one command
#
#   ./bench/artemis/bench.sh                 full run, tag = timestamp
#   TAG=before ./bench/artemis/bench.sh      name the run, for before/after tables
#   RUNS=7 ./bench/artemis/bench.sh          repetitions per case (default 7)
#   QUICK=1 ./bench/artemis/bench.sh         skip the two slow python extractions
#   ./bench/artemis/bench.sh --baseline      also write bench/artemis/baseline.json
#   ./bench/artemis/bench.sh --check         compare against baseline.json, exit 1
#                                            if any timing regressed > --threshold
#                                            percent (default 20)
#
# What it does
#   1. snapshots the machine and, more importantly, *which build* it is measuring
#      (every binary's mtime + sha, artemis/ HEAD, dirty file count)
#   2. builds two bench harnesses against the current libartemis.a - it never
#      builds artemis/ itself, because three other people are editing it
#   3. measures the shipped tools (pfstool, agame, sips, the python reference)
#      N times each, wall clock + peak RSS, via measure.py
#   4. measures the library in-process (open, random read, sequential extract,
#      single member, image decode) via pfsbench/pngbench, which link the same
#      libartemis.a the tools do
#   5. writes out/<tag>/{raw.tsv,env.txt,report.txt,results.csv,results.json,tables.md}
#      and refreshes out/{results.csv,results.json,report.txt} with the latest run
#
# Real data is read-only: everything the bench writes goes to out/ and $SCRATCH.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}
RUNS=${RUNS:-7}
PRUNES=${PRUNES:-5}          # python reference: 5 runs is plenty, it is 10x slower
SCRATCH=${SCRATCH:-/tmp/artemis-bench}
OUT=$HERE/out/$TAG

PF6=$ROOT/testdata/artemis-nicolan/root.pfs
PF8=$ROOT/testdata/artemis-henpri-trial/raw/HENPRI.pfs.060
RASPBERRY_INDEX=$ROOT/testdata/artemis-raspberry-web/archive_index.txt
PYREF=$ROOT/testdata/artemis-tools/pfs_extract.py
PYMIN=$ROOT/testdata/artemis-tools/pfs_min.py
BUILD=$ROOT/artemis/build
FIXTURE_AST=$ROOT/artemis/tests/sample-scene.ast
NICOLAN=$ROOT/testdata/artemis-nicolan/extracted
SCENE=$NICOLAN/scenario/scene.iet
HENPRI=$ROOT/testdata/artemis-henpri-trial/extracted
AST_CORPUS=$HENPRI/script
ONE_AST=$(ls "$AST_CORPUS"/*.ast 2>/dev/null | head -1)
ONE_OGG=$NICOLAN/bgm/kujira.ogg
SAVE_DIR=$ROOT/artemis/src/save
SAVETOOL=$BUILD/savetool

MODE=run
THRESHOLD=20
for arg in "$@"; do
	case "$arg" in
	--baseline) MODE=baseline ;;
	--check) MODE=check ;;
	--threshold=*) THRESHOLD="${arg#--threshold=}" ;;
	*) echo "bench.sh: unknown option $arg" >&2; exit 2 ;;
	esac
done

mkdir -p "$OUT" "$SCRATCH"
RAW=$OUT/raw.tsv
: >"$RAW"

# a teammate may be rebuilding artemis/ right now.  Measuring across a relink
# would put two builds in one table, so wait until the binaries have held still
# for a few seconds first - and say so if the wait was long.
build_signature() {
	shasum -a 256 "$ROOT"/artemis/build/{pfstool,iettool,asttool,asbtool,agame,arun,pfsgame,libartemis.a} 2>/dev/null | awk '{print $1}'
}
WAITED=0
PREV=$(build_signature)
while :; do
	sleep 5
	WAITED=$((WAITED + 5))
	CUR=$(build_signature)
	[ "$CUR" = "$PREV" ] && break
	PREV=$CUR
	if [ "$WAITED" -ge 180 ]; then
		echo "bench.sh: artemis/build keeps changing; measuring anyway after ${WAITED}s" >&2
		break
	fi
done
[ "$WAITED" -gt 5 ] && echo "bench.sh: waited ${WAITED}s for a teammate's rebuild to land"
FAILED=()

say() { printf '%s\n' "$*"; }
step() { printf '\n=== %s\n' "$*"; }
record_fail() { FAILED+=("$*"); printf 'META\t@env\tfailed_case\t%s\n' "$*" >>"$RAW"; }

# ---------------------------------------------------------------- 1. the build
step "snapshot: machine, build, data"

{
	echo "tag              $TAG"
	echo "when             $(date '+%Y-%m-%dT%H:%M:%S%z')"
	echo "host             $(hostname)"
	echo "cpu              $(sysctl -n machdep.cpu.brand_string)"
	echo "cores            $(sysctl -n hw.ncpu)"
	echo "memory_bytes     $(sysctl -n hw.memsize)"
	echo "os               $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
	echo "kernel           $(uname -r) $(uname -m)"
	echo "page_size        $(getconf PAGESIZE)"
	echo "loadavg_at_start $(sysctl -n vm.loadavg)"
	echo "compiler         $(clang++ --version | head -1)"
	echo "libpng           $(pkg-config --modversion libpng)"
	echo "python           $(python3 -VV 2>&1 | head -1)"
	echo "numpy            $(python3 -c 'import numpy;print(numpy.__version__)' 2>/dev/null || echo absent)"
	echo "sips             $(which sips)"
	echo "repo_head        $(cd "$ROOT" && git rev-parse HEAD 2>/dev/null)"
	echo "artemis_head     $(cd "$ROOT/artemis" && git rev-parse HEAD 2>/dev/null)"
	echo "artemis_dirty    $(cd "$ROOT/artemis" && git status --porcelain 2>/dev/null | wc -l | tr -d ' ') file(s) modified or untracked"
	echo "data_pf6         $PF6 $(stat -f '%z bytes, mtime %Sm' -t '%Y-%m-%dT%H:%M:%S' "$PF6" 2>/dev/null)"
	echo "data_pf8         $PF8 $(stat -f '%z bytes, mtime %Sm' -t '%Y-%m-%dT%H:%M:%S' "$PF8" 2>/dev/null)"
	echo "data_raspberry   $RASPBERRY_INDEX $(stat -f '%z bytes' "$RASPBERRY_INDEX" 2>/dev/null)"
	echo "--- binaries under measurement ---"
	for b in pfstool iettool asttool asbtool agame arun pfsgame; do
		if [ -x "$BUILD/$b" ]; then
			printf '%-10s %s  %s bytes  sha256:%s\n' "$b" \
				"$(stat -f '%Sm' -t '%Y-%m-%dT%H:%M:%S' "$BUILD/$b")" \
				"$(stat -f '%z' "$BUILD/$b")" \
				"$(shasum -a 256 "$BUILD/$b" | cut -c1-12)"
		fi
	done
	printf '%-10s %s  %s bytes  sha256:%s\n' "libartemis" \
		"$(stat -f '%Sm' -t '%Y-%m-%dT%H:%M:%S' "$BUILD/libartemis.a")" \
		"$(stat -f '%z' "$BUILD/libartemis.a")" \
		"$(shasum -a 256 "$BUILD/libartemis.a" | cut -c1-12)"
	echo "--- data inventory ---"
	echo "pf6_entries      $("$BUILD/pfstool" list "$PF6" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
	echo "pf8_entries      $("$BUILD/pfstool" list "$PF8" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
} >"$OUT/env.txt" 2>&1

while IFS= read -r line; do
	key=${line%%  *}
	val=${line#*  }
	while [ "${val:0:1}" = " " ]; do val=${val# }; done
	printf 'META\t@env\t%s\t%s\n' "$(printf '%s' "$key" | tr -s ' ' '_')" "$val" >>"$RAW"
done <"$OUT/env.txt"

cat "$OUT/env.txt"

# the binaries must not change under us: a rebuild by a teammate mid-run would
# silently mix two builds in one table
BEFORE=$(shasum -a 256 "$BUILD"/pfstool "$BUILD"/iettool "$BUILD"/agame "$BUILD"/libartemis.a 2>/dev/null)

# --------------------------------------------------------- 2. bench harnesses
step "building bench harnesses (linking the current libartemis.a; artemis/ is not rebuilt)"

PNG_CFLAGS=$(pkg-config --cflags libpng)
PNG_LIBS=$(pkg-config --libs libpng)
CXXFLAGS="-O2 -std=c++17 -I $ROOT/artemis/src $PNG_CFLAGS"
LINK="$BUILD/libartemis.a $PNG_LIBS -framework CoreFoundation"

if ! ( cd "$HERE" && clang++ $CXXFLAGS src/pfsbench.cpp $LINK -o "$OUT/pfsbench" ); then
	echo "bench.sh: cannot build pfsbench" >&2
	exit 1
fi
if ! ( cd "$HERE" && clang++ $CXXFLAGS src/pngbench.cpp $LINK -o "$OUT/pngbench" ); then
	echo "bench.sh: cannot build pngbench" >&2
	exit 1
fi
if ! ( cd "$HERE" && clang++ -O2 -std=c++17 src/applykey-check.cpp -o "$OUT/applykey-check" ); then
	echo "bench.sh: cannot build applykey-check" >&2
	exit 1
fi
PFSBENCH=$OUT/pfsbench
PNGBENCH=$OUT/pngbench
AKCHECK=$OUT/applykey-check
say "pfsbench + pngbench built against libartemis.a $(stat -f '%Sm' -t '%H:%M:%S' "$BUILD/libartemis.a")"

M() { python3 "$HERE/measure.py" "$@" >>"$RAW"; }

step "preflight: which layers exist and answer right now"

# A layer that is being written by someone else may be absent, or present and
# broken.  Record that as a fact with a version, and skip its cases - a benchmark
# that cannot tell "not built yet" from "zero" is worse than no benchmark.
LAYER_READY() { [ -x "$1" ]; }
for tool in pfstool iettool asttool asbtool agame fxtool audiotool arun pfsgame savetool; do
	if LAYER_READY "$BUILD/$tool"; then
		printf 'META\t@env\tlayer_%s\tready (%s)\n' "$tool" \
			"$(stat -f '%Sm' -t '%H:%M:%S' "$BUILD/$tool")" >>"$RAW"
	else
		printf 'META\t@env\tlayer_%s\tabsent\n' "$tool" >>"$RAW"
		say "  $tool: absent"
	fi
done
if [ -f "$AST_CORPUS"/d01_c01_2.ast ] || [ -n "$(ls "$AST_CORPUS"/*.ast 2>/dev/null | head -1)" ]; then
	printf 'META\t@env\tast_corpus\t%s file(s)\n' "$(ls "$AST_CORPUS"/*.ast 2>/dev/null | wc -l | tr -d ' ')" >>"$RAW"
fi
# save/ is still being built: say so rather than reporting a zero
if [ -d "$SAVE_DIR" ] || LAYER_READY "$SAVETOOL"; then
	printf 'META\t@env\tsave_layer\tpresent\n' >>"$RAW"
else
	say "  save layer: 未就绪 (no artemis/src/save/, no savetool) - recorded, not measured"
	printf 'META\t@env\tsave_layer\tnot ready: no src/save/ and no savetool\n' >>"$RAW"
fi

step "container: first touch of the 202 MB archive, before anything else reads it"
say "  the file is probably already in the unified buffer cache from earlier"
say "  activity; an I/O-cold number would need 'sudo purge', which this does not do"
M --case pfsbench_rand_page_pf8 --runs "$RUNS" --stat rand_page_ms -- "$PFSBENCH" rand "$PF8" 1000 --mode page

# ------------------------------------------------------------- 3. the cases
step "proof from the OS itself: what mapping a 202 MB archive did to the process"

# pfsbench hold opens the archive and idles, so vmmap has something to look at:
# a tool that finishes in 10 ms is impossible to sample, which is why the
# in-process mincore numbers above exist at all.  This is the same claim from
# the other side, in Apple's own words.
"$PFSBENCH" hold "$PF8" 5 >"$OUT/hold.txt" 2>&1 &
HOLD_JOB=$!
sleep 2
HOLD_PID=$(awk -F'\t' '$1=="HOLDING"{print $2}' "$OUT/hold.txt")
if [ -n "${HOLD_PID:-}" ]; then
	vmmap "$HOLD_PID" >"$OUT/vmmap.txt" 2>&1
	vmmap -summary "$HOLD_PID" >>"$OUT/vmmap.txt" 2>&1
fi
wait $HOLD_JOB 2>/dev/null

if [ -s "$OUT/vmmap.txt" ]; then
	MAPPED=$(grep -m1 'HENPRI.pfs.060' "$OUT/vmmap.txt")
	BRACKET=$(printf '%s' "$MAPPED" | sed -n 's/.*\[\([^]]*\)\].*/\1/p' | tr -s ' ' '\n')
	VIRT=$(printf '%s' "$BRACKET" | sed -n '1p')
	RES=$(printf '%s' "$BRACKET" | sed -n '2p')
	DIRTY=$(printf '%s' "$BRACKET" | sed -n '3p')
	FOOT=$(sed -n 's/^Physical footprint: *\([0-9]*\)K.*/\1/p' "$OUT/vmmap.txt" | head -1)
	say "  vmmap: $MAPPED"
	say "  virtual $VIRT, resident $RES, dirty $DIRTY, physical footprint ${FOOT:-?}K"
	# the bracket is virtual/resident/dirty/swapped.  It is quoted, not scored:
	# vmmap counts pages faulted into *this* task, and a freshly mapped archive
	# legitimately reads 0K even when the page cache holds all of it, so a single
	# KiB row in a table would be more confusing than the two observations that
	# matter (this line, and mincore's count of the same mapping).
	say "  vmmap bracket  [virtual $VIRT | resident $RES | dirty $DIRTY]"
	printf 'META\t@env\tvmmap_pf8_bracket\tvirtual %s / resident %s / dirty %s\n' \
		"$VIRT" "$RES" "$DIRTY" >>"$RAW"
	printf 'META\t@env\tvmmap_pf8_footprint_kib\t%s\n' "${FOOT:-?}" >>"$RAW"
	printf 'META\t@env\tvmmap_pf8\t%s\n' "$(printf '%s' "$MAPPED" | tr -s ' ' | cut -c1-160)" >>"$RAW"
fi

step "container: open, list, info (real data, $RUNS runs each)"

M --case pfstool_usage --runs "$RUNS" --allow-exit 2 --discard -- "$BUILD/pfstool"
M --case pfstool_list_pf6 --runs "$RUNS" -- "$BUILD/pfstool" list "$PF6"
M --case pfstool_list_pf8 --runs "$RUNS" -- "$BUILD/pfstool" list "$PF8"
M --case pfstool_info_pf6 --runs "$RUNS" -- "$BUILD/pfstool" info "$PF6"
M --case pfstool_info_pf8 --runs "$RUNS" -- "$BUILD/pfstool" info "$PF8"

step "container: list --types (this is the one that sniffs every member)"

M --case pfstool_list_types_pf6 --runs "$RUNS" -- "$BUILD/pfstool" list --types "$PF6"
M --case pfstool_list_types_pf8 --runs "$RUNS" -- "$BUILD/pfstool" list --types "$PF8"

step "container: single member out of a 44 MB and a 202 MB archive"

M --case pfstool_cat_pf6_ttf --runs "$RUNS" --discard -- "$BUILD/pfstool" cat "$PF6" 'GenShinGothic-P-Medium.ttf'
M --case pfstool_cat_pf8_png --runs "$RUNS" --discard -- "$BUILD/pfstool" cat "$PF8" 'image\cg\SYUURAKU.png'
M --case pfstool_cat_pf8_ast --runs "$RUNS" --discard -- "$BUILD/pfstool" cat "$PF8" 'script\d01_c08_3.ast'

step "container: whole-volume extract to disk (pf6 plaintext vs pf8 XOR)"

rm -rf "$SCRATCH/ex6" "$SCRATCH/ex8"
M --case pfstool_extract_pf6 --runs "$RUNS" --pre "rm -rf $SCRATCH/ex6" -- "$BUILD/pfstool" extract "$PF6" "$SCRATCH/ex6"
M --case pfstool_extract_pf8 --runs "$RUNS" --pre "rm -rf $SCRATCH/ex8" -- "$BUILD/pfstool" extract "$PF8" "$SCRATCH/ex8"

step "script layer: real .ast parse, one file and the whole corpus"

M --case asttool_dump --runs "$RUNS" --discard -- "$BUILD/asttool" dump "$ONE_AST"
M --case agame_astinfo_corpus --runs "$RUNS" --discard -- "$BUILD/agame" --ast-info "$AST_CORPUS"

step "library in-process: random access, 1000 picks"

M --case pfsbench_rand_pf8 --runs "$RUNS" --stat rand_content_ms -- "$PFSBENCH" rand "$PF8" 1000 --mode content
M --case pfsbench_rand_pf6 --runs "$RUNS" --stat rand_content_ms -- "$PFSBENCH" rand "$PF6" 1000 --mode content

step "library in-process: Open()"

M --case pfsbench_open_pf6 --runs "$RUNS" --stat open_warm_ms -- "$PFSBENCH" open "$PF6" 20
M --case pfsbench_open_pf8 --runs "$RUNS" --stat open_warm_ms -- "$PFSBENCH" open "$PF8" 20

step "library in-process: sequential extract with no disk at all"

M --case pfsbench_seq_pf6 --runs "$RUNS" --stat seq_total_ms -- "$PFSBENCH" seq "$PF6" --out devnull
M --case pfsbench_seq_pf8 --runs "$RUNS" --stat seq_total_ms -- "$PFSBENCH" seq "$PF8" --out devnull

step "library in-process: name lookup, the miss that costs"

M --case pfsbench_find_pf8 --runs "$RUNS" --stat find_miss_us -- "$PFSBENCH" find "$PF8" 'image\cg\SYUURAKU.png' 1000
M --case pfsbench_find_pf6 --runs "$RUNS" --stat find_miss_us -- "$PFSBENCH" find "$PF6" 'backlog.iet' 1000

step "library in-process: one member, warm"

M --case pfsbench_one_pf6_ttf --runs "$RUNS" --stat one_ms -- "$PFSBENCH" one "$PF6" 'GenShinGothic-P-Medium.ttf' 10
M --case pfsbench_one_pf6_script --runs "$RUNS" --stat one_ms -- "$PFSBENCH" one "$PF6" 'backlog.iet' 50
M --case pfsbench_one_pf6_png --runs "$RUNS" --stat one_ms -- "$PFSBENCH" one "$PF6" 'bgimage\utyu2.png' 20
M --case pfsbench_one_pf8_png --runs "$RUNS" --stat one_ms -- "$PFSBENCH" one "$PF8" 'image\cg\SYUURAKU.png' 10
M --case pfsbench_one_pf8_ast --runs "$RUNS" --stat one_ms -- "$PFSBENCH" one "$PF8" 'script\d01_c08_3.ast' 50

step "library in-process: sniffing every member, today vs bounded sample"

M --case pfsbench_types_pf8 --runs "$RUNS" --stat types_content_ms -- "$PFSBENCH" types "$PF8"
M --case pfsbench_types_fix_pf8 --runs "$RUNS" --stat types_fixed_ms -- "$PFSBENCH" types "$PF8" --fix
M --case pfsbench_types_pf6 --runs "$RUNS" --stat types_content_ms -- "$PFSBENCH" types "$PF6"

step "reference: the XOR and memcpy ceilings this CPU can do"

"$PFSBENCH" memref 67108864 >"$OUT/memref.txt" 2>&1
awk -F'\t' '$1=="STAT"{printf "CLI\tmemref_%s\t1\t0\t%.4f\t0\t0\t0\n",$2,$3}' \
	"$OUT/memref.txt" >>"$RAW"
sed -n 's/^STAT\t//p' "$OUT/memref.txt" | awk -F'\t' '{printf "  %-24s %10.1f %s\n",$1,$2,$3}'

step "reference: what ExtractTo's per-entry 256 KiB buffer costs"

"$PFSBENCH" alloc 1682 262144 >"$OUT/alloc.txt" 2>&1
awk -F'\t' '$1=="STAT"{printf "CLI\talloc_%s\t1\t0\t%.4f\t0\t0\t0\n",$2,$3}' \
	"$OUT/alloc.txt" >>"$RAW"
sed -n 's/^STAT\t//p' "$OUT/alloc.txt" | awk -F'\t' '{printf "  %-24s %10.3f %s\n",$1,$2,$3}'

step "reference: the proposed ApplyKey rewrite - same bytes, and how much faster"

"$AKCHECK" >"$OUT/applykey.txt" 2>&1
awk -F'\t' '$1=="STAT"{printf "CLI\t%s\t1\t0\t%.4f\t0\t0\t0\n",$2,$3}' \
	"$OUT/applykey.txt" >>"$RAW"
awk -F'\t' '$1=="verify"{printf "META\t@env\tapplykey_verify\t%s case(s), %s mismatch(es)\n",$2,$3}' \
	"$OUT/applykey.txt" >>"$RAW"
sed -n 's/^STAT\t//p' "$OUT/applykey.txt" | awk -F'\t' '{printf "  %-26s %10.1f %s\n",$1,$2,$3}'
grep -E "^verify" "$OUT/applykey.txt" | sed 's/^/  /'

step "reference: python pfs_extract.py on the same data"

# pfs_extract.py reads its parser from /tmp/pfs_min.py; the tree ships it beside
# the script, so put it where the reference expects to find it.
cp "$PYMIN" /tmp/pfs_min.py 2>/dev/null
mkdir -p "$SCRATCH/py6" "$SCRATCH/py8"
if [ -z "${QUICK:-}" ]; then
	M --case python_extract_pf6 --runs "$PRUNES" --pre "rm -rf $SCRATCH/py6/*" -- \
		python3 "$PYREF" "$PF6" "$SCRATCH/py6/out"
	if python3 -c 'import numpy' 2>/dev/null; then
		M --case python_extract_pf8 --runs "$PRUNES" --pre "rm -rf $SCRATCH/py8/*" -- \
			python3 "$PYREF" "$PF8" "$SCRATCH/py8/out"
	else
		record_fail "python_extract_pf8(no numpy)"
	fi
else
	say "  (QUICK=1: python reference skipped)"
fi

# ------------------------------------------------------------- 4. image layer
step "image: 73 real PNGs through src/pngio.cpp, and the system's own pipeline"

rm -rf "$SCRATCH/png6"
"$BUILD/pfstool" extract "$PF6" "$SCRATCH/png6" >/dev/null 2>&1
find "$SCRATCH/png6" -name '*.png' | sort >"$OUT/pngs.list"
PNG_COUNT=$(wc -l <"$OUT/pngs.list" | tr -d ' ')
say "  $PNG_COUNT PNG(s) extracted from pf6"

if [ "$PNG_COUNT" -gt 0 ]; then
	M --case pngbench_pf6_file --runs "$RUNS" --stat png_pass_best_ms -- \
		"$PNGBENCH" --list "$OUT/pngs.list" --repeat 3 --io file
	M --case pngbench_pf6_memory --runs "$RUNS" --stat png_pass_best_ms -- \
		"$PNGBENCH" --list "$OUT/pngs.list" --repeat 3 --io memory
	# one sips process for all 73: per-file invocations would measure fork+exec
	SIPS_FILES=$(tr '\n' ' ' <"$OUT/pngs.list")
	M --case sips_pf6_pngs --runs "$PRUNES" --pre "rm -rf $SCRATCH/sips; mkdir -p $SCRATCH/sips" --discard -- \
		sips -s format bmp $SIPS_FILES --out "$SCRATCH/sips"
	# decode dominates this one: the BMP written back is 64 px wide
	M --case sips_pf6_tiny --runs "$PRUNES" --pre "rm -rf $SCRATCH/sipst; mkdir -p $SCRATCH/sipst" --discard -- \
		sips -s format bmp --resampleWidth 64 $SIPS_FILES --out "$SCRATCH/sipst"
fi

# ------------------------------------------------------------ 5. first frame
step "runtime: first frame on screen (agame headless, fixture scenario)"

rm -rf "$SCRATCH/frames"
python3 "$HERE/firstframe.py" --case agame_first_frame --runs "${FFRUNS:-5}" \
	--frames "$SCRATCH/frames" -- "$BUILD/agame" "$FIXTURE_AST" --frames "$SCRATCH/frames" >>"$RAW"

step "runtime: the real first screen - from the archive, versus from the unpacked folder"

# The product claim is "play a shipped game without unpacking it".  Same scene,
# same engine, two data sources, one command apart.  The frames each path writes
# are compared byte for byte, so the timing is not comparing two different jobs.
if [ -f "$SCENE" ]; then
	python3 "$HERE/firstframe.py" --case agame_real_archive --runs "${FFRUNS:-5}" \
		--frames "$SCRATCH/frames-arch" -- \
		"$BUILD/agame" --script "$SCENE" --archive "$PF6" \
		--frames "$SCRATCH/frames-arch" --max-pages 4 >>"$RAW"
	python3 "$HERE/firstframe.py" --case agame_real_dir --runs "${FFRUNS:-5}" \
		--frames "$SCRATCH/frames-dir" -- \
		"$BUILD/agame" --script "$SCENE" --data-dir "$NICOLAN" \
		--frames "$SCRATCH/frames-dir" --max-pages 4 >>"$RAW"
	if diff -r "$SCRATCH/frames-arch" "$SCRATCH/frames-dir" >/dev/null 2>&1; then
		say "  archive path and unpacked path wrote identical frames"
		printf 'META\t@env\tframe_parity\tidentical\n' >>"$RAW"
	else
		say "  !! the two paths disagree on the frames"
		printf 'META\t@env\tframe_parity\tdifferent\n' >>"$RAW"
	fi
	# the timeline of every frame each path produced, once, for the record
	for tag in arch dir; do
		say "  $tag timeline: $(awk -F'\t' -v c="agame_real_$([ $tag = arch ] && echo archive || echo dir)" \
			'$1=="META" && $2==c && $3=="timeline_ms"{print $4}' "$RAW" | head -1)"
	done
else
	say "  (no unpacked scenario at $SCENE; real-path cases skipped)"
	record_fail "agame_real_archive(no scene.iet)"
fi

step "runtime: what capturing a real frame costs, with rendering taken out"

# The synthetic gradient below encodes ~10x faster than a photograph, so the
# honest number comes from re-encoding a frame this very run produced: same
# pixels, same settings, byte-identical output (verified).  --roundtrip does that.
REALFRAME=$SCRATCH/frames-arch/page004.png
if [ -f "$REALFRAME" ]; then
	M --case capture_real_frame --runs "$RUNS" --stat capture_writepng_ms -- \
		"$PNGBENCH" --roundtrip "$REALFRAME" --repeat 3
	M --case capture_real_count --runs "$RUNS" --stat capture_countcolors_ms -- \
		"$PNGBENCH" --roundtrip "$REALFRAME" --repeat 3
fi

# A frame is 1024x698 (measured from the real first screen).  Encoding it and
# counting its colours are two pieces of work that are not rendering but sit in
# the per-frame budget until they are moved out; both are measured here, at that
# exact size, so the tool's per-frame cost can be split instead of guessed at.
M --case capture_writepng --runs "$RUNS" --stat capture_writepng_ms -- \
	"$PNGBENCH" --write 1024x698 --repeat 5 "$SCRATCH/capture.png"
M --case capture_countcolors --runs "$RUNS" --stat capture_countcolors_ms -- \
	"$PNGBENCH" --write 1024x698 --repeat 5 "$SCRATCH/capture.png"

# a real script would be the better scenario; record whether it parses today
rm -rf "$SCRATCH/realscript"
mkdir -p "$SCRATCH/realscript" "$SCRATCH/realframes"
if "$BUILD/pfstool" cat "$PF8" 'script\d01_c08_3.ast' >"$SCRATCH/realscript/d01.ast" 2>/dev/null; then
	if SDL_VIDEODRIVER=dummy "$BUILD/agame" "$SCRATCH/realscript/d01.ast" --frames "$SCRATCH/realframes" \
		>"$OUT/agame-real-ast.log" 2>&1; then
		say "  real pf8 script: played"
		printf 'META\t@env\treal_ast_first_frame\tplayed\n' >>"$RAW"
	else
		REASON=$(tail -1 "$OUT/agame-real-ast.log" | cut -c1-120)
		say "  real pf8 script: refused ($REASON)"
		printf 'META\t@env\treal_ast_first_frame\trefused: %s\n' "$REASON" >>"$RAW"
	fi
fi

step "audio layer: decode, mix, A-B loop on real material"

# The tool's own wall clock includes process start and opening the file; the
# per-frame question is what the layer itself costs, which these tools print.
I() { python3 "$HERE/internal.py" "$@" >>"$RAW"; }

# 3.4 MB / 3:02 OGG from the shipped game.  probe decodes a window, play runs the
# whole mixing path (Engine::Mix is the callback), sli reads a real .sli A-B file.
if [ -f "$ONE_OGG" ]; then
	M --case audiotool_probe --runs "$RUNS" --discard -- "$BUILD/audiotool" probe "$ONE_OGG" --seconds 10
	M --case audiotool_play --runs "$RUNS" --discard -- "$BUILD/audiotool" play "$ONE_OGG" --seconds 3
	I --case audiotool_probe_internal --runs "$RUNS" --scale 1000 \
		--regex 'decode time:\s+([0-9.]+) s' --unit ms -- \
		"$BUILD/audiotool" probe "$ONE_OGG" --seconds 10
	I --case audiotool_play_internal --runs "$RUNS" --scale 1000 \
		--regex 'rendered:.*in ([0-9.]+) s wall' --unit ms -- \
		"$BUILD/audiotool" play "$ONE_OGG" --seconds 3
else
	record_fail "audiotool(no ogg)"
fi
SLI_MEMBER=$( "$BUILD/pfstool" list "$PF8" 2>/dev/null | awk '$3 ~ /\.sli$/ {print $3}' | head -1 )
if [ -n "$SLI_MEMBER" ]; then
	mkdir -p "$SCRATCH/fx"
	"$BUILD/pfstool" cat "$PF8" "$SLI_MEMBER" >"$SCRATCH/fx/real.sli" 2>/dev/null
	if [ -s "$SCRATCH/fx/real.sli" ]; then
		M --case audiotool_sli --runs "$RUNS" --discard -- "$BUILD/audiotool" sli "$SCRATCH/fx/real.sli"
	fi
fi
# the layer's own self test: recorded, not gated (it is a correctness verdict)
if "$BUILD/audiotool" selftest >"$OUT/audiotool-selftest.txt" 2>&1; then
	printf 'META\t@env\taudiotool_selftest\tpass\n' >>"$RAW"
else
	printf 'META\t@env\taudiotool_selftest\tFAIL (%s)\n' \
		"$(grep -E '^[0-9]+ checks' "$OUT/audiotool-selftest.txt" | tail -1)" >>"$RAW"
	say "  audiotool selftest: $(grep -E '^[0-9]+ checks' "$OUT/audiotool-selftest.txt" | tail -1)"
fi

step "fx layer: brightness, typewriter, tween, quake on real frames and scripts"

REALFRAME_FX=$SCRATCH/frames-arch/page004.png
if [ -f "$REALFRAME_FX" ]; then
	M --case fxtool_sfade --runs "$RUNS" --discard -- "$BUILD/fxtool" sfade "$REALFRAME_FX" "$SCRATCH/fx/sfade.png"
	# the brightness pass measured on its own: no decode, no encode, no process start
	I --case fxtool_sfade_internal --runs "$RUNS" \
		--regex 'apply\s+([0-9.]+) ms' --unit ms -- \
		"$BUILD/fxtool" sfade "$REALFRAME_FX" "$SCRATCH/fx/sfade.png"
else
	record_fail "fxtool_sfade(no frame)"
fi
M --case fxtool_tween --runs "$RUNS" --discard -- "$BUILD/fxtool" tween --from 0 --to 1024 --ms 300
M --case fxtool_quake --runs "$RUNS" --discard -- "$BUILD/fxtool" quake --seed 7 --size 20 --count 5 --time 300
if [ -n "$ONE_AST" ]; then
	M --case fxtool_typewriter --runs "$RUNS" --discard -- \
		"$BUILD/fxtool" typewriter "$ONE_AST" --page 0 --at 0,1000,2000 --msgspeed 30
fi
if "$BUILD/fxtool" selftest >"$OUT/fxtool-selftest.txt" 2>&1; then
	printf 'META\t@env\tfxtool_selftest\tpass\n' >>"$RAW"
else
	printf 'META\t@env\tfxtool_selftest\tFAIL\n' >>"$RAW"
	say "  fxtool selftest: $(tail -2 "$OUT/fxtool-selftest.txt" | tr '\n' ' ')"
fi

step "image layer: IPT composition out of the real archive"

IPT_MEMBER=$( "$BUILD/pfstool" list "$PF8" 2>/dev/null | awk '$3 ~ /\.ipt$/ {print $3}' | head -1 )
if [ -n "$IPT_MEMBER" ]; then
	M --case iettool_ipt_compose --runs "$RUNS" --discard -- \
		"$BUILD/iettool" decode --archive "$PF8" "$IPT_MEMBER" "$SCRATCH/fx/ev.png"
	# iettool now reports its own split: "apply X ms (compose + blend + clip)" and
	# "encode Y ms (PNG write + colour count)".  Without these two lines the whole
	# 500 ms looked like "composition", which it never was.
	I --case iettool_decode_compose --runs "$RUNS" \
		--regex 'apply\s+([0-9.]+) ms' --unit ms -- \
		"$BUILD/iettool" decode --archive "$PF8" "$IPT_MEMBER" "$SCRATCH/fx/ev.png"
	I --case iettool_decode_encode --runs "$RUNS" \
		--regex 'encode\s+([0-9.]+) ms' --unit ms -- \
		"$BUILD/iettool" decode --archive "$PF8" "$IPT_MEMBER" "$SCRATCH/fx/ev.png"
fi

step "raspberry-web index (2891 entries) - parse cost at that entry count"

# the real archives here list 125 and 1682 members; a shipped web release lists
# 2891.  Measure the third point of the curve instead of extrapolating it.
if python3 "$HERE/index_scale.py" --pfstool "$BUILD/pfstool" --pfsbench "$PFSBENCH" \
	--index "$RASPBERRY_INDEX" --scratch "$SCRATCH/synth" >"$OUT/index_scale.txt" 2>&1; then
	sed 's/^/  /' "$OUT/index_scale.txt"
	grep -E '^META\tindex_' "$OUT/index_scale.txt" | while IFS=$'\t' read -r _ k v; do
		printf 'META\t@env\t%s\t%s\n' "$k" "$v" >>"$RAW"
	done
	awk -F'\t' '$1=="STAT"{printf "CLI\t%s\t1\t0\t%s\t0\t0\t0\n",$2,$3}' \
		"$OUT/index_scale.txt" >>"$RAW"
else
	say "  (index scaling probe skipped)"
	record_fail "index_scale"
fi

# ------------------------------------------------------------------ 6. pipe
step "summary"

cat >"$OUT/labels.tsv" <<'LABELS'
pfstool_usage	process_startup	ms	fork+exec+dyld floor, no archive
pfstool_list_pf6	list	ms	125 entries, 44 MB archive
pfstool_list_pf8	list	ms	1682 entries, 202 MB archive
pfstool_info_pf6	info	ms	open + phase detection + 5 sniffs
pfstool_info_pf8	info	ms	open + phase detection + 5 sniffs
pfstool_list_types_pf6	list--types	ms	sniffs all 73 PNG/16 JPG/17 OGG
pfstool_list_types_pf8	list--types	ms	sniffs all 1682 members
pfstool_cat_pf6_ttf	cat 4.9 MB	ms	single member out of 44 MB, plaintext
pfstool_cat_pf8_png	cat 2.0 MB	ms	single member out of 202 MB, XORed
pfstool_cat_pf8_ast	cat 192 KB	ms	single member out of 202 MB, XORed
pfstool_extract_pf6	extract all	ms	44 MB to disk
pfstool_extract_pf8	extract all	ms	202 MB to disk
pfsbench_open_pf6	Open (warm)	ms	mmap + index + phase detect
pfsbench_open_pf8	Open (warm)	ms	mmap + index + phase detect
pfsbench_rand_page_pf8	random 1st byte	ms	1000 random entries, mmap page fault
pfsbench_rand_pf8	random Content()	ms	1000 random entries, full decrypt each
pfsbench_rand_pf6	random Content()	ms	1000 random entries, plaintext
pfsbench_seq_pf6	extract devnull	ms	44 MB, decrypt+copy, no disk
pfsbench_seq_pf8	extract devnull	ms	202 MB, decrypt+copy, no disk
pfsbench_find_pf8	Find() miss	us	walks all 1682 names, normlising each
pfsbench_find_pf6	Find() miss	us	walks all 125 names
pfsbench_one_pf6_ttf	Content() 4.9 MB	ms	warm, plaintext
pfsbench_one_pf6_script	Content() 17 KB	ms	warm, plaintext
pfsbench_one_pf6_png	Content() 1.1 MB	ms	warm, plaintext - same kind as the pf8 PNG
pfsbench_one_pf8_png	Content() 2.0 MB	ms	warm, XORed
pfsbench_one_pf8_ast	Content() 192 KB	ms	warm, XORed
pfsbench_types_pf8	sniff all (today)	ms	Content() per member, keep 512 B
pfsbench_types_fix_pf8	sniff all (bounded)	ms	same answer from a 384 B sample
pfsbench_types_pf6	sniff all (today)	ms	Content() per member, plaintext
synth_open_warm_ms	Open 2891 entries	ms	synthetic archive the size the web release lists
synth_open_first_ms	Open 2891 (first)	ms	fresh process
synth_open_p95_ms	Open 2891 p95	ms	within one process
memref_memref_memcpy_mibs	memcpy ceiling	MiB/s	same bytes, no XOR, no alloc
memref_memref_xor1_mibs	XOR byte loop	MiB/s	what ApplyKey does today
memref_memref_xor4_mibs	XOR 4-byte words	MiB/s	uint32 at a time
memref_memref_xor20_mibs	XOR 20-byte blocks	MiB/s	8+8+4, the key period
memref_memref_bytes_mib	bytes tested	MiB	single pass, best of 3
alloc_alloc_per_call_ms	buffer per entry	ms	1682 x (malloc 256 KiB + memset)
alloc_alloc_reused_ms	buffer reused	ms	the same loop with one buffer
alloc_alloc_per_call_us	buffer per entry	us	per member
alloc_alloc_memset_mibs	buffer fill rate	MiB/s	what the zeroing actually runs at
applykey_shipped_mibs	ApplyKey today	MiB/s	byte-at-a-time loop from pfs.cpp:155
applykey_proposed_mibs	ApplyKey 20-byte	MiB/s	same bytes, 8+8+4 blocks, verified
python_extract_pf6	extract all	ms	python reference, 44 MB
python_extract_pf8	extract all	ms	python reference, 202 MB
pngbench_pf6_file	decode 73 PNG	ms	libpng via src/pngio.cpp, from file
pngbench_pf6_memory	decode 73 PNG	ms	libpng via src/pngio.cpp, from RAM
sips_pf6_pngs	decode 73 PNG	ms	sips, one process, decode + full BMP write
sips_pf6_tiny	decode 73 PNG	ms	sips, same decode, 64 px output (write is noise)
asttool_dump	.ast parse	ms	one real shipped .ast, dump whole tree
agame_astinfo_corpus	.ast parse corpus	ms	all 59 real .ast, parse + census only
audiotool_probe	ogg decode	ms	3.4 MB / 3:02 real OGG, 10 s window
audiotool_play	mix path	ms	3 s of audio through Engine::Mix
audiotool_sli	A-B loop parse	ms	real .sli from the archive
audiotool_probe_internal	ogg decode (internal)	ms	the layer's own decode timer, 10 s of audio
audiotool_play_internal	mix (internal)	ms	the layer's own mix timer, 3 s of audio
fxtool_sfade	brightness	ms	whole tool: decode + transform + encode
fxtool_sfade_internal	brightness (internal)	ms	the transform alone, as the tool times it
fxtool_tween	tween step	ms	0->1024 over 300 ms
fxtool_quake	quake path	ms	5 jolts over 300 ms
fxtool_typewriter	typewriter	ms	real .ast, 3 sample times
iettool_ipt_compose	IPT decode (whole tool)	ms	real .ipt -> 1668x998 PNG, incl. process start
iettool_decode_compose	compose (internal)	ms	compose + blend + clip, the tool's own timer
iettool_decode_encode	encode (internal)	ms	PNG write + colour count, the tool's own timer
agame_first_frame	first frame	ms	fixture scenario, exec -> first frame on disk
agame_real_archive	real first frame	ms	scene.iet + root.pfs: exec -> first page
agame_real_dir	real first frame	ms	same scene from the unpacked folder
capture_writepng	PNG encode (gradient)	ms	synthetic gradient, compressible: lower bound
capture_countcolors	colour counting	ms	CountColors of the same frame
capture_real_frame	PNG encode (real frame)	ms	re-encode page004.png, byte-identical out
capture_real_count	colour counting (real)	ms	CountColors of the real frame
LABELS

python3 "$HERE/summarize.py" --raw "$RAW" --labels "$OUT/labels.tsv" \
	--csv "$OUT/results.csv" --json "$OUT/results.json" --md "$OUT/tables.md" \
	>"$OUT/report.txt" 2>&1
RC=$?
cat "$OUT/report.txt"

if [ "$MODE" = "baseline" ]; then
	step "writing the regression baseline"
	cp "$OUT/results.json" "$HERE/baseline.json"
	say "  baseline <- $OUT/results.json"
	say "  $(python3 -c "
import json
d=json.load(open('$HERE/baseline.json'))
n=sum(1 for r in d['rows'] if r['kind'] in ('command','derived') and r['unit'] in ('ms','us','MiB/s','/s'))
print(str(n)+' timing metric(s) recorded')")"
fi

if [ "$MODE" = "check" ]; then
	step "regression gate (baseline.json, threshold ${THRESHOLD}%)"
	if [ ! -f "$HERE/baseline.json" ]; then
		say "  no baseline.json - run './bench/artemis/bench.sh --baseline' on a known-good build first"
		GATE_RC=2
	else
		python3 "$HERE/gate.py" --baseline "$HERE/baseline.json" \
			--current "$OUT/results.json" --threshold "$THRESHOLD" | tee "$OUT/gate.txt"
		GATE_RC=${PIPESTATUS[0]}
		say "  gate exit: $GATE_RC"
	fi
fi

AFTER=$(shasum -a 256 "$BUILD"/pfstool "$BUILD"/iettool "$BUILD"/agame "$BUILD"/libartemis.a 2>/dev/null)
step "done"
if [ "$BEFORE" != "$AFTER" ]; then
	say "!! a binary was rebuilt during this run: the table below mixes two builds"
	printf 'META\t@env\tbuild_changed_during_run\tyes\n' >>"$RAW"
else
	say "binaries unchanged for the whole run (good)"
	printf 'META\t@env\tbuild_changed_during_run\tno\n' >>"$RAW"
fi
if [ ${#FAILED[@]} -gt 0 ]; then
	say "!! cases that did not produce numbers: ${FAILED[*]}"
fi
say "raw   $RAW"
say "env   $OUT/env.txt"
say "csv   $OUT/results.csv"
say "json  $OUT/results.json"
say "human $OUT/report.txt"

cp "$OUT/results.csv" "$HERE/out/results.csv" 2>/dev/null
cp "$OUT/results.json" "$HERE/out/results.json" 2>/dev/null
cp "$OUT/report.txt" "$HERE/out/report.txt" 2>/dev/null
cp "$OUT/env.txt" "$HERE/out/env.txt" 2>/dev/null
if [ "$MODE" = "check" ] && [ "${GATE_RC:-0}" != "0" ]; then
	exit "$GATE_RC"
fi
exit $RC
