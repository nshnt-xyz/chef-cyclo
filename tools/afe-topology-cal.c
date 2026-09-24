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
 * *** THIS IS NOT REVERSIBLE WITHOUT A REBOOT. ***
 * An earlier version of this file claimed that closing /dev/msm_audio_cal
 * frees the block and restores the pre-state. That was wrong, and the
 * 2026-09-22 live run disproved it: the probe's 4th phase still logged
 * "AFE set topology id 0x112fc enable for port 0x1004 ret 0" with this
 * helper long dead and no fd open. audio_cal_release() does call
 * dealloc_all_clients(), but call_deallocs() begins with
 *     if (client_info_node->callbacks->dealloc == NULL) continue;
 * and q6afe.c registers AFE_TOPOLOGY_CAL_TYPE as
 *     {NULL, NULL, NULL, afe_set_cal, NULL, NULL}
 * -- dealloc is NULL, so the block is skipped and never freed. The same
 * NULL makes AUDIO_DEALLOCATE_CALIBRATION a no-op for this cal type, so
 * userspace has no way to remove it at all. Once installed, the topology is
 * sent by afe_send_port_topology_id() at every later RX port start for the
 * rest of the boot. It lives in RAM only, so a reboot clears it -- that is
 * the only verified reset. (Overwriting buffer 0 with topology 0 would make
 * afe_get_cal_topology_id() fail and afe_send_port_topology_id() skip the
 * send, which looks equivalent from the port's point of view, but the block
 * still exists and nothing has tested that path on a device; do not treat it
 * as a restore.)
 *
 * This process therefore stays resident for a different reason than it used
 * to: not to hold the state up, but to hold the flock and to give the
 * driving script one lifecycle to scope the experiment with. Its exit
 * changes nothing in the kernel. Nothing here is persistent across a reboot
 * and nothing but the one (or two) SET ioctl is ever written.
 *
 * Only one instance may run at a time: the device fd is flock()ed. The
 * kernel matches cal blocks by buffer_number only (cal_utils_match_buf_num;
 * hence RX = buffer 0, TX = buffer 1 below), so a second installer would
 * silently overwrite the first block's topology with no way to put the old
 * one back; refusing the second instance keeps the experiment's state
 * attributable to one process.
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
/* Shared with initramfs/usr/bin/spk-protect-probe. Written by whichever of
 * us learns first that a topology block exists in this boot, so the fact
 * survives the probe being killed, the helper being killed, or the helper
 * being run by hand outside the probe. tmpfs, so it dies with the topology
 * at reboot. Best-effort: failing to write it never fails the install. */
#define AFE_TOP_DEFAULT_SENTINEL "/run/spk-protect-probe.topology-installed"
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
	/* NULL or "" disables the sentinel write (host tests, -S ""). */
	const char *sentinel;
	struct afe_top_block rx;
	int install_tx;
	struct afe_top_block tx;
	/* Neutralise mode (-N): overwrite the RX block (buffer 0) with
	 * topology 0 and exit, instead of installing a topology and staying
	 * resident. See afe_top_neutralise_is_sound() for why this works. */
	int neutralise;
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

/* A neutralising RX block: topology EXACTLY 0, RX path, buffer 0. Kept
 * separate from block_is_valid() on purpose -- an install must never be
 * allowed to send topology 0 by accident, because that silently produces a
 * port with no topology instead of the one the caller asked for.
 *
 * Why topology 0 neutralises, from the kernel source (4.4 vendor tree):
 *   - cal_utils_set_cal() memcpy's cal_info into the block matched by
 *     buffer_number and validates nothing inside it, so 0 can be written;
 *     audio_cal_shared_ioctl() checks only size/cal_type/buffer_number.
 *   - afe_find_cal_topo_id_by_port() matches on `path` alone, so it returns
 *     this same buffer-0 RX block for the speaker port.
 *   - afe_get_cal_topology_id() then hits `if (!afe_top_info->topology)`,
 *     logs "invalid topology id" and returns -EINVAL with topology_id 0.
 *   - afe_send_port_topology_id() does `if (ret || !topology_id) goto done;`
 *     so NO AFE_PARAM_ID_SET_TOPOLOGY is sent at the next port start, and
 *     all four of its call sites discard its return value, so the port
 *     still starts normally.
 *   - afe_close() clears this_afe.topology[port_index] (with the sample
 *     rate and acdb id), so the per-port cache that afe_get_topology()
 *     reads resets when the port closes.
 * What this is NOT: a delete. The cal block still exists and is still
 * matched; AFE_TOPOLOGY_CAL_TYPE has dealloc=NULL so nothing can remove it.
 * And it does not un-tell the DSP about a topology it was already sent
 * earlier in the boot -- it only stops the kernel sending one again. Treat
 * a neutralised phase as "no SET_TOPOLOGY was sent", never as "this device
 * is in its pre-topology state".
 */
