#include "fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Gamepad field: 8 characters "RLDUTSBA"; any character other than '.' or ' ' means pressed. */
static int parse_pad(const char *s, const char *end, uint8_t *out)
{
    int i;
    uint8_t v = 0;
    if (end - s < 8)
        return -1;
    for (i = 0; i < 8; i++)
        if (s[i] != '.' && s[i] != ' ')
            v |= (uint8_t)(0x80 >> i);
    *out = v;
    return 0;
}

static int header_int(const char *line, const char *key, int *out)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) == 0 && line[n] == ' ') {
        *out = atoi(line + n + 1);
        return 1;
    }
    return 0;
}

int fm2_load(const char *path, Fm2 *m, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    char line[512];
    long cap = 0;

    memset(m, 0, sizeof *m);
    if (!f) {
        snprintf(err, (size_t)errlen, "cannot open %s", path);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '|') {
            /* |cmd|port0|port1|port2| */
            char *p = line + 1, *bar;
            Fm2Frame fr = {0};
            int port;
            fr.cmd = (uint8_t)strtol(p, &bar, 10);
            if (*bar != '|')
                goto bad;
            p = bar + 1;
            for (port = 0; port < 2; port++) {
                char *e = strchr(p, '|');
                if (!e)
                    goto bad;
                if (e > p && parse_pad(p, e, &fr.pad[port]) != 0)
                    goto bad;
                p = e + 1;
            }
            if (m->count == cap) {
                Fm2Frame *nf;
                cap = cap ? cap * 2 : 65536;
                nf = realloc(m->frames, (size_t)cap * sizeof *nf);
                if (!nf) {
                    snprintf(err, (size_t)errlen, "out of memory");
                    fclose(f);
                    fm2_free(m);
                    return -1;
                }
                m->frames = nf;
            }
            m->frames[m->count++] = fr;
        } else {
            header_int(line, "NewPPU", &m->new_ppu);
            header_int(line, "palFlag", &m->pal);
            header_int(line, "fourscore", &m->fourscore);
            if (strncmp(line, "romChecksum ", 12) == 0) {
                snprintf(m->rom_checksum, sizeof m->rom_checksum, "%.*s",
                         (int)strcspn(line + 12, "\r\n"), line + 12);
            }
        }
    }
    fclose(f);
    return 0;
bad:
    snprintf(err, (size_t)errlen, "%s: bad input line %ld", path, m->count + 1);
    fclose(f);
    fm2_free(m);
    return -1;
}

void fm2_free(Fm2 *m)
{
    free(m->frames);
    m->frames = NULL;
    m->count = 0;
}
