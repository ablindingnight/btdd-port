#!/bin/sh
# Build ..\Gamefiles\Movies\m5\btdd_jit.exe: no generated code inside. At first start it generates C from the
# ROM (recomp/gen.c in-process), compiles it to a DLL in the player's cache folder and loads it (recomp/native_jit.c).
# Compiler: libtcc when built with -DBTDD_LIBTCC (see CMake option BTDD_LIBTCC), else the command in BTDD_CC.
#   sh recomp/build_jit.sh [OPT]
set -e
cd "$(dirname "$0")/.."
M5=../Gamefiles/Movies/m5
OPT=${1:--O2}
CC=${CC:-zig cc}
SRC=$(pwd -W 2>/dev/null || pwd)
# cache key part: commit + hash of everything that shapes the generated code or its ABI
VER="$(git rev-parse --short HEAD)-$(cat recomp/gen.c recomp/native_rt.h src/runtime/cpu.h src/nes/nes.h src/nes/ppu.h | sha1sum | cut -c1-12)"
$CC $OPT $EXTRA -std=c11 -D_CRT_SECURE_NO_WARNINGS -Isrc -Irecomp -DBTDD_GEN_NO_MAIN "-DBTDD_SOURCE_DIR=\"$SRC\"" "-DBTDD_EXE_VERSION=\"$VER\"" \
    src/headless_main.c src/rom/rom.c src/rom/sha1.c src/runtime/fm2.c src/nes/ppu.c src/nes/ppu_new.c src/nes/nes.c \
    recomp/native_cpu.c recomp/native_jit.c recomp/gen.c -o $M5/${EXE:-btdd_jit}.exe $LIBS
echo built $M5/${EXE:-btdd_jit}.exe
