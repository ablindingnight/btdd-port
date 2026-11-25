/* Rollback netplay (see netplay.h, docs/M13_NETPLAY.md). */
#include "netplay.h"
#include "nes/state.h"
#include "rom/sha1.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- packets (little-endian); every one starts with 'B' 'N', protocol, type ---- */
enum { PK_HELLO = 1, PK_ACCEPT = 2, PK_REJECT = 3, PK_INPUT = 4, PK_CHECK = 5, PK_STATE = 6, PK_STATE_DONE = 7 };
#define HDR 4
#define MAX_INPUTS_PER_PACKET 64
#define INPUT_FIXED (HDR + 4 + 4 + 4 + 1 + 4 + 4 + 1) /* frame, ack, start, count, ts, echo, frame advantage */
#define HELLO_SIZE (HDR + 20 + 1 + NET_SUMMARY)
#define ACCEPT_SIZE (HDR + 20 + 1)
#define REJECT_SIZE (HDR + NET_SUMMARY)
#define CHECK_SIZE (HDR + 4 + 4)
#define RATE_LIMIT 200 /* packets per second from the peer */
#define CHUNK 1024
#define STATE_FIXED (HDR + 4 + 4 + 4 + 4 + 2) /* frame, offset, total, layout fingerprint, length */
#define DONE_SIZE (HDR + 4)

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

uint32_t net_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void header(uint8_t *p, int type)
{
    p[0] = 'B';
    p[1] = 'N';
    p[2] = NET_PROTOCOL;
    p[3] = (uint8_t)type;
}

static void send_packet(NetSession *s, const uint8_t *p, int len)
{
    if (s->tr.send(s->tr.ctx, p, len) == 0)
        s->stats.packets_out++;
}

static uint32_t now(NetSession *s)
{
    return s->tr.now_ms(s->tr.ctx);
}

/* ---- setup ---- */

int net_init(NetSession *s, Nes *nes, int local, int delay, const char *summary, NetTransport tr, NetRunFrame run,
             NetConfirmed confirmed, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->nes = nes;
    s->local = local ? 1 : 0;
    s->delay = delay < 0 ? 0 : delay > 3 ? 3 : delay;
    snprintf(s->summary, sizeof s->summary, "%s", summary);
    sha1((const uint8_t *)s->summary, strlen(s->summary), s->hash);
    s->tr = tr;
    s->run = run;
    s->confirmed = confirmed;
    s->ctx = ctx;
    s->state_size = nes_state_size();
    s->states = malloc(s->state_size * NET_STATE_RING);
    if (!s->states)
        return -1;
    s->rollback_from = UINT32_MAX;
    s->stats.desync_frame = -1;
    s->state = NET_HANDSHAKE;
    snprintf(s->message, sizeof s->message, s->local ? "connecting to the host..." : "waiting for player 2...");
    return 0;
}

void net_free(NetSession *s)
{
    free(s->states);
    free(s->rs_buf);
    free(s->rs_have);
    s->states = s->rs_buf = s->rs_have = NULL;
}

static void clear_checks(NetSession *s)
{
    memset(s->my_crc_frame, 0, sizeof s->my_crc_frame);
    memset(s->peer_crc_frame, 0, sizeof s->peer_crc_frame);
    s->stats.desync_frame = -1;
}

static void start_running(NetSession *s)
{
    int p;
    uint32_t f;
    s->state = NET_RUNNING;
    s->frame = 0;
    for (p = 0; p < 2; p++) { /* the first `delay` frames have no input on either side: both use 0 */
        for (f = 0; f < (uint32_t)s->delay; f++)
            s->in[p][f] = 0;
        s->have[p] = (uint32_t)s->delay;
    }
    s->peer_ack = 0;
    s->reported = 0;
    snprintf(s->message, sizeof s->message, "connected: %s, input delay %d", s->local ? "you are player 2" : "you are player 1",
             s->delay);
}

/* ---- receiving ---- */

