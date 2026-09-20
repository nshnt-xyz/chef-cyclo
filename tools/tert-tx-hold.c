/*
 * Hold the TERT_MI2S_TX AFE port (0x1005) open through the kernel's own
 * hostless capture front-end, until signalled.
 *
 * The TAS2560 smart-amp module on the ADSP refuses to open its RX side until
 * the TX (voltage/current feedback) port is open ("smartamp RX: Can not open
 * RX port until TX port is open"), and stock's kernel log shows port 4101
 * being started right before the FF readback that returns 1. This build's
 * speaker-test-tone never starts port 0x1005. Stock's mixer_paths.xml does
 * not show how its HAL does it, so rather than guess a HAL route this helper
 * uses the machine driver's "TERT MI2S_TX Hostless" DAI link
 * (kernel/sound/soc/msm/sdm660-internal.c, cpu dai TERT_MI2S_TX_HOSTLESS,
 * platform msm-pcm-hostless, no_host_mode): the DAPM route
 * {"TERT_MI2S_UL_HL", NULL, "TERT_MI2S_TX"} (msm-pcm-routing-v2.c) has no
 * mixer switch, so merely opening, configuring and starting that capture PCM
 * makes DPCM bring the TERT_MI2S_TX back-end -- and with it AFE port 0x1005
 * -- up. No mixer control is touched. No audio data is ever read: the
 * hostless platform has no pointer callback, so the stream sits RUNNING with
 * an idle one-page buffer for as long as it is held.
 *
 * Sequence, each step printed with its result: discover the capture device
 * from /proc/asound/pcm (never a hardcoded device number -- msm_int_dai[]'s
 * order is not ABI), open /dev/snd/pcmC<card>D<dev>c, HW_PARAMS 48000 Hz /
 * 2 ch / S16_LE / RW interleaved / 512-frame periods x 2 (that is one
 * 4096-byte buffer, inside soc-pcm's no_host_hardware window of 1-2 KiB
 * periods, 2-4 periods), PREPARE, START, then block in sigwait until
 * SIGTERM/SIGINT/SIGHUP, then DROP and close. Closing the PCM tears the
 * back-end down (afe_close on 0x1005) whether or not DROP was reached, so
 * an unexpected death of this process also releases the port.
 *
 * Uses only the kernel ALSA PCM UAPI (<sound/asound.h>), no tinyalsa or
 * alsa-lib; statically linked into the initramfs like tas2560-send-cal.
 * Its match rule and ioctl order are host-tested with injected boundaries
 * in tools/tests/test_tert_tx_hold.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sound/asound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TERT_TX_PCM_LIST "/proc/asound/pcm"
#define TERT_TX_LINK_NAME "TERT MI2S_TX Hostless"
#define TERT_TX_MAX_CARD 255U
#define TERT_TX_RATE 48000U
#define TERT_TX_CHANNELS 2U
#define TERT_TX_FORMAT SNDRV_PCM_FORMAT_S16_LE
#define TERT_TX_PERIOD_FRAMES 512U
#define TERT_TX_PERIODS 2U

struct tert_tx_syscalls {
	int (*open_pcm)(const char *path, int flags);
	int (*pcm_ioctl)(int fd, unsigned long request, void *arg);
	/* Blocks until a termination signal; returns its number or -1/errno. */
	int (*wait_for_signal)(void);
	int (*close_pcm)(int fd);
};

static int system_open_pcm(const char *path, int flags)
{
	return open(path, flags);
}

static int system_pcm_ioctl(int fd, unsigned long request, void *arg)
{
	return ioctl(fd, request, arg);
}

static int system_wait_for_signal(void)
{
	sigset_t set;
	int sig;
	int rc;

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
		return -1;
	rc = sigwait(&set, &sig);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return sig;
}

static int system_close_pcm(int fd)
{
	return close(fd);
}

static const struct tert_tx_syscalls system_calls = {
	.open_pcm = system_open_pcm,
	.pcm_ioctl = system_pcm_ioctl,
	.wait_for_signal = system_wait_for_signal,
	.close_pcm = system_close_pcm,
};

/* Parses one /proc/asound/pcm line (e.g. "00-40: TERT MI2S_TX Hostless (*) :  :
 * capture 1"). Returns 1 and stores the card and device numbers when the line is the
 * hostless TERT_MI2S_TX capture link, 0 otherwise. The name must contain
 * TERT_TX_LINK_NAME exactly (so "MultiMedia1" or the plain "Tertiary MI2S
 * Capture" back-end never match) and the line must advertise capture. */
