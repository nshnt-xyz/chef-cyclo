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

	/* Topology 0 on an RX block is the NEUTRALISE payload (-N): it is a
	 * legal message, byte-identical to an install except for the topology
	 * word. The kernel's afe_get_cal_topology_id() then rejects the block
	 * (`if (!afe_top_info->topology)`) and afe_send_port_topology_id()
	 * sends nothing at the next port start. An install may still never
	 * carry topology 0 -- that is enforced by block_is_valid() and by the
	 * CLI, checked below. */
	block.topology = 0;
	memset(&msg, 0xAA, sizeof(msg));
	check(afe_top_build_msg(&block, &msg) == 0,
	      "topology 0 on an RX block builds (the neutralise payload)");
	expected_bytes(want, 0, 14, 0, 48000);
	check(memcmp(&msg, want, sizeof(want)) == 0,
	      "neutralise payload is byte-exact and differs from an install only in the topology word");
	check(!block_is_valid(&block),
	      "topology 0 is still refused as an INSTALL");
	block.path = AFE_TOP_PATH_TX;
	check(!neutralise_block_is_valid(&block),
	      "a TX block may not be neutralised (buffer 1 is not what the speaker port matches)");
	block.path = AFE_TOP_PATH_RX;
	block.topology = 0x000112FC;
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
	config.sentinel = "";
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
	config.sentinel = "";
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

/* -N: the explicit neutralisation. One SET of topology 0 on buffer 0 (RX),
 * then exit -- no sigwait, because there is nothing to hold up and closing
 * frees nothing. The probe uses this to make its restored phase genuine
 * instead of relying on close, which the 2026-09-22 live run disproved. */
static void test_neutralise(void)
{
	struct afe_top_config config;
	unsigned char want[48];

	afe_top_default_config(&config);
	config.sentinel = "";
	config.neutralise = 1;
	config.rx.topology = 0;

	reset_mock();
	check(run(&config) == 0, "neutralise returns 0");
	check(open_calls == 1 && lock_calls == 1,
	      "neutralise opens and locks the device like an install");
	check(ioctl_calls == 1, "neutralise issues exactly one SET ioctl");
	check(captured_request[0] == 0xC00861CBUL,
	      "neutralise uses AUDIO_SET_CALIBRATION, never DEALLOCATE (dealloc=NULL makes that a no-op)");
	expected_bytes(want, 0, 14, 0, 48000);
	check(memcmp(&captured_msg[0], want, 48) == 0,
	      "neutralise writes topology 0 to buffer 0 on the RX path");
	check(wait_calls == 0,
	      "neutralise does NOT wait for a signal -- there is nothing to hold");
	check(close_calls == 1, "neutralise closes the fd before returning");

	/* Guard rails: neutralise is RX-only and carries no topology. */
	afe_top_default_config(&config);
	config.sentinel = "";
	config.neutralise = 1;
	config.rx.topology = 0;
	config.install_tx = 1;
	config.tx.topology = 0x000112FB;
	reset_mock();
	check(run(&config) == 2 && ioctl_calls == 0,
	      "neutralise with a TX block is refused before the device is opened");

	afe_top_default_config(&config);
	config.sentinel = "";
	config.neutralise = 1;
	config.rx.topology = 0x000112FC;
	reset_mock();
	check(run(&config) == 2 && ioctl_calls == 0,
	      "neutralise with a non-zero topology is refused (that would be an install)");

	afe_top_default_config(&config);
	config.sentinel = "";
	config.neutralise = 1;
	config.rx.topology = 0;
	config.rx.sample_rate = 1;
	reset_mock();
	check(run(&config) == 2 && ioctl_calls == 0,
	      "neutralise still range-checks the sample rate");
}

/* F9: the helper records the install itself, so the fact that this boot has
 * a topology block survives the probe being killed, this helper being
 * killed, or the helper being run by hand. Written only AFTER a SET
 * succeeds -- unlike the probe's deliberately conservative pre-latch, here
 * we know whether the ioctl returned. Best-effort: an unwritable path must
 * never turn a successful install into a failure. */
