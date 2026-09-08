#!/usr/bin/env bash
# tools/build-all.sh — Build all Windows XP test applications and verify PE imports
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "========================================================"
echo " Building All Windows XP Graphics & Display Test Tools"
echo "========================================================"

TOOLS=(
    "test-gdi"
    "test-ddraw"
    "test-d3d8"
    "test-d3d9"
    "test-opengl"
    "test-modeswitch"
    "probe-video"
    "eax-test"
)

mkdir -p tools/bin

for tool in "${TOOLS[@]}"; do
    echo ""
    echo "--- Building $tool ---"
    bash "tools/$tool/build.sh"
    # Copy binary to central tools/bin/
    cp "tools/$tool/bin/"*.exe tools/bin/
done

echo ""
echo "========================================================"
echo " All Tools Built Successfully:"
ls -lh tools/bin/*.exe
echo "========================================================"
