/* nettest: two netplay peers in one process over a simulated link, playing a 2-player movie split between them
 * (pad 1 on the host, pad 2 on the guest). Pass: on both peers the RAM after every confirmed frame equals the solo
 * run's RAM for that frame, for the whole movie, and both finish.
 *
 *   nettest ROM MOVIE.fm2 [--ping MS] [--jitter MS] [--loss PCT] [--dup PCT] [--delay N] [--frames N] [--seed N]
 *                         [--ppu old|new] [--desync-at F]
 *
 * --desync-at F corrupts the guest's RAM once at frame F (player 1's lives): the RAM checksums must report the
 * desync, the host resyncs (sends its state), and from the resync frame on both peers must equal the solo run
 * again.
 *
 * The clock is virtual (one tick = one 60 Hz display frame), so runs are reproducible for a seed. */
#include "net/netplay.h"
#include "nes/nes.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QCAP 4096

typedef struct Datagram {
    uint32_t due;
    int len;
    uint8_t data[NET_MAX_PACKET];
} Datagram;

typedef struct Queue { /* packets in flight towards one peer */
    Datagram *q;
    int count;
} Queue;

typedef struct Link {
    Queue to[2];
    uint32_t now;
    int ping, jitter, loss, dup;
    uint64_t lost, dupd;
} Link;

typedef struct End {
    Link *link;
    int me;
} End;

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void push(Link *l, int to, const uint8_t *d, int len)
{
    Queue *q = &l->to[to];
    Datagram *g;
    if (q->count == QCAP)
        return;
    g = &q->q[q->count++];
    g->due = l->now + (uint32_t)(l->ping / 2) + (l->jitter ? rnd() % (uint32_t)(l->jitter + 1) : 0);
    g->len = len;
    memcpy(g->data, d, (size_t)len);
}

static int t_send(void *ctx, const uint8_t *d, int len)
{
    End *e = ctx;
    Link *l = e->link;
    if ((int)(rnd() % 100) < l->loss) {
        l->lost++;
        return 0;
    }
    push(l, !e->me, d, len);
    if ((int)(rnd() % 100) < l->dup) {
        l->dupd++;
        push(l, !e->me, d, len);
    }
    return 0;
}

static int t_recv(void *ctx, uint8_t *buf, int cap)
{
    End *e = ctx;
    Queue *q = &e->link->to[e->me];
    int i, best = -1;
    for (i = 0; i < q->count; i++) /* earliest due packet that has arrived (jitter reorders) */
        if (q->q[i].due <= e->link->now && (best < 0 || q->q[i].due < q->q[best].due))
            best = i;
    if (best < 0 || q->q[best].len > cap)
        return 0;
    i = q->q[best].len;
    memcpy(buf, q->q[best].data, (size_t)i);
    q->q[best] = q->q[--q->count];
    return i;
}

static uint32_t t_now(void *ctx)
{
    return ((End *)ctx)->link->now;
}

/* --desync-at: the guest's emulation differs once, at that frame, whether run for the first time or re-simulated
 * (like real non-determinism would; a one-off poke would be undone by the next rollback) */
static long desync_at = -1;
static uint64_t frame_base[2];
static int desyncs_injected;
static void *guest_ctx;   /* the guest's Peer */
static int guest_resynced; /* after a resync the host's state rules: no more injection */

static void run_frame(void *ctx, Nes *nes, uint8_t p1, uint8_t p2, int resim)
{
    int who = ctx == guest_ctx;
    (void)resim;
    nes_set_pads(nes, p1, p2);
    nes_run_frame(nes);
    if (who && !guest_resynced && desync_at >= 0 && (long)(nes->ppu.frame - frame_base[1]) == desync_at + 1) {
        nes->ram[0x11] ^= 0x06; /* player 1's lives */
        desyncs_injected++;
    }
}

typedef struct Peer {
    NetSession s;
    End end;
    long confirmed, bad, first_bad;
    uint8_t *ok; /* per frame: 1 = equal to the solo run, 2 = different (re-reports after a resync overwrite) */
} Peer;

static uint32_t *solo; /* RAM CRC after each frame of the solo run */
static long solo_frames;

static void on_confirmed(void *ctx, uint32_t f, const uint8_t *ram)
{
    Peer *p = ctx;
    if ((long)f >= solo_frames)
        return;
    p->confirmed = (long)f + 1;
    p->ok[f] = net_crc32(ram, 0x800) == solo[f] ? 1 : 2;
}

