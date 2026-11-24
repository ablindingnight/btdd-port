/* Machine state (see state.h). */
#include "state.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char MAGIC[8] = {'B', 'T', 'D', 'D', 'S', 'T', 'A', 'T'};

uint32_t nes_state_fingerprint(void)
{
    uint32_t h = 2166136261u;
    size_t v[] = {sizeof(Nes), sizeof(Cpu), sizeof(Ppu), sizeof(Apu), offsetof(Nes, cpu), offsetof(Nes, ppu),
                  offsetof(Nes, ram), offsetof(Nes, prg_bank), offsetof(Nes, pad), offsetof(Nes, apu_regs),
                  offsetof(Nes, stats), offsetof(Nes, watch), offsetof(Nes, apu), offsetof(Nes, apu_on),
                  offsetof(Cpu, budget), offsetof(Cpu, cycles), offsetof(Cpu, tick_hook), offsetof(Ppu, chr),
                  offsetof(Ppu, oam), offsetof(Ppu, fb), offsetof(Ppu, line_sprites), offsetof(Ppu, sc),
                  offsetof(Ppu, oams), offsetof(Ppu, line_done), offsetof(Apu, step_timer),
                  offsetof(Apu, synth_cycle), offsetof(Apu, hp1_prev_in), offsetof(Apu, host)};
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        h = (h ^ (uint32_t)v[i]) * 16777619u;
    return h;
}

void nes_state_clear_host(Nes *n)
{
    n->rom = NULL;
    n->cpu.ctx = NULL;
    n->cpu.read = NULL;
    n->cpu.write = NULL;
    n->cpu.trap = NULL;
    n->cpu.exec_hook = NULL;
    n->cpu.exec_watch = NULL;
    n->cpu.insn_hook = NULL;
    n->cpu.vector_hook = NULL;
    n->cpu.tick_hook = NULL;
    n->ppu.cpu = NULL;
    n->ppu.line_done = NULL;
    n->ppu.line_done_user = NULL;
    memset(n->ppu.fb, 0, sizeof n->ppu.fb);
    n->apu_hook = NULL;
    n->apu_user = NULL;
    n->watch_hook = NULL;
    n->user = NULL;
    memset(n->watch, 0, sizeof n->watch);
    n->log_traps = 0;
    memset(&n->apu.host, 0, sizeof n->apu.host);
    n->apu.out = NULL;
    n->apu.out_cap = 0;
    n->apu.out_len = 0;
    n->apu.samples_dropped = 0;
}

void nes_state_adopt_host(Nes *n, const Nes *host)
{
    n->rom = host->rom;
    n->cpu.ctx = host->cpu.ctx;
    n->cpu.read = host->cpu.read;
    n->cpu.write = host->cpu.write;
    n->cpu.trap = host->cpu.trap;
    n->cpu.exec_hook = host->cpu.exec_hook;
    n->cpu.exec_watch = host->cpu.exec_watch;
    n->cpu.insn_hook = host->cpu.insn_hook;
    n->cpu.vector_hook = host->cpu.vector_hook;
    n->cpu.tick_hook = host->cpu.tick_hook;
    n->ppu.cpu = host->ppu.cpu;
    n->ppu.line_done = host->ppu.line_done;
    n->ppu.line_done_user = host->ppu.line_done_user;
    memcpy(n->ppu.fb, host->ppu.fb, sizeof n->ppu.fb);
    n->apu_hook = host->apu_hook;
    n->apu_user = host->apu_user;
    n->watch_hook = host->watch_hook;
    n->user = host->user;
    memcpy(n->watch, host->watch, sizeof n->watch);
    n->log_traps = host->log_traps;
    if (!n->apu_on || !host->apu_on) {
        n->apu = host->apu; /* nothing to carry over (or nowhere to put it) */
    } else {
        n->apu.host = host->apu.host;
        n->apu.out = host->apu.out;
        n->apu.out_cap = host->apu.out_cap;
        n->apu.out_len = host->apu.out_len;
        n->apu.samples_dropped = host->apu.samples_dropped;
        if (n->apu.sample_rate != host->apu.sample_rate) { /* output stage of the host's rate */
            n->apu.sample_rate = host->apu.sample_rate;
            n->apu.out_step = host->apu.out_step;
            n->apu.hp1_k = host->apu.hp1_k;
            n->apu.hp2_k = host->apu.hp2_k;
            n->apu.lp_k = host->apu.lp_k;
        }
    }
    n->apu_on = host->apu_on;
}

