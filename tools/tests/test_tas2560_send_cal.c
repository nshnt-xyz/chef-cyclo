#define TAS2560_SEND_CAL_NO_MAIN
#include "../tas2560-send-cal.c"

static int passed;
static int failed;
static int mock_open_errno;
static int mock_info_errno;
static int mock_write_errno;
static int mock_close_errno;
static int mock_fd;
static int open_calls;
static int info_calls;
static int write_calls;
static int close_calls;
static char opened_path[64];
static int opened_flags;
static unsigned int mock_type;
static unsigned int mock_count;
static unsigned int mock_access;
static long mock_min;
static long mock_max;
static const char *mock_resolved_name;
static struct snd_ctl_elem_value captured_value;

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
	mock_open_errno = 0;
	mock_info_errno = 0;
	mock_write_errno = 0;
	mock_close_errno = 0;
	mock_fd = 23;
	open_calls = 0;
	info_calls = 0;
	write_calls = 0;
	close_calls = 0;
	opened_path[0] = '\0';
	opened_flags = 0;
	mock_type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	mock_count = TAS2560_CAL_COUNT;
	mock_access = SNDRV_CTL_ELEM_ACCESS_WRITE;
	mock_min = 0;
	mock_max = INT32_MAX;
	mock_resolved_name = TAS2560_CAL_CONTROL;
	memset(&captured_value, 0, sizeof(captured_value));
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
	check(fd == mock_fd, "ioctl uses the opened control fd");
	if (request == SNDRV_CTL_IOCTL_ELEM_INFO) {
		struct snd_ctl_elem_info *info = arg;

		info_calls++;
		check(info->id.iface == SNDRV_CTL_ELEM_IFACE_MIXER,
		      "lookup requests the mixer interface");
		check(strcmp((char *)info->id.name, TAS2560_CAL_CONTROL) == 0,
		      "lookup uses the exact TAS2560 control name");
		if (mock_info_errno) {
			errno = mock_info_errno;
			return -1;
		}
		info->id.numid = 77;
		snprintf((char *)info->id.name, sizeof(info->id.name), "%s",
			 mock_resolved_name);
		info->type = mock_type;
		info->count = mock_count;
		info->access = mock_access;
		info->value.integer.min = mock_min;
		info->value.integer.max = mock_max;
		return 0;
	}
	if (request == SNDRV_CTL_IOCTL_ELEM_WRITE) {
		write_calls++;
		captured_value = *(struct snd_ctl_elem_value *)arg;
		if (mock_write_errno) {
			errno = mock_write_errno;
			return -1;
		}
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

static int mock_close(int fd)
{
	close_calls++;
	check(fd == mock_fd, "close uses the opened control fd");
	if (mock_close_errno) {
		errno = mock_close_errno;
		return -1;
	}
	return 0;
}

static const struct tas2560_syscalls mock_calls = {
	.open_control = mock_open,
	.control_ioctl = mock_ioctl,
	.close_control = mock_close,
};

static int run_send(unsigned int card, long q19)
{
	FILE *errors = tmpfile();
	int rc;

	check(errors != NULL, "tmpfile for error capture opens");
	if (!errors)
		return 99;
	rc = tas2560_send_calibration(card, q19, &mock_calls, errors);
	fclose(errors);
	return rc;
}

int main(void)
{
	unsigned int card;
	long q19;
	int rc;

	check(parse_card("0", &card) == 0 && card == 0, "card 0 parses");
	check(parse_card("255", &card) == 0 && card == 255, "maximum card parses");
	check(parse_card("", &card) < 0, "empty card is rejected");
	check(parse_card("-1", &card) < 0, "negative card is rejected");
	check(parse_card("256", &card) < 0, "out-of-range card is rejected");
	check(parse_card("0x1", &card) < 0, "non-decimal card is rejected");
	check(parse_q19("3712528", &q19) == 0 && q19 == 3712528,
	      "valid Q19 parses");
	check(parse_q19("0", &q19) < 0, "zero Q19 is rejected");
	check(parse_q19("-1", &q19) < 0, "negative Q19 is rejected");
	check(parse_q19("2147483648", &q19) < 0, "overflowing Q19 is rejected");
	check(parse_q19("1x", &q19) < 0, "garbage Q19 is rejected");

	reset_mock();
	rc = run_send(7, 3712528);
	check(rc == 0, "valid calibration succeeds");
	check(open_calls == 1 && strcmp(opened_path, "/dev/snd/controlC7") == 0,
	      "the selected card control device is opened");
	check((opened_flags & O_RDWR) != 0 && (opened_flags & O_CLOEXEC) != 0,
	      "control device is opened read-write and close-on-exec");
	check(info_calls == 1 && write_calls == 1 && close_calls == 1,
	      "one lookup, one atomic write, and one close occur");
	check(captured_value.id.numid == 77 &&
	      strcmp((char *)captured_value.id.name, TAS2560_CAL_CONTROL) == 0,
	      "write uses the resolved control id");
	check(captured_value.value.integer.value[0] == 1 &&
	      captured_value.value.integer.value[1] == 3712528 &&
	      captured_value.value.integer.value[2] == 0 &&
	      captured_value.value.integer.value[3] == 0 &&
	      captured_value.value.integer.value[4] == 0,
	      "one ioctl carries exactly [1,q19,0,0,0]");

	reset_mock();
	mock_open_errno = EACCES;
	check(run_send(0, 1) == 3 && info_calls == 0 && close_calls == 0,
	      "open failure is reported without ioctl or close");

	reset_mock();
	mock_info_errno = ENOENT;
	check(run_send(0, 1) == 4 && write_calls == 0 && close_calls == 1,
	      "lookup failure is reported and closed without a write");

	reset_mock();
	mock_resolved_name = "wrong control";
	check(run_send(0, 1) == 5 && write_calls == 0,
	      "an unexpectedly resolved name is rejected");

	reset_mock();
	mock_type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	check(run_send(0, 1) == 5 && write_calls == 0,
	      "non-integer metadata is rejected");

	reset_mock();
	mock_count = 4;
	check(run_send(0, 1) == 5 && write_calls == 0,
	      "non-five-value metadata is rejected");

	reset_mock();
	mock_access = SNDRV_CTL_ELEM_ACCESS_READ;
	check(run_send(0, 1) == 5 && write_calls == 0,
	      "non-writable metadata is rejected");

	reset_mock();
	mock_min = 1;
	check(run_send(0, 1) == 5 && write_calls == 0,
	      "metadata that cannot hold zero is rejected");

	reset_mock();
	mock_max = 3712527;
	check(run_send(0, 3712528) == 5 && write_calls == 0,
	      "metadata that cannot hold Q19 is rejected");

	reset_mock();
	mock_write_errno = EIO;
	check(run_send(0, 3712528) == 6 && write_calls == 1 && close_calls == 1,
	      "atomic write failure is reported and closed");

	reset_mock();
	mock_close_errno = EIO;
	check(run_send(0, 3712528) == 7 && write_calls == 1 && close_calls == 1,
	      "close failure after a write is reported");

	reset_mock();
	check(tas2560_send_calibration(256, 1, &mock_calls, stderr) == 2 &&
	      open_calls == 0, "direct card range validation precedes open");
	check(tas2560_send_calibration(0, 0, &mock_calls, stderr) == 2 &&
	      open_calls == 0, "direct Q19 range validation precedes open");

	printf("tas2560-send-cal: %d/%d checks passed\n", passed,
	       passed + failed);
	return failed ? 1 : 0;
}
