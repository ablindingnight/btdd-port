/* Minimal .zip reading for "pick your ROM": the first *.nes entry (stored or deflate). */
#ifndef BTDD_LAUNCHER_UNZIP_H
#define BTDD_LAUNCHER_UNZIP_H
#include <stddef.h>
#include <stdint.h>

/* Returns 0 and a malloc'd buffer (free it) on success; -1 not a zip / damaged, -2 unsupported entry,
 * -3 no .nes inside. `name` receives the entry's file name. */
int unzip_first_nes(const uint8_t *zip, size_t len, uint8_t **out, size_t *out_len, char *name, size_t name_len);

/* number of *.nes entries in the zip, or -1 if it is not a readable zip */
int unzip_count_nes(const uint8_t *zip, size_t len);

#endif
