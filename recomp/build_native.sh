#!/bin/sh
# Build the M5 native test exe (headless runner + generated code linked in) with zig cc (clang).
# Generated C, objects and the exe live outside the repo (ROM-derived).
#   sh recomp/build_native.sh [OPT]          OPT defaults to -O2
# Environment: GEN  generated C folder (default ../Gamefiles/Movies/m5/generated)
#              OUT  output folder for obj/ and the exe (default ../Gamefiles/Movies/m5)
#              EXE  exe name without .exe (default btdd_native)
#              TREE source tree that provides src/ (default this checkout; only read)
#              CC, EXTRA  compiler and extra flags
set -e
cd "$(dirname "$0")/.."
M5=../Gamefiles/Movies/m5
OUT=${OUT:-$M5}
GEN=${GEN:-$M5/generated}
TREE=${TREE:-.}
EXE=${EXE:-btdd_native}
OPT=${1:--O2}
CC=${CC:-zig cc}
HOOKS=$(sh recomp/core_sources.sh "$TREE" --flags)
FLAGS="$OPT $EXTRA $HOOKS -std=c11 -D_CRT_SECURE_NO_WARNINGS -I$TREE/src -Irecomp -Wno-unused-label -DBTDD_NATIVE_STATIC"
mkdir -p "$OUT/obj"
for b in 0 1 2 3 4 5 6 7; do
    $CC $FLAGS -c "$GEN/native_b$b.c" -o "$OUT/obj/${EXE}_b$b.o" &
done
wait
$CC $FLAGS $(sh recomp/core_sources.sh "$TREE") recomp/native_cpu.c "$GEN/native_all.c" "$OUT"/obj/${EXE}_b*.o \
    -o "$OUT/$EXE.exe"
echo "built $OUT/$EXE.exe ${HOOKS:+($HOOKS)}"
