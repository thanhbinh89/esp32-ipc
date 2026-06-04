#!/usr/bin/env bash
# Apply local patches to IDF-managed components.
#
# managed_components/ is git-ignored and re-downloaded from the component
# registry by the IDF component manager, which overwrites any local edits.
# Run this after every `idf.py reconfigure` / fresh clone / managed_components
# wipe to re-apply our fixes.
#
# Idempotent: already-applied patches are detected and skipped.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MC="$ROOT/managed_components"

if [ ! -d "$MC" ]; then
  echo "error: $MC not found. Run 'idf.py reconfigure' first to fetch components." >&2
  exit 1
fi

# patch file <-> target component dir
declare -A TARGETS=(
  [sepfy__libpeer.patch]="sepfy__libpeer"
  [sepfy__srtp.patch]="sepfy__srtp"
)

rc=0
for patch in "${!TARGETS[@]}"; do
  comp="${TARGETS[$patch]}"
  dir="$MC/$comp"
  pf="$SCRIPT_DIR/$patch"

  if [ ! -d "$dir" ]; then
    echo "SKIP  $comp (not installed)"
    continue
  fi

  # Already applied? reverse dry-run succeeds when patch is present.
  if patch -p1 -R --dry-run -d "$dir" < "$pf" >/dev/null 2>&1; then
    echo "OK    $comp (already patched)"
    continue
  fi

  # Applies cleanly to pristine?
  if patch -p1 --dry-run -d "$dir" < "$pf" >/dev/null 2>&1; then
    patch -p1 -d "$dir" < "$pf" >/dev/null
    echo "PATCH $comp (applied)"
  else
    echo "FAIL  $comp (does not apply cleanly — pristine version may have changed)" >&2
    rc=1
  fi
done

exit $rc
