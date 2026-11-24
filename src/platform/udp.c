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

struct Udp {
    sock_t s;
    struct sockaddr_storage peer;
    int peer_len, peer_known, host;
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

Udp *udp_host(int port, int loopback, char *err, size_t errlen)
{
    Udp *u;
    struct sockaddr_in a;
    if (port <= 0 || port > 65535) {
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
    u->host = 1;
    u->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (u->s == BAD_SOCK || bind(u->s, (struct sockaddr *)&a, sizeof a) != 0 || nonblocking(u->s) != 0) {
        snprintf(err, errlen, "cannot listen on UDP port %d (in use?)", port);
        if (u->s != BAD_SOCK)
            close_sock(u->s);
        free(u);
        return NULL;
    }
    return u;
}

Udp *udp_join(const char *host_port, int default_port, char *err, size_t errlen)
{
    Udp *u;
    char host[256], port[16];
    const char *colon = strrchr(host_port, ':');
    struct addrinfo hints, *res = NULL;
    if (start() != 0) {
        snprintf(err, errlen, "network not available");
        return NULL;
    }
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
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(err, errlen, "cannot resolve %s", host_port);
        return NULL;
    }
    u = calloc(1, sizeof *u);
    if (!u) {
        freeaddrinfo(res);
        return NULL;
    }
    u->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u->s == BAD_SOCK || nonblocking(u->s) != 0) {
        snprintf(err, errlen, "cannot open a UDP socket");
        freeaddrinfo(res);
        free(u);
        return NULL;
    }
    memcpy(&u->peer, res->ai_addr, res->ai_addrlen);
    u->peer_len = (int)res->ai_addrlen;
    u->peer_known = 1;
    freeaddrinfo(res);
    return u;
}

void udp_close(Udp *u)
{
    if (!u)
        return;
    close_sock(u->s);
    free(u);
}

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    const struct sockaddr_in *x = (const struct sockaddr_in *)a, *y = (const struct sockaddr_in *)b;
    return x->sin_family == y->sin_family && x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}

static int u_send(void *ctx, const uint8_t *d, int len)
{
    Udp *u = ctx;
    if (!u->peer_known)
        return -1;
    return sendto(u->s, (const char *)d, len, 0, (struct sockaddr *)&u->peer, u->peer_len) == len ? 0 : -1;
}

static int u_recv(void *ctx, uint8_t *buf, int cap)
{
    Udp *u = ctx;
    for (;;) {
        struct sockaddr_storage from;
#ifdef _WIN32
        int fl = (int)sizeof from;
#else
        socklen_t fl = sizeof from;
#endif
        int n = (int)recvfrom(u->s, (char *)buf, cap, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) /* ICMP "port unreachable" from an earlier send: skip it */
                continue;
#endif
            return 0; /* nothing pending */
        }
        if (n == 0)
            return 0;
        if (!u->peer_known) {
            if (!u->host || n < 4 || buf[0] != 'B' || buf[1] != 'N')
                continue;
            memcpy(&u->peer, &from, sizeof from); /* the host locks onto its first peer */
            u->peer_len = (int)fl;
            u->peer_known = 1;
        } else if (!same_addr(&from, &u->peer)) {
            continue; /* not our peer */
        }
        return n;
    }
}

static uint32_t u_now(void *ctx)
{
    (void)ctx;
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000);
#endif
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

const char *udp_peer_text(const Udp *u, char *buf, size_t len)
{
    const struct sockaddr_in *a = (const struct sockaddr_in *)&u->peer;
    unsigned long ip = ntohl(a->sin_addr.s_addr);
    if (!u->peer_known)
        snprintf(buf, len, "-");
    else
        snprintf(buf, len, "%lu.%lu.%lu.%lu:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                 (unsigned)ntohs(a->sin_port));
    return buf;
}
