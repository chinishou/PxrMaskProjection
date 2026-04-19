#!/usr/bin/env bash
# =============================================================
#  Fetch vendored third-party sources into ./thirdparty/.
#
#  PxrMaskProjection bundles (at build time, not in git) these
#  single-header libraries:
#
#    - tinyexr  — BSD 3-Clause — https://github.com/syoyo/tinyexr
#    - miniz    — MIT          — https://github.com/richgel999/miniz
#
#  Run this script once before build.sh.  Re-run to update
#  pinned versions after bumping the refs below.
#
#  Requires: curl, unzip.
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$SCRIPT_DIR/thirdparty"
mkdir -p "$DEST"

# Pinned refs — bump and re-run this script to update.
TINYEXR_REF="release"   # branch; swap to a commit SHA for strict pinning
MINIZ_VER="3.0.2"       # GitHub release tag (== miniz version 11.0.2)

TINYEXR_BASE="https://raw.githubusercontent.com/syoyo/tinyexr/${TINYEXR_REF}"
MINIZ_ZIP_URL="https://github.com/richgel999/miniz/releases/download/${MINIZ_VER}/miniz-${MINIZ_VER}.zip"

echo "Fetching tinyexr @ ${TINYEXR_REF}"
for f in tinyexr.h exr_reader.hh streamreader.hh; do
    curl -fL --progress-bar -o "$DEST/$f" "$TINYEXR_BASE/$f"
done

# miniz's git tag ships the split source; the amalgamated single-file
# form lives only in the GitHub release zip.
echo
echo "Fetching miniz @ ${MINIZ_VER}"
TMP_ZIP="$(mktemp -t miniz.XXXXXX.zip)"
trap 'rm -f "$TMP_ZIP"' EXIT
curl -fL --progress-bar -o "$TMP_ZIP" "$MINIZ_ZIP_URL"
unzip -j -o -q "$TMP_ZIP" miniz.h miniz.c -d "$DEST"

echo
echo "[OK] thirdparty/ populated:"
ls -la "$DEST"
