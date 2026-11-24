/* btdd-port entry point. Milestone M0: verify the user's ROM and print each bank's vectors. */
#include "rom/rom.h"
#include <stdio.h>
#include <stdlib.h>

static uint16_t vec(const Rom *rom, int bank, uint16_t at)
{
    return (uint16_t)(rom_read(rom, bank, at) | rom_read(rom, bank, (uint16_t)(at + 1)) << 8);
}

int main(int argc, char **argv)
{
    static Rom rom;
    char hash[41] = "";
    RomResult r;
    int b;

    if (argc < 2) {
        fprintf(stderr, "usage: btdd <path to your Battletoads-Double Dragon (USA).nes>\n");
        return 2;
    }
    r = rom_load(argv[1], &rom, hash);
    if (r != ROM_OK) {
        fprintf(stderr, "ROM rejected: %s\n", rom_result_text(r));
        if (hash[0])
            fprintf(stderr, "  file SHA-1: %s\n", hash);
        return 1;
    }
    printf("ROM ok, SHA-1 %s\n", hash);
    printf("bank   NMI    RESET  IRQ\n");
    for (b = 0; b < ROM_BANKS; b++)
        printf("  %d   $%04X  $%04X  $%04X\n", b, vec(&rom, b, 0xFFFA), vec(&rom, b, 0xFFFC), vec(&rom, b, 0xFFFE));
    return EXIT_SUCCESS;
}
