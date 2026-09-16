#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include "util.h"

int rmtfs_recv_is_retryable_error(int ret)
{
	return ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR;
}

int rmtfs_recv_requires_rebind(int ret)
{
	return ret == -ENETRESET;
}

static uint8_t to_hex(uint8_t ch)
{
	ch &= 0xf;
	return ch <= 9 ? '0' + ch : 'a' + ch - 10;
}

void print_hex_dump(const char *prefix, const void *buf, size_t len)
{
	const uint8_t *ptr = buf;
	size_t linelen;
	uint8_t ch;
	char line[16 * 3 + 16 + 1];
	size_t li;
	size_t i;
	size_t j;

	for (i = 0; i < len; i += 16) {
		linelen = MIN(16, len - i);
		li = 0;

		for (j = 0; j < linelen; j++) {
			ch = ptr[i + j];
			line[li++] = to_hex(ch >> 4);
			line[li++] = to_hex(ch);
			line[li++] = ' ';
		}

		for (; j < 16; j++) {
			line[li++] = ' ';
			line[li++] = ' ';
			line[li++] = ' ';
		}

		for (j = 0; j < linelen; j++) {
			ch = ptr[i + j];
			line[li++] = isprint(ch) ? ch : '.';
		}

		line[li] = '\0';

		printf("%s %04zx: %s\n", prefix, i, line);
	}
}