static int tert_tx_parse_pcm_line(const char *line, unsigned int *card,
				  unsigned int *device)
{
	const char *colon;
	const char *name_start;
	const char *device_start;
	char *end;
	unsigned long c;
	unsigned long d;

	if (!line || !card || !device)
		return 0;
	colon = strchr(line, ':');
	if (!colon)
		return 0;
	name_start = colon + 1;
	if (!strstr(name_start, TERT_TX_LINK_NAME))
		return 0;
	if (!strstr(name_start, "capture"))
		return 0;
	errno = 0;
	c = strtoul(line, &end, 10);
	if (errno || end == line || *end != '-')
		return 0;
	device_start = end + 1;
	d = strtoul(device_start, &end, 10);
	if (errno || end == device_start || *end != ':' || end != colon)
		return 0;
	if (c > TERT_TX_MAX_CARD || d > INT_MAX)
		return 0;
	*card = (unsigned int)c;
	*device = (unsigned int)d;
	return 1;
}

/* Scans the pcm list for the hostless capture device on the given card.
 * Returns 0 and stores the device number, -1 if the list had no matching
 * line, or -2 (errno set) if the list could not be opened. */
static int tert_tx_find_device(const char *pcm_list, unsigned int card,
			       unsigned int *device)
{
	FILE *list;
	char line[512];
	unsigned int line_card;
	unsigned int line_device;
	int found = 0;

	list = fopen(pcm_list, "r");
	if (!list)
		return -2;
	while (fgets(line, sizeof(line), list)) {
		if (tert_tx_parse_pcm_line(line, &line_card, &line_device) &&
		    line_card == card) {
			*device = line_device;
			found = 1;
			break;
		}
	}
	fclose(list);
	return found ? 0 : -1;
}

static void mask_set_only(struct snd_pcm_hw_params *params, int param,
			  unsigned int bit)
{
	struct snd_mask *mask = &params->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[bit >> 5] |= 1U << (bit & 31);
}

static void interval_set_exact(struct snd_pcm_hw_params *params, int param,
			       unsigned int value)
{
	struct snd_interval *interval =
		&params->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	interval->min = value;
	interval->max = value;
	interval->openmin = 0;
	interval->openmax = 0;
	interval->integer = 1;
	interval->empty = 0;
}

/* The fixed hostless capture configuration, in the same "everything open,
 * then pin what we care about" shape tinyalsa's pcm_open uses. */
