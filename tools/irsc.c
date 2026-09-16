/* irsc: unblock the AF_MSM_IPC IPC-router security gate for the whole
 * system (must run once as root before any QMI client sendto(), see
 * gps-userspace-handoff.md 2.2 and msmipc_irsc() in msmipc.c). Idempotent.
 *
 * Usage: irsc   (no arguments; exit 0 on success)
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "msmipc.h"

int main(void)
{
	if (msmipc_irsc() < 0) {
		fprintf(stderr, "irsc: CONFIG_SEC_RULES failed: %s\n", strerror(errno));
		return 1;
	}

	return 0;
}
