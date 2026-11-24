/* Event tracker and gate log (see tracker.h). */
#include "tracker.h"
#include "snapshot.h"
#include <string.h>

static uint32_t camera(const Nes *n)
{
    return (uint32_t)n->ram[0x8D] | (uint32_t)n->ram[0x8E] << 8 | (uint32_t)n->ram[0x8F] << 16 |
           (uint32_t)n->ram[0x90] << 24;
}

void tracker_init(Tracker *t, const Nes *nes, long frame)
{
    int i;
    FILE *log = t->log, *tsv = t->tsv;
    const char *dir = t->ram_dir;
    memset(t, 0, sizeof *t);
    t->log = log;
    t->tsv = tsv;
    t->ram_dir = dir;
    for (i = 0; i < TRACK_HIST; i++)
        memcpy(t->hist[i], nes->ram, 0x800);
    t->frames = frame;
    t->level = nes->ram[0x0D];
    t->section = nes->ram[0x15];
    t->cam = camera(nes);
    if (t->tsv)
        fprintf(t->tsv, "frame\tevent\tlevel\tsection\tcamera\tchanged_this_frame\twent_to_zero_before\n");
}

/* Gate details: bytes that changed between the previous frame and now, and bytes that became 0 within
 * the history window and stayed 0 (with how many frames ago and the value before). */
static void gate_entry(Tracker *t, const Nes *nes, long frame, const char *what)
{
    const uint8_t *prev = t->hist[(frame - 1) % TRACK_HIST];
    int a, k, nchg = 0;
    char zeros[4096];
    int zl = 0;
    zeros[0] = 0;
    for (a = 0; a < 0x800; a++) {
        /* went to 0: now 0, and at some point in the window it was nonzero; report the last transition */
        if (nes->ram[a] == 0 && a >= 0x10 && (a < 0x100 || a >= 0x300)) { /* skip temps, stack, OAM */
            for (k = 1; k < TRACK_HIST - 1 && k <= frame; k++) {
                uint8_t v = t->hist[(frame - k) % TRACK_HIST][a];
                if (v != 0) {
                    if (k > 1 && zl < (int)sizeof zeros - 32)
                        zl += snprintf(zeros + zl, sizeof zeros - (size_t)zl, "%s%04X(-%d,was %02X)",
                                       zl ? " " : "", a, k - 1, v);
                    break;
                }
            }
        }
    }
    t->events++;
    if (t->log) {
        fprintf(t->log, "frame %ld %s: level %02X section %02X camera %08X\n  changed on this frame:", frame, what,
                nes->ram[0x0D], nes->ram[0x15], (unsigned)camera(nes));
        for (a = 0; a < 0x800; a++)
            if (prev[a] != nes->ram[a] && (a < 0x100 || a >= 0x300)) {
                if (nchg < 96)
                    fprintf(t->log, " %03X:%02X>%02X", a, prev[a], nes->ram[a]);
                nchg++;
            }
        if (nchg > 96)
            fprintf(t->log, " ... (%d bytes)", nchg);
        fprintf(t->log, "\n  went to 0 in the last %d frames and stayed: %s\n", TRACK_HIST, zeros);
    }
    if (t->tsv) {
        fprintf(t->tsv, "%ld\t%s\t%02X\t%02X\t%08X\t%d\t%s\n", frame, what, nes->ram[0x0D], nes->ram[0x15],
                (unsigned)camera(nes), nchg, zeros);
    }
    if (t->ram_dir) {
        char path[1024];
        FILE *f;
        snprintf(path, sizeof path, "%s/gate-f%06ld-%s.ram", t->ram_dir, frame, what);
        f = fopen(path, "wb");
        if (f) {
            fwrite(prev, 1, 0x800, f);
            fclose(f);
        }
    }
}

unsigned tracker_frame(Tracker *t, const Nes *nes, long frame)
{
    unsigned r = 0;
    uint8_t lv = nes->ram[0x0D], sc = nes->ram[0x15];
    uint32_t cam = camera(nes);
    int in_level = lv < 0x10; /* $FE/$FD while loading */

    if (lv != t->level) {
        r |= SNAP_LEVEL;
        gate_entry(t, nes, frame, "level");
    }
    if (sc != t->section) {
        r |= SNAP_SECTION;
        gate_entry(t, nes, frame, "section");
    }
    if (cam != t->cam) {
        if (t->still >= TRACK_STILL && in_level && !(r & (SNAP_LEVEL | SNAP_SECTION))) {
            r |= SNAP_UNLOCK;
            gate_entry(t, nes, frame, "unlock");
        }
        t->moving++;
        t->still = 0;
    } else {
        t->still++;
        if (t->still == TRACK_STILL && t->moving >= TRACK_STILL && in_level)
            r |= SNAP_LOCK;
        if (t->still > TRACK_STILL)
            t->moving = 0;
    }
    t->level = lv;
    t->section = sc;
    t->cam = cam;
    memcpy(t->hist[frame % TRACK_HIST], nes->ram, 0x800);
    t->frames = frame;
    return r;
}
