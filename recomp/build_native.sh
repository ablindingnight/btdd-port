#!/bin/sh
# Build the M5 native test exe (headless runner + generated code) with zig cc (clang).
# Output and generated C live in ..\Gamefiles\Movies\m5 (ROM-derived: never in the repo).
#   sh recomp/build_native.sh [OPT]      OPT defaults to -O2
set -e
cd "$(dirname "$0")/.."
M5=../Gamefiles/Movies/m5
GEN=$M5/generated
OPT=${1:--O2}
CC=${CC:-zig cc}
FLAGS="$OPT $EXTRA -std=c11 -D_CRT_SECURE_NO_WARNINGS -Isrc -Irecomp -Wno-unused-label -DBTDD_NATIVE_STATIC"
mkdir -p $M5/obj
for b in 0 1 2 3 4 5 6 7; do
    $CC $FLAGS -c $GEN/native_b$b.c -o $M5/obj/${EXE:-btdd_native}_b$b.o &
done
wait
$CC $FLAGS src/headless_main.c src/rom/rom.c src/rom/sha1.c recomp/native_cpu.c src/runtime/fm2.c \
    src/nes/ppu.c src/nes/ppu_new.c src/nes/nes.c $GEN/native_all.c $M5/obj/${EXE:-btdd_native}_b*.o -o $M5/${EXE:-btdd_native}.exe
echo built $M5/${EXE:-btdd_native}.exe
