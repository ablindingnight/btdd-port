/* UDP transport for netplay (see udp.h). Platform code: Winsock on Windows, BSD sockets elsewhere. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif
#include "udp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CAND 4
#define STUN_MAGIC 0x2112A442u

struct Udp {
    sock_t s;
    struct sockaddr_in peer;
    int peer_known, host;
    struct sockaddr_in cand[MAX_CAND];
    int ncand;
    struct sockaddr_in stun;
    int stun_on, stun_done;
    uint32_t stun_last, stun_ip;
    uint16_t stun_port;
    uint8_t stun_tid[12];
};

static int net_started;

static int start(void)
{
#ifdef _WIN32
    if (!net_started) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
            return -1;
        net_started = 1;
    }
#else
    net_started = 1;
#endif
    return 0;
}

static int nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : -1;
#else
    return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000);
#endif
}

static Udp *open_bound(int port, int loopback, int host, char *err, size_t errlen)
{
    Udp *u;
    struct sockaddr_in a;
    if (port < 0 || port > 65535) {
        snprintf(err, errlen, "bad UDP port %d", port);
        return NULL;
    }
    if (start() != 0) {
        snprintf(err, errlen, "network not available");
        return NULL;
    }
    u = calloc(1, sizeof *u);
    if (!u)
        return NULL;
    u->host = host;
    u->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (u->s == BAD_SOCK || bind(u->s, (struct sockaddr *)&a, sizeof a) != 0 || nonblocking(u->s) != 0) {
        snprintf(err, errlen, "cannot open UDP port %d (in use?)", port);
        if (u->s != BAD_SOCK)
            close_sock(u->s);
        free(u);
        return NULL;
    }
    return u;
}

Udp *udp_host(int port, int loopback, char *err, size_t errlen)
{
    if (port <= 0) {
        snprintf(err, errlen, "bad UDP port %d", port);
        return NULL;
    }
    return open_bound(port, loopback, 1, err, errlen);
}

Udp *udp_open(int port, int loopback, char *err, size_t errlen)
{
    return open_bound(port, loopback, 0, err, errlen);
}

static int resolve(const char *host_port, int default_port, struct sockaddr_in *out)
{
    char host[256], port[16];
    const char *colon = strrchr(host_port, ':');
    struct addrinfo hints, *res = NULL;
    if (colon && (size_t)(colon - host_port) < sizeof host) {
        memcpy(host, host_port, (size_t)(colon - host_port));
        host[colon - host_port] = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    } else {
        snprintf(host, sizeof host, "%s", host_port);
        snprintf(port, sizeof port, "%d", default_port);
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}

Udp *udp_join(const char *host_port, int default_port, char *err, size_t errlen)
{
    Udp *u = open_bound(0, 0, 0, err, errlen);
    if (!u)
        return NULL;
    if (resolve(host_port, default_port, &u->peer) != 0) {
        snprintf(err, errlen, "cannot resolve %s", host_port);
        udp_close(u);
        return NULL;
    }
    u->peer_known = 1;
    return u;
}

void udp_close(Udp *u)
{
    if (!u)
        return;
    close_sock(u->s);
    free(u);
}

static int same_ip(const struct sockaddr_in *x, const struct sockaddr_in *y)
{
    return x->sin_addr.s_addr == y->sin_addr.s_addr;
}

static int same_addr(const struct sockaddr_in *x, const struct sockaddr_in *y)
{
    return same_ip(x, y) && x->sin_port == y->sin_port;
}

static void send_to(Udp *u, const struct sockaddr_in *a, const uint8_t *d, int len)
{
    sendto(u->s, (const char *)d, len, 0, (const struct sockaddr *)a, sizeof *a);
}

static int u_send(void *ctx, const uint8_t *d, int len)
{
    Udp *u = ctx;
    int i;
    if (u->peer_known) {
        send_to(u, &u->peer, d, len);
        return 0;
    }
    for (i = 0; i < u->ncand; i++) /* hole punching: every candidate until the peer answers */
        send_to(u, &u->cand[i], d, len);
    return u->ncand ? 0 : -1;
}

/* STUN binding response: take XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS) */
static void got_stun(Udp *u, const uint8_t *b, int n)
{
    int off = 20, len = b[2] << 8 | b[3];
    if (memcmp(b + 8, u->stun_tid, 12) != 0 || len + 20 > n)
        return;
    while (off + 4 <= 20 + len) {
        int type = b[off] << 8 | b[off + 1], al = b[off + 2] << 8 | b[off + 3];
        const uint8_t *v = b + off + 4;
        if (off + 4 + al > n)
            break;
        if ((type == 0x0020 || type == 0x0001) && al >= 8 && v[1] == 0x01) {
            uint16_t port = (uint16_t)(v[2] << 8 | v[3]);
            uint32_t ip = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            if (type == 0x0020) {
                port ^= (uint16_t)(STUN_MAGIC >> 16);
                ip ^= STUN_MAGIC;
            }
            u->stun_ip = ip;
            u->stun_port = port;
            u->stun_done = 1;
            if (type == 0x0020)
                return;
        }
        off += 4 + ((al + 3) & ~3);
    }
}

