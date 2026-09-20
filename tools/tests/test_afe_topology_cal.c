#define AFE_TOPOLOGY_CAL_NO_MAIN
#include "../afe-topology-cal.c"

/* The vendor kernel's UAPI header, with the two shims it needs from
 * userspace, so every hand-spelled constant/layout in the tool is checked
 * against the real definitions rather than trusted. */
#define __packed __attribute__((packed))
#include <linux/msm_audio_calibration.h>

#include <stddef.h>
#include <sys/wait.h>
#include <time.h>

static int passed;
static int failed;

static int mock_fd;
static int mock_open_errno;
static int mock_lock_errno;
static int mock_ioctl_errno_on_call;	/* 1-based ioctl call index to fail, 0 = never */
static int mock_ioctl_errno;
static int mock_close_errno;
static int mock_wait_errno;
static int mock_wait_signal;
static int open_calls;
static int lock_calls;
static int ioctl_calls;
static int wait_calls;
static int close_calls;
static int close_calls_at_wait;
static int ioctl_calls_at_wait;
static char opened_path[128];
static int opened_flags;
static unsigned long captured_request[4];
static struct afe_top_cal_msg captured_msg[4];

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
	mock_fd = 31;
	mock_open_errno = 0;
	mock_lock_errno = 0;
	mock_ioctl_errno_on_call = 0;
	mock_ioctl_errno = 0;
	mock_close_errno = 0;
	mock_wait_errno = 0;
	mock_wait_signal = SIGTERM;
	open_calls = lock_calls = ioctl_calls = wait_calls = close_calls = 0;
	close_calls_at_wait = ioctl_calls_at_wait = -1;
	opened_path[0] = '\0';
	opened_flags = 0;
	memset(captured_request, 0, sizeof(captured_request));
	memset(captured_msg, 0, sizeof(captured_msg));
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

static int mock_lock(int fd)
{
	lock_calls++;
	check(fd == mock_fd, "lock uses the opened fd");
	if (mock_lock_errno) {
		errno = mock_lock_errno;
		return -1;
	}
	return 0;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
	check(fd == mock_fd, "ioctl uses the opened fd");
	if (ioctl_calls < 4) {
		captured_request[ioctl_calls] = request;
		memcpy(&captured_msg[ioctl_calls], arg, sizeof(captured_msg[0]));
	}
	ioctl_calls++;
	if (mock_ioctl_errno_on_call && ioctl_calls == mock_ioctl_errno_on_call) {
		errno = mock_ioctl_errno;
		return -1;
	}
	return 0;
}

static int mock_wait(void)
{
	wait_calls++;
	close_calls_at_wait = close_calls;
	ioctl_calls_at_wait = ioctl_calls;
	if (mock_wait_errno) {
		errno = mock_wait_errno;
		return -1;
	}
	return mock_wait_signal;
}

static int mock_close(int fd)
{
	close_calls++;
	check(fd == mock_fd, "close uses the opened fd");
	if (mock_close_errno) {
		errno = mock_close_errno;
		return -1;
	}
	return 0;
}

static const struct afe_top_syscalls mock_calls = {
	.open_device = mock_open,
	.lock_device = mock_lock,
	.device_ioctl = mock_ioctl,
	.wait_for_signal = mock_wait,
	.close_device = mock_close,
};

/* Real sigwait, mocked device: for the fork-based residency test. */
static const struct afe_top_syscalls resident_calls = {
	.open_device = mock_open,
	.lock_device = mock_lock,
	.device_ioctl = mock_ioctl,
	.wait_for_signal = system_wait_for_signal,
	.close_device = mock_close,
};

static int run(const struct afe_top_config *config)
{
	FILE *out = tmpfile();
	FILE *errors = tmpfile();
	int rc;

	check(out != NULL && errors != NULL, "tmpfiles for output capture open");
	if (!out || !errors)
		return 99;
	rc = afe_topology_cal_run(config, &mock_calls, out, errors);
	fclose(out);
	fclose(errors);
	return rc;
}

static void put_le32(unsigned char *at, int32_t value)
{
	uint32_t u = (uint32_t)value;

	at[0] = u & 0xff;
	at[1] = (u >> 8) & 0xff;
	at[2] = (u >> 16) & 0xff;
	at[3] = (u >> 24) & 0xff;
}

