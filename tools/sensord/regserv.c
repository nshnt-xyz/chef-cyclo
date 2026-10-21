/*
 * sensord's QMI servers: REG2 (the registry, from its RAM copy, and the
 * write-back over the /run copy) and the optional TIME2, plus the
 * datagram receive and service publish helpers. See main.c, sections 1
 * and 3.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sensord.h"

static void hexdump_log(struct server *s, const char *what, const void *data, size_t len)
{
	const uint8_t *p = data;
	char buf[3 * 48 + 1];
	size_t i, o = 0;

	for (i = 0; i < len && i < 48; i++)
		o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%02x ", p[i]);
	buf[o ? o - 1 : 0] = '\0';
	s->log("%s (%zu bytes): %s%s", what, len, buf, len > 48 ? " ..." : "");
}

static void send_resp(struct server *s, int sock, uint32_t node, uint32_t port, unsigned msg,
		      uint16_t txn, const void *resp, struct qmi_elem_info *ei)
{
	DEFINE_QRTR_PACKET(out, SENSORD_QMI_BUF);
	ssize_t len;
	int rc;

	len = qmi_encode_message(&out, QMI_RESPONSE, (int)msg, txn, resp, ei);
	if (len < 0) {
		s->log("encode response 0x%02x failed: %s", msg, strerror((int)-len));
		return;
	}
	rc = qrtr_sendto(sock, node, port, out.data, (unsigned)out.data_len);
	if (rc < 0)
		s->log("send response 0x%02x to %u:%u failed: %s", msg, node, port, strerror(-rc));
}

static void send_generic(struct server *s, int sock, uint32_t node, uint32_t port, unsigned msg,
			 uint16_t txn, uint8_t result, uint8_t err)
{
	struct sns_generic_resp r = { { result, err } };

	send_resp(s, sock, node, port, msg, txn, &r, sns_generic_resp_ei);
}

static void send_version(struct server *s, int sock, uint32_t node, uint32_t port, uint16_t txn,
			 uint32_t minor, uint16_t max_msg)
{
	struct sns_version_resp r = { { SNS_RESULT_SUCCESS, SNS_ERR_NONE }, minor, max_msg };

	send_resp(s, sock, node, port, SNS_MSG_VERSION, txn, &r, sns_version_resp_ei);
}

/* QMAG_CAL's registry: group 2970 = items 3800..3840 (stock map). */
static bool reg2_is_qmag(bool group, uint16_t id)
{
	return group ? id == SNS_REG2_QMAG_GROUP : id >= 3800 && id <= 3840;
}

#define SENSORD_REG_SAVE_QUIET_MS 3000
#define SENSORD_REG_SAVE_MAX_MS	30000	/* ... but never later than this */

/* Write the RAM registry back over the /run copy. A failure is logged
 * and dropped (the RAM copy still serves the DSP); the next DSP write
 * tries again. */
void sn_reg_writeback(struct server *s, const char *why)
{
	int rc;

	if (!s->reg_path || !s->reg_dirty)
		return;
	s->reg_dirty = false;
	rc = sns_reg_save(&s->reg, s->reg_path);
	if (rc < 0) {
		s->reg_save_errors++;
		s->log("registry write-back to %s failed (%s)%s; the DSP's writes stay in RAM only",
		       s->reg_path, strerror(-rc), rc == -EXDEV ? ": not a RAM filesystem" : "");
		return;
	}
	s->reg_saves++;
	s->mag_pending = true;
	s->log("registry written back to %s (%s; %lu DSP writes so far; persist untouched)",
	       s->reg_path, why, s->reg.writes);
}

/* -------------------------------------------- the bias save (chef-state) */

/* `mag_cmd mag-save` in its own process group, stdio on /dev/null, the
 * registry paths in its environment; it gets TERM if sensord dies. */
