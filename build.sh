#!/usr/bin/env bash
# =============================================================
#  PxrMaskProjection - Linux build (g++)
#
#  EXR loading uses the bundled tinyexr (thirdparty/), so the
#  plugin does not depend on Houdini's OpenEXR or any system
#  OpenEXR package at build or run time.
#
#  Set RMANTREE to your RenderManProServer install before
#  running. Example:
#      export RMANTREE=/opt/pixar/RenderManProServer-27.2
#      ./build.sh
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

: "${RMANTREE:?Set RMANTREE to your RenderManProServer-XX.Y install}"

if [ ! -d "$RMANTREE/include" ] || [ ! -d "$RMANTREE/lib" ]; then
    echo "[ERROR] RMANTREE='$RMANTREE' doesn't look like a RenderMan install" >&2
    exit 1
fi

g++ -shared -fPIC -O2 -std=c++17 \
    -DNOMINMAX -D_USE_MATH_DEFINES \
    -I"$RMANTREE/include" \
    -I"$SCRIPT_DIR/thirdparty" \
    -o PxrMaskProjection.so \
    PxrMaskProjection.cpp thirdparty/miniz.c \
    -L"$RMANTREE/lib" -lprman -lpxrcore \
    -Wl,-rpath,"$RMANTREE/lib"

echo
echo "[OK] Built PxrMaskProjection.so"
