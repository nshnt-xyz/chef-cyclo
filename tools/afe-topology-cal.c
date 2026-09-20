/*
 * Install an AFE port topology calibration block through /dev/msm_audio_cal
 * and hold it until signalled.
 *
 * Stock Android installs the per-device AFE topology (ACDB property 0x13150:
 * speaker devices -> 0x000112FC, mic devices -> 0x000112FB; see
 * tools/acdb-afe-topology.py) through libacdbloader's AUDIO_SET_CALIBRATION
 * of AFE_TOPOLOGY_CAL_TYPE before the first port start. This build has no
 * ACDB loader, so q6afe's afe_send_port_topology_id() finds no cal block for
 * port 0x1004 and never sends AFE_PARAM_ID_SET_TOPOLOGY. This helper installs
 * exactly that one block (RX; optionally a TX block too) with the smallest
 * possible ioctl: AFE_TOPOLOGY_CAL_TYPE carries only a 16-byte cal_info and
 * needs no ION memory (cal_size 0, mem_handle -1).
 *
 * The kernel's audio_calibration.c frees EVERY installed cal block when the
 * last /dev/msm_audio_cal file descriptor closes (audio_cal_release ->
 * dealloc_all_clients), so this process stays resident, holding the fd,
 * until SIGTERM/SIGINT/SIGHUP; closing the fd on exit restores the pre-state
 * without any explicit deallocation. Nothing here is persistent and nothing
 * but the one (or two) SET ioctl is ever written.
 *
 * Only one instance may run at a time: the device fd is flock()ed. The
 * kernel matches cal blocks by buffer_number only (cal_utils_match_buf_num;
 * hence RX = buffer 0, TX = buffer 1 below), so a second installer would
 * silently overwrite the first block's topology and neither instance's exit
 * would release it until both had closed; refusing the second instance
 * keeps "helper exited => state restored" true.
 *
 * The struct layout below mirrors kernel/include/uapi/linux/
 * msm_audio_calibration.h (audio_cal_afe_top and what it nests). It is
 * spelled out here rather than included because that header is not
 * userspace-clean (int32_t without <stdint.h>, kernel-only __packed);
 * tools/tests/test_afe_topology_cal.c includes the real header with the
 * needed shims and checks sizes, offsets, the cal type index and the ioctl
 * number byte for byte against these definitions.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AFE_TOP_DEFAULT_DEVICE "/dev/msm_audio_cal"
#define AFE_TOP_DEFAULT_RX_TOPOLOGY 0x000112FCU	/* stock speaker AFE topology */
#define AFE_TOP_DEFAULT_RX_ACDB_ID 14		/* SPKR_PHONE_SPKR_MONO */
#define AFE_TOP_DEFAULT_RATE 48000
#define AFE_TOP_MIN_RATE 8000
#define AFE_TOP_MAX_RATE 384000
#define AFE_TOP_MAX_TOPOLOGY 0x7FFFFFFFU	/* field is int32_t; 0 is refused */
#define AFE_TOP_MAX_ACDB_ID INT32_MAX

/* CAL_IOCTL_MAGIC 'a', number 203, argument type "void *" (8 bytes on the
 * phone's aarch64 and on the x86_64 host): 0xC00861CB on both. */
#define AFE_TOP_IOCTL_SET_CALIBRATION _IOWR('a', 203, void *)
/* Index of AFE_TOPOLOGY_CAL_TYPE in the header's anonymous cal-type enum. */
#define AFE_TOP_CAL_TYPE 23
#define AFE_TOP_VERSION_0_0 0
#define AFE_TOP_PATH_RX 0		/* RX_DEVICE */
#define AFE_TOP_PATH_TX 1		/* TX_DEVICE */
#define AFE_TOP_NO_MEM_HANDLE (-1)

struct afe_top_cal_header {
	int32_t data_size;
	int32_t version;
	int32_t cal_type;
	int32_t cal_type_size;
};

struct afe_top_cal_type_header {
	int32_t version;
	int32_t buffer_number;
};

struct afe_top_cal_data {
	int32_t cal_size;
	int32_t mem_handle;
};

struct afe_top_cal_info {
	int32_t topology;
	int32_t acdb_id;
	int32_t path;
	int32_t sample_rate;
};

struct afe_top_cal_type {
	struct afe_top_cal_type_header cal_hdr;
	struct afe_top_cal_data cal_data;
	struct afe_top_cal_info cal_info;
};

