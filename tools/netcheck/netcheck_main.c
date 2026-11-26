/* btdd_netcheck: read-only check of what online play can use on this network (M13).
 *
 *   btdd_netcheck [--stun host:port] [--no-upnp] [--no-stun]
 *
 * 1. UPnP: discovers the router and asks for its public IP. Never adds, changes or removes a port mapping.
 * 2. STUN: one binding request to the STUN server (default stun.l.google.com:19302; it sees this PC's public IP),
 *    then prints the public address and port the join code would carry. Sends nothing else anywhere. */
#include "platform/udp.h"
#include "platform/upnp.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

int main(int argc, char **argv)
{
    const char *stun = "stun.l.google.com:19302";
    int do_upnp = 1, do_stun = 1, i, rc = 0;
    char ip[64], status[200], err[200];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stun") && i + 1 < argc)
            stun = argv[++i];
        else if (!strcmp(argv[i], "--no-upnp"))
            do_upnp = 0;
        else if (!strcmp(argv[i], "--no-stun"))
            do_stun = 0;
    }
    if (do_upnp) {
        upnp_probe(ip, sizeof ip, status, sizeof status);
        printf("%s\n", status);
    }
    if (do_stun) {
        Udp *u = udp_open(0, 0, err, sizeof err);
        uint32_t pip = 0, lip = 0;
        uint16_t pport = 0, lport = 0;
        int t;
        if (!u || udp_stun_start(u, stun, err, sizeof err) != 0) {
            printf("STUN: could not start (%s)\n", err);
            return 1;
        }
        for (t = 0; t < 60 && !udp_stun_result(u, &pip, &pport); t++) { /* up to 3 s */
            uint8_t buf[64];
            udp_stun_poll(u);
            { NetTransport tr = udp_transport(u); tr.recv(tr.ctx, buf, sizeof buf); } /* STUN answers are parsed in recv */
            sleep_ms(50);
        }
        udp_local_endpoint(u, &lip, &lport);
        if (pip)
            printf("STUN (%s): public endpoint %u.%u.%u.%u:%u (this PC %u.%u.%u.%u:%u)%s\n", stun, pip >> 24, pip >> 16 & 255,
                   pip >> 8 & 255, pip & 255, pport, lip >> 24, lip >> 16 & 255, lip >> 8 & 255, lip & 255, lport,
                   pip == lip ? ": no NAT" : ": behind a NAT, the join code will carry the public one");
        else {
            printf("STUN (%s): no answer in 3 s (blocked UDP or no internet); join codes will only work on the same "
                   "network\n", stun);
            rc = 1;
        }
        udp_close(u);
    }
    return rc;
}
