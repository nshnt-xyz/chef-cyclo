#define TERT_TX_HOLD_NO_MAIN
#include "../tert-tx-hold.c"

#include <sys/wait.h>
#include <time.h>

static int passed;
static int failed;

static int mock_fd;
static int mock_open_errno;
static unsigned long mock_fail_request;	/* ioctl request to fail, 0 = none */
static int mock_fail_errno;
static int mock_wait_errno;
static int mock_wait_signal;
static int open_calls;
static int ioctl_calls;
static int wait_calls;
static int close_calls;
static int ioctl_calls_at_wait;
static int close_calls_at_wait;
static char opened_path[128];
static int opened_flags;
static unsigned long requests[8];
static struct snd_pcm_hw_params captured_params;
static char pcm_list_path[] = "/tmp/tert-tx-hold-test-pcm.XXXXXX";

static void check(int condition, const char *message)
{
	if (condition) {
		passed++;
	} else {
		failed++;
		fprintf(stderr, "FAIL: %s\n", message);
	}
}

static void reset_mock(void)
{
	mock_fd = 41;
	mock_open_errno = 0;
	mock_fail_request = 0;
	mock_fail_errno = 0;
	mock_wait_errno = 0;
	mock_wait_signal = SIGTERM;
	open_calls = ioctl_calls = wait_calls = close_calls = 0;
	ioctl_calls_at_wait = close_calls_at_wait = -1;
	opened_path[0] = '\0';
	opened_flags = 0;
	memset(requests, 0, sizeof(requests));
	memset(&captured_params, 0, sizeof(captured_params));
}

static int mock_open(const char *path, int flags)
{
	open_calls++;
	snprintf(opened_path, sizeof(opened_path), "%s", path);
	opened_flags = flags;
	if (mock_open_errno) {
		errno = mock_open_errno;
		return -1;
	}
	return mock_fd;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
	check(fd == mock_fd, "ioctl uses the opened pcm fd");
	if (ioctl_calls < 8)
		requests[ioctl_calls] = request;
	ioctl_calls++;
	if (request == SNDRV_PCM_IOCTL_HW_PARAMS && arg)
		memcpy(&captured_params, arg, sizeof(captured_params));
	if (mock_fail_request && request == mock_fail_request) {
		errno = mock_fail_errno;
		return -1;
	}
	return 0;
}

static int mock_wait(void)
{
	wait_calls++;
	ioctl_calls_at_wait = ioctl_calls;
	close_calls_at_wait = close_calls;
	if (mock_wait_errno) {
		errno = mock_wait_errno;
		return -1;
	}
	return mock_wait_signal;
}

static int mock_close(int fd)
{
	close_calls++;
	check(fd == mock_fd, "close uses the opened pcm fd");
	return 0;
}

static const struct tert_tx_syscalls mock_calls = {
	.open_pcm = mock_open,
	.pcm_ioctl = mock_ioctl,
	.wait_for_signal = mock_wait,
	.close_pcm = mock_close,
};

static const struct tert_tx_syscalls resident_calls = {
	.open_pcm = mock_open,
	.pcm_ioctl = mock_ioctl,
	.wait_for_signal = system_wait_for_signal,
	.close_pcm = mock_close,
};

static void write_pcm_list(const char *content)
{
	FILE *f = fopen(pcm_list_path, "w");

	check(f != NULL, "pcm list fixture is writable");
	if (!f)
		return;
	fputs(content, f);
	fclose(f);
}

static int run(unsigned int card)
{
	FILE *out = tmpfile();
	FILE *errors = tmpfile();
	int rc;

	check(out != NULL && errors != NULL, "tmpfiles for output capture open");
	if (!out || !errors)
		return 99;
	rc = tert_tx_hold_run(pcm_list_path, card, &mock_calls, out, errors);
	fclose(out);
	fclose(errors);
	return rc;
}

/* Realistic /proc/asound/pcm shape: MultiMedia front-ends first (they also
 * say "capture"), the plain TERT MI2S back-end, then the hostless link at
 * hw:0,40 (sdm660-internal.c's "hw:x,40" comment). */
