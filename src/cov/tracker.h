/* Per-frame event tracker for replays: level ($000D) / section ($0015) changes and camera lock/unlock
 * (camera = $8D-$8E X, $8F-$90 Y, measured 2026-11-23 on the 2P TAS), plus the gate log: for every
 * section/level change and every unlock, the RAM of the frame before, the bytes that changed on that
 * frame, and the bytes that dropped to 0 (or changed for the last time) shortly before (candidate
 * "wave cleared" counters / triggers). */
#ifndef BTDD_TRACKER_H
#define BTDD_TRACKER_H
#include "nes/nes.h"
#include <stdint.h>
#include <stdio.h>

#define TRACK_HIST 128      /* frames of RAM history kept */
#define TRACK_STILL 30      /* frames without camera motion that count as "locked" */

typedef struct Tracker {
    uint8_t hist[TRACK_HIST][0x800];
    long frames;            /* frames seen */
    uint8_t level, section;
    uint32_t cam;
    int still, moving;      /* run lengths */
    int locked;
    FILE *log;              /* gate log (text), optional */
    FILE *tsv;              /* one line per event, optional */
    const char *ram_dir;    /* if set, RAM before each gate event is written there */
    long events;
} Tracker;

void tracker_init(Tracker *t, const Nes *nes, long frame);
/* Call after each frame (frame = frames emulated so far). Returns SNAP_* reason bits (0 = nothing). */
unsigned tracker_frame(Tracker *t, const Nes *nes, long frame);

#endif