static pid_t mag_spawn(struct server *s)
{
	pid_t parent = getpid(), pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(127);
		setpgid(0, 0);
		dup2(nul, 0);
		dup2(nul, 1);
		dup2(nul, 2);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (s->mag_env_run)
			setenv("CHEF_STATE_SENSORS_RUN", s->mag_env_run, 1);
		if (s->mag_env_map)
			setenv("CHEF_STATE_MAP", s->mag_env_map, 1);
		execlp(s->mag_cmd, s->mag_cmd, "mag-save", (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	return pid;
}

void sn_mag_save_tick(struct server *s)
{
	int64_t now = s->now_ms();
	pid_t pid;

	if (s->mag_pid > 0) {
		if (now >= s->mag_until) {
			s->log("%s mag-save still running after %d s: killed", s->mag_cmd,
			       SENSORD_MAG_SAVE_MAX_MS / 1000);
			kill(-s->mag_pid, SIGKILL);	/* its exit, reaped next, retries */
			s->mag_until = INT64_MAX;
		}
		return;
	}
	if (!s->mag_cmd || !s->mag_pending || (s->mag_at && now < s->mag_at + SENSORD_MAG_SAVE_EVERY_MS))
		return;
	s->mag_pending = false;
	s->mag_at = now;
	pid = mag_spawn(s);
	if (pid < 0) {
		s->mag_save_fails++;
		s->mag_pending = true;
		s->log("cannot run %s mag-save: %s; the bias is saved later", s->mag_cmd, strerror((int)-pid));
		return;
	}
	s->mag_pid = pid;
	s->mag_until = now + SENSORD_MAG_SAVE_MAX_MS;
}

void sn_mag_save_reap(struct server *s)
{
	int status, rc;

	if (s->mag_pid <= 0 || waitpid(s->mag_pid, &status, WNOHANG) != s->mag_pid)
		return;
	kill(-s->mag_pid, SIGKILL);	/* nothing of it outlives the leader */
	s->mag_pid = 0;
	rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	if (rc == 0) {
		s->mag_saves++;
		return;
	}
	/* 3: chef-state's own once-a-minute limit; anything else failed */
	s->mag_pending = true;
	if (rc != 3) {
		s->mag_save_fails++;
		s->log("%s mag-save exited %d: the bias is saved later", s->mag_cmd, rc);
	}
}

/* The earlier of NEXT and when the save needs the loop next. */
int64_t sn_mag_save_due(const struct server *s, int64_t next)
{
	int64_t due;

	if (s->mag_pid > 0)
		due = s->mag_until;
	else if (s->mag_cmd && s->mag_pending)
		due = s->mag_at ? s->mag_at + SENSORD_MAG_SAVE_EVERY_MS : 0;
	else
		return next;
	return due < next ? due : next;
}

/* Exiting: a save in flight is killed (chef-state shutdown makes the
 * final one once sensord is gone), and reaped. */
void sn_mag_save_stop(struct server *s)
{
	if (s->mag_pid <= 0)
		return;
	kill(-s->mag_pid, SIGKILL);
	waitpid(s->mag_pid, NULL, 0);
	s->mag_pid = 0;
}

/* When the pending write-back is due: 3 s after the last DSP write,
 * capped at 30 s after the first unsaved one (steady writes cannot
 * postpone it for ever). */
int64_t sn_reg_save_due(const struct server *s)
{
	int64_t quiet = s->reg_dirty_at + SENSORD_REG_SAVE_QUIET_MS;
	int64_t cap = s->reg_dirty_since + SENSORD_REG_SAVE_MAX_MS;

	return quiet < cap ? quiet : cap;
}

void sn_reg_tick(struct server *s)
{
	if (s->reg_dirty && s->now_ms() >= sn_reg_save_due(s))
		sn_reg_writeback(s, "after DSP writes");
}

void sn_reg2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;
	int rc;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("reg2: malformed QMI message from %u:%u, dropped", node, port);
		return;
	}
	if (h[0] != QMI_REQUEST)
		return;
	txn = (uint16_t)(h[1] | h[2] << 8);

	switch (msg) {
	case SNS_MSG_CANCEL:
		sn_vlog("reg2: cancel from %u:%u", node, port);
		send_generic(s, s->reg_sock, node, port, msg, txn, SNS_RESULT_SUCCESS, SNS_ERR_NONE);
		return;
	case SNS_MSG_VERSION:
		sn_vlog("reg2: version query from %u:%u", node, port);
		send_version(s, s->reg_sock, node, port, txn, SNS_REG2_IDL_MINOR, SNS_REG2_MAX_MSG_ID);
		return;
	case SNS_REG2_ITEM_READ:
	case SNS_REG2_GROUP_READ: {
		bool group = msg == SNS_REG2_GROUP_READ;
		struct sns_reg2_id_req req = { 0 };
		static struct sns_reg2_group_read_resp gr;
		struct sns_reg2_item_read_resp ir;
		int n;

		rc = qmi_decode_message(&req, &t, pkt, QMI_REQUEST, (int)msg, sns_reg2_id_req_ei);
		memset(&gr, 0, sizeof(gr));
		memset(&ir, 0, sizeof(ir));
		n = rc < 0 ? -1 : group ? sns_reg_read(&s->reg, 1, req.id, gr.data, sizeof(gr.data))
					: sns_reg_read(&s->reg, 0, req.id, ir.data, sizeof(ir.data));
		if (n < 0)
			s->log("reg2: %s read %u from %u:%u: %s", group ? "group" : "item", req.id,
			       node, port, rc < 0 ? "malformed" : "no such id");
		else if (reg2_is_qmag(group, req.id))
			/* Not read at boot live: shows QMAG_CAL starting up. */
			s->log("reg2: %s read %u (QMAG_CAL) from %u:%u -> %d bytes",
			       group ? "group" : "item", req.id, node, port, n);
		else
			sn_vlog("reg2: %s read %u from %u:%u -> %d bytes", group ? "group" : "item",
			     req.id, node, port, n);
		if (group) {
			gr.resp.result = n < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS;
			gr.resp.err = n < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE;
			gr.id = req.id;
			gr.data_len = n < 0 ? 0 : (uint32_t)n;
			send_resp(s, s->reg_sock, node, port, msg, txn, &gr, sns_reg2_group_read_resp_ei);
		} else {
			ir.resp.result = n < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS;
			ir.resp.err = n < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE;
			ir.id = req.id;
			ir.data_len = n < 0 ? 0 : (uint32_t)n;
			send_resp(s, s->reg_sock, node, port, msg, txn, &ir, sns_reg2_item_read_resp_ei);
		}
		return;
	}
	case SNS_REG2_ITEM_WRITE:
	case SNS_REG2_GROUP_WRITE: {
		bool group = msg == SNS_REG2_GROUP_WRITE;
		static struct sns_reg2_group_write_req gw;
		struct sns_reg2_item_write_req iw;
		const uint8_t *data;
		uint32_t len;
		uint16_t id;
		char what[96];

		memset(&gw, 0, sizeof(gw));
		memset(&iw, 0, sizeof(iw));
		rc = group ? qmi_decode_message(&gw, &t, pkt, QMI_REQUEST, (int)msg,
						sns_reg2_group_write_req_ei)
			   : qmi_decode_message(&iw, &t, pkt, QMI_REQUEST, (int)msg,
						sns_reg2_item_write_req_ei);
		id = group ? gw.id : iw.id;
		data = group ? gw.data : iw.data;
		len = group ? gw.data_len : iw.data_len;
		/* The decoder stores the wire count before it rejects it: never
		 * log (or write) more than the array actually holds. */
		if (rc < 0)
			len = 0;
		if (len > (group ? sizeof(gw.data) : sizeof(iw.data)))
			len = 0;
		if (rc >= 0)
			rc = sns_reg_write(&s->reg, group, id, data, len);
		if (rc >= 0) {
			if (!s->reg_dirty)
				s->reg_dirty_since = s->now_ms();
			s->reg_dirty = true;
			s->reg_dirty_at = s->now_ms();	/* debounce: wait for quiet */
		}
		snprintf(what, sizeof(what), "reg2: %s write %u%s from %u:%u (RAM, /run copy)%s",
			 group ? "group" : "item", id, reg2_is_qmag(group, id) ? " (QMAG_CAL)" : "",
			 node, port, rc == -ENOENT ? " no such id" : rc < 0 ? " rejected" : "");
		hexdump_log(s, what, data, len);
		send_generic(s, s->reg_sock, node, port, msg, txn,
			     rc < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS,
			     rc < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE);
		if (rc >= 0)
			sn_mag_cal_check(s, "DSP write");	/* group 2980: the learned bias */
		return;
	}
	default:
		s->unknown_reqs++;
		hexdump_log(s, "reg2: unsupported request, nacked", h, pkt->data_len);
		send_generic(s, s->reg_sock, node, port, msg, txn, SNS_RESULT_FAILURE,
			     SNS_ERR_BAD_MSG_ID);
		return;
	}
}

