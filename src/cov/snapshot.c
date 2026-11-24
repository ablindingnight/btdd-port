/* Snapshots (see snapshot.h). */
#include "snapshot.h"
#include "nes/state.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char MAGIC[8] = {'B', 'T', 'D', 'D', 'S', 'N', 'P', '1'};

/* A cheap fingerprint of the struct layout: offsets of a few fields that move when anything changes. */
static uint32_t layout_fingerprint(void)
{
    uint32_t h = 2166136261u;
    size_t v[] = {offsetof(Nes, cpu), offsetof(Nes, ppu), offsetof(Nes, ram), offsetof(Nes, prg_bank),
                  offsetof(Nes, pad), offsetof(Nes, apu_regs), offsetof(Nes, stats), offsetof(Nes, watch),
                  offsetof(Cpu, budget), offsetof(Cpu, cycles), offsetof(Ppu, chr), offsetof(Ppu, oam),
                  offsetof(Ppu, fb), offsetof(Ppu, line_sprites), sizeof(Cpu), sizeof(Ppu)};
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        h = (h ^ (uint32_t)v[i]) * 16777619u;
    return h;
}

/* Copies every host field (hooks, buffers, picture) from `from` into `to` (shared with save states). */
static void keep_pointers(Nes *to, const Nes *from)
{
    nes_state_adopt_host(to, from);
}

void snap_copy(Nes *dst, const Nes *src)
{
    static Nes keep; /* not reentrant; the tools are single-threaded */
    memcpy(&keep, dst, sizeof keep);
    memcpy(dst, src, sizeof *dst);
    keep_pointers(dst, &keep);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int snap_save(const Nes *nes, const SnapInfo *info, const char *path)
{
    static Nes tmp;
    uint8_t hdr[32] = {0};
    FILE *f;
    int ok;
    memcpy(&tmp, nes, sizeof tmp);
    nes_state_clear_host(&tmp); /* clear pointers and debugging state */
    memcpy(hdr, MAGIC, 8);
    put32(hdr + 8, (uint32_t)sizeof(Nes));
    put32(hdr + 12, layout_fingerprint());
    put32(hdr + 16, info->frame);
    hdr[20] = info->level;
    hdr[21] = info->section;
    hdr[22] = (uint8_t)info->reason;
    hdr[23] = (uint8_t)(info->reason >> 8);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(&tmp, 1, sizeof tmp, f) == sizeof tmp;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int snap_load(Nes *nes, SnapInfo *info, const char *path)
{
    static Nes tmp;
    uint8_t hdr[32];
    FILE *f = fopen(path, "rb");
    int ok;
    if (!f)
        return -1;
    ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, MAGIC, 8) &&
         get32(hdr + 8) == (uint32_t)sizeof(Nes) && get32(hdr + 12) == layout_fingerprint() &&
         fread(&tmp, 1, sizeof tmp, f) == sizeof tmp;
    fclose(f);
    if (!ok)
        return -2;
    keep_pointers(&tmp, nes);
    memcpy(nes, &tmp, sizeof tmp);
    if (info) {
        info->frame = get32(hdr + 16);
        info->level = hdr[20];
        info->section = hdr[21];
        info->reason = (uint16_t)(hdr[22] | hdr[23] << 8);
    }
    return 0;
}

const char *snap_reason_text(uint16_t r, char *buf, int len)
{
    snprintf(buf, (size_t)len, "%s%s%s%s%s", r & SNAP_PERIODIC ? "p" : "", r & SNAP_LEVEL ? "L" : "",
             r & SNAP_SECTION ? "S" : "", r & SNAP_LOCK ? "lock" : "", r & SNAP_UNLOCK ? "unlock" : "");
    if (!r)
        snprintf(buf, (size_t)len, "x");
    return buf;
}