static void got_input(NetSession *s, const uint8_t *p, int len)
{
    int r = !s->local, count, i;
    uint32_t start, ack;
    if (len < INPUT_FIXED)
        goto bad;
    count = p[HDR + 12];
    if (count > MAX_INPUTS_PER_PACKET || len != INPUT_FIXED + count)
        goto bad;
    start = get32(p + HDR + 8);
    ack = get32(p + HDR + 4);
    /* plausibility: within a window of what we know */
    if (start > s->have[r] + NET_INPUT_RING / 2 || ack > s->have[s->local] || start + (uint32_t)count < start)
        goto bad;
    s->peer_frame = get32(p + HDR);
    if (ack > s->peer_ack)
        s->peer_ack = ack;
    s->echo_ts = get32(p + HDR + 13);
    s->peer_adv = (int8_t)p[HDR + 21];
    {
        uint32_t echoed = get32(p + HDR + 17), t = now(s);
        if (echoed && t >= echoed && t - echoed < 5000) /* round trip, smoothed */
            s->stats.ping_ms = s->stats.ping_ms ? (s->stats.ping_ms * 7 + (int)(t - echoed)) / 8 : (int)(t - echoed);
    }
    for (i = 0; i < count; i++) {
        uint32_t f = start + (uint32_t)i;
        uint8_t v = p[INPUT_FIXED + i];
        if (f != s->have[r])
            continue; /* already have it, or a gap (the peer resends until acknowledged) */
        if (f >= s->frame + NET_INPUT_RING / 2)
            break;
        s->in[r][f % NET_INPUT_RING] = v;
        s->have[r]++;
        if (f < s->frame && s->used_remote[f % NET_INPUT_RING] != v && f < s->rollback_from)
            s->rollback_from = f; /* that frame ran with a wrong prediction */
    }
    return;
bad:
    s->stats.bad_packets++;
}

static void got_check(NetSession *s, const uint8_t *p, int len)
{
    uint32_t f, crc;
    int i;
    if (len != CHECK_SIZE) {
        s->stats.bad_packets++;
        return;
    }
    f = get32(p + HDR);
    crc = get32(p + HDR + 4);
    i = (int)((f / 30) % 8);
    s->peer_crc[i] = crc;
    s->peer_crc_frame[i] = f + 1; /* +1: 0 = empty */
    if (s->my_crc_frame[i] == f + 1) {
        if (s->my_crc[i] == crc)
            s->stats.checks_ok++;
        else if (s->stats.desync_frame < 0) {
            s->stats.desync_frame = (int)f;
            s->state = NET_DESYNC;
            snprintf(s->message, sizeof s->message, "DESYNC at frame %lu: the two games differ", (unsigned long)f);
        }
    }
}

/* forward declarations */
static uint8_t *state_at(NetSession *s, uint32_t f);

static void send_done(NetSession *s, uint32_t f)
{
    uint8_t d[DONE_SIZE];
    header(d, PK_STATE_DONE);
    put32(d + HDR, f);
    send_packet(s, d, sizeof d);
}

/* guest: a chunk of the host's state */
static void got_state(NetSession *s, const uint8_t *p, int len)
{
    uint32_t f, off, total, fp;
    int n;
    if (len < STATE_FIXED || s->local != 1)
        goto bad;
    f = get32(p + HDR);
    off = get32(p + HDR + 4);
    total = get32(p + HDR + 8);
    fp = get32(p + HDR + 12);
    n = p[HDR + 16] | p[HDR + 17] << 8;
    if (total != s->state_size || fp != nes_state_fingerprint() || n <= 0 || n > CHUNK || len != STATE_FIXED + n ||
        off % CHUNK || off + (uint32_t)n > total)
        goto bad;
    if (!s->rs_buf && !(s->rs_buf = malloc(s->state_size)))
        return;
    if (!s->rs_have && !(s->rs_have = calloc(s->state_size / CHUNK + 1, 1)))
        return;
    if (!s->rs_active || s->rs_frame != f) { /* a new resync */
        s->rs_active = 1;
        s->rs_frame = f;
        s->rs_got = 0;
        memset(s->rs_have, 0, s->state_size / CHUNK + 1);
        s->state = NET_RESYNC;
    }
    if (!s->rs_have[off / CHUNK]) {
        memcpy(s->rs_buf + off, p + STATE_FIXED, (size_t)n);
        s->rs_have[off / CHUNK] = 1;
        s->rs_got += (uint32_t)n;
    }
    if (s->rs_got == total && s->state == NET_RESYNC) { /* complete: continue from the host's state */
        nes_state_restore(s->nes, s->rs_buf);
        s->frame = f;
        s->reported = f;
        s->rollback_from = UINT32_MAX;
        if (s->have[s->local] < f)
            s->have[s->local] = f;
        clear_checks(s);
        s->state = NET_RUNNING;
        s->stats.resyncs++;
        snprintf(s->message, sizeof s->message, "resynced at frame %lu", (unsigned long)f);
        send_done(s, f);
    } else if (s->rs_got == total) { /* the host is still sending: confirm again */
        send_done(s, f);
    }
    return;
bad:
    s->stats.bad_packets++;
}