struct afe_top_cal_msg {
	struct afe_top_cal_header hdr;
	struct afe_top_cal_type cal_type;
};

struct afe_top_block {
	uint32_t topology;
	int32_t acdb_id;
	int32_t path;
	int32_t sample_rate;
};

struct afe_top_config {
	const char *device;
	struct afe_top_block rx;
	int install_tx;
	struct afe_top_block tx;
};

struct afe_top_syscalls {
	int (*open_device)(const char *path, int flags);
	int (*lock_device)(int fd);
	int (*device_ioctl)(int fd, unsigned long request, void *arg);
	/* Blocks until a termination signal arrives; returns the signal
	 * number, or -1 with errno set. Injected so tests never block. */
	int (*wait_for_signal)(void);
	int (*close_device)(int fd);
};

static int system_open_device(const char *path, int flags)
{
	return open(path, flags);
}

static int system_lock_device(int fd)
{
	return flock(fd, LOCK_EX | LOCK_NB);
}

static int system_device_ioctl(int fd, unsigned long request, void *arg)
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
	/* Block them first so a signal that arrives between install and
	 * sigwait() is queued rather than killing us with the fd still open
	 * (which would be harmless -- the kernel frees on close -- but would
	 * skip the "released" line the probe scripts look for). */
	if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
		return -1;
	rc = sigwait(&set, &sig);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return sig;
}

static int system_close_device(int fd)
{
	return close(fd);
}

static const struct afe_top_syscalls system_calls = {
	.open_device = system_open_device,
	.lock_device = system_lock_device,
	.device_ioctl = system_device_ioctl,
	.wait_for_signal = system_wait_for_signal,
	.close_device = system_close_device,
};

static int block_is_valid(const struct afe_top_block *block)
{
	return block->topology >= 1 && block->topology <= AFE_TOP_MAX_TOPOLOGY &&
	       block->acdb_id >= 0 && block->acdb_id <= AFE_TOP_MAX_ACDB_ID &&
	       (block->path == AFE_TOP_PATH_RX || block->path == AFE_TOP_PATH_TX) &&
	       block->sample_rate >= AFE_TOP_MIN_RATE &&
	       block->sample_rate <= AFE_TOP_MAX_RATE;
}

/* Fills the exact AUDIO_SET_CALIBRATION payload for one topology block.
 * Returns -1 (EINVAL) for anything out of range so the caller never sends
 * a zero or negative topology. */
static int afe_top_build_msg(const struct afe_top_block *block,
			     struct afe_top_cal_msg *msg)
{
	if (!block || !msg || !block_is_valid(block)) {
		errno = EINVAL;
		return -1;
	}
	memset(msg, 0, sizeof(*msg));
	msg->hdr.data_size = (int32_t)sizeof(*msg);
	msg->hdr.version = AFE_TOP_VERSION_0_0;
	msg->hdr.cal_type = AFE_TOP_CAL_TYPE;
	msg->hdr.cal_type_size = (int32_t)sizeof(msg->cal_type);
	msg->cal_type.cal_hdr.version = AFE_TOP_VERSION_0_0;
	/* AFE_TOPOLOGY_CAL blocks are matched by buffer_number ALONE
	 * (q6afe.c registers the cal type with cal_utils_match_buf_num, and
	 * cal_utils_set_cal memcpy's cal_info into the matching block), not by
	 * path or acdb_id. A TX block sent with buffer 0 would therefore
	 * overwrite the RX block's cal_info instead of adding a second block,
	 * losing the RX topology. RX is buffer 0, TX is buffer 1. */
	msg->cal_type.cal_hdr.buffer_number =
		block->path == AFE_TOP_PATH_TX ? 1 : 0;
	msg->cal_type.cal_data.cal_size = 0;
	msg->cal_type.cal_data.mem_handle = AFE_TOP_NO_MEM_HANDLE;
	msg->cal_type.cal_info.topology = (int32_t)block->topology;
	msg->cal_type.cal_info.acdb_id = block->acdb_id;
	msg->cal_type.cal_info.path = block->path;
	msg->cal_type.cal_info.sample_rate = block->sample_rate;
	return 0;
}

