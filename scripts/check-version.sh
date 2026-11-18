#!/usr/bin/env bash
# Verifies that the version is consistent across the source and changelog,
# and (optionally) that a release tag matches it.
#   scripts/check-version.sh            # source vs CHANGELOG
#   scripts/check-version.sh v1.2.3     # additionally: tag must be v<version>
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
src=$(sed -n 's/^#define FTR_VERSION_STRING "\(.*\)"$/\1/p' "$root/foobar2000/foo_true_random/version.h")
log=$(sed -n 's/^## \([0-9][0-9.]*\).*/\1/p' "$root/CHANGELOG.md" | head -n1)
echo "version.h: ${src:-<missing>}   CHANGELOG top entry: ${log:-<missing>}"
[ -n "$src" ] && [ "$src" = "$log" ] || { echo "ERROR: version.h and CHANGELOG.md disagree"; exit 1; }
if [ $# -ge 1 ] && [ -n "$1" ]; then
  [ "$1" = "v$src" ] || { echo "ERROR: release tag '$1' does not match version v$src"; exit 1; }
  echo "release tag $1 matches"
fi
