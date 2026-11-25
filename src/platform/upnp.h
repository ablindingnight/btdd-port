/* UPnP-IGD port mapping for hosting an online game (M13): asks the router to forward a UDP port to this PC, so the
 * host needs no manual port forwarding. Uses miniupnpc (BSD licence, fetched by CMake). Optional: routers often have
 * UPnP off, then hosting falls back to the join code (hole punching) or a VPN. */
#ifndef BTDD_UPNP_H
#define BTDD_UPNP_H
#include <stddef.h>

/* Maps external UDP `port` -> this PC's `port`. Returns 0 on success; ext_ip receives the router's public IP.
 * `status` receives a plain-language line either way. Blocks up to ~2 s (discovery). */
int upnp_map(int port, char *ext_ip, size_t ext_len, char *status, size_t status_len);
void upnp_unmap(void); /* removes the mapping made by upnp_map, if any */

#endif
