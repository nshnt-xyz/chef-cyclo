/*
 * powerd's outputs: the state file, the CSV rows with their header and
 * rotation, and with -l the batched log (rows wait in RUNDIR/log.pending
 * and reach LOGDIR/log.csv in batches, see main.c's header) with its
 * flush, also the `flush` subcommand's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "powerd.h"

/* ------------------------------------------------------------ outputs */

void write_file_atomic(const char *dir, const char *name, const char *data)
{
	char tmp[512], path[512];
	int fd;
	size_t len = strlen(data);

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, data, len) != (ssize_t)len) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) < 0)
		unlink(tmp);
}

static void fmt_int(char *buf, size_t len, int v)
{
	if (v == POWERD_NA || v == POWERD_ABSENT)
		snprintf(buf, len, "na");
	else
		snprintf(buf, len, "%d", v);
}

/* uV/uA -> mV/mA, "na" kept. */
static void fmt_milli(char *buf, size_t len, int v)
{
	if (v == POWERD_NA)
		snprintf(buf, len, "na");
	else
		snprintf(buf, len, "%d", v / 1000);
}

static void fmt_tempc(char *buf, size_t len, int dc)
{
	if (dc == POWERD_NA)
		snprintf(buf, len, "na");
	else
		fmt_temp(buf, len, dc);
}

size_t state_format(const struct powerd *p, const struct sample *s, double uptime,
		    char *buf, size_t len)
{
	char cap[16], mv[16], ma[16], tc[16], ocv[16], full[16], cyc[16], rdy[16];
	char umax[16], uin[16], nstl[16], stl[16], cc[16], uv[16], settled[16], fcc[16];

	fmt_int(cap, sizeof(cap), s->capacity);
	fmt_milli(mv, sizeof(mv), s->voltage_uv);
	fmt_milli(ma, sizeof(ma), s->current_ua);
	fmt_tempc(tc, sizeof(tc), s->temp);
	fmt_milli(ocv, sizeof(ocv), s->ocv_uv);
	fmt_int(full, sizeof(full), s->charge_full_uah);
	fmt_int(cyc, sizeof(cyc), s->cycle_count);
	fmt_int(rdy, sizeof(rdy), s->soc_ready);
	fmt_milli(umax, sizeof(umax), s->usb_current_max_ua);
	fmt_milli(uin, sizeof(uin), s->usb_input_ua);
	fmt_int(nstl, sizeof(nstl), s->nstl);
	fmt_int(cc, sizeof(cc), s->charge_counter_uah);
	fmt_int(stl, sizeof(stl), s->stl);
	fmt_milli(uv, sizeof(uv), s->usb_voltage_uv);
	fmt_milli(settled, sizeof(settled), s->input_settled_ua);
	fmt_milli(fcc, sizeof(fcc), s->profile_fcc_ua);
	return (size_t)snprintf(buf, len,
		"uptime=%.1f\n"
		"status=%s\n"
		"source=%s\n"
		"online=%d\n"
		"usb_type=%s\n"
		"capacity=%s\n"
		"voltage_mv=%s\n"
		"current_ma=%s\n"
		"temp_c=%s\n"
		"health=%s\n"
		"charge_type=%s\n"
		"ocv_mv=%s\n"
		"charge_full_uah=%s\n"
		"charge_counter_uah=%s\n"
		"cycle_count=%s\n"
		"soc_ready=%s\n"
		"usb_current_max_ma=%s\n"
		"usb_input_ma=%s\n"
		"usb_voltage_mv=%s\n"
		"input_settled_ma=%s\n"
		"profile_fcc_ma=%s\n"
		"typec_mode=%s\n"
		"throttle_level=%d\n"
		"system_temp_level=%s\n"
		"num_system_temp_levels=%s\n"
		"alert=%s\n"
		"shutdown_reason=%s\n"
		"overrides=%s\n",
		uptime, s->status[0] ? s->status : "na", source_name(s), supply_online(s) ? 1 : 0,
		s->usb_type[0] ? s->usb_type : "na", cap, mv, ma, tc,
		s->health[0] ? s->health : "na", s->charge_type[0] ? s->charge_type : "na",
		ocv, full, cc, cyc, rdy, umax, uin, uv, settled, fcc, s->typec_mode[0] ? s->typec_mode : "na",
		p->level, stl, nstl, alert_name(p), p->shutdown_reason, p->overrides);
}

const char csv_header[] =
	"uptime,utc,reason,status,source,usb_type,capacity,voltage_mv,current_ma,temp_c,"
	"health,charge_type,ocv_mv,charge_full_uah,charge_counter_uah,cycle_count,soc_ready,usb_current_max_ma,"
	"usb_input_ma,usb_voltage_mv,input_settled_ma,profile_fcc_ma,typec_mode,pc_port_online,dc_online,system_temp_level,throttle_level,alert,boot_id\n";

