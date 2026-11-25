/* Join-code path test (M13), on one PC.
 *
 *   joincode_test ROM 2P.fm2 [--frames N]
 *
 * 1. Join codes: encode/decode round trip; a changed character is rejected.
 * 2. Two real UDP sockets on 127.0.0.1. Each side's join code carries a fake public endpoint (TEST-NET addresses,
 *    unreachable, as a NAT's public side would be from here) and its real local one. Both paste each other's code
 *    and punch: they must lock onto the working endpoints, finish the handshake, and play the 2P movie split between
 *    them with every confirmed frame equal to the solo run. */
#include "net/joincode.h"
#include "net/netplay.h"
#include "platform/udp.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failed |= !ok;
}

static void run_frame(void *ctx, Nes *nes, uint8_t p1, uint8_t p2, int resim)
{
    (void)ctx;
    (void)resim;
    nes_set_pads(nes, p1, p2);
    nes_run_frame(nes);
}

static uint32_t *solo;
static long frames, confirmed[2], bad[2];

static void on_confirmed(void *ctx, uint32_t f, const uint8_t *ram)
{
    int k = (int)(intptr_t)ctx;
    if ((long)f >= frames)
        return;
    confirmed[k] = (long)f + 1;
    if (net_crc32(ram, 0x800) != solo[f])
        bad[k]++;
}

int main(int argc, char **argv)
{
    static Rom rom;
    static Nes solo_nes, nes[2];
    static NetSession s[2];
    Udp *u[2];
    JoinEndpoints e[2], d;
    char code[2][JOINCODE_LEN], text[300], err[200], hash[41], summary[NET_SUMMARY];
    Fm2 movie = {0};
    long i, guard;
    int k;

    frames = 1500;
    if (argc < 3) {
        fprintf(stderr, "usage: joincode_test ROM 2P.fm2 [--frames N]\n");
        return 2;
    }
    for (i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 10);
    if (rom_load(argv[1], &rom, hash) != ROM_OK || fm2_load(argv[2], &movie, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load the ROM or the movie\n");
        return 2;
    }
    if (frames > movie.count)
        frames = movie.count;

    /* 1: codes */
    e[0].public_ip = 0xCB007107u; /* 203.0.113.7 */
    e[0].public_port = 51234;
    e[0].local_ip = 0xC0A8010Au; /* 192.168.1.10 */
    e[0].local_port = 7845;
    joincode_encode(&e[0], code[0]);
    snprintf(text, sizeof text, "join code %s decodes to the same endpoints", code[0]);
    check(joincode_decode(code[0], &d) == 0 && !memcmp(&d, &e[0], sizeof d), text);
    {
        char low[JOINCODE_LEN], *p;
        snprintf(low, sizeof low, "%s", code[0]);
        for (p = low; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p = (char)(*p + 32);
        check(joincode_decode(low, &d) == 0 && !memcmp(&d, &e[0], sizeof d), "lower case and dashes accepted");
        snprintf(low, sizeof low, "%s", code[0]);
        low[6] = low[6] == 'A' ? 'B' : 'A';
        check(joincode_decode(low, &d) != 0, "a changed character is rejected (checksum)");
    }

    /* 2: two sockets on 127.0.0.1 */
    solo = malloc(sizeof *solo * (size_t)frames);
    nes_init(&solo_nes, &rom);
    nes_power(&solo_nes, PPU_MODEL_OLD);
    for (i = 0; i < frames; i++) {
        nes_set_pads(&solo_nes, movie.frames[i].pad[0], movie.frames[i].pad[1]);
        nes_run_frame(&solo_nes);
        solo[i] = net_crc32(solo_nes.ram, 0x800);
    }
    u[0] = udp_host(7849, 1, err, sizeof err);
    u[1] = u[0] ? udp_open(0, 1, err, sizeof err) : NULL;
    if (!u[0] || !u[1]) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    for (k = 0; k < 2; k++) {
        uint32_t ip;
        uint16_t port;
        udp_local_endpoint(u[k], &ip, &port);
        e[k].local_ip = ip;
        e[k].local_port = port;
        e[k].public_ip = k ? 0xC6336409u : 0xCB007107u; /* 198.51.100.9 / 203.0.113.7: unreachable on purpose */
        e[k].public_port = (uint16_t)(port + 1000);
        joincode_encode(&e[k], code[k]);
    }
    for (k = 0; k < 2; k++) { /* each side pastes the other's code */
        JoinEndpoints o;
        if (joincode_decode(code[!k], &o) != 0)
            check(0, "decode the other side's code");
        udp_add_candidate(u[k], o.public_ip, o.public_port);
        udp_add_candidate(u[k], o.local_ip, o.local_port);
    }
    snprintf(summary, sizeof summary, "joincode-test rom %.12s", hash);
    for (k = 0; k < 2; k++) {
        nes_init(&nes[k], &rom);
        nes_power(&nes[k], PPU_MODEL_OLD);
        if (net_init(&s[k], &nes[k], k, 1, summary, udp_transport(u[k]), run_frame, on_confirmed,
                     (void *)(intptr_t)k) != 0)
            return 1;
    }
    for (guard = 0; (confirmed[0] < frames || confirmed[1] < frames) && guard < 2000000; guard++) {
        for (k = 0; k < 2; k++) {
            uint32_t f = s[k].have[k];
            udp_punch(u[k]);
            net_tick(&s[k], f < (uint32_t)movie.count ? movie.frames[f].pad[k] : 0);
        }
    }
    {
        char a[64], b[64];
        snprintf(text, sizeof text, "codes %s / %s: connected (host locked onto %s, guest onto %s)", code[0], code[1],
                 udp_peer_text(u[0], a, sizeof a), udp_peer_text(u[1], b, sizeof b));
        check(s[0].state == NET_RUNNING && s[1].state == NET_RUNNING, text);
    }
    snprintf(text, sizeof text, "%ld/%ld frames confirmed on both, %ld/%ld differ from the solo run", confirmed[0],
             confirmed[1], bad[0], bad[1]);
    check(confirmed[0] >= frames && confirmed[1] >= frames && !bad[0] && !bad[1], text);
    for (k = 0; k < 2; k++) {
        net_free(&s[k]);
        udp_close(u[k]);
    }
    printf("%s\n", failed ? "FAIL" : "PASS");
    fm2_free(&movie);
    return failed;
}
