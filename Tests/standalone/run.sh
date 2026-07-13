#!/usr/bin/env bash
# Build + run the standalone Lore parser fixture test (no Unreal Engine needed).
# Compiles the SAME LoreParse.cpp / LoreErrors.cpp the UE module uses.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LORE="$HERE/../../Source/BranchiveSourceControl/Private/Lore"
FIX="${1:-$HERE/../../../contract/fixtures}"
OUT="$HERE/parser_test.exe"

CXX="${CXX:-g++}"
echo "Compiling with $CXX ..."
"$CXX" -std=c++17 -O0 -Wall -I"$LORE" \
  "$HERE/parser_test.cpp" "$LORE/LoreParse.cpp" "$LORE/LoreErrors.cpp" \
  -o "$OUT"

echo "Running against fixtures: $FIX"
"$OUT" "$FIX"
