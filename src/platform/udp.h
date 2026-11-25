/* UDP transport for netplay (M13): non-blocking datagram socket.
 *  - Direct: the host listens on a port and locks onto the first peer whose packet carries the netplay magic; the
 *    guest sends to the address it was given.
 *  - Join code (no port forwarding): each side learns its public endpoint from a STUN server (udp_stun_*), the codes
 *    are exchanged by the players, each side adds the other's endpoints as candidates and sends to all of them (hole
 *    punching) until a packet from the peer arrives; then it locks onto that address. Works for most home NATs; not
 *    for symmetric NAT / carrier-grade NAT (then: UPnP on the host, a VPN, or later a relay). */
#ifndef BTDD_UDP_H
#define BTDD_UDP_H
#include "net/netplay.h"
#include <stddef.h>
#include <stdint.h>

typedef struct Udp Udp;

/* loopback = 1: listen on 127.0.0.1 only (tests on one PC; no firewall prompt) */
Udp *udp_host(int port, int loopback, char *err, size_t errlen);
Udp *udp_join(const char *host_port, int default_port, char *err, size_t errlen); /* "1.2.3.4:7845" or "name" */
/* A socket on an ephemeral port (0) or `port`, peer unknown, for the join-code path. */
Udp *udp_open(int port, int loopback, char *err, size_t errlen);
void udp_close(Udp *u);
NetTransport udp_transport(Udp *u);
int udp_peer_known(const Udp *u);
const char *udp_peer_text(const Udp *u, char *buf, size_t len);

/* Candidate peer endpoints (host byte order); 0 if added. */
int udp_add_candidate(Udp *u, uint32_t ip, uint16_t port);
/* Sends a small punch packet to every candidate while no peer is locked (call every frame). */
void udp_punch(Udp *u);
/* STUN: ask `server` ("host:port") for our public endpoint; results arrive through the normal receive path. */
int udp_stun_start(Udp *u, const char *server, char *err, size_t errlen);
void udp_stun_poll(Udp *u); /* resends the request now and then until answered (call every frame) */
int udp_stun_result(const Udp *u, uint32_t *ip, uint16_t *port); /* 1 when known */
/* This PC's LAN address and the socket's port (no packet is sent). */
int udp_local_endpoint(const Udp *u, uint32_t *ip, uint16_t *port);

#endif
