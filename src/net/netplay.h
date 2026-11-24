/* M13: rollback netplay for two players (docs/M13_NETPLAY.md). GGPO's model, own code: only inputs are exchanged;
 * the remote input of a frame not received yet is predicted (repeat the last one); when the real one differs, the
 * state of that frame is restored and the frames since are re-simulated.
 *
 * Platform independent: no SDL, no sockets. The caller provides a transport (send/receive datagrams; real UDP in
 * src/platform/udp.c, or the simulated link of tools/nettest) and a frame runner (sets the pads and runs one frame;
 * `resim` tells it the frame is a re-simulation: no sound, no picture needed). */
#ifndef BTDD_NETPLAY_H
#define BTDD_NETPLAY_H
#include "nes/nes.h"
#include <stddef.h>
#include <stdint.h>

#define NET_PROTOCOL 1
#define NET_MAX_PACKET 1200
#define NET_INPUT_RING 128  /* frames of inputs kept */
#define NET_STATE_RING 16   /* frames of machine states kept (>= NET_MAX_ROLLBACK + 1) */
#define NET_MAX_ROLLBACK 8  /* furthest a peer runs ahead of the last confirmed remote input */
#define NET_SUMMARY 96

typedef struct NetTransport {
    void *ctx;
    int (*send)(void *ctx, const uint8_t *data, int len);   /* 0 on success */
    int (*recv)(void *ctx, uint8_t *buf, int cap);          /* bytes received, 0 = nothing pending */
    uint32_t (*now_ms)(void *ctx);                           /* a millisecond clock */
} NetTransport;

typedef void (*NetRunFrame)(void *ctx, Nes *nes, uint8_t pad1, uint8_t pad2, int resim);
typedef void (*NetConfirmed)(void *ctx, uint32_t frame, const uint8_t *ram); /* RAM after a confirmed frame */

enum NetState { NET_IDLE, NET_HANDSHAKE, NET_RUNNING, NET_REJECTED, NET_DESYNC, NET_RESYNC };

typedef struct NetStats {
    uint64_t rollbacks, resim_frames, stalls, sync_waits, packets_in, packets_out, bad_packets, rate_dropped;
    int last_rollback;     /* frames re-simulated by the last rollback */
    int ping_ms;           /* round trip, smoothed */
    int desync_frame;      /* -1 = none */
    uint32_t checks_ok;    /* RAM checksums compared equal */
    uint32_t resyncs;      /* completed resyncs */
} NetStats;

typedef struct NetSession {
    int state;
    int local;                          /* 0 = host (player 1), 1 = guest (player 2) */
    int delay;                          /* input delay in frames (0-3) */
    uint8_t hash[20];                   /* session hash (ROM, build, settings) */
    char summary[NET_SUMMARY];          /* the same, readable */
    char peer_summary[NET_SUMMARY];
    char message[160];                  /* last status / error for the user */
    Nes *nes;
    NetTransport tr;
    NetRunFrame run;
    NetConfirmed confirmed;
    void *ctx;

    uint32_t frame;                     /* next frame to run */
    uint8_t in[2][NET_INPUT_RING];      /* inputs by frame (frame % ring) */
    uint32_t have[2];                   /* frames < have[p] have player p's real input */
    uint8_t used_remote[NET_INPUT_RING];/* the remote input each frame ran with */
    uint32_t rollback_from;             /* earliest frame to redo, or UINT32_MAX */
    uint32_t peer_ack;                  /* the peer has our inputs for frames < peer_ack */
    uint32_t peer_frame;                /* the peer's frame counter, last reported */
    uint32_t reported;                  /* confirmed frames reported so far */
    uint8_t *states;                    /* NET_STATE_RING blobs */
    size_t state_size;
    uint32_t my_crc[8], my_crc_frame[8], peer_crc[8], peer_crc_frame[8];
    uint32_t last_hello_ms, last_rx_ms, rate_window_ms, rate_count;
    uint32_t echo_ts;                   /* the peer's newest timestamp, echoed back */
    int peer_adv;                       /* the peer's frame advantage over us, as it sees it */
    uint32_t last_sync_wait;
    /* resync: the host's state of frame rs_frame, sent in chunks (host) / assembled (guest) */
    uint8_t *rs_buf, *rs_have;
    uint32_t rs_frame, rs_next, rs_got;
    int rs_active;
    NetStats stats;
} NetSession;

/* summary: readable session settings (ROM SHA-1, build, PPU model, CPU path, logic mods); the hash is its SHA-1. */
int net_init(NetSession *s, Nes *nes, int local, int delay, const char *summary, NetTransport tr, NetRunFrame run,
             NetConfirmed confirmed, void *ctx);
void net_free(NetSession *s);
/* Call once per display frame with this frame's local pad bits. Returns 1 if a new frame was run, 0 if waiting
 * (handshake, stall, rejected). */
int net_tick(NetSession *s, uint8_t local_pad);
const char *net_status(const NetSession *s, char *buf, size_t len); /* one line for the overlay */
/* Host, after a desync: send our state of the last confirmed frame to the guest; both continue from there. The
 * state is the machine state only (RAM, CHR-RAM, registers), checked by the guest like a save state. */
int net_resync(NetSession *s);

uint32_t net_crc32(const uint8_t *p, size_t n);

#endif