size_t csv_format(const struct powerd *p, const struct sample *s, double uptime,
		  const char *utc, const char *reason, char *buf, size_t len)
{
	char cap[16], mv[16], ma[16], tc[16], ocv[16], full[16], cyc[16], rdy[16];
	char umax[16], uin[16], dc[16], pc[16], stl[16], cc[16], uv[16], settled[16], fcc[16];

	fmt_int(cap, sizeof(cap), s->capacity);
	fmt_milli(mv, sizeof(mv), s->voltage_uv);
	fmt_milli(ma, sizeof(ma), s->current_ua);
	fmt_tempc(tc, sizeof(tc), s->temp);
	fmt_milli(ocv, sizeof(ocv), s->ocv_uv);
	fmt_int(full, sizeof(full), s->charge_full_uah);
	fmt_int(cyc, sizeof(cyc), s->cycle_count);
	fmt_int(rdy, sizeof(rdy), s->soc_ready);
	fmt_milli(umax, sizeof(umax), s->usb_current_max_ua);
	fmt_milli(uin, sizeof(uin), s->usb_input_ua);
	fmt_int(dc, sizeof(dc), s->dc_online);
	fmt_int(pc, sizeof(pc), s->pc_online);
	fmt_int(cc, sizeof(cc), s->charge_counter_uah);
	fmt_int(stl, sizeof(stl), s->stl);
	fmt_milli(uv, sizeof(uv), s->usb_voltage_uv);
	fmt_milli(settled, sizeof(settled), s->input_settled_ua);
	fmt_milli(fcc, sizeof(fcc), s->profile_fcc_ua);
	return (size_t)snprintf(buf, len, "%.1f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%s\n",
		uptime, utc, reason, s->status, source_name(s), s->usb_type, cap, mv, ma, tc,
		s->health, s->charge_type, ocv, full, cc, cyc, rdy, umax, uin, uv, settled, fcc, s->typec_mode, pc, dc, stl,
		p->level, alert_name(p), strcmp(reason, "start") ? "" : p->boot_id);
}

/* Does the file at path start with csv_header? */
static bool csv_header_matches(const char *path)
{
	char buf[sizeof(csv_header)];
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(csv_header) - 1);
	close(fd);
	return n == (ssize_t)sizeof(csv_header) - 1 && !memcmp(buf, csv_header, (size_t)n);
}

/*
 * Append one row to LOGDIR/log.csv, starting a file with the header, and
 * rotating to log.csv.1 (replacing it) first if the row would take the
 * file past log_max or the file has another header (an older format).
 */
static int csv_append(const struct powerd *p, const char *row)
{
	const char *dir = p->logdir ? p->logdir : p->dir;
	char path[512], old[512];
	struct stat st;
	size_t rl = strlen(row);
	int fd;

	snprintf(path, sizeof(path), "%s/log.csv", dir);
	if (stat(path, &st) == 0 && st.st_size > 0 &&
	    ((long)st.st_size + (long)rl > p->log_max || !csv_header_matches(path))) {
		snprintf(old, sizeof(old), "%s/log.csv.1", dir);
		if (rename(path, old) < 0)
			return -errno;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) == 0 && st.st_size == 0 &&
	    write(fd, csv_header, sizeof(csv_header) - 1) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	if (write(fd, row, rl) != (ssize_t)rl) {
		int err = errno;

		close(fd);
		return -err;
	}
	close(fd);
	return 0;
}

/* ------------------------------------------------- -l: batched log.csv */

static void pending_path(const struct powerd *p, char *buf, size_t len)
{
	snprintf(buf, len, "%s/log.pending", p->dir);
}

/* The whole of path into a malloc'd, NUL-terminated buffer. NULL with
 * *len 0 if it does not exist; NULL with -errno in *err on a read error. */
