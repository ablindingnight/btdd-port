/* UPnP-IGD port mapping (see upnp.h), through miniupnpc. */
#include "upnp.h"
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <stdio.h>
#include <string.h>

static struct UPNPUrls urls;
static struct IGDdatas data;
static int have_urls, mapped_port;

int upnp_map(int port, char *ext_ip, size_t ext_len, char *status, size_t status_len)
{
    struct UPNPDev *devs;
    char lan[64] = "", wan[64] = "", ext[40] = "", portstr[8];
    int err = 0, r;
    ext_ip[0] = 0;
    upnp_unmap();
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
        snprintf(status, status_len, "UPnP: the router is not connected to the internet through UPnP");
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