int main(int argc, char **argv)
{
    static Rom rom;
    static Nes solo_nes, nes[2];
    static Peer peer[2];
    static Link link;
    char hash[41], err[256], summary[NET_SUMMARY];
    const char *rom_path = NULL, *movie_path = NULL;
    Fm2 movie = {0};
    long frames = -1, ticks = 0, i;
    int delay = 1, k, model = -1;
    long resync_frame = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ping") && i + 1 < argc)
            link.ping = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc)
            link.jitter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc)
            link.loss = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dup") && i + 1 < argc)
            link.dup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
            delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            rng = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--desync-at") && i + 1 < argc)
            desync_at = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            model = !strcmp(argv[++i], "new") ? PPU_MODEL_NEW : PPU_MODEL_OLD;
        else if (!rom_path)
            rom_path = argv[i];
        else
            movie_path = argv[i];
    }
    if (!rom_path || !movie_path) {
        fprintf(stderr, "usage: nettest ROM MOVIE.fm2 [--ping MS] [--jitter MS] [--loss PCT] [--dup PCT] [--delay N] "
                        "[--frames N] [--seed N] [--ppu old|new]\n");
        return 2;
    }
    if (rom_load(rom_path, &rom, hash) != ROM_OK || fm2_load(movie_path, &movie, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load the ROM or the movie\n");
        return 2;
    }
    if (model < 0)
        model = movie.new_ppu ? PPU_MODEL_NEW : PPU_MODEL_OLD;
    if (frames < 0 || frames > movie.count)
        frames = movie.count;

    /* solo reference */
    solo = malloc(sizeof *solo * (size_t)frames);
    nes_init(&solo_nes, &rom);
    nes_power(&solo_nes, (PpuModel)model);
    for (i = 0; i < frames; i++) {
        nes_set_pads(&solo_nes, movie.frames[i].pad[0], movie.frames[i].pad[1]);
        nes_run_frame(&solo_nes);
        solo[i] = net_crc32(solo_nes.ram, 0x800);
    }
    solo_frames = frames;

    link.to[0].q = calloc(QCAP, sizeof(Datagram));
    link.to[1].q = calloc(QCAP, sizeof(Datagram));
    snprintf(summary, sizeof summary, "rom %.12s ppu %s nettest", hash, model == PPU_MODEL_NEW ? "new" : "old");
    for (k = 0; k < 2; k++) {
        NetTransport tr;
        nes_init(&nes[k], &rom);
        nes_power(&nes[k], (PpuModel)model);
        peer[k].ok = calloc((size_t)frames, 1);
        peer[k].end.link = &link;
        peer[k].end.me = k;
        tr.ctx = &peer[k].end;
        tr.send = t_send;
        tr.recv = t_recv;
        tr.now_ms = t_now;
        frame_base[k] = nes[k].ppu.frame;
        if (k == 1)
            guest_ctx = &peer[k];
        if (net_init(&peer[k].s, &nes[k], k, delay, summary, tr, run_frame, on_confirmed, &peer[k]) != 0)
            return 1;
    }

    while (peer[0].confirmed < frames || peer[1].confirmed < frames) {
        for (k = 0; k < 2; k++) {
            NetSession *s = &peer[k].s;
            long f = (long)s->have[k]; /* the frame this tick's local input is for */
            uint8_t pad = f < movie.count ? movie.frames[f].pad[k] : 0;
            net_tick(s, pad);
        }
        if (peer[0].s.state == NET_DESYNC) /* the host offers a resync, as the player would from the menu */
            net_resync(&peer[0].s);
        if (peer[1].s.stats.resyncs)
            guest_resynced = 1;
        if (peer[0].s.stats.resyncs && !resync_frame)
            resync_frame = (long)peer[0].s.rs_frame;
        ticks++;
        link.now = (uint32_t)(ticks * 1000 / 60);
        if (ticks > frames * 4 + 10000) {
            printf("FAIL: no progress (host confirmed %ld, guest %ld of %ld)\n", peer[0].confirmed, peer[1].confirmed,
                   frames);
            return 1;
        }
    }
    for (k = 0; k < 2; k++) { /* frames that differ from the solo run (after a resync: only from its frame on) */
        long f;
        for (f = resync_frame; f < frames; f++)
            if (peer[k].ok[f] != 1) {
                if (!peer[k].bad)
                    peer[k].first_bad = f + 1;
                peer[k].bad++;
            }
    }
    for (k = 0; k < 2; k++) {
        NetStats *t = &peer[k].s.stats;
        printf("%s: %ld frames confirmed, %ld differ from the solo run (first %ld); rollbacks %llu (%llu frames "
               "re-simulated), stalls %llu, sync waits %llu, packets %llu out / %llu in, bad %llu, ping %d ms, checksums equal %u\n",
               k ? "guest" : "host ", peer[k].confirmed, peer[k].bad, peer[k].first_bad,
               (unsigned long long)t->rollbacks, (unsigned long long)t->resim_frames, (unsigned long long)t->stalls, (unsigned long long)t->sync_waits,
               (unsigned long long)t->packets_out, (unsigned long long)t->packets_in, (unsigned long long)t->bad_packets,
               t->ping_ms, t->checks_ok);
    }
    printf("link: ping %d ms, jitter %d ms, loss %d%% (%llu lost), dup %d%% (%llu); delay %d; %ld ticks for %ld frames\n",
           link.ping, link.jitter, link.loss, (unsigned long long)link.lost, link.dup, (unsigned long long)link.dupd,
           delay, ticks, frames);
    if (desync_at >= 0)
        printf("desync injected at guest frame %ld (%d time(s)); detected and resynced at frame %ld "
               "(host resyncs %u, guest %u)\n",
               desync_at, desyncs_injected, resync_frame, peer[0].s.stats.resyncs, peer[1].s.stats.resyncs);
    k = !peer[0].bad && !peer[1].bad && peer[0].s.stats.desync_frame < 0 && peer[1].s.stats.desync_frame < 0 &&
        (desync_at < 0 || (resync_frame > 0 && peer[1].s.stats.resyncs > 0));
    printf("%s\n", k ? "PASS" : "FAIL");
    fm2_free(&movie);
    return k ? 0 : 1;
}
