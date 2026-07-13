#!/usr/bin/env bash
# Build + run the standalone Branchive Cloud sign-in core unit test (no Unreal Engine).
# Compiles the SAME LorePkce.cpp / LoreLoopback.cpp / LoreConfigPin.cpp the UE module uses.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LORE="$HERE/../../Source/BranchiveSourceControl/Private/Lore"
OUT="$HERE/auth_test"

CXX="${CXX:-g++}"
echo "Compiling auth_test with $CXX ..."
"$CXX" -std=c++17 -O0 -Wall -pthread -I"$LORE" \
  "$HERE/auth_test.cpp" \
  "$LORE/LorePkce.cpp" "$LORE/LoreLoopback.cpp" "$LORE/LoreConfigPin.cpp" \
  -o "$OUT"

echo "Running auth_test ..."
"$OUT"