static void test_sentinel(void)
{
	struct afe_top_config config;
	char path[] = "/tmp/afe-top-sentinel.XXXXXX";
	char line[256];
	int fd;
	FILE *f;
	int saw_state_yes = 0, saw_state_neutralised = 0, saw_dealloc = 0;

	fd = mkstemp(path);
	check(fd >= 0, "sentinel temp path is created");
	close(fd);

	/* Install: state: yes */
	afe_top_default_config(&config);
	config.sentinel = path;
	reset_mock();
	check(run(&config) == 0, "install with a sentinel path succeeds");
	f = fopen(path, "r");
	check(f != NULL, "the sentinel file is written after a successful SET");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "state: yes", 10) == 0)
				saw_state_yes = 1;
			if (strstr(line, "dealloc=NULL"))
				saw_dealloc = 1;
		}
		fclose(f);
	}
	check(saw_state_yes, "an install records state: yes");
	check(saw_dealloc, "the sentinel says why the block cannot be removed");

	/* Neutralise: state: neutralised */
	afe_top_default_config(&config);
	config.sentinel = path;
	config.neutralise = 1;
	config.rx.topology = 0;
	reset_mock();
	check(run(&config) == 0, "neutralise with a sentinel path succeeds");
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (strncmp(line, "state: neutralised", 18) == 0)
				saw_state_neutralised = 1;
		fclose(f);
	}
	check(saw_state_neutralised,
	      "neutralising records state: neutralised, not yes");
	/* G3: that run followed an install, so claiming one is correct. */
	{
		int saw_prior_claim = 0;

		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f))
				if (strstr(line, "was installed in this boot and then"))
					saw_prior_claim = 1;
			fclose(f);
		}
		check(saw_prior_claim,
		      "neutralising AFTER a recorded install does say a block was installed");
	}

	/* G3: standalone -N with nothing recorded beforehand must NOT invent a
	 * prior install -- it knows only that buffer 0 now reads topology 0. */
	unlink(path);
	{
		int saw_prior_claim = 0, saw_not_known = 0;

		afe_top_default_config(&config);
		config.sentinel = path;
		config.neutralise = 1;
		config.rx.topology = 0;
		reset_mock();
		check(run(&config) == 0, "standalone neutralise succeeds");
		f = fopen(path, "r");
		check(f != NULL, "standalone neutralise still records its own state");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				if (strstr(line, "was installed in this boot and then"))
					saw_prior_claim = 1;
				if (strstr(line, "NOT known from"))
					saw_not_known = 1;
			}
			fclose(f);
		}
		check(!saw_prior_claim,
		      "standalone neutralise does NOT claim a prior install it has no evidence of");
		check(saw_not_known,
		      "standalone neutralise says the prior state is not known from this file");
	}

	/* A failed SET must leave no record: nothing was installed. */
	unlink(path);
	afe_top_default_config(&config);
	config.sentinel = path;
	reset_mock();
	mock_ioctl_errno_on_call = 1;
	check(run(&config) != 0, "a failing SET still fails");
	check(fopen(path, "r") == NULL,
	      "a failed SET writes no sentinel (nothing was installed)");
	mock_ioctl_errno_on_call = 0;

	/* Disabled, and unwritable: neither may affect the install. */
	afe_top_default_config(&config);
	config.sentinel = "";
	reset_mock();
	check(run(&config) == 0, "an empty sentinel path disables the record");
	afe_top_default_config(&config);
	config.sentinel = "/proc/nonexistent-dir/sentinel";
	reset_mock();
	check(run(&config) == 0,
	      "an unwritable sentinel path does NOT fail the install (best-effort)");
	unlink(path);
}

/* The real CLI, exec'd: -N takes no topology, and both refusals must happen
 * before the calibration device is opened. */
static void test_cli_neutralise_refusals(void)
{
	static const char *const bad[][4] = {
		{ "./afe-topology-cal", "-N", "-r", "112FC" },
		{ "./afe-topology-cal", "-N", "-t", "112FB" },
	};
	size_t i;
	pid_t pid;
	int status;

	if (access("./afe-topology-cal", X_OK) != 0) {
		check(0, "./afe-topology-cal is built for the CLI checks");
		return;
	}
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		pid = fork();
		check(pid >= 0, "fork for the CLI check");
		if (pid == 0) {
			int devnull = open("/dev/null", O_WRONLY);

			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
			}
			execl(bad[i][0], bad[i][0], bad[i][1], bad[i][2],
			      bad[i][3], (char *)NULL);
			_exit(127);
		}
		check(waitpid(pid, &status, 0) == pid, "CLI child is reaped");
		check(WIFEXITED(status) && WEXITSTATUS(status) == 2,
		      i == 0 ? "-N with -r exits 2 (usage), never installing"
			     : "-N with -t exits 2 (usage), never installing");
	}
}

static void test_failures(void)
{
	struct afe_top_config config;

	afe_top_default_config(&config);
	config.sentinel = "";

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
	int saw_remains_installed = 0;
	int saw_false_release_claim = 0;

	check(out_fd >= 0, "temp file for the child's output opens");
	if (out_fd < 0)
		return;
	afe_top_default_config(&config);
	config.sentinel = "";
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
			if (strstr(line, "exiting on signal 15"))
				saw_released = 1;
			/* The block is NOT freed on close: AFE_TOPOLOGY_CAL_TYPE
			 * is registered with dealloc=NULL, so call_deallocs()
			 * skips it. The 2026-09-22 live run saw the topology
			 * still being sent after this helper had exited, so
			 * neither message may claim a release. */
			if (strstr(line, "REMAINS INSTALLED"))
				saw_remains_installed = 1;
			if (strstr(line, "releases the block") ||
			    strstr(line, "frees the block") ||
			    strstr(line, "released on signal"))
				saw_false_release_claim = 1;
		}
		fclose(out);
	}
	check(saw_resident, "child announced residency before the signal");
	check(saw_released, "child announced its exit on signal 15 after closing");
	check(saw_remains_installed,
	      "child says the topology block REMAINS INSTALLED after its exit");
	check(!saw_false_release_claim,
	      "child never claims to have released/freed the block (dealloc=NULL)");
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
	test_neutralise();
	test_sentinel();
	test_cli_neutralise_refusals();
	test_resident_until_signal();

	printf("afe-topology-cal: %d/%d checks passed\n", passed, passed + failed);
	return failed ? 1 : 0;
}
