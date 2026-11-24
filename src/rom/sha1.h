#ifndef BTDD_SHA1_H
#define BTDD_SHA1_H
#include <stddef.h>
#include <stdint.h>

/* Writes the 20-byte SHA-1 digest of data[0..len) to out. */
void sha1(const uint8_t *data, size_t len, uint8_t out[20]);

#endif
