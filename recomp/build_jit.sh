#!/bin/sh
# Build btdd_jit.exe: no generated code inside. At first start it generates C from the ROM (recomp/gen.c
# in-process), compiles it to a DLL in the player's cache folder and loads it (recomp/native_jit.c).
# Compiler: libtcc when built with -DBTDD_LIBTCC (CMake option BTDD_LIBTCC), else the command in BTDD_CC.
#   sh recomp/build_jit.sh [OPT]
# Environment: OUT (default ../Gamefiles/Movies/m5), EXE (default btdd_jit), TREE (src/ provider, read-only), CC, EXTRA
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-../Gamefiles/Movies/m5}
TREE=${TREE:-.}
OPT=${1:--O2}
CC=${CC:-zig cc}
HOOKS=$(sh recomp/core_sources.sh "$TREE" --flags)
TREE_ABS=$(cd "$TREE" && (pwd -W 2>/dev/null || pwd))
RECOMP_ABS=$(pwd -W 2>/dev/null || pwd)
# cache key part: commit + hash of everything that shapes the generated code or its ABI
VER="$(git rev-parse --short HEAD)-$(cat recomp/gen.c recomp/native_rt.h "$TREE/src/runtime/cpu.h" "$TREE/src/nes/nes.h" \
    "$TREE/src/nes/ppu.h" | sha1sum | cut -c1-12)"
mkdir -p "$OUT"
# BTDD_SOURCE_DIR: where the DLL compile finds src/ and recomp/ headers and meta/btdd.meta
$CC $OPT $EXTRA $HOOKS -std=c11 -D_CRT_SECURE_NO_WARNINGS -I"$TREE/src" -Irecomp -DBTDD_GEN_NO_MAIN \
    "-DBTDD_SOURCE_DIR=\"$RECOMP_ABS\"" "-DBTDD_TREE_DIR=\"$TREE_ABS\"" "-DBTDD_EXE_VERSION=\"$VER\"" \
    $(sh recomp/core_sources.sh "$TREE") recomp/native_cpu.c recomp/native_jit.c recomp/gen.c \
    -o "$OUT/${EXE:-btdd_jit}.exe" $LIBS
echo "built $OUT/${EXE:-btdd_jit}.exe ${HOOKS:+($HOOKS)}"