int net_resync(NetSession *s)
{
    if (s->local != 0 || (s->state != NET_DESYNC && s->state != NET_RUNNING))
        return -1;
    if (!s->rs_buf && !(s->rs_buf = malloc(s->state_size)))
        return -1;
    s->rs_frame = s->reported; /* the state at the start of this frame = after the last confirmed one */
    if (s->rs_frame == s->frame)
        nes_state_capture(s->nes, s->rs_buf);
    else
        memcpy(s->rs_buf, state_at(s, s->rs_frame), s->state_size);
    if (s->rs_frame != s->frame) { /* the host continues from that frame too */
        nes_state_restore(s->nes, s->rs_buf);
        s->frame = s->rs_frame;
        s->rollback_from = UINT32_MAX;
    }
    s->rs_next = 0;
    s->rs_active = 1;
    s->state = NET_RESYNC;
    snprintf(s->message, sizeof s->message, "resyncing: sending our game to player 2...");
    return 0;
}

static void send_state_chunks(NetSession *s)
{
    int k;
    for (k = 0; k < 8; k++) { /* 8 chunks per frame (rate limit), cycling until the guest confirms */
        uint8_t p[STATE_FIXED + CHUNK];
        uint32_t off = s->rs_next * CHUNK, n;
        if (off >= s->state_size) {
            s->rs_next = 0;
            off = 0;
        }
        n = s->state_size - off < CHUNK ? (uint32_t)(s->state_size - off) : CHUNK;
        header(p, PK_STATE);
        put32(p + HDR, s->rs_frame);
        put32(p + HDR + 4, off);
        put32(p + HDR + 8, (uint32_t)s->state_size);
        put32(p + HDR + 12, nes_state_fingerprint());
        p[HDR + 16] = (uint8_t)n;
        p[HDR + 17] = (uint8_t)(n >> 8);
        memcpy(p + STATE_FIXED, s->rs_buf + off, n);
        send_packet(s, p, (int)(STATE_FIXED + n));
        s->rs_next++;
    }
}

static void receive(NetSession *s)
{
    uint8_t p[NET_MAX_PACKET];
    int len;
    uint32_t t = now(s);
    while ((len = s->tr.recv(s->tr.ctx, p, sizeof p)) > 0) {
        if (t - s->rate_window_ms >= 1000) {
            s->rate_window_ms = t;
            s->rate_count = 0;
        }
        if (++s->rate_count > RATE_LIMIT) {
            s->stats.rate_dropped++;
            continue;
        }
        if (len < HDR || p[0] != 'B' || p[1] != 'N' || p[2] != NET_PROTOCOL) {
            s->stats.bad_packets++;
            continue;
        }
        s->stats.packets_in++;
        s->last_rx_ms = t;
        switch (p[3]) {
        case PK_HELLO: /* host side: the guest asks to join */
            if (len != HELLO_SIZE || s->local != 0) {
                s->stats.bad_packets++;
                break;
            }
            memcpy(s->peer_summary, p + HDR + 21, NET_SUMMARY);
            s->peer_summary[NET_SUMMARY - 1] = 0;
            if (memcmp(p + HDR, s->hash, 20) != 0) {
                uint8_t r[REJECT_SIZE];
                memset(r, 0, sizeof r);
                header(r, PK_REJECT);
                memcpy(r + HDR, s->summary, strlen(s->summary));
                send_packet(s, r, sizeof r);
                snprintf(s->message, sizeof s->message, "refused player 2: different settings (theirs: %.90s)",
                         s->peer_summary);
                break;
            }
            {
                uint8_t a[ACCEPT_SIZE];
                header(a, PK_ACCEPT);
                memcpy(a + HDR, s->hash, 20);
                a[HDR + 20] = (uint8_t)s->delay;
                send_packet(s, a, sizeof a);
            }
            if (s->state == NET_HANDSHAKE)
                start_running(s);
            break;
        case PK_ACCEPT: /* guest side */
            if (len != ACCEPT_SIZE || s->local != 1 || memcmp(p + HDR, s->hash, 20) != 0 || p[HDR + 20] > 3) {
                s->stats.bad_packets++;
                break;
            }
            if (s->state == NET_HANDSHAKE) {
                s->delay = p[HDR + 20]; /* the host's input delay */
                start_running(s);
            }
            break;
        case PK_REJECT:
            if (len != REJECT_SIZE || s->local != 1 || s->state != NET_HANDSHAKE) {
                s->stats.bad_packets++;
                break;
            }
            memcpy(s->peer_summary, p + HDR, NET_SUMMARY);
            s->peer_summary[NET_SUMMARY - 1] = 0;
            s->state = NET_REJECTED;
            snprintf(s->message, sizeof s->message, "the host refused: different settings. Host: %.60s / you: %.60s",
                     s->peer_summary, s->summary);
            break;
        case PK_INPUT:
            if (s->state == NET_RUNNING || s->state == NET_DESYNC || s->state == NET_RESYNC)
                got_input(s, p, len);
            break;
        case PK_CHECK:
            if (s->state == NET_RUNNING)
                got_check(s, p, len);
            break;
        case PK_STATE:
            if (s->state == NET_RUNNING || s->state == NET_DESYNC || s->state == NET_RESYNC)
                got_state(s, p, len);
            break;
        case PK_STATE_DONE: /* host: the guest has our state */
            if (len != DONE_SIZE || s->local != 0) {
                s->stats.bad_packets++;
                break;
            }
            if (s->state == NET_RESYNC && get32(p + HDR) == s->rs_frame) {
                s->state = NET_RUNNING;
                s->rs_active = 0;
                s->reported = s->rs_frame;
                clear_checks(s);
                s->stats.resyncs++;
                snprintf(s->message, sizeof s->message, "resynced at frame %lu", (unsigned long)s->rs_frame);
            }
            break;
        case 9: /* punch packet (hole punching, src/platform/udp.c): nothing to do */
            break;
        default:
            s->stats.bad_packets++;
            break;
        }
    }
}