static char *read_file(const char *path, size_t *len, int *err)
{
	struct stat st;
	char *buf;
	size_t n = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	*len = 0;
	*err = 0;
	if (fd < 0) {
		*err = errno == ENOENT ? 0 : -errno;
		return NULL;
	}
	if (fstat(fd, &st) < 0 || !(buf = malloc((size_t)st.st_size + 1))) {
		*err = -ENOMEM;
		close(fd);
		return NULL;
	}
	while (n < (size_t)st.st_size) {
		ssize_t r = read(fd, buf + n, (size_t)st.st_size - n);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			*err = -errno;
			free(buf);
			close(fd);
			return NULL;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	buf[n] = '\0';
	*len = n;
	return buf;
}

/* Replace log.pending with len bytes of data (tmp + rename, tmpfs). */
static int pending_replace(const struct powerd *p, const char *data, size_t len)
{
	char path[512], tmp[512];
	int fd;

	pending_path(p, path, sizeof(path));
	if (len == 0)
		return truncate(path, 0) < 0 && errno != ENOENT ? -errno : 0;
	snprintf(tmp, sizeof(tmp), "%s/.log.pending.tmp", p->dir);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (write(fd, data, len) != (ssize_t)len) {
		int err = errno ? errno : EIO;

		close(fd);
		unlink(tmp);
		return -err;
	}
	close(fd);
	if (rename(tmp, path) < 0) {
		int err = errno;

		unlink(tmp);
		return -err;
	}
	return 0;
}

/* Write all of buf; -errno on error (a partial write may have happened). */
static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? -errno : -EIO;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * Append the complete rows of log.pending to LOGDIR/log.csv with the same
 * rules as csv_append: a header on a new file, rotation to log.csv.1 first
 * when another header is there or the rows would pass log_max (a batch
 * bigger than that is split at row boundaries, so the file stays within
 * log_max unless one row alone is bigger). Usually one write. With durable,
 * log.csv and LOGDIR are fsynced afterwards, even with nothing to add. A failed write
 * truncates log.csv back to its size before it, so no partial row is left
 * to be duplicated by the retry. Rows that reached log.csv are removed from
 * log.pending, the rest stay. 0, or -errno of the first failure.
 */
int log_flush(struct powerd *p, bool durable)
{
	char path[512], old[512], pp[512];
	size_t len, end, off = 0;
	char *buf;
	int rc = 0, err;

	pending_path(p, pp, sizeof(pp));
	buf = read_file(pp, &len, &err);
	if (!buf)
		return err;
	/* a torn last row (a powerd killed mid-append) is never flushed */
	for (end = len; end > 0 && buf[end - 1] != '\n'; end--)
		;
	snprintf(path, sizeof(path), "%s/log.csv", p->logdir);
	snprintf(old, sizeof(old), "%s/log.csv.1", p->logdir);
	while (off < end) {
		struct stat st;
		size_t chunk = end - off, hl = sizeof(csv_header) - 1;
		off_t size = stat(path, &st) == 0 ? st.st_size : 0;
		long room;
		int fd;

		if (size > 0 && !csv_header_matches(path)) {
			if (rename(path, old) < 0) {
				rc = -errno;
				break;
			}
			size = 0;
		}
		room = p->log_max - (size > 0 ? (long)size : (long)hl);
		if ((long)chunk > room) {
			/* the whole rows that fit */
			size_t fit = 0, i;

			for (i = off; i < end && (long)(i - off) < room; i++)
				if (buf[i] == '\n' && (long)(i + 1 - off) <= room)
					fit = i + 1 - off;
			if (fit == 0 && size > 0) {
				if (rename(path, old) < 0) {
					rc = -errno;
					break;
				}
				continue;
			}
			if (fit == 0)	/* one row bigger than log_max on its own */
				fit = (size_t)((char *)memchr(buf + off, '\n', end - off) - (buf + off)) + 1;
			chunk = fit;
		}
		fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (fd < 0) {
			rc = -errno;
			break;
		}
		if (fstat(fd, &st) == 0)
			size = st.st_size;
		rc = size == 0 ? write_all(fd, csv_header, hl) : 0;
		if (rc == 0)
			rc = write_all(fd, buf + off, chunk);
		if (rc < 0) {
			if (ftruncate(fd, size) < 0)
				p->log("log.csv in %s: cannot truncate a failed append (%s)",
				       p->logdir, strerror(errno));
			close(fd);
			break;
		}
		close(fd);
		off += chunk;
	}
	/* durable: log.csv and the directory, also when this flush had
	 * nothing to add (the ordinary batches before it were not synced) */
	if (rc == 0 && durable) {
		int fd = open(path, O_RDONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (p->sync_fd(fd) < 0)
				rc = -errno;
			close(fd);
		}
		fd = open(p->logdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd >= 0) {
			if (p->sync_fd(fd) < 0 && rc == 0)
				rc = -errno;
			close(fd);
		}
	}
	/* keep only the rows that did not make it (and drop a torn tail) */
	if (off > 0 || end < len) {
		err = pending_replace(p, buf + off, end - off);
		if (err < 0 && rc == 0)
			rc = err;
	}
	free(buf);
	return rc;
}

/* Flush now and report a failure once, with recovery. */
void log_flush_now(struct powerd *p, bool durable, int64_t now)
{
	int rc = log_flush(p, durable);

	if (rc < 0) {
		if (!p->log_failed)
			p->log("log.csv in %s: %s; rows kept in %s/log.pending until it works again",
			       p->logdir, strerror(-rc), p->dir);
		p->log_failed = true;
		p->flush_due = now + p->flush_ms;
		return;
	}
	if (p->log_failed) {
		if (p->pending_dropped)
			p->log("log.csv in %s writable again (%ld oldest rows were dropped meanwhile)",
			       p->logdir, p->pending_dropped);
		else
			p->log("log.csv in %s writable again", p->logdir);
	}
	p->log_failed = false;
	p->pending_dropped = 0;
	p->flush_due = -1;
}

/*
 * Before the first append of this process, cut a torn last row that an
 * earlier powerd (killed mid-write) left in log.pending, so the next row
 * does not run into it. When the file is full, drop its oldest rows (a
 * quarter of the bound at a time, so this is rare) with one log line per
 * failure episode.
 */
static int pending_make_room(struct powerd *p, size_t rl)
{
	char pp[512];
	size_t len, cut = 0, i;
	struct stat st;
	char *buf;
	long rows = 0;
	int err;

	pending_path(p, pp, sizeof(pp));
	if (stat(pp, &st) < 0)
		return errno == ENOENT ? 0 : -errno;
	if (p->pending_checked && st.st_size + (long)rl <= p->pending_max)
		return 0;
	buf = read_file(pp, &len, &err);
	if (!buf)
		return err;
	if (!p->pending_checked)
		while (len > 0 && buf[len - 1] != '\n')
			len--;
	if ((long)(len + rl) > p->pending_max) {
		cut = len + rl - (size_t)p->pending_max + (size_t)p->pending_max / 4;
		if (cut > len)
			cut = len;
		while (cut < len && buf[cut - 1] != '\n')
			cut++;
		for (i = 0; i < cut; i++)
			rows += buf[i] == '\n';
	}
	if (cut > 0 || len < (size_t)st.st_size) {
		err = pending_replace(p, buf + cut, len - cut);
		if (err < 0) {
			free(buf);
			return err;
		}
	}
	free(buf);
	p->pending_checked = true;
	if (rows) {
		if (!p->pending_dropped)
			p->log("%s full: dropped %ld oldest rows; log.csv in %s is not taking them",
			       pp, rows, p->logdir);
		p->pending_dropped += rows;
	}
	return 0;
}

/* Append one row to log.pending; a short write (tmpfs full) is truncated
 * back, so the next row never runs into a torn one. */
static int pending_add(struct powerd *p, const char *row)
{
	char pp[512];
	struct stat st;
	int fd, rc;

	rc = pending_make_room(p, strlen(row));
	if (rc < 0)
		return rc;
	pending_path(p, pp, sizeof(pp));
	fd = open(pp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	rc = write_all(fd, row, strlen(row));
	if (rc < 0 && ftruncate(fd, st.st_size) < 0)
		p->pending_checked = false;	/* cut the torn tail before the next row */
	close(fd);
	return rc;
}

/*
 * One finished row. Without -l: appended to RUNDIR/log.csv at once. With
 * -l: queued in log.pending and flushed when urgent (durable: the shutdown
 * row, fsynced) or when the oldest pending row is flush_ms old.
 */
void log_row(struct powerd *p, const char *row, bool urgent, bool durable, int64_t now)
{
	int rc;

	if (!p->logdir) {
		rc = csv_append(p, row);
		if (rc < 0 && !p->log_failed)
			p->log("log.csv in %s: %s; rows are lost until it works again", p->dir, strerror(-rc));
		else if (rc == 0 && p->log_failed)
			p->log("log.csv in %s writable again", p->dir);
		p->log_failed = rc < 0;
		return;
	}
	rc = pending_add(p, row);
	if (rc < 0 && !p->pending_failed)
		p->log("%s/log.pending: %s; rows are lost until it works again", p->dir, strerror(-rc));
	else if (rc == 0 && p->pending_failed)
		p->log("%s/log.pending writable again", p->dir);
	p->pending_failed = rc < 0;
	if (rc == 0 && p->flush_due < 0)
		p->flush_due = now + p->flush_ms;
	if (urgent || (p->flush_due >= 0 && now >= p->flush_due))
		log_flush_now(p, durable, now);
}

/* -l: flush if the batch interval is up (also between samples). */
void log_flush_if_due(struct powerd *p, int64_t now)
{
	if (p->logdir && p->flush_due >= 0 && now >= p->flush_due)
		log_flush_now(p, false, now);
}

/*
 * `powerd -d RUNDIR -l LOGDIR [-L MAX] flush`: append what a powerd that is
 * no longer running left in RUNDIR/log.pending to LOGDIR, with the same
 * rules and fsyncs as its own exit flush. For chef-state shutdown, only
 * once no powerd can still be writing. 0: done or nothing pending.
 */
int flush_main(struct powerd *p)
{
	int rc = log_flush(p, true);

	if (rc < 0) {
		p->log("flush: log.csv in %s: %s; rows left in %s/log.pending",
		       p->logdir, strerror(-rc), p->dir);
		return 1;
	}
	return 0;
}
