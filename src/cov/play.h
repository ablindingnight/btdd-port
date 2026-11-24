/* Play helpers for coverage runs: stage warp, god mode, RAM pokes.
 *
 * Warp: the game starts every stage through bank 0 $8312 (`sta $10; sta $0D`, A = level ID; found
 * 2026-11-23 with --watch on the 2P TAS). When that store to $000D happens, the warp replaces $0010 and
 * $000D with the wanted level and, if a section is given, presets $0015 (bank 6 sub_a0fb only looks up
 * the level's first section when $0015 is 0). Applies to the first stage start only unless warp_every.
 *
 * God mode: after every frame, players' HP ($051B/$051C) is refilled to 47 (the TAS's full value) when
 * it is between 1 and 46, and lives ($0011/$0012) are set back to 3 when lower.
 */
#ifndef BTDD_PLAY_H
#define BTDD_PLAY_H
#include "nes/nes.h"

#define PLAY_MAX_POKES 32

typedef struct PlayPoke {
    long frame;              /* applied after this many frames have run (0 = before the first) */
    uint16_t addr;
    uint8_t value;
    uint8_t every_frame;     /* 1 = keep it frozen from that frame on */
} PlayPoke;

typedef struct Play {
    int warp_level;          /* -1 = no warp */
    int warp_section;        /* -1 = keep the level's own first section */
    int warp_every;
    int warps_done;
    int god;
    PlayPoke pokes[PLAY_MAX_POKES];
    int npokes;
    void (*next_watch)(Nes *nes, uint16_t addr, uint8_t value);
    uint8_t user_watch[0x800]; /* addresses the caller wants forwarded to next_watch */
} Play;

void play_init(Play *p);
/* "11=00,12=00@900" or "16=02@700" or "51B=2F@900+" (+ = freeze); returns -1 on a syntax error */
int play_parse_pokes(Play *p, const char *spec);
void play_install(Play *p, Nes *nes);           /* takes over nes->watch_hook (chains the old one) */
void play_after_frame(Play *p, Nes *nes, long frames_done);

#endif
