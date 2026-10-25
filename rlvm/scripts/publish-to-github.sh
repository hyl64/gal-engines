#!/bin/sh
# publish-to-github.sh: put this branch on GitHub, with its release files.
#
# You authenticate; the script does the rest. It forks the upstream project
# (so the original history stays attached to your copy), pushes the revival
# branch to the fork, and attaches the two release artifacts.
#
#   $ gh auth login          # once, interactive, your credentials
#   $ scripts/publish-to-github.sh
#
# Nothing here writes to the upstream repository.

set -eu

BRANCH="${BRANCH:-revival/macos-arm}"
TAG="${TAG:-v0.14.1-revival}"
UPSTREAM="${UPSTREAM:-eglaysher/rlvm}"
ARTIFACTS="${ARTIFACTS:-../rlvm-deliverables}"

say() { printf '%s\n' "$*"; }
die() { printf 'publish: %s\n' "$*" >&2; exit 1; }

# ---- Who are we? -------------------------------------------------------

command -v gh >/dev/null 2>&1 || die "gh is not installed; brew install gh"
gh auth status >/dev/null 2>&1 ||
  die "not signed in. Run 'gh auth login' first, then try again."

me="$(gh api user --jq .login)"
say "publishing as $me"

# git needs an identity to author the merge-less push; borrow the account's.
if [ -z "$(git config user.name || true)" ]; then
  git config user.name "$(gh api user --jq '.name // .login')"
fi
if [ -z "$(git config user.email || true)" ]; then
  git config user.email "$(gh api user --jq '.id')+${me}@users.noreply.github.com"
fi
say "commits will be authored as $(git config user.name) <$(git config user.email)>"

# ---- The fork ----------------------------------------------------------

if ! gh repo view "$me/rlvm" >/dev/null 2>&1; then
  say "forking $UPSTREAM ..."
  gh repo fork "$UPSTREAM" --clone=false
  # A fork appears a moment after the API says it does.
  sleep 5
fi

if git remote | grep -qx fork; then
  git remote set-url fork "https://github.com/$me/rlvm.git"
else
  git remote add fork "https://github.com/$me/rlvm.git"
fi

# ---- The branch --------------------------------------------------------

say "pushing $BRANCH ..."
git push -u fork "$BRANCH"

# ---- The release -------------------------------------------------------

if [ -f "$ARTIFACTS/rlvm_0.14.1-revival.dmg" ]; then
  if gh release view "$TAG" --repo "$me/rlvm" >/dev/null 2>&1; then
    say "release $TAG already exists; uploading any missing assets"
  else
    say "creating release $TAG ..."
    gh release create "$TAG" \
      --repo "$me/rlvm" \
      --target "$BRANCH" \
      --title "rlvm 0.14.1-revival" \
      --notes "A RealLive interpreter for modern macOS.

Built and verified on Apple Silicon: 260 unit tests pass, all 231
scenarios of CLANNAD Full Voice parse, and the game runs -- text,
sprites, system menu, save, load and preferences.

- \`rlvm_0.14.1-revival.dmg\` -- the app. Drag it to /Applications.
- \`rlvm-revival-source.tar.gz\` -- sources; \`scons --release --fullstatic\` builds it.

RLVM does not include games. Point it at a directory you own that
contains Seen.txt and Gameexe.ini. Keep that directory out of
cloud-synced folders; see PERF.md."
  fi
  gh release upload "$TAG" \
    "$ARTIFACTS/rlvm_0.14.1-revival.dmg" \
    "$ARTIFACTS/rlvm-revival-source.tar.gz" \
    --repo "$me/rlvm" --clobber
else
  say "no dmg in $ARTIFACTS; skipping the release"
fi

say ""
say "done: https://github.com/$me/rlvm/tree/$BRANCH"
