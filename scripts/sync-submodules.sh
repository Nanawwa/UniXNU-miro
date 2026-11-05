#!/usr/bin/env bash
#
# Refresh this repository's module pointers from the reset UniXNU.
#
# This board declares base's modules as its own submodules (see .gitmodules) so it
# resolves without depending on the UniXNU checkout. That only stays true while the
# gitlinks here name the same revisions base names, so this script copies them.
#
#   scripts/sync-submodules.sh [--check] [--no-init]
#
#   base    UniXNU (UNIXNU_BASE_REPO) .gitmodules urls and `git ls-tree` gitlinks
#   keep    drivers/MiroUfs is carried here as a plain directory (the SM8750
#           patched transport), so it is excluded from the submodule set.
#
# --check reports drift and missing .gitmodules entries without changing anything
# and exits non-zero when there is any. --no-init skips `git submodule update`.
#
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MIRO_ROOT=$(cd "$REPO/.." && pwd)
BASE_REPO=${UNIXNU_BASE_REPO:-$MIRO_ROOT/UniXNU}
EXCLUDE=${SYNC_EXCLUDE:-drivers/MiroUfs}

CHECK=0
INIT=1
for arg in "$@"; do
  case "$arg" in
    --check) CHECK=1; INIT=0 ;;
    --no-init) INIT=0 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "sync-submodules.sh: unknown argument: $arg" >&2; exit 2 ;;
  esac
done

if [ ! -d "$BASE_REPO/.git" ]; then
  echo "sync-submodules.sh: base repository not found: $BASE_REPO" >&2
  echo "sync-submodules.sh: set UNIXNU_BASE_REPO to the UniXNU checkout" >&2
  exit 1
fi

drift=0
changed=0

# Base's module list, in tree order: "path sha".
while read -r path sha; do
  [ -n "${path:-}" ] && [ -n "${sha:-}" ] || continue
  [ "$path" = "$EXCLUDE" ] && continue

  url=$(git -C "$BASE_REPO" config -f "$BASE_REPO/.gitmodules" --get "submodule.$path.url" || true)

  current=$(git -C "$REPO" ls-files -s -- "$path" | awk '{print $2}')
  if [ "$current" = "$sha" ]; then
    echo "ok      $path"
  else
    if [ "$CHECK" = 1 ]; then
      echo "drift   $path $current -> $sha"
      drift=$((drift + 1))
    else
      git -C "$REPO" update-index --add --cacheinfo "160000,$sha,$path"
      echo "synced  $path -> $sha"
      changed=$((changed + 1))
    fi
  fi

  # Keep a url in .gitmodules for every module so a fresh clone can fetch it.
  have_url=$(git -C "$REPO" config -f "$REPO/.gitmodules" --get "submodule.$path.url" || true)
  if [ -z "$have_url" ] && [ -n "$url" ]; then
    if [ "$CHECK" = 1 ]; then
      echo "missing $path in .gitmodules"
      drift=$((drift + 1))
    else
      git -C "$REPO" config -f "$REPO/.gitmodules" "submodule.$path.path" "$path"
      git -C "$REPO" config -f "$REPO/.gitmodules" "submodule.$path.url" "$url"
      echo "added   $path to .gitmodules"
      changed=$((changed + 1))
    fi
  fi
done < <(git -C "$BASE_REPO" ls-tree HEAD drivers/ kits/ | grep '^160000' | awk '{print $4, $3}')

if [ "$CHECK" = 1 ]; then
  if [ "$drift" != 0 ]; then
    echo "sync-submodules.sh: $drift module(s) out of date" >&2
    exit 1
  fi
  echo "sync-submodules.sh: all modules match base"
  exit 0
fi

if [ "$INIT" = 1 ]; then
  git -C "$REPO" submodule update --init --recursive
fi
echo "sync-submodules.sh: $changed module(s) updated"