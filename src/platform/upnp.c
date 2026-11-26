/* UPnP-IGD port mapping (see upnp.h), through miniupnpc. */
#include "upnp.h"
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#endif

static struct UPNPUrls urls;
static struct IGDdatas data;
static int have_urls, mapped_port;

/* miniupnpc UPNP_GetValidIGD results 2-4 in plain words */
static const char *igd_text(int r)
{
    switch (r) {
    case 2: return "the router has no public address itself (two routers in a row, or the provider's CGNAT)";
    case 3: return "the router says it is not connected to the internet";
    case 4: return "only other UPnP devices answered (TV, media box), not the router";
    default: return "no usable internet gateway";
    }
}

/* miniupnpc uses sockets: on Windows they need WSAStartup first (the game's UDP socket usually did it already) */
static void net_up(void)
{
#ifdef _WIN32
    static int done;
    WSADATA w;
    if (!done && WSAStartup(MAKEWORD(2, 2), &w) == 0)
        done = 1;
#endif
}


int upnp_map(int port, char *ext_ip, size_t ext_len, char *status, size_t status_len)
{
    struct UPNPDev *devs;
    char lan[64] = "", wan[64] = "", ext[40] = "", portstr[8];
    int err = 0, r;
    ext_ip[0] = 0;
    upnp_unmap();
    net_up();
    devs = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &err);
    if (!devs) {
        snprintf(status, status_len, "UPnP: no router answered (UPnP may be off)");
        return -1;
    }
    r = UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof lan, wan, sizeof wan);
    freeUPNPDevlist(devs);
    if (r <= 0) {
        snprintf(status, status_len, "UPnP: no usable internet gateway found");
        return -1;
    }
    have_urls = 1;
    if (r != 1) { /* found a device, but not a connected internet gateway */
        snprintf(status, status_len, "UPnP: %s; using the join code instead", igd_text(r));
        upnp_unmap();
        return -1;
    }
    snprintf(portstr, sizeof portstr, "%d", port);
    r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portstr, portstr, lan, "Battletoads-DD port",
                            "UDP", NULL, "0");
    if (r != UPNPCOMMAND_SUCCESS) {
        snprintf(status, status_len, "UPnP: the router refused to open UDP port %d (code %d)", port, r);
        upnp_unmap();
        return -1;
    }
    mapped_port = port;
    if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext) == UPNPCOMMAND_SUCCESS && ext[0])
        snprintf(ext_ip, ext_len, "%s", ext);
    snprintf(status, status_len, "UPnP opened UDP port %d on the router%s%s", port, ext[0] ? " (public IP " : "",
             ext[0] ? ext : "");
    if (ext[0])
        snprintf(status + strlen(status), status_len - strlen(status), ")");
    return 0;
}

void upnp_unmap(void)
{
    if (have_urls && mapped_port) {
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%d", mapped_port);
        UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, portstr, "UDP", NULL);
    }
    mapped_port = 0;
    if (have_urls)
        FreeUPNPUrls(&urls);
    have_urls = 0;
}

int upnp_probe(char *ext_ip, size_t ext_len, char *status, size_t status_len)
{
    struct UPNPDev *devs;
    struct UPNPUrls u;
    struct IGDdatas d;
    char lan[64] = "", wan[64] = "", ext[40] = "";
    int err = 0, r;
    ext_ip[0] = 0;
    net_up();
    devs = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &err);
    if (!devs) {
        snprintf(status, status_len, "router supports UPnP: no answer (UPnP is off or not supported)");
        return 0;
    }
    memset(&u, 0, sizeof u);
    r = UPNP_GetValidIGD(devs, &u, &d, lan, sizeof lan, wan, sizeof wan);
    freeUPNPDevlist(devs);
    if (r <= 0) {
        snprintf(status, status_len, "router supports UPnP: no (a UPnP device answered, but no internet gateway)");
        return 0;
    }
    if (r == 1 && UPNP_GetExternalIPAddress(u.controlURL, d.first.servicetype, ext) == UPNPCOMMAND_SUCCESS && ext[0])
        snprintf(ext_ip, ext_len, "%s", ext);
    if (r == 1)
        snprintf(status, status_len, "router supports UPnP: yes (this PC %s, public IP %s)", lan, ext[0] ? ext : "not given");
    else
        snprintf(status, status_len, "router supports UPnP: no (%s)", igd_text(r));
    FreeUPNPUrls(&u);
    return r == 1;
}
