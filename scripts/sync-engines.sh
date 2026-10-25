#!/bin/bash
# sync-engines.sh - put the vendored engines at the commits engines.lock names
#
# Each engine here is a copy of a repository that keeps its own history.  Those
# repositories move; this one follows them deliberately, one pinned commit at a
# time, so a checkout of this repository builds the same thing twice.
#
#   ./scripts/sync-engines.sh                 restore the locked commits
#   ./scripts/sync-engines.sh --bump artemis  move one engine to its HEAD
#   ./scripts/sync-engines.sh --only kirikiri  one engine only
#
# A bump writes the lock and nothing else; build and check before committing it:
#   ./build-all.sh && ./build-all.sh --check --data <data dir>
#
# patches/*.patch are applied after every copy, so a local fix survives a sync
# instead of being silently overwritten by the next one.

set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
LOCK="$ROOT/engines.lock"
BUMP=""
ONLY=""

while [ $# -gt 0 ]; do
	case "$1" in
		--bump) shift; BUMP="$BUMP ${1:-}" ;;
		--only) shift; ONLY="$ONLY ${1:-}" ;;
		--help|-h) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

want() { [ -z "$ONLY" ] && return 0; case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }
bumping() { [ -z "$BUMP" ] && return 1; case " $BUMP " in *" $1 "*) return 0 ;; esac; return 1; }

apply_patches() { # name, destination
	local name="$1" dst="$2" p n=0
	for p in "$ROOT"/patches/"$name"-*.patch; do
		[ -f "$p" ] || continue
		# git apply inside another repository's work tree is fussy about paths;
		# plain patch(1) is the fallback so a patch never silently disappears.
		if ( cd "$dst" && git apply --unsafe-paths -p1 "$p" 2>/dev/null ) \
			|| ( cd "$dst" && patch -p1 --forward --silent < "$p" 2>/dev/null ); then
			n=$((n + 1))
		else
			printf '    \033[31mpatch failed:\033[0m %s\n' "$(basename "$p")"
		fi
	done
	[ "$n" -gt 0 ] && printf '    %d patch(es) applied\n' "$n"
}

echo "同步引擎（锁定版本）"
while read -r name dir sha src note; do
	case "$name" in ''|'#'*) continue ;; esac
	[ -n "${dir:-}" ] && [ -n "${sha:-}" ] || continue
	want "$name" || continue
	src="${src/#\~/$HOME}"
	if [ ! -d "$src/.git" ]; then
		printf '  \033[33m·\033[0m %-10s no repository at %s\n' "$name" "$src"
		continue
	fi
	bumping "$name" && sha=$(git -C "$src" rev-parse HEAD)
	if ! git -C "$src" cat-file -e "$sha^{commit}" 2>/dev/null; then
		printf '  \033[31m✗\033[0m %-10s commit %s not in %s\n' "$name" "${sha:0:12}" "$src"
		continue
	fi
	rm -rf "$ROOT/$dir" && mkdir -p "$ROOT/$dir"
	if ! git -C "$src" archive "$sha" | tar -x -C "$ROOT/$dir"; then
		printf '  \033[31m✗\033[0m %-10s archive failed\n' "$name"
		continue
	fi
	apply_patches "$name" "$ROOT/$dir"
	printf '  \033[32m✓\033[0m %-10s %s  %s\n' "$name" "${sha:0:10}" \
		"$(git -C "$src" log --oneline -1 "$sha" | cut -c1-46)"
	if bumping "$name"; then
		python3 - "$LOCK" "$name" "$sha" <<'PY'
import sys
lock, name, sha = sys.argv[1], sys.argv[2], sys.argv[3]
out = []
for line in open(lock).read().splitlines():
    parts = line.split()
    if parts and not line.lstrip().startswith('#') and parts[0] == name:
        parts[2] = sha
        line = '  '.join(parts)
    out.append(line)
open(lock, 'w').write('\n'.join(out) + '\n')
PY
		printf '    lock updated: %s -> %s\n' "$name" "${sha:0:10}"
	fi
done < "$LOCK"

echo
echo "下一步："
echo "  git status --short                       同步带来了什么"
echo "  ./build-all.sh                           五个引擎是否仍然构建"
echo "  ./build-all.sh --check --data <目录>     自检是否仍然成立"