static int install_block(int fd, const struct afe_top_block *block,
			 const struct afe_top_syscalls *calls, FILE *out,
			 FILE *errors)
{
	struct afe_top_cal_msg msg;
	int saved_errno;

	if (afe_top_build_msg(block, &msg) < 0) {
		fprintf(errors, "afe-topology-cal: refusing %s block: topology 0x%08x acdb_id %d rate %d out of range\n",
			block->path == AFE_TOP_PATH_TX ? "TX" : "RX",
			block->topology, block->acdb_id, block->sample_rate);
		return -1;
	}
	if (calls->device_ioctl(fd, AFE_TOP_IOCTL_SET_CALIBRATION, &msg) < 0) {
		saved_errno = errno;
		fprintf(errors, "afe-topology-cal: AUDIO_SET_CALIBRATION (%s topology 0x%08x) failed: %s\n",
			block->path == AFE_TOP_PATH_TX ? "TX" : "RX",
			block->topology, strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	fprintf(out, "afe-topology-cal: installed %s topology 0x%08x acdb_id %d rate %d (cal_type %d, path %d, no shared memory)\n",
		block->path == AFE_TOP_PATH_TX ? "TX" : "RX", block->topology,
		block->acdb_id, block->sample_rate, AFE_TOP_CAL_TYPE,
		block->path);
	return 0;
}

/* Exit codes: 2 usage/range, 3 open, 4 another instance holds the device,
 * 5 ioctl failure (fd closed, nothing left installed by us), 6 signal wait
 * failure. 0 after a termination signal released the block(s). */
static int afe_topology_cal_run(const struct afe_top_config *config,
				const struct afe_top_syscalls *calls,
				FILE *out, FILE *errors)
{
	int fd;
	int sig;
	int saved_errno;

	if (!config || !config->device || !calls || !calls->open_device ||
	    !calls->lock_device || !calls->device_ioctl ||
	    !calls->wait_for_signal || !calls->close_device || !out ||
	    !errors || !block_is_valid(&config->rx) ||
	    config->rx.path != AFE_TOP_PATH_RX ||
	    (config->install_tx && (!block_is_valid(&config->tx) ||
				    config->tx.path != AFE_TOP_PATH_TX))) {
		errno = EINVAL;
		return 2;
	}

	fd = calls->open_device(config->device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(errors, "afe-topology-cal: cannot open %s: %s\n",
			config->device, strerror(errno));
		return 3;
	}
	if (calls->lock_device(fd) < 0) {
		saved_errno = errno;
		if (saved_errno == EWOULDBLOCK)
			fprintf(errors, "afe-topology-cal: another instance already holds %s; refusing to overwrite its block\n",
				config->device);
		else
			fprintf(errors, "afe-topology-cal: cannot lock %s: %s\n",
				config->device, strerror(saved_errno));
		(void)calls->close_device(fd);
		return 4;
	}

	if (install_block(fd, &config->rx, calls, out, errors) < 0 ||
	    (config->install_tx &&
	     install_block(fd, &config->tx, calls, out, errors) < 0)) {
		/* Closing may free a partially installed RX block (if this is
		 * the last fd), which is exactly the restore we want. */
		(void)calls->close_device(fd);
		return 5;
	}

	fprintf(out, "afe-topology-cal: resident, holding %s open; SIGTERM/SIGINT releases the block%s\n",
		config->device, config->install_tx ? "s" : "");
	fflush(out);
	sig = calls->wait_for_signal();
	if (sig < 0) {
		saved_errno = errno;
		fprintf(errors, "afe-topology-cal: signal wait failed: %s; releasing\n",
			strerror(saved_errno));
		(void)calls->close_device(fd);
		return 6;
	}
	if (calls->close_device(fd) < 0)
		fprintf(errors, "afe-topology-cal: close after signal %d failed: %s\n",
			sig, strerror(errno));
	fprintf(out, "afe-topology-cal: released on signal %d (device closed; kernel frees the block%s when the last fd closes)\n",
		sig, config->install_tx ? "s" : "");
	return 0;
}

static int parse_hex_u32(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed;

	if (!text || !text[0] || text[0] == '-' || text[0] == '+')
		return -1;
	errno = 0;
	parsed = strtoul(text, &end, 16);
	if (errno || *end || parsed > 0xFFFFFFFFUL)
		return -1;
	*value = (uint32_t)parsed;
	return 0;
}

static int parse_dec_i32(const char *text, int32_t *value)
{
	char *end;
	long parsed;

	if (!text || !text[0] || text[0] == '-' || text[0] == '+')
		return -1;
	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || *end || parsed < 0 || parsed > INT32_MAX)
		return -1;
	*value = (int32_t)parsed;
	return 0;
}

static void afe_top_default_config(struct afe_top_config *config)
{
	memset(config, 0, sizeof(*config));
	config->device = AFE_TOP_DEFAULT_DEVICE;
	config->rx.topology = AFE_TOP_DEFAULT_RX_TOPOLOGY;
	config->rx.acdb_id = AFE_TOP_DEFAULT_RX_ACDB_ID;
	config->rx.path = AFE_TOP_PATH_RX;
	config->rx.sample_rate = AFE_TOP_DEFAULT_RATE;
	config->install_tx = 0;
	config->tx.topology = 0;
	config->tx.acdb_id = 0;
	config->tx.path = AFE_TOP_PATH_TX;
	config->tx.sample_rate = AFE_TOP_DEFAULT_RATE;
}

#ifndef AFE_TOPOLOGY_CAL_NO_MAIN
static void usage(FILE *to)
{
	fprintf(to,
		"usage: afe-topology-cal [-d DEVICE] [-r RX_TOPOLOGY_HEX] [-a RX_ACDB_ID] [-s RATE]\n"
		"                        [-t TX_TOPOLOGY_HEX [-A TX_ACDB_ID]]\n"
		"  -d DEVICE            calibration device (default %s)\n"
		"  -r RX_TOPOLOGY_HEX   RX (speaker) AFE topology, 1..7fffffff (default %08x)\n"
		"  -a RX_ACDB_ID        informational ACDB device id for the RX block (default %d)\n"
		"  -s RATE              sample rate for both blocks, %d..%d (default %d)\n"
		"  -t TX_TOPOLOGY_HEX   also install a TX block with this topology (default: none)\n"
		"  -A TX_ACDB_ID        ACDB device id for the TX block (default 0)\n"
		"Installs the block(s) with one AUDIO_SET_CALIBRATION each, then stays\n"
		"resident; SIGTERM/SIGINT closes the device, which frees them again.\n",
		AFE_TOP_DEFAULT_DEVICE, AFE_TOP_DEFAULT_RX_TOPOLOGY,
		AFE_TOP_DEFAULT_RX_ACDB_ID, AFE_TOP_MIN_RATE, AFE_TOP_MAX_RATE,
		AFE_TOP_DEFAULT_RATE);
}

int main(int argc, char **argv)
{
	struct afe_top_config config;
	int opt;
	int32_t rate;

	afe_top_default_config(&config);
	while ((opt = getopt(argc, argv, "d:r:a:s:t:A:h")) != -1) {
		switch (opt) {
		case 'd':
			config.device = optarg;
			break;
		case 'r':
			if (parse_hex_u32(optarg, &config.rx.topology) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -r '%s' (hex topology id)\n", optarg);
				return 2;
			}
			break;
		case 'a':
			if (parse_dec_i32(optarg, &config.rx.acdb_id) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -a '%s' (decimal ACDB id)\n", optarg);
				return 2;
			}
			break;
		case 's':
			if (parse_dec_i32(optarg, &rate) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -s '%s' (decimal sample rate)\n", optarg);
				return 2;
			}
			config.rx.sample_rate = rate;
			config.tx.sample_rate = rate;
			break;
		case 't':
			if (parse_hex_u32(optarg, &config.tx.topology) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -t '%s' (hex topology id)\n", optarg);
				return 2;
			}
			config.install_tx = 1;
			break;
		case 'A':
			if (parse_dec_i32(optarg, &config.tx.acdb_id) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -A '%s' (decimal ACDB id)\n", optarg);
				return 2;
			}
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
	if (!block_is_valid(&config.rx)) {
		fprintf(stderr, "afe-topology-cal: RX topology 0x%08x / acdb_id %d / rate %d out of range (topology 1..%x, rate %d..%d)\n",
			config.rx.topology, config.rx.acdb_id,
			config.rx.sample_rate, AFE_TOP_MAX_TOPOLOGY,
			AFE_TOP_MIN_RATE, AFE_TOP_MAX_RATE);
		return 2;
	}
	if (config.install_tx && !block_is_valid(&config.tx)) {
		fprintf(stderr, "afe-topology-cal: TX topology 0x%08x / acdb_id %d / rate %d out of range (topology 1..%x, rate %d..%d)\n",
			config.tx.topology, config.tx.acdb_id,
			config.tx.sample_rate, AFE_TOP_MAX_TOPOLOGY,
			AFE_TOP_MIN_RATE, AFE_TOP_MAX_RATE);
		return 2;
	}
	return afe_topology_cal_run(&config, &system_calls, stdout, stderr);
}
#endif