static int neutralise_block_is_valid(const struct afe_top_block *block)
{
	return block->topology == 0 && block->path == AFE_TOP_PATH_RX &&
	       block->acdb_id >= 0 && block->acdb_id <= AFE_TOP_MAX_ACDB_ID &&
	       block->sample_rate >= AFE_TOP_MIN_RATE &&
	       block->sample_rate <= AFE_TOP_MAX_RATE;
}

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
	if (!block || !msg ||
	    !(block_is_valid(block) || neutralise_block_is_valid(block))) {
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

/* Records, on tmpfs, that this boot now has an AFE topology block. Called
 * immediately after a SET succeeds -- never before, because unlike the
 * probe's own conservative pre-latch we know here exactly whether the ioctl
 * returned. The block cannot be freed by anything in userspace
 * (AFE_TOPOLOGY_CAL_TYPE has dealloc=NULL), so this outlives us on purpose.
 * Best-effort by design: a read-only or missing /run must not turn a
 * successful install into a failure, so every error is swallowed. */
static void write_sentinel(const struct afe_top_config *config,
			   const struct afe_top_block *block, FILE *errors)
{
	FILE *f;
	int had_prior_state = 0;

	if (!config->sentinel || !config->sentinel[0])
		return;
	/* Did anything record a topology state in this boot before us? Only
	 * used to keep the neutralise wording honest (see below). */
	f = fopen(config->sentinel, "r");
	if (f) {
		char line[128];

		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "state: ", 7) == 0) {
				had_prior_state = 1;
				break;
			}
		}
		fclose(f);
	}
	f = fopen(config->sentinel, "w");
	if (!f) {
		fprintf(errors, "afe-topology-cal: note: could not record the install in %s: %s (the block is installed regardless)\n",
			config->sentinel, strerror(errno));
		return;
	}
	if (block->topology == 0) {
		fprintf(f, "state: neutralised\n");
		if (had_prior_state)
			fprintf(f, "An AFE topology block was installed in this boot and then\n"
				   "NEUTRALISED (buffer 0 rewritten with topology 0), so the kernel\n"
				   "sends no SET_TOPOLOGY at the next port start. The block itself\n"
				   "still exists and always will.\n");
		else
			/* Standalone -N with nothing recorded beforehand: we know
			 * buffer 0 now reads topology 0, and nothing more. Whether
			 * a topology was ever installed in this boot -- by an
			 * earlier run, by hand, or not at all -- is not ours to
			 * assert. Saying so would manufacture a prior install out
			 * of thin air, which is exactly the kind of claim this
			 * file exists to stop. */
			fprintf(f, "Buffer 0 (RX) was NEUTRALISED here: it now carries topology 0,\n"
				   "so the kernel sends no SET_TOPOLOGY at the next port start.\n"
				   "No prior install was recorded in this boot before this write, so\n"
				   "whether a topology had already been installed is NOT known from\n"
				   "this file. A cal block exists either way and can never be removed.\n");
	} else {
		fprintf(f, "state: yes\n");
		fprintf(f, "An AFE topology block WAS installed in this boot and is still being\n"
			   "sent at every RX port start.\n");
	}
	fprintf(f, "written by afe-topology-cal (%s topology 0x%08x acdb_id %d rate %d)\n",
		block->path == AFE_TOP_PATH_TX ? "TX" : "RX", block->topology,
		block->acdb_id, block->sample_rate);
	fprintf(f, "It cannot be removed from userspace: AFE_TOPOLOGY_CAL_TYPE is registered\n"
		   "with dealloc=NULL (q6afe.c), so audio_calibration.c's call_deallocs()\n"
		   "skips it and closing /dev/msm_audio_cal does not free it.\n"
		   "REBOOT to return to the pre-topology state.\n");
	if (fclose(f) != 0)
		fprintf(errors, "afe-topology-cal: note: %s may be incomplete: %s\n",
			config->sentinel, strerror(errno));
}

static int install_block(int fd, const struct afe_top_config *config,
			 const struct afe_top_block *block,
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
	/* The SET returned 0: the block now exists (or now reads topology 0)
	 * for the rest of the boot. Record that before anything else -- this
	 * is the only point at which we know the ioctl actually succeeded. */
	write_sentinel(config, block, errors);
	if (block->topology == 0)
		fprintf(out, "afe-topology-cal: neutralised the %s block: buffer 0 now carries topology 0x00000000 acdb_id %d rate %d, so afe_get_cal_topology_id() rejects it and no SET_TOPOLOGY is sent at the next port start (the block itself still exists and cannot be removed)\n",
			block->path == AFE_TOP_PATH_TX ? "TX" : "RX",
			block->acdb_id, block->sample_rate);
	else
		fprintf(out, "afe-topology-cal: installed %s topology 0x%08x acdb_id %d rate %d (cal_type %d, path %d, no shared memory)\n",
			block->path == AFE_TOP_PATH_TX ? "TX" : "RX",
			block->topology, block->acdb_id, block->sample_rate,
			AFE_TOP_CAL_TYPE, block->path);
	return 0;
}

