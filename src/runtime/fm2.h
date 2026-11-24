/* FCEUX .fm2 movie reader (text format: header lines, then one "|cmd|port0|port1|port2|" line per frame). */
#ifndef BTDD_FM2_H
#define BTDD_FM2_H
#include <stdint.h>

enum { FM2_CMD_RESET = 1, FM2_CMD_POWER = 2 };

typedef struct Fm2Frame {
    uint8_t cmd;
    uint8_t pad[2]; /* NES button bits: A=0x01 B=0x02 Select=0x04 Start=0x08 Up Down Left Right=0x80 */
} Fm2Frame;

typedef struct Fm2 {
    Fm2Frame *frames;
    long count;
    int new_ppu;      /* header "NewPPU 1" */
    int pal;
    int fourscore;
    char rom_checksum[64];
} Fm2;

/* Returns 0 on success; on failure writes a message to err (errlen bytes). */
int fm2_load(const char *path, Fm2 *movie, char *err, int errlen);
void fm2_free(Fm2 *movie);

#endif