static void tert_tx_fill_hw_params(struct snd_pcm_hw_params *params)
{
	size_t i;

	memset(params, 0, sizeof(*params));
	for (i = 0; i < sizeof(params->masks) / sizeof(params->masks[0]); i++) {
		params->masks[i].bits[0] = ~0U;
		params->masks[i].bits[1] = ~0U;
	}
	for (i = 0; i < sizeof(params->intervals) / sizeof(params->intervals[0]); i++) {
		params->intervals[i].min = 0;
		params->intervals[i].max = ~0U;
	}
	params->rmask = ~0U;
	params->cmask = 0;
	params->info = 0;

	mask_set_only(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set_only(params, SNDRV_PCM_HW_PARAM_FORMAT, TERT_TX_FORMAT);
	mask_set_only(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set_exact(params, SNDRV_PCM_HW_PARAM_CHANNELS, TERT_TX_CHANNELS);
	interval_set_exact(params, SNDRV_PCM_HW_PARAM_RATE, TERT_TX_RATE);
	interval_set_exact(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, TERT_TX_PERIOD_FRAMES);
	interval_set_exact(params, SNDRV_PCM_HW_PARAM_PERIODS, TERT_TX_PERIODS);
}

static int step(int fd, unsigned long request, void *arg, const char *name,
		const struct tert_tx_syscalls *calls, FILE *out, FILE *errors)
{
	int saved_errno;

	if (calls->pcm_ioctl(fd, request, arg) < 0) {
		saved_errno = errno;
		fprintf(errors, "tert-tx-hold: %s failed: %s\n", name,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	fprintf(out, "tert-tx-hold: %s ok\n", name);
	return 0;
}

/* Exit codes: 2 bad arguments, 3 pcm list unreadable or no hostless
 * TERT_MI2S_TX capture device on the card, 4 open, 5 HW_PARAMS, 6 PREPARE,
 * 7 START, 8 signal wait; 0 once a termination signal led to DROP + close.
 * Every failure after open closes the PCM before returning. */
static int tert_tx_hold_run(const char *pcm_list, unsigned int card,
			    const struct tert_tx_syscalls *calls, FILE *out,
			    FILE *errors)
{
	struct snd_pcm_hw_params params;
	char path[64];
	unsigned int device;
	int fd;
	int sig;
	int saved_errno;

	if (!pcm_list || card > TERT_TX_MAX_CARD || !calls || !calls->open_pcm ||
	    !calls->pcm_ioctl || !calls->wait_for_signal || !calls->close_pcm ||
	    !out || !errors) {
		errno = EINVAL;
		return 2;
	}

	switch (tert_tx_find_device(pcm_list, card, &device)) {
	case 0:
		break;
	case -1:
		fprintf(errors, "tert-tx-hold: no '%s' capture device on card %u in %s (is the sdm660 card up?)\n",
			TERT_TX_LINK_NAME, card, pcm_list);
		return 3;
	default:
		fprintf(errors, "tert-tx-hold: cannot read %s: %s\n", pcm_list,
			strerror(errno));
		return 3;
	}
	snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%uc", card, device);
	fprintf(out, "tert-tx-hold: '%s' is card %u device %u (%s)\n",
		TERT_TX_LINK_NAME, card, device, path);

	fd = calls->open_pcm(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(errors, "tert-tx-hold: cannot open %s: %s\n", path,
			strerror(errno));
		return 4;
	}

	tert_tx_fill_hw_params(&params);
	if (step(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params,
		 "HW_PARAMS 48000 Hz / 2 ch / S16_LE / 512 x 2 frames", calls,
		 out, errors) < 0) {
		(void)calls->close_pcm(fd);
		return 5;
	}
	if (step(fd, SNDRV_PCM_IOCTL_PREPARE, NULL, "PREPARE", calls, out,
		 errors) < 0) {
		(void)calls->close_pcm(fd);
		return 6;
	}
	if (step(fd, SNDRV_PCM_IOCTL_START, NULL, "START", calls, out,
		 errors) < 0) {
		(void)calls->close_pcm(fd);
		return 7;
	}

	fprintf(out, "tert-tx-hold: holding %s RUNNING (TERT_MI2S_TX back-end up); SIGTERM/SIGINT drops and closes it\n",
		path);
	fflush(out);
	sig = calls->wait_for_signal();
	if (sig < 0) {
		saved_errno = errno;
		fprintf(errors, "tert-tx-hold: signal wait failed: %s; dropping and closing\n",
			strerror(saved_errno));
		(void)step(fd, SNDRV_PCM_IOCTL_DROP, NULL, "DROP", calls, out, errors);
		(void)calls->close_pcm(fd);
		return 8;
	}
	(void)step(fd, SNDRV_PCM_IOCTL_DROP, NULL, "DROP", calls, out, errors);
	if (calls->close_pcm(fd) < 0)
		fprintf(errors, "tert-tx-hold: close after signal %d failed: %s\n",
			sig, strerror(errno));
	fprintf(out, "tert-tx-hold: released on signal %d (%s closed)\n", sig,
		path);
	return 0;
}

static int parse_card(const char *text, unsigned int *card)
{
	char *end;
	unsigned long value;

	if (!text || !text[0] || text[0] == '-')
		return -1;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || value > TERT_TX_MAX_CARD)
		return -1;
	*card = (unsigned int)value;
	return 0;
}

#ifndef TERT_TX_HOLD_NO_MAIN
static void usage(FILE *to)
{
	fprintf(to,
		"usage: tert-tx-hold [-D CARD] [-P PCM_LIST]\n"
		"  -D CARD      ALSA card number (default 0)\n"
		"  -P PCM_LIST  pcm list to discover the device from (default %s)\n"
		"Opens the '%s' capture PCM, configures and starts it so the\n"
		"TERT_MI2S_TX back-end (AFE port 0x1005) comes up, then stays resident;\n"
		"SIGTERM/SIGINT drops and closes it. Never touches mixer controls.\n",
		TERT_TX_PCM_LIST, TERT_TX_LINK_NAME);
}

int main(int argc, char **argv)
{
	const char *pcm_list = TERT_TX_PCM_LIST;
	unsigned int card = 0;
	int opt;

	while ((opt = getopt(argc, argv, "D:P:h")) != -1) {
		switch (opt) {
		case 'D':
			if (parse_card(optarg, &card) < 0) {
				fprintf(stderr, "tert-tx-hold: invalid -D '%s' (expected 0..%u)\n",
					optarg, TERT_TX_MAX_CARD);
				return 2;
			}
			break;
		case 'P':
			pcm_list = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind != argc) {
		usage(stderr);
		return 2;
	}
	return tert_tx_hold_run(pcm_list, card, &system_calls, stdout, stderr);
}
#endif