static void expected_bytes(unsigned char *bytes, int32_t topology,
			   int32_t acdb_id, int32_t path, int32_t rate)
{
	/* audio_cal_afe_top, all little-endian int32: hdr{data_size 48,
	 * version 0, cal_type 23, cal_type_size 32}, cal_hdr{version 0,
	 * buffer_number 0 for RX / 1 for TX -- the kernel matches topology
	 * blocks by buffer_number only, so a TX block on buffer 0 would
	 * overwrite the RX block}, cal_data{cal_size 0, mem_handle -1},
	 * cal_info{topology, acdb_id, path, sample_rate}. */
	put_le32(bytes + 0, 48);
	put_le32(bytes + 4, 0);
	put_le32(bytes + 8, 23);
	put_le32(bytes + 12, 32);
	put_le32(bytes + 16, 0);
	put_le32(bytes + 20, path == 1 ? 1 : 0);
	put_le32(bytes + 24, 0);
	put_le32(bytes + 28, -1);
	put_le32(bytes + 32, topology);
	put_le32(bytes + 36, acdb_id);
	put_le32(bytes + 40, path);
	put_le32(bytes + 44, rate);
}

static void test_layout_against_kernel_header(void)
{
	check(sizeof(struct afe_top_cal_msg) == sizeof(struct audio_cal_afe_top) &&
	      sizeof(struct afe_top_cal_msg) == 48,
	      "message size matches the kernel's audio_cal_afe_top (48 bytes)");
	check(sizeof(struct afe_top_cal_type) == sizeof(struct audio_cal_type_afe_top) &&
	      sizeof(struct afe_top_cal_type) == 32,
	      "cal_type size matches audio_cal_type_afe_top (32 bytes)");
	check(offsetof(struct afe_top_cal_msg, cal_type) ==
	      offsetof(struct audio_cal_afe_top, cal_type),
	      "cal_type offset matches the kernel header");
	check(offsetof(struct afe_top_cal_msg, cal_type.cal_data.mem_handle) ==
	      offsetof(struct audio_cal_afe_top, cal_type.cal_data.mem_handle),
	      "mem_handle offset matches the kernel header");
	check(offsetof(struct afe_top_cal_msg, cal_type.cal_info.sample_rate) ==
	      offsetof(struct audio_cal_afe_top, cal_type.cal_info.sample_rate),
	      "sample_rate offset matches the kernel header");
	check(AFE_TOP_CAL_TYPE == AFE_TOPOLOGY_CAL_TYPE,
	      "cal type index 23 is the kernel's AFE_TOPOLOGY_CAL_TYPE");
	check(AFE_TOP_IOCTL_SET_CALIBRATION == AUDIO_SET_CALIBRATION,
	      "ioctl number matches the kernel's AUDIO_SET_CALIBRATION");
	check(AFE_TOP_IOCTL_SET_CALIBRATION == 0xC00861CBUL,
	      "ioctl number is 0xC00861CB (IOWR 'a' 203, 8-byte pointer arg)");
	check(AFE_TOP_PATH_RX == RX_DEVICE && AFE_TOP_PATH_TX == TX_DEVICE,
	      "path values are the kernel's RX_DEVICE/TX_DEVICE");
	check(AFE_TOP_VERSION_0_0 == VERSION_0_0, "version is VERSION_0_0");
	check(sizeof(struct afe_top_cal_msg) >= sizeof(struct audio_cal_basic) &&
	      sizeof(struct afe_top_cal_msg) <= MAX_IOCTL_CMD_SIZE,
	      "data_size is within the driver's accepted size window");
	check(sizeof(struct afe_top_cal_type) >= sizeof(struct audio_cal_type_basic),
	      "cal_type_size is at least the driver's minimum");
}

static void test_build_msg(void)
{
	struct afe_top_block block = { 0x000112FC, 14, AFE_TOP_PATH_RX, 48000 };
	struct afe_top_cal_msg msg;
	unsigned char want[48];

	memset(&msg, 0xAA, sizeof(msg));
	check(afe_top_build_msg(&block, &msg) == 0, "default RX block builds");
	expected_bytes(want, 0x000112FC, 14, 0, 48000);
	check(memcmp(&msg, want, sizeof(want)) == 0,
	      "RX payload is byte-exact (48 bytes, LE, cal_type 23, size 32, mem_handle -1)");

	block.topology = 0;
	check(afe_top_build_msg(&block, &msg) < 0 && errno == EINVAL,
	      "topology 0 is refused");
	block.topology = 0x80000000U;
	check(afe_top_build_msg(&block, &msg) < 0, "topology above int32 range is refused");
	block.topology = 0x000112FB;
	block.path = 2;
	check(afe_top_build_msg(&block, &msg) < 0, "path other than RX/TX is refused");
	block.path = AFE_TOP_PATH_TX;
	block.sample_rate = 7999;
	check(afe_top_build_msg(&block, &msg) < 0, "rate below 8000 is refused");
	block.sample_rate = 384001;
	check(afe_top_build_msg(&block, &msg) < 0, "rate above 384000 is refused");
	block.sample_rate = 48000;
	block.acdb_id = -1;
	check(afe_top_build_msg(&block, &msg) < 0, "negative acdb_id is refused");
	block.acdb_id = 0;
	check(afe_top_build_msg(&block, &msg) == 0, "acdb_id 0 (informational) is allowed");
	expected_bytes(want, 0x000112FB, 0, 1, 48000);
	check(memcmp(&msg, want, sizeof(want)) == 0, "TX payload is byte-exact");
	check(msg.cal_type.cal_hdr.buffer_number == 1,
	      "TX block uses buffer_number 1 so it cannot overwrite the RX block (buffer 0)");
	block.path = AFE_TOP_PATH_RX;
	check(afe_top_build_msg(&block, &msg) == 0 && msg.cal_type.cal_hdr.buffer_number == 0,
	      "RX block uses buffer_number 0");
}

