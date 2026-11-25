/* Join codes for online play without port forwarding (M13): a short text a player copies to the other (chat,
 * message). It carries two UDP endpoints of the player's game: the public one (what the internet sees, learned from a
 * STUN server or the router's UPnP) and the local one (for players on the same network). Format: "BT1-" + 21 base32
 * characters in groups (13 bytes: public IPv4 + port, local IPv4 + port, checksum). No secrets in it: it only says
 * where to send packets; the session handshake still checks ROM and settings. */
#ifndef BTDD_JOINCODE_H
#define BTDD_JOINCODE_H
#include <stddef.h>
#include <stdint.h>

typedef struct JoinEndpoints {
    uint32_t public_ip, local_ip; /* host byte order; 0 = unknown */
    uint16_t public_port, local_port;
} JoinEndpoints;

#define JOINCODE_LEN 32 /* buffer size incl. the terminator */

void joincode_encode(const JoinEndpoints *e, char out[JOINCODE_LEN]);
/* Accepts the code with or without dashes/spaces, any letter case. Returns 0 if valid. */
int joincode_decode(const char *code, JoinEndpoints *e);

#endif