/* Exit codes: 2 usage/range, 3 open, 4 another instance holds the device,
 * 5 ioctl failure (fd closed; note that an RX block installed before a TX
 * failure stays installed -- nothing here can roll back), 6 signal wait
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
	    !errors ||
	    (config->neutralise
		     ? (!neutralise_block_is_valid(&config->rx) ||
			config->install_tx)
		     : !block_is_valid(&config->rx)) ||
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

	if (install_block(fd, config, &config->rx, calls, out, errors) < 0 ||
	    (config->install_tx &&
	     install_block(fd, config, &config->tx, calls, out, errors) < 0)) {
		/* Closing frees nothing for this cal type (dealloc=NULL, see
		 * the header): if the RX block went in and the TX block then
		 * failed, the RX topology stays installed until reboot. The
		 * close is just fd hygiene, not a rollback. */
		(void)calls->close_device(fd);
		return 5;
	}

	if (config->neutralise) {
		/* Nothing to hold up: the write has already taken effect and
		 * no fd keeps it alive (or could free it). Exit at once so a
		 * caller can treat this as a plain command. */
		if (calls->close_device(fd) < 0)
			fprintf(errors, "afe-topology-cal: close after neutralising failed: %s\n",
				strerror(errno));
		fprintf(out, "afe-topology-cal: neutralised and exiting; the next RX port start will send no SET_TOPOLOGY. This is NOT a delete and NOT a return to the pre-topology state -- the block still exists and the DSP was already told the old topology earlier in this boot.\n");
		return 0;
	}

	fprintf(out, "afe-topology-cal: resident, holding %s open (flock only); SIGTERM/SIGINT exits but does NOT remove the block%s -- AFE_TOPOLOGY_CAL_TYPE has dealloc=NULL, so it stays installed until reboot\n",
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
	fprintf(out, "afe-topology-cal: exiting on signal %d (device closed); the topology block%s REMAINS INSTALLED -- the kernel cannot free it (dealloc=NULL) and neither can any ioctl; reboot to clear it\n",
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
	config->sentinel = AFE_TOP_DEFAULT_SENTINEL;
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
		"       afe-topology-cal -N [-d DEVICE] [-a RX_ACDB_ID] [-s RATE]\n"
		"  -d DEVICE            calibration device (default %s)\n"
		"  -S PATH              record the install in PATH (default /run/spk-protect-probe.\n"
		"                       topology-installed); -S '' disables the record\n"
		"  -r RX_TOPOLOGY_HEX   RX (speaker) AFE topology, 1..7fffffff (default %08x)\n"
		"  -a RX_ACDB_ID        informational ACDB device id for the RX block (default %d)\n"
		"  -s RATE              sample rate for both blocks, %d..%d (default %d)\n"
		"  -t TX_TOPOLOGY_HEX   also install a TX block with this topology (default: none)\n"
		"  -A TX_ACDB_ID        ACDB device id for the TX block (default 0)\n"
		"  -N                   NEUTRALISE: rewrite buffer 0 (RX) with topology 0 and\n"
		"                       exit, so the next RX port start sends no SET_TOPOLOGY.\n"
		"                       Not a delete: the block stays (dealloc=NULL) and the\n"
		"                       DSP is not untold a topology it already received.\n"
		"                       Refuses -r and -t.\n"
		"Without -N: installs the block(s) with one AUDIO_SET_CALIBRATION each, then\n"
		"stays resident holding an flock. Exiting does NOT remove them -- the kernel\n"
		"cannot free this cal type. Reboot, or -N, is how you stop them being sent.\n",
		AFE_TOP_DEFAULT_DEVICE, AFE_TOP_DEFAULT_RX_TOPOLOGY,
		AFE_TOP_DEFAULT_RX_ACDB_ID, AFE_TOP_MIN_RATE, AFE_TOP_MAX_RATE,
		AFE_TOP_DEFAULT_RATE);
}

int main(int argc, char **argv)
{
	struct afe_top_config config;
	int opt;
	int32_t rate;
	int saw_r = 0;

	afe_top_default_config(&config);
	while ((opt = getopt(argc, argv, "d:S:r:a:s:t:A:Nh")) != -1) {
		switch (opt) {
		case 'd':
			config.device = optarg;
			break;
		case 'S':
			config.sentinel = optarg;
			break;
		case 'r':
			if (parse_hex_u32(optarg, &config.rx.topology) < 0) {
				fprintf(stderr, "afe-topology-cal: invalid -r '%s' (hex topology id)\n", optarg);
				return 2;
			}
			saw_r = 1;
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
		case 'N':
			config.neutralise = 1;
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
	if (config.neutralise) {
		if (saw_r || config.install_tx) {
			fprintf(stderr, "afe-topology-cal: -N takes no topology: it writes topology 0 to buffer 0 (RX). Drop -r/-t.\n");
			return 2;
		}
		config.rx.topology = 0;
		if (!neutralise_block_is_valid(&config.rx)) {
			fprintf(stderr, "afe-topology-cal: -N acdb_id %d / rate %d out of range (rate %d..%d)\n",
				config.rx.acdb_id, config.rx.sample_rate,
				AFE_TOP_MIN_RATE, AFE_TOP_MAX_RATE);
			return 2;
		}
		return afe_topology_cal_run(&config, &system_calls, stdout,
					    stderr);
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