void sn_time2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0 ||
	    h[0] != QMI_REQUEST)
		return;
	txn = (uint16_t)(h[1] | h[2] << 8);
	s->time2_reqs++;
	if (msg == SNS_MSG_VERSION) {
		s->log("time2: version query from %u:%u", node, port);
		send_version(s, s->time_sock, node, port, txn, SNS_TIME2_IDL_MINOR, SNS_TIME2_MAX_MSG_ID);
	} else if (msg == SNS_TIME2_TIMESTAMP) {
		struct sns_time2_req req = { 0 };
		struct sns_time2_resp r;
		struct timespec ts;
		uint32_t ticks;

		qmi_decode_message(&req, &t, pkt, QMI_REQUEST, (int)msg, sns_time2_req_ei);
		memset(&r, 0, sizeof(r));
		if (sn_read_dsp_ticks(s, &ticks)) {
			r.dsps_ticks_valid = 1;
			r.dsps_ticks = ticks;
		}
		clock_gettime(CLOCK_BOOTTIME, &ts);
		r.apps_ns_valid = r.apps_boot_ns_valid = 1;
		r.apps_ns = r.apps_boot_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
		s->log("time2: timestamp request from %u:%u (reg_report %s)", node, port,
		       req.reg_report_valid ? (req.reg_report ? "1" : "0") : "unset");
		send_resp(s, s->time_sock, node, port, msg, txn, &r, sns_time2_resp_ei);
	} else {
		s->unknown_reqs++;
		hexdump_log(s, "time2: unsupported request, nacked", h, pkt->data_len);
		send_generic(s, s->time_sock, node, port, msg, txn, SNS_RESULT_FAILURE,
			     SNS_ERR_BAD_MSG_ID);
	}
}

