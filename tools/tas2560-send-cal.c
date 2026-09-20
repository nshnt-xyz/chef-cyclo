/*
 * Atomically write the TAS2560's five-value calibration mixer control.
 *
 * Alpine tinyalsa 2.0.0's tinymix writes integer-array controls one index at
 * a time. That requires a read-modify-write, but TAS2560_ALGO_CMD_SEND_CAL is
 * deliberately write-only (get = NULL), so tinymix cannot program it. This
 * helper uses the kernel ALSA control UAPI directly and submits all five
 * integers in one SNDRV_CTL_IOCTL_ELEM_WRITE. It has no alsa-lib or tinyalsa
 * dependency and is statically linked into the initramfs.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <sound/asound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TAS2560_CAL_CONTROL "TAS2560_ALGO_CMD_SEND_CAL"
#define TAS2560_CAL_COUNT 5U
#define TAS2560_MAX_CARD 255U
#define TAS2560_MAX_Q19 INT32_MAX

struct tas2560_syscalls {
	int (*open_control)(const char *path, int flags);
	int (*control_ioctl)(int fd, unsigned long request, void *arg);
	int (*close_control)(int fd);
};

static int system_open_control(const char *path, int flags)
{
	return open(path, flags);
}

static int system_control_ioctl(int fd, unsigned long request, void *arg)
{
	return ioctl(fd, request, arg);
}

static int system_close_control(int fd)
{
	return close(fd);
}

static const struct tas2560_syscalls system_calls = {
	.open_control = system_open_control,
	.control_ioctl = system_control_ioctl,
	.close_control = system_close_control,
};

static int parse_card(const char *text, unsigned int *card)
{
	char *end;
	unsigned long value;

	if (!text || !text[0] || text[0] == '-')
		return -1;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || value > TAS2560_MAX_CARD)
		return -1;
	*card = (unsigned int)value;
	return 0;
}

static int parse_q19(const char *text, long *q19)
{
	char *end;
	long value;

	if (!text || !text[0])
		return -1;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || *end || value < 1 || value > TAS2560_MAX_Q19)
		return -1;
	*q19 = value;
	return 0;
}

static int tas2560_send_calibration(unsigned int card, long q19,
				    const struct tas2560_syscalls *calls,
				    FILE *errors)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value value;
	char path[64];
	int fd;
	int saved_errno;

	if (!calls || !calls->open_control || !calls->control_ioctl ||
	    !calls->close_control || !errors || card > TAS2560_MAX_CARD ||
	    q19 < 1 || q19 > TAS2560_MAX_Q19) {
		errno = EINVAL;
		return 2;
	}

	snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
	fd = calls->open_control(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(errors, "tas2560-send-cal: cannot open %s: %s\n",
			path, strerror(errno));
		return 3;
	}

	memset(&info, 0, sizeof(info));
	info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf((char *)info.id.name, sizeof(info.id.name), "%s",
		 TAS2560_CAL_CONTROL);
	if (calls->control_ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
		saved_errno = errno;
		fprintf(errors, "tas2560-send-cal: cannot resolve control '%s': %s\n",
			TAS2560_CAL_CONTROL, strerror(saved_errno));
		(void)calls->close_control(fd);
		return 4;
	}
	if (strcmp((char *)info.id.name, TAS2560_CAL_CONTROL) != 0) {
		fprintf(errors, "tas2560-send-cal: kernel resolved unexpected control '%s'\n",
			info.id.name);
		(void)calls->close_control(fd);
		return 5;
	}
	if (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER ||
	    info.count != TAS2560_CAL_COUNT ||
	    !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
		fprintf(errors,
			"tas2560-send-cal: control '%s' has type=%u count=%u access=0x%x; expected writable integer count=5\n",
			TAS2560_CAL_CONTROL, info.type, info.count, info.access);
		(void)calls->close_control(fd);
		return 5;
	}
	if (info.value.integer.min > 0 || info.value.integer.max < q19) {
		fprintf(errors,
			"tas2560-send-cal: control '%s' range %ld..%ld cannot hold [1,%ld,0,0,0]\n",
			TAS2560_CAL_CONTROL, info.value.integer.min,
			info.value.integer.max, q19);
		(void)calls->close_control(fd);
		return 5;
	}

	memset(&value, 0, sizeof(value));
	value.id = info.id;
	value.value.integer.value[0] = 1;
	value.value.integer.value[1] = q19;
	value.value.integer.value[2] = 0;
	value.value.integer.value[3] = 0;
	value.value.integer.value[4] = 0;
	if (calls->control_ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) {
		saved_errno = errno;
		fprintf(errors, "tas2560-send-cal: atomic write of '%s' failed: %s\n",
			TAS2560_CAL_CONTROL, strerror(saved_errno));
		(void)calls->close_control(fd);
		return 6;
	}
	if (calls->close_control(fd) < 0) {
		fprintf(errors, "tas2560-send-cal: write succeeded but close failed: %s\n",
			strerror(errno));
		return 7;
	}
	return 0;
}

#ifndef TAS2560_SEND_CAL_NO_MAIN
int main(int argc, char **argv)
{
	unsigned int card;
	long q19;

	if (argc != 3) {
		fprintf(stderr, "usage: tas2560-send-cal CARD RDC_Q19\n");
		return 2;
	}
	if (parse_card(argv[1], &card) < 0) {
		fprintf(stderr, "tas2560-send-cal: invalid card '%s' (expected 0..%u)\n",
			argv[1], TAS2560_MAX_CARD);
		return 2;
	}
	if (parse_q19(argv[2], &q19) < 0) {
		fprintf(stderr,
			"tas2560-send-cal: invalid Rdc Q19 '%s' (expected 1..%d)\n",
			argv[2], TAS2560_MAX_Q19);
		return 2;
	}
	return tas2560_send_calibration(card, q19, &system_calls, stderr);
}
#endif
