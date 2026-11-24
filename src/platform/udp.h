/* UDP transport for netplay (M13): non-blocking datagram socket. The host listens on a port and locks onto the
 * first peer whose packet carries the netplay magic; all other senders are ignored. The guest sends to the address
 * it was given and accepts packets from that address only. */
#ifndef BTDD_UDP_H
#define BTDD_UDP_H
#include "net/netplay.h"
#include <stddef.h>

typedef struct Udp Udp;

/* loopback = 1: listen on 127.0.0.1 only (tests on one PC; no firewall prompt) */
Udp *udp_host(int port, int loopback, char *err, size_t errlen);
Udp *udp_join(const char *host_port, int default_port, char *err, size_t errlen); /* "1.2.3.4:7845" or "name" */
void udp_close(Udp *u);
NetTransport udp_transport(Udp *u);
int udp_peer_known(const Udp *u);
const char *udp_peer_text(const Udp *u, char *buf, size_t len);

#endif
