#!/bin/sh
# Lockstep gate for a native build: both TAS movies, RAM compared every frame with the FCEUX dumps.
#   sh recomp/lockstep.sh [2p|1p|both] [OUTDIR]
# OUTDIR (default ../Gamefiles/Movies/m5) receives the RAM dumps; EXE = path of the exe to test
# (default OUTDIR/btdd_native.exe; a bare name is looked up in OUTDIR).
cd "$(dirname "$0")/.."
OUT=${2:-../Gamefiles/Movies/m5}
M1=../Gamefiles/Movies/m1
ROM="../Gamefiles/Battletoads-Double Dragon (USA).nes"
case "${EXE:-btdd_native}" in
    */*|*\*) BIN=$EXE ;;
    *) BIN=$OUT/${EXE:-btdd_native}.exe ;;
esac
which=${1:-both}
mkdir -p "$OUT"
if [ "$which" != 1p ]; then
    "$BIN" "$ROM" --movie ../Gamefiles/Movies/tasvideos-4323M-2p.fm2 --ppu old --dump-ram "$OUT/native_2p.bin" --dump-every 1
    python tools/ramdiff.py $M1/fceux_2p.bin "$OUT/native_2p.bin" | tail -4
fi
if [ "$which" != 2p ]; then
    "$BIN" "$ROM" --movie ../Gamefiles/Movies/tasvideos-2312S-1p.fm2 --ppu new --dump-ram "$OUT/native_1p.bin" --dump-every 1
    python tools/ramdiff.py $M1/fceux_1p.bin "$OUT/native_1p.bin" | tail -4
fi