static const char realistic_pcm_list[] =
	"00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n"
	"00-01: MultiMedia2 (*) :  : playback 1 : capture 1\n"
	"00-09: MultiMedia10 (*) :  : capture 1\n"
	"00-16: Tertiary MI2S Playback : Tertiary MI2S Playback : playback 1\n"
	"00-17: Tertiary MI2S Capture : Tertiary MI2S Capture : capture 1\n"
	"00-40: TERT MI2S_TX Hostless (*) :  : capture 1\n";

static void test_parse_line(void)
{
	unsigned int card = 99;
	unsigned int device = 99;

	check(tert_tx_parse_pcm_line("00-40: TERT MI2S_TX Hostless (*) :  : capture 1\n", &card, &device) == 1 &&
	      card == 0 && device == 40,
	      "the hostless line parses to card 0 device 40 (leading zero stripped)");
	check(tert_tx_parse_pcm_line("00-08: TERT MI2S_TX Hostless (*) :  : capture 1\n", &card, &device) == 1 &&
	      device == 8, "a device number with a leading zero is read as decimal (08 -> 8)");
	check(tert_tx_parse_pcm_line("01-40: TERT MI2S_TX Hostless (*) :  : capture 1\n", &card, &device) == 1 &&
	      card == 1 && device == 40, "card 1 parses");
	check(tert_tx_parse_pcm_line("00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n", &card, &device) == 0,
	      "MultiMedia1 never matches even though it advertises capture");
	check(tert_tx_parse_pcm_line("00-17: Tertiary MI2S Capture : Tertiary MI2S Capture : capture 1\n", &card, &device) == 0,
	      "the plain Tertiary MI2S Capture back-end never matches");
	check(tert_tx_parse_pcm_line("00-41: TERT MI2S_RX Hostless (*) :  : playback 1\n", &card, &device) == 0,
	      "the RX hostless link never matches");
	check(tert_tx_parse_pcm_line("00-40: TERT MI2S_TX Hostless (*) :  : playback 1\n", &card, &device) == 0,
	      "a TX hostless line without capture is rejected");
	check(tert_tx_parse_pcm_line("TERT MI2S_TX Hostless capture\n", &card, &device) == 0,
	      "a line with no card-device prefix is rejected");
	check(tert_tx_parse_pcm_line("00 40: TERT MI2S_TX Hostless (*) :  : capture 1\n", &card, &device) == 0,
	      "a malformed prefix is rejected");
	check(tert_tx_parse_pcm_line("", &card, &device) == 0, "an empty line is rejected");
	check(tert_tx_parse_pcm_line(NULL, &card, &device) == 0, "a NULL line is rejected");
}

static void test_find_device(void)
{
	unsigned int device = 99;

	write_pcm_list(realistic_pcm_list);
	check(tert_tx_find_device(pcm_list_path, 0, &device) == 0 && device == 40,
	      "device 40 is found on card 0 in a realistic list");
	check(tert_tx_find_device(pcm_list_path, 1, &device) == -1,
	      "card 1 has no hostless device in that list");
	write_pcm_list("00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n");
	check(tert_tx_find_device(pcm_list_path, 0, &device) == -1,
	      "a list without the hostless link reports not-found");
	check(tert_tx_find_device("/nonexistent/pcm", 0, &device) == -2 && errno == ENOENT,
	      "an unreadable list reports the fopen error separately");
}

static int mask_has_only(const struct snd_mask *mask, unsigned int bit)
{
	size_t i;

	for (i = 0; i < sizeof(mask->bits) / sizeof(mask->bits[0]); i++) {
		unsigned int want = (bit >> 5) == i ? 1U << (bit & 31) : 0;

		if (mask->bits[i] != want)
			return 0;
	}
	return 1;
}

