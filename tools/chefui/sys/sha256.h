/* sha256.h - SHA-256 (FIPS 180-4) for the settings file checksum. */
#ifndef CHEF_SHA256_H
#define CHEF_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t n;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
/* Lower-case hex of the digest of data into hex[65]. */
void sha256_hex(const void *data, size_t len, char hex[65]);

#endif /* CHEF_SHA256_H */
