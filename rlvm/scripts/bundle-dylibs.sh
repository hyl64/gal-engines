#!/bin/sh
# bundle-dylibs.sh: make rlvm.app run where Homebrew never existed.
#
#   usage: bundle-dylibs.sh <freshly-built-rlvm-static> <bundle-dir> [extra-dylib ...]
#
# The script owns the two places it writes -- Contents/MacOS/rlvm and
# Contents/Frameworks -- and rebuilds both from the built binary on every
# run, so no rewritten load command from a previous pass can linger. Every
# third-party dylib moves into Frameworks with its loads repointed at
# @rpath. otool only sees load commands, so libraries that arrive through
# a bare dlopen() -- sdl12-compat's SDL3 backend -- are passed explicitly
# as extra arguments.

set -eu

SRC="$1"; BUNDLE="$2"; shift 2
FW="$BUNDLE/Contents/Frameworks"

# A tree built from scratch has no Contents/MacOS yet: this script is the
# only thing that puts the executable there.
mkdir -p "$BUNDLE/Contents/MacOS"
rm -rf "$FW"
mkdir -p "$FW"
# Info.plist names the executable; the bundle lives or dies by that name,
# whatever the build called its binary.
EXEC_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
  "$BUNDLE/Contents/Info.plist" 2>/dev/null || basename "$SRC")"
BIN="$BUNDLE/Contents/MacOS/$EXEC_NAME"
cp -LX "$SRC" "$BIN"

is_third_party() {
  case "$1" in
    /System/*|/usr/lib/*|@*) return 1 ;;
    *) return 0 ;;
  esac
}

# Copy <path> into Frameworks and give it an @rpath identity, once.
# -X: leave the source's extended attributes behind; a fresh signature
# wants a clean slate.
copy() {
  path="$1"
  name="$(basename "$path")"
  if [ ! -f "$FW/$name" ]; then
    cp -LX "$path" "$FW/$name"
    install_name_tool -id "@rpath/$name" "$FW/$name"
  fi
}

# Seed with the binary's own third-party loads plus the dlopen-only extras.
for dep in $(otool -L "$BIN" | awk 'NR>1 {print $1}') "$@"; do
  if is_third_party "$dep"; then
    copy "$dep"
  fi
done

# And with whatever those libraries themselves load.
for lib in "$FW"/*; do
  for dep in $(otool -L "$lib" | awk 'NR>1 {print $1}'); do
    if is_third_party "$dep"; then
      copy "$dep"
    fi
  done
done

# The loaders ask for version-less names that the shipped files do not
# carry: sdl12-compat wants libSDL2-2.0.dylib and sdl2-compat wants
# libSDL3.dylib, while the bottles install the fully versioned names.
for alias in libSDL2-2.0 libSDL3; do
  if [ -f "$FW/$alias.0.dylib" ] && [ ! -e "$FW/$alias.dylib" ]; then
    ln -s "$alias.0.dylib" "$FW/$alias.dylib"
  fi
done

# Repoint every load command at what we just brought home.
repoint() {
  image="$1"
  for dep in $(otool -L "$image" | awk 'NR>1 {print $1}'); do
    if is_third_party "$dep"; then
      install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$image"
    fi
  done
}
repoint "$BIN"
for lib in "$FW"/*; do repoint "$lib"; done

# codesign signing inside the .app scans the whole bundle and refuses
# files still carrying Finder metadata. It signs right over
# com.apple.provenance -- the kernel re-attaches that to freshly created
# files no matter what a copy does -- so clear exactly the two attributes
# it minds, forgive whichever are absent.
for image in $(find "$BUNDLE" -type f); do
  xattr -d com.apple.FinderInfo "$image" 2>/dev/null || true
  xattr -d com.apple.ResourceFork "$image" 2>/dev/null || true
done
for lib in "$FW"/*; do codesign --force --sign - "$lib"; done

# In a synced tree (Desktop & Documents sync) the File Provider daemon
# re-stamps the bundle directory with FinderInfo between the cleanup and
# codesign's scan -- a race that cannot be prevented, only outlasted.
attempt=0
while :; do
  xattr -d com.apple.FinderInfo "$BUNDLE" 2>/dev/null || true
  if codesign --force --sign - "$BIN"; then
    break
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 5 ]; then
    echo "bundle-dylibs: signing kept failing; see codesign output above." >&2
    exit 1
  fi
  sleep 1
done

echo "bundle-dylibs: $(ls "$FW" | wc -l | tr -d ' ') libraries brought home."
