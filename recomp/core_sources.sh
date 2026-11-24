#!/bin/sh
# Prints the C sources of the headless runner for a source tree, without src/runtime/cpu.c (recomp/native_cpu.c
# compiles that in itself), and the flags the tree needs. Derived from the tree's folders instead of a hand-written
# list, so merged trees (coverage, APU, ...) need no edit here. CMake does the same from the btdd_headless target.
#   sh recomp/core_sources.sh [TREE]          -> source list
#   sh recomp/core_sources.sh [TREE] --flags  -> -D flags (BTDD_CPU_HOOKS when the tree's Cpu has insn/tick hooks)
TREE=${1:-.}
if [ "$2" = "--flags" ]; then
    grep -q 'tick_hook' "$TREE/src/runtime/cpu.h" && grep -q 'insn_hook' "$TREE/src/runtime/cpu.h" && echo "-DBTDD_CPU_HOOKS"
    exit 0
fi
echo "$TREE/src/headless_main.c"
for d in rom runtime nes cov mods; do
    [ -d "$TREE/src/$d" ] || continue
    for f in "$TREE"/src/$d/*.c; do
        case "$f" in */runtime/cpu.c) ;; *) echo "$f" ;; esac
    done
done