/* Receive one datagram. Returns 1 with *pkt filled, 0 for nothing, or
 * -ENETRESET when the router reset the socket (DSP restart). */
int sn_ipc_recv(int sock, char *buf, size_t bsz, struct qrtr_packet *pkt, uint32_t *node,
		uint32_t *port)
{
	struct sockaddr_qrtr sq;
	int ret;

	ret = qrtr_recvfrom(sock, buf, (unsigned)bsz, node, port);
	if (ret == QRTR_RECV_RESUME_TX || ret == -EAGAIN || ret == -EINTR)
		return 0;
	if (ret == -ENETRESET)
		return ret;
	if (ret < 0)
		return 0;
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = *node;
	sq.sq_port = *port;
	qrtr_decode(pkt, buf, (size_t)ret, &sq);
	return 1;
}

int sn_publish(struct server *s, int *sock, uint32_t svc, uint16_t vers, uint16_t inst)
{
	if (*sock >= 0)
		qrtr_close(*sock);
	*sock = qrtr_open(0);
	if (*sock < 0) {
		s->log("socket for service 0x%x: %s", svc, strerror(errno));
		return -1;
	}
	if (qrtr_publish(*sock, svc, vers, inst) < 0) {
		s->log("publish service 0x%x inst 0x%x: %s", svc, (inst << 8) | vers, strerror(errno));
		qrtr_close(*sock);
		*sock = -1;
		return -1;
	}
	s->log("serving service 0x%x inst 0x%04x", svc, (inst << 8) | vers);
	return 0;
}