static void test_parsers(void)
{
	uint32_t u;
	int32_t i;

	check(parse_hex_u32("112FC", &u) == 0 && u == 0x112FC, "hex without prefix parses");
	check(parse_hex_u32("0x000112fc", &u) == 0 && u == 0x112FC, "hex with 0x prefix parses");
	check(parse_hex_u32("", &u) < 0, "empty hex is rejected");
	check(parse_hex_u32("-1", &u) < 0, "negative hex is rejected");
	check(parse_hex_u32("112FCzz", &u) < 0, "trailing garbage hex is rejected");
	check(parse_hex_u32("100000000", &u) < 0, "hex over 32 bits is rejected");
	check(parse_dec_i32("14", &i) == 0 && i == 14, "decimal parses");
	check(parse_dec_i32("0", &i) == 0 && i == 0, "decimal zero parses");
	check(parse_dec_i32("-14", &i) < 0, "negative decimal is rejected");
	check(parse_dec_i32("2147483648", &i) < 0, "decimal over int32 is rejected");
	check(parse_dec_i32("14x", &i) < 0, "trailing garbage decimal is rejected");
	check(parse_dec_i32("0x14", &i) < 0, "hex-looking decimal is rejected");
}

static void test_defaults_and_success(void)
{
	struct afe_top_config config;
	unsigned char want[48];

	afe_top_default_config(&config);
	check(strcmp(config.device, "/dev/msm_audio_cal") == 0, "default device");
	check(config.rx.topology == 0x000112FC && config.rx.acdb_id == 14 &&
	      config.rx.path == 0 && config.rx.sample_rate == 48000,
	      "default RX block is 0x000112FC / acdb 14 / RX / 48000");
	check(config.install_tx == 0, "no TX block by default");

	reset_mock();
	check(run(&config) == 0, "default install then SIGTERM exits 0");
	check(open_calls == 1 && strcmp(opened_path, "/dev/msm_audio_cal") == 0,
	      "the calibration device is opened once");
	check((opened_flags & O_RDWR) && (opened_flags & O_CLOEXEC),
	      "device is opened read-write, close-on-exec");
	check(lock_calls == 1, "the device is locked before any ioctl");
	check(ioctl_calls == 1, "exactly one ioctl for RX-only");
	check(captured_request[0] == 0xC00861CBUL, "the one ioctl is AUDIO_SET_CALIBRATION");
	expected_bytes(want, 0x000112FC, 14, 0, 48000);
	check(memcmp(&captured_msg[0], want, 48) == 0,
	      "the ioctl carried the byte-exact default RX block");
	check(wait_calls == 1 && close_calls_at_wait == 0 && ioctl_calls_at_wait == 1,
	      "process waits for a signal after installing, with the fd still open");
	check(close_calls == 1, "fd is closed exactly once, after the signal");
}

static void test_tx_block(void)
{
	struct afe_top_config config;
	unsigned char want[48];

	afe_top_default_config(&config);
	config.install_tx = 1;
	config.tx.topology = 0x000112FB;
	config.tx.acdb_id = 4;
	config.rx.sample_rate = 16000;
	config.tx.sample_rate = 16000;
	reset_mock();
	check(run(&config) == 0, "RX + TX install succeeds");
	check(ioctl_calls == 2, "two ioctls for RX + TX");
	expected_bytes(want, 0x000112FC, 14, 0, 16000);
	check(memcmp(&captured_msg[0], want, 48) == 0, "first ioctl is the RX block");
	expected_bytes(want, 0x000112FB, 4, 1, 16000);
	check(memcmp(&captured_msg[1], want, 48) == 0, "second ioctl is the TX block");
	check(captured_msg[0].cal_type.cal_hdr.buffer_number == 0 &&
	      captured_msg[1].cal_type.cal_hdr.buffer_number == 1,
	      "RX and TX ioctls carry distinct buffer numbers (0, 1)");
	check(close_calls == 1 && wait_calls == 1, "still one wait and one close");
}