/* ---- sending ---- */

static void send_inputs(NetSession *s)
{
    uint8_t p[INPUT_FIXED + MAX_INPUTS_PER_PACKET];
    int l = s->local, count;
    uint32_t start = s->peer_ack, i;
    if (s->have[l] - start > MAX_INPUTS_PER_PACKET)
        start = s->have[l] - MAX_INPUTS_PER_PACKET; /* the peer is far behind: newest inputs first */
    count = (int)(s->have[l] - start);
    header(p, PK_INPUT);
    put32(p + HDR, s->frame);
    put32(p + HDR + 4, s->have[!l]);
    put32(p + HDR + 8, start);
    p[HDR + 12] = (uint8_t)count;
    put32(p + HDR + 13, now(s) | 1); /* never 0 (0 = no timestamp) */
    put32(p + HDR + 17, s->echo_ts);
    {
        long adv = (long)s->frame - (long)s->peer_frame; /* how far ahead of the peer we seem to be */
        p[HDR + 21] = (uint8_t)(int8_t)(adv < -127 ? -127 : adv > 127 ? 127 : adv);
    }
    for (i = 0; i < (uint32_t)count; i++)
        p[INPUT_FIXED + i] = s->in[l][(start + i) % NET_INPUT_RING];
    send_packet(s, p, INPUT_FIXED + count);
}

static void send_hello(NetSession *s)
{
    uint8_t p[HELLO_SIZE];
    memset(p, 0, sizeof p);
    header(p, PK_HELLO);
    memcpy(p + HDR, s->hash, 20);
    p[HDR + 20] = (uint8_t)s->delay;
    memcpy(p + HDR + 21, s->summary, strlen(s->summary));
    send_packet(s, p, sizeof p);
}

/* ---- frames ---- */

static uint8_t *state_at(NetSession *s, uint32_t f)
{
    return s->states + (size_t)(f % NET_STATE_RING) * s->state_size;
}

static uint8_t remote_input(NetSession *s, uint32_t f)
{
    int r = !s->local;
    if (f < s->have[r])
        return s->in[r][f % NET_INPUT_RING];
    return s->have[r] ? s->in[r][(s->have[r] - 1) % NET_INPUT_RING] : 0; /* prediction: repeat the last */
}