size_t nes_state_size(void)
{
    return sizeof(Nes);
}

void nes_state_capture(const Nes *nes, uint8_t *blob)
{
    static Nes t; /* blob need not be aligned for Nes */
    memcpy(&t, nes, sizeof t);
    nes_state_clear_host(&t);
    memcpy(blob, &t, sizeof t);
}

void nes_state_restore(Nes *nes, const uint8_t *blob)
{
    static Nes t; /* not reentrant; the emulator is single-threaded */
    memcpy(&t, blob, sizeof t);
    nes_state_adopt_host(&t, nes);
    memcpy(nes, &t, sizeof t);
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

int nes_state_save(const Nes *nes, uint64_t frame, const char *rom_sha1, const char *path)
{
    static Nes blob;
    uint8_t hdr[NES_STATE_HEADER] = {0};
    char tmp[1100];
    FILE *f;
    int ok;
    nes_state_capture(nes, (uint8_t *)&blob);
    memcpy(hdr, MAGIC, 8);
    put32(hdr + 8, NES_STATE_VERSION);
    put32(hdr + 12, (uint32_t)sizeof(Nes));
    put32(hdr + 16, nes_state_fingerprint());
    put32(hdr + 24, (uint32_t)frame);
    put32(hdr + 28, (uint32_t)(frame >> 32));
    hdr[32] = (uint8_t)nes->ppu.model;
    hdr[33] = (uint8_t)nes->apu_on;
    if (rom_sha1)
        memcpy(hdr + 40, rom_sha1, strlen(rom_sha1) < 40 ? strlen(rom_sha1) : 40);
    /* write to a temporary name first so a failed save never destroys the previous one */
    if (strlen(path) > sizeof tmp - 5)
        return NES_STATE_IO;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return NES_STATE_IO;
    ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(&blob, 1, sizeof blob, f) == sizeof blob;
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return NES_STATE_IO;
    }
    remove(path);
    return rename(tmp, path) == 0 ? NES_STATE_OK : NES_STATE_IO;
}

int nes_state_load(Nes *nes, uint64_t *frame, const char *rom_sha1, const char *path)
{
    static Nes blob;
    uint8_t hdr[NES_STATE_HEADER];
    FILE *f = fopen(path, "rb");
    int ok;
    if (!f)
        return NES_STATE_IO;
    ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (ok && (memcmp(hdr, MAGIC, 8) || get32(hdr + 8) != NES_STATE_VERSION ||
               get32(hdr + 12) != (uint32_t)sizeof(Nes) || get32(hdr + 16) != nes_state_fingerprint())) {
        fclose(f);
        return NES_STATE_FORMAT;
    }
    if (ok && rom_sha1 && strncmp((const char *)hdr + 40, rom_sha1, 40) != 0) {
        fclose(f);
        return NES_STATE_OTHER_ROM;
    }
    ok = ok && fread(&blob, 1, sizeof blob, f) == sizeof blob;
    fclose(f);
    if (!ok)
        return NES_STATE_FORMAT;
    nes_state_restore(nes, (const uint8_t *)&blob);
    if (frame)
        *frame = (uint64_t)get32(hdr + 24) | (uint64_t)get32(hdr + 28) << 32;
    return NES_STATE_OK;
}

const char *nes_state_error(int code)
{
    switch (code) {
    case NES_STATE_OK: return "ok";
    case NES_STATE_IO: return "cannot read or write the file";
    case NES_STATE_FORMAT: return "not a state of this version/build";
    case NES_STATE_OTHER_ROM: return "state of another ROM";
    default: return "error";
    }
}