static void test_failures(void)
{
	struct afe_top_config config;

	afe_top_default_config(&config);

	reset_mock();
	mock_open_errno = ENOENT;
	check(run(&config) == 3 && lock_calls == 0 && ioctl_calls == 0 && close_calls == 0,
	      "open failure exits 3 with no lock/ioctl/close");

	reset_mock();
	mock_lock_errno = EWOULDBLOCK;
	check(run(&config) == 4 && ioctl_calls == 0 && close_calls == 1 && wait_calls == 0,
	      "a second instance (lock busy) exits 4, closes, installs nothing");

	reset_mock();
	mock_ioctl_errno_on_call = 1;
	mock_ioctl_errno = EINVAL;
	check(run(&config) == 5 && ioctl_calls == 1 && close_calls == 1 && wait_calls == 0,
	      "RX ioctl failure exits 5 after closing, without waiting");

	config.install_tx = 1;
	config.tx.topology = 0x000112FB;
	reset_mock();
	mock_ioctl_errno_on_call = 2;
	mock_ioctl_errno = EIO;
	check(run(&config) == 5 && ioctl_calls == 2 && close_calls == 1 && wait_calls == 0,
	      "TX ioctl failure exits 5 after closing (RX block freed by the close)");
	config.install_tx = 0;

	reset_mock();
	mock_wait_errno = EINTR;
	check(run(&config) == 6 && close_calls == 1,
	      "signal-wait failure still closes and exits 6");

	reset_mock();
	mock_close_errno = EIO;
	check(run(&config) == 0 && close_calls == 1,
	      "a close error after the signal is reported but exits 0 (kernel frees regardless)");

	reset_mock();
	config.rx.topology = 0;
	check(afe_topology_cal_run(&config, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "topology 0 is refused before the device is opened");
	config.rx.topology = 0x112FC;
	config.rx.path = AFE_TOP_PATH_TX;
	check(afe_topology_cal_run(&config, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "an RX block with the TX path is refused before open");
	config.rx.path = AFE_TOP_PATH_RX;
	config.install_tx = 1;
	config.tx.topology = 0;
	check(afe_topology_cal_run(&config, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "TX topology 0 is refused before open");
	config.tx.topology = 0x112FB;
	config.tx.path = AFE_TOP_PATH_RX;
	check(afe_topology_cal_run(&config, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "a TX block with the RX path is refused before open");

	reset_mock();
	config.install_tx = 0;
	config.device = NULL;
	check(afe_topology_cal_run(&config, &mock_calls, stdout, stderr) == 2 && open_calls == 0,
	      "missing device path is refused");
}

/* Residency: fork a child that runs the real signal wait against the mocked
 * device. It must still be alive well after installing, and exit 0 once
 * SIGTERM arrives. Output is captured in a temp file so the child's stdout
 * can be inspected for the released line. */
static void test_resident_until_signal(void)
{
	struct afe_top_config config;
	char path[] = "/tmp/afe-topology-cal-test.XXXXXX";
	int out_fd = mkstemp(path);
	pid_t child;
	int status;
	struct timespec pause_for = { 0, 200 * 1000 * 1000 };
	FILE *out;
	char line[256];
	int saw_resident = 0;
	int saw_released = 0;

	check(out_fd >= 0, "temp file for the child's output opens");
	if (out_fd < 0)
		return;
	afe_top_default_config(&config);
	reset_mock();

	child = fork();
	check(child >= 0, "fork for the residency test succeeds");
	if (child == 0) {
		FILE *child_out = fdopen(out_fd, "w");
		int rc;

		if (!child_out)
			_exit(98);
		setvbuf(child_out, NULL, _IOLBF, 0);
		rc = afe_topology_cal_run(&config, &resident_calls, child_out,
					  child_out);
		fclose(child_out);
		_exit(rc);
	}
	close(out_fd);
	nanosleep(&pause_for, NULL);
	check(waitpid(child, &status, WNOHANG) == 0,
	      "child is still resident 200 ms after installing (no signal yet)");
	check(kill(child, SIGTERM) == 0, "SIGTERM is delivered to the resident child");
	check(waitpid(child, &status, 0) == child, "child is reaped after SIGTERM");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "child exits 0 on SIGTERM (not killed by the signal)");

	out = fopen(path, "r");
	check(out != NULL, "child's output file reopens");
	if (out) {
		while (fgets(line, sizeof(line), out)) {
			if (strstr(line, "resident, holding /dev/msm_audio_cal open"))
				saw_resident = 1;
			if (strstr(line, "released on signal 15"))
				saw_released = 1;
		}
		fclose(out);
	}
	check(saw_resident, "child announced residency before the signal");
	check(saw_released, "child announced release on signal 15 after closing");
	unlink(path);
}

int main(void)
{
	test_layout_against_kernel_header();
	test_build_msg();
	test_parsers();
	test_defaults_and_success();
	test_tx_block();
	test_failures();
	test_resident_until_signal();

	printf("afe-topology-cal: %d/%d checks passed\n", passed, passed + failed);
	return failed ? 1 : 0;
}