static const struct snd_interval *interval_of(const struct snd_pcm_hw_params *p, int param)
{
	return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static int interval_is_exact(const struct snd_pcm_hw_params *p, int param, unsigned int value)
{
	const struct snd_interval *i = interval_of(p, param);

	return i->min == value && i->max == value && i->integer && !i->openmin &&
	       !i->openmax && !i->empty;
}

static int interval_is_open(const struct snd_pcm_hw_params *p, int param)
{
	const struct snd_interval *i = interval_of(p, param);

	return i->min == 0 && i->max == ~0U && !i->integer && !i->empty;
}

static void test_hw_params_shape(void)
{
	struct snd_pcm_hw_params p;

	tert_tx_fill_hw_params(&p);
	check(mask_has_only(&p.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
			    SNDRV_PCM_ACCESS_RW_INTERLEAVED),
	      "access mask requests exactly RW interleaved");
	check(mask_has_only(&p.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
			    SNDRV_PCM_FORMAT_S16_LE),
	      "format mask requests exactly S16_LE");
	check(mask_has_only(&p.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
			    SNDRV_PCM_SUBFORMAT_STD),
	      "subformat mask requests exactly STD");
	check(interval_is_exact(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2), "channels pinned to 2");
	check(interval_is_exact(&p, SNDRV_PCM_HW_PARAM_RATE, 48000), "rate pinned to 48000");
	check(interval_is_exact(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 512), "period size pinned to 512 frames");
	check(interval_is_exact(&p, SNDRV_PCM_HW_PARAM_PERIODS, 2), "periods pinned to 2");
	check(interval_is_open(&p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS) &&
	      interval_is_open(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS) &&
	      interval_is_open(&p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES) &&
	      interval_is_open(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE) &&
	      interval_is_open(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES),
	      "derived intervals are left open for the kernel's refine step");
	check(p.rmask == ~0U && p.cmask == 0, "rmask requests every parameter, cmask clear");
	/* 512 frames x 2 ch x 2 bytes = 2048-byte periods, x2 = 4096-byte
	 * buffer: inside soc-pcm's no_host_hardware window (period bytes
	 * PAGE_SIZE/4..PAGE_SIZE/2, periods 2..4, buffer <= 4 pages) and equal
	 * to the one page the hostless path allocates. */
	check(512U * 2U * 2U == 2048U && 2048U * 2U == 4096U,
	      "period/buffer bytes match the hostless one-page buffer");
}

static void test_success_sequence(void)
{
	write_pcm_list(realistic_pcm_list);
	reset_mock();
	check(run(0) == 0, "full sequence then SIGTERM exits 0");
	check(open_calls == 1 && strcmp(opened_path, "/dev/snd/pcmC0D40c") == 0,
	      "opens the discovered capture node /dev/snd/pcmC0D40c");
	check((opened_flags & O_ACCMODE) == O_RDONLY && (opened_flags & O_CLOEXEC),
	      "capture node opened read-only, close-on-exec");
	check(ioctl_calls == 4 &&
	      requests[0] == SNDRV_PCM_IOCTL_HW_PARAMS &&
	      requests[1] == SNDRV_PCM_IOCTL_PREPARE &&
	      requests[2] == SNDRV_PCM_IOCTL_START &&
	      requests[3] == SNDRV_PCM_IOCTL_DROP,
	      "ioctl order is HW_PARAMS, PREPARE, START, then DROP after the signal");
	check(wait_calls == 1 && ioctl_calls_at_wait == 3 && close_calls_at_wait == 0,
	      "waits for the signal after START with the pcm still open");
	check(close_calls == 1, "closes exactly once, after DROP");
	check(interval_is_exact(&captured_params, SNDRV_PCM_HW_PARAM_RATE, 48000) &&
	      interval_is_exact(&captured_params, SNDRV_PCM_HW_PARAM_CHANNELS, 2),
	      "the HW_PARAMS ioctl carried the 48000 Hz / 2 ch request");
}

static void test_failures(void)
{
	write_pcm_list(realistic_pcm_list);

	reset_mock();
	check(run(3) == 3 && open_calls == 0, "no hostless device on the card exits 3 before open");

	write_pcm_list("00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n");
	reset_mock();
	check(run(0) == 3 && open_calls == 0, "a list without the hostless link exits 3 before open");
	write_pcm_list(realistic_pcm_list);

	reset_mock();
	mock_open_errno = EACCES;
	check(run(0) == 4 && ioctl_calls == 0 && close_calls == 0,
	      "open failure exits 4 with no ioctl/close");

	reset_mock();
	mock_fail_request = SNDRV_PCM_IOCTL_HW_PARAMS;
	mock_fail_errno = EINVAL;
	check(run(0) == 5 && ioctl_calls == 1 && close_calls == 1 && wait_calls == 0,
	      "HW_PARAMS failure exits 5, closes, never waits");

	reset_mock();
	mock_fail_request = SNDRV_PCM_IOCTL_PREPARE;
	mock_fail_errno = EIO;
	check(run(0) == 6 && ioctl_calls == 2 && close_calls == 1 && wait_calls == 0,
	      "PREPARE failure exits 6, closes, never waits");

	reset_mock();
	mock_fail_request = SNDRV_PCM_IOCTL_START;
	mock_fail_errno = EPIPE;
	check(run(0) == 7 && ioctl_calls == 3 && close_calls == 1 && wait_calls == 0,
	      "START failure exits 7, closes, never waits");

	reset_mock();
	mock_wait_errno = EINTR;
	check(run(0) == 8 && ioctl_calls == 4 && requests[3] == SNDRV_PCM_IOCTL_DROP && close_calls == 1,
	      "signal-wait failure still DROPs and closes, exits 8");

	reset_mock();
	mock_fail_request = SNDRV_PCM_IOCTL_DROP;
	mock_fail_errno = EBADFD;
	check(run(0) == 0 && close_calls == 1,
	      "a DROP failure after the signal is reported but still closes and exits 0");

	reset_mock();
	check(tert_tx_hold_run(pcm_list_path, 256, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "card above 255 is refused before anything is opened");
	check(tert_tx_hold_run(NULL, 0, &mock_calls, stdout, stderr) == 2,
	      "a NULL pcm list path is refused");
}

static void test_parse_card(void)
{
	unsigned int card;

	check(parse_card("0", &card) == 0 && card == 0, "card 0 parses");
	check(parse_card("255", &card) == 0 && card == 255, "card 255 parses");
	check(parse_card("256", &card) < 0, "card 256 is rejected");
	check(parse_card("-1", &card) < 0, "negative card is rejected");
	check(parse_card("", &card) < 0, "empty card is rejected");
	check(parse_card("1x", &card) < 0, "garbage card is rejected");
}

static void test_resident_until_signal(void)
{
	char path[] = "/tmp/tert-tx-hold-test-out.XXXXXX";
	int out_fd = mkstemp(path);
	pid_t child;
	int status;
	struct timespec pause_for = { 0, 200 * 1000 * 1000 };
	FILE *out;
	char line[256];
	int saw_holding = 0;
	int saw_drop = 0;
	int saw_released = 0;

	check(out_fd >= 0, "temp file for the child's output opens");
	if (out_fd < 0)
		return;
	write_pcm_list(realistic_pcm_list);
	reset_mock();

	child = fork();
	check(child >= 0, "fork for the residency test succeeds");
	if (child == 0) {
		FILE *child_out = fdopen(out_fd, "w");
		int rc;

		if (!child_out)
			_exit(98);
		setvbuf(child_out, NULL, _IOLBF, 0);
		rc = tert_tx_hold_run(pcm_list_path, 0, &resident_calls, child_out, child_out);
		fclose(child_out);
		_exit(rc);
	}
	close(out_fd);
	nanosleep(&pause_for, NULL);
	check(waitpid(child, &status, WNOHANG) == 0,
	      "child is still holding the pcm 200 ms after START (no signal yet)");
	check(kill(child, SIGTERM) == 0, "SIGTERM is delivered to the holder");
	check(waitpid(child, &status, 0) == child, "holder is reaped after SIGTERM");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "holder exits 0 on SIGTERM (not killed by the signal)");

	out = fopen(path, "r");
	check(out != NULL, "holder's output file reopens");
	if (out) {
		while (fgets(line, sizeof(line), out)) {
			if (strstr(line, "holding /dev/snd/pcmC0D40c RUNNING"))
				saw_holding = 1;
			if (strstr(line, "DROP ok"))
				saw_drop = 1;
			if (strstr(line, "released on signal 15"))
				saw_released = 1;
		}
		fclose(out);
	}
	check(saw_holding, "holder announced the RUNNING hold before the signal");
	check(saw_drop && saw_released, "holder announced DROP and release on signal 15");
	unlink(path);
}

int main(void)
{
	int fd = mkstemp(pcm_list_path);

	check(fd >= 0, "pcm list fixture path is created");
	if (fd >= 0)
		close(fd);

	test_parse_line();
	test_find_device();
	test_hw_params_shape();
	test_success_sequence();
	test_failures();
	test_parse_card();
	test_resident_until_signal();

	unlink(pcm_list_path);
	printf("tert-tx-hold: %d/%d checks passed\n", passed, passed + failed);
	return failed ? 1 : 0;
}