static int u_recv(void *ctx, uint8_t *buf, int cap)
{
    Udp *u = ctx;
    for (;;) {
        struct sockaddr_in from;
#ifdef _WIN32
        int fl = (int)sizeof from;
#else
        socklen_t fl = sizeof from;
#endif
        int n = (int)recvfrom(u->s, (char *)buf, cap, 0, (struct sockaddr *)&from, &fl), i, ok;
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) /* ICMP "port unreachable" from an earlier send: skip it */
                continue;
#endif
            return 0; /* nothing pending */
        }
        if (n == 0)
            return 0;
        if (u->stun_on && n >= 20 && buf[0] == 0x01 && buf[1] == 0x01 && same_addr(&from, &u->stun)) {
            got_stun(u, buf, n); /* our public endpoint, not a game packet */
            continue;
        }
        if (n < 4 || buf[0] != 'B' || buf[1] != 'N')
            continue;
        if (u->peer_known) {
            if (!same_addr(&from, &u->peer))
                continue; /* not our peer */
            return n;
        }
        /* not locked yet: the host accepts its first peer; otherwise only a candidate's IP (its port may differ
         * behind a NAT) */
        ok = u->host;
        for (i = 0; i < u->ncand && !ok; i++)
            ok = same_ip(&from, &u->cand[i]);
        if (!ok)
            continue;
        u->peer = from;
        u->peer_known = 1;
        return n;
    }
}

static uint32_t u_now(void *ctx)
{
    (void)ctx;
    return now_ms();
}

NetTransport udp_transport(Udp *u)
{
    NetTransport t;
    t.ctx = u;
    t.send = u_send;
    t.recv = u_recv;
    t.now_ms = u_now;
    return t;
}

int udp_peer_known(const Udp *u)
{
    return u && u->peer_known;
}

static const char *ip_text(uint32_t ip, uint16_t port, char *buf, size_t len)
{
    snprintf(buf, len, "%lu.%lu.%lu.%lu:%u", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 255),
             (unsigned long)((ip >> 8) & 255), (unsigned long)(ip & 255), (unsigned)port);
    return buf;
}

const char *udp_peer_text(const Udp *u, char *buf, size_t len)
{
    if (!u->peer_known)
        snprintf(buf, len, "-");
    else
        ip_text(ntohl(u->peer.sin_addr.s_addr), ntohs(u->peer.sin_port), buf, len);
    return buf;
}

int udp_add_candidate(Udp *u, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    int i;
    if (!ip || !port || u->ncand == MAX_CAND)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    for (i = 0; i < u->ncand; i++)
        if (same_addr(&a, &u->cand[i]))
            return 0;
    u->cand[u->ncand++] = a;
    return 0;
}

void udp_punch(Udp *u)
{
    static const uint8_t punch[4] = {'B', 'N', NET_PROTOCOL, 9}; /* type 9: ignored by the session */
    int i;
    if (u->peer_known)
        return;
    for (i = 0; i < u->ncand; i++)
        send_to(u, &u->cand[i], punch, sizeof punch);
}

static void stun_send(Udp *u)
{
    uint8_t r[20];
    r[0] = 0x00; /* binding request */
    r[1] = 0x01;
    r[2] = r[3] = 0;
    r[4] = (uint8_t)((STUN_MAGIC >> 24) & 0xFFu);
    r[5] = (uint8_t)((STUN_MAGIC >> 16) & 0xFFu);
    r[6] = (uint8_t)((STUN_MAGIC >> 8) & 0xFFu);
    r[7] = (uint8_t)(STUN_MAGIC & 0xFFu);
    memcpy(r + 8, u->stun_tid, 12);
    send_to(u, &u->stun, r, sizeof r);
    u->stun_last = now_ms();
}

int udp_stun_start(Udp *u, const char *server, char *err, size_t errlen)
{
    int i;
    uint32_t seed = now_ms() ^ (uint32_t)(uintptr_t)u;
    if (resolve(server, 3478, &u->stun) != 0) {
        snprintf(err, errlen, "cannot resolve the STUN server %s", server);
        return -1;
    }
    for (i = 0; i < 12; i++) { /* transaction id: only has to match our own request */
        seed = seed * 1103515245u + 12345u;
        u->stun_tid[i] = (uint8_t)(seed >> 16);
    }
    u->stun_on = 1;
    u->stun_done = 0;
    stun_send(u);
    return 0;
}

void udp_stun_poll(Udp *u)
{
    if (u->stun_on && !u->stun_done && now_ms() - u->stun_last > 500)
        stun_send(u);
}

int udp_stun_result(const Udp *u, uint32_t *ip, uint16_t *port)
{
    if (!u->stun_done)
        return 0;
    *ip = u->stun_ip;
    *port = u->stun_port;
    return 1;
}

int udp_local_endpoint(const Udp *u, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a, probe;
    sock_t t;
#ifdef _WIN32
    int l = (int)sizeof a;
#else
    socklen_t l = sizeof a;
#endif
    if (getsockname(u->s, (struct sockaddr *)&a, &l) != 0)
        return -1;
    *port = ntohs(a.sin_port);
    *ip = ntohl(a.sin_addr.s_addr);
    if (*ip == 0) { /* bound to all interfaces: find the LAN address by "connecting" a UDP socket (sends nothing) */
        t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        memset(&probe, 0, sizeof probe);
        probe.sin_family = AF_INET;
        probe.sin_port = htons(9);
        probe.sin_addr.s_addr = htonl(0xC0000201u); /* 192.0.2.1 (TEST-NET-1): only selects a route */
        l = sizeof a;
        if (t != BAD_SOCK && connect(t, (struct sockaddr *)&probe, sizeof probe) == 0 &&
            getsockname(t, (struct sockaddr *)&a, &l) == 0)
            *ip = ntohl(a.sin_addr.s_addr);
        if (t != BAD_SOCK)
            close_sock(t);
    }
    return 0;
}
