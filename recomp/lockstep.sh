#!/bin/sh
# Lockstep gate for the native build: both TAS movies, RAM compared every frame with the FCEUX dumps.
#   sh recomp/lockstep.sh [2p|1p|both]
cd "$(dirname "$0")/.."
M5=../Gamefiles/Movies/m5
M1=../Gamefiles/Movies/m1
ROM="../Gamefiles/Battletoads-Double Dragon (USA).nes"
EXE=$M5/btdd_native.exe
which=${1:-both}
if [ "$which" != 1p ]; then
    $EXE "$ROM" --movie ../Gamefiles/Movies/tasvideos-4323M-2p.fm2 --ppu old --dump-ram $M5/native_2p.bin --dump-every 1
    python tools/ramdiff.py $M1/fceux_2p.bin $M5/native_2p.bin | tail -4
fi
if [ "$which" != 2p ]; then
    $EXE "$ROM" --movie ../Gamefiles/Movies/tasvideos-2312S-1p.fm2 --ppu new --dump-ram $M5/native_1p.bin --dump-every 1
    python tools/ramdiff.py $M1/fceux_1p.bin $M5/native_1p.bin | tail -4
fi