static void run_one(NetSession *s, uint32_t f, int resim)
{
    uint8_t loc = s->in[s->local][f % NET_INPUT_RING], rem = remote_input(s, f);
    nes_state_capture(s->nes, state_at(s, f));
    s->used_remote[f % NET_INPUT_RING] = rem;
    if (s->local == 0)
        s->run(s->ctx, s->nes, loc, rem, resim);
    else
        s->run(s->ctx, s->nes, rem, loc, resim);
}

static void rollback(NetSession *s)
{
    uint32_t f, from = s->rollback_from;
    s->rollback_from = UINT32_MAX;
    if (from >= s->frame)
        return;
    nes_state_restore(s->nes, state_at(s, from));
    for (f = from; f < s->frame; f++)
        run_one(s, f, 1);
    s->stats.rollbacks++;
    s->stats.resim_frames += s->frame - from;
    s->stats.last_rollback = (int)(s->frame - from);
}

static void report_confirmed(NetSession *s)
{
    uint32_t upto = s->have[!s->local] < s->frame ? s->have[!s->local] : s->frame;
    for (; s->reported < upto; s->reported++) {
        uint32_t f = s->reported;
        const uint8_t *ram = f + 1 == s->frame ? s->nes->ram : state_at(s, f + 1) + offsetof(Nes, ram);
        if (s->confirmed)
            s->confirmed(s->ctx, f, ram);
        if (f % 30 == 29) { /* RAM checksum every 30 confirmed frames */
            uint8_t p[CHECK_SIZE];
            uint32_t crc = net_crc32(ram, 0x800);
            int i = (int)((f / 30) % 8);
            s->my_crc[i] = crc;
            s->my_crc_frame[i] = f + 1;
            header(p, PK_CHECK);
            put32(p + HDR, f);
            put32(p + HDR + 4, crc);
            send_packet(s, p, sizeof p);
            if (s->peer_crc_frame[i] == f + 1) {
                if (s->peer_crc[i] == crc)
                    s->stats.checks_ok++;
                else if (s->stats.desync_frame < 0) {
                    s->stats.desync_frame = (int)f;
                    s->state = NET_DESYNC;
                    snprintf(s->message, sizeof s->message, "DESYNC at frame %lu: the two games differ", (unsigned long)f);
                }
            }
        }
    }
}

int net_tick(NetSession *s, uint8_t local_pad)
{
    int l = s->local;
    receive(s);
    if (s->state == NET_HANDSHAKE) {
        uint32_t t = now(s);
        if (s->local == 1 && t - s->last_hello_ms >= 250) {
            send_hello(s);
            s->last_hello_ms = t;
        }
        return 0;
    }
    if (s->state == NET_RESYNC) {
        if (s->local == 0)
            send_state_chunks(s);
        send_inputs(s); /* keep the input stream (and its acks) going */
        return 0;
    }
    if (s->state != NET_RUNNING) {
        if (s->state == NET_DESYNC)
            send_inputs(s);
        return 0;
    }
    /* this display frame's local input is for frame `frame + delay` */
    if (s->have[l] <= s->frame + (uint32_t)s->delay && s->have[l] - s->peer_ack < NET_INPUT_RING / 2) {
        s->in[l][s->have[l] % NET_INPUT_RING] = local_pad;
        s->have[l]++;
    }
    if (s->rollback_from != UINT32_MAX)
        rollback(s);
    send_inputs(s);
    report_confirmed(s);
    if (s->state != NET_RUNNING)
        return 0;
    /* advance, unless too far ahead of the remote input or the local input is not there yet */
    if (s->frame >= s->have[l] || s->frame >= s->have[!l] + NET_MAX_ROLLBACK) {
        s->stats.stalls++;
        return 0;
    }
    { /* time sync (GGPO's frame advantage): the side that runs ahead waits a display frame now and then */
        long mine = (long)s->frame - (long)s->peer_frame;
        if (s->frame % 10 == 0 && (mine - s->peer_adv) / 2 >= 1 && s->frame != s->last_sync_wait) {
            s->last_sync_wait = s->frame;
            s->stats.sync_waits++;
            return 0;
        }
    }
    run_one(s, s->frame, 0);
    s->frame++;
    report_confirmed(s);
    return 1;
}

const char *net_status(const NetSession *s, char *buf, size_t len)
{
    if (s->state == NET_RUNNING)
        snprintf(buf, len, "P%d ping %d ms, rollback %d, delay %d", s->local + 1, s->stats.ping_ms, s->stats.last_rollback,
                 s->delay);
    else
        snprintf(buf, len, "%s", s->message);
    return buf;
}
