/*
 * gps-manager - demand-driven GNSS for chef-cyclo.
 *
 * gps-up (inittab ::once:) is the boot-resident shared modem owner for GPS
 * and Wi-Fi; this daemon never starts, signals, reaps or restarts it and
 * never changes the modem's operating mode. What it owns is everything
 * GNSS-specific on top of that modem: one QMI LOC client id (CID) with LOC
 * session 1, gpsd, and the `qmicli --loc-follow-nmea | nmea-broker`
 * pipeline. They run only while someone holds a lease on
 * /run/chef/gps-manager.sock (the line protocol is in server.c's header).
 *
 * The GPS switch (`enable`/`disable`) persists through `chef-state
 * gps-save 0|1` (a child, so storage never blocks this loop) and is read
 * back at start with `chef-state gps-load` (bounded; enabled when nothing
 * valid is saved). Until that answer arrives leases are accepted and held,
 * not acted on.
 *
 * A lease lives on its connection, so a client that dies releases it. The
 * first lease starts the bring-up; when the last lease goes (in STARTING,
 * ACQUIRING or RUNNING alike) a 30 s grace starts that a new lease cancels,
 * then the owned set is torn down. A lease during STOPPING lets the teardown
 * finish and then brings everything up again; a teardown is never aborted.
 *
 * States: OFF, STARTING, ACQUIRING, RUNNING, STOPPING, FAILED. RUNNING vs.
 * ACQUIRING is a hint only: RUNNING from a status-A RMC, back to ACQUIRING
 * after 10 s without one. Fix data and positions stay in gpsd; nothing here
 * logs or reports them.
 *
 * Bring-up (every qmicli step is a child bounded at 25 s, so the socket never
 * blocks; a step in flight is never killed early, so a CID it allocates is
 * always learnt and later released):
 *   1. wait for the modem owner, polled every 2 s (the C form of
 *      modem-owner.sh's modem_owner_pid: ready marker, QMUX socket, owner
 *      pid + start time, not a zombie): STARTING for up to 240 s, then
 *      FAILED "no modem owner" until it appears. gps-up runs once, so a
 *      missing owner normally lasts until reboot; no qmicli runs meanwhile.
 *   2. qmicli --loc-noop --client-no-release-cid   (CID; proves the bridge)
 *   3. qmicli --client-cid=N --client-no-release-cid
 *             --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg
 *   4. qmicli ... --loc-start --loc-session-id=1
 *   5. gpsd -N -n -b udp://127.0.0.1:20175          (no -G: loopback only)
 *   6. qmicli ... --loc-follow-nmea | nmea-broker -n -t, both forked and
 *      exec'd directly; the broker's tee comes back to this daemon, which
 *      drains it in every state (NMEA silence, fix hint).
 * Teardown, in reverse: SIGINT the follower (TERM after 5 s, KILL after 8 s;
 * the broker exits at EOF and gets the same escalation), --loc-stop, release
 * the CID (--client-cid=N --loc-noop without --client-no-release-cid), TERM
 * gpsd (KILL after 5 s). loc-stop is sent whenever loc-start was attempted
 * (the modem may have started session 1 even if the step failed). A failed
 * loc-stop still releases; a failed release is logged and counted as leaked
 * (status), never retried. With the QMUX socket gone the qmicli steps are
 * skipped: the CID died with the bridge. One window cannot be closed: if the
 * allocation step is killed (its timeout, or the manager dying) after the
 * bridge allocated a CID but before qmicli printed it, that CID stays in
 * qmuxd-lite without a session (a bridge slot, no GNSS cost); a timed-out
 * allocation is therefore counted as (possibly) leaked.
 *
 * Only this daemon may talk on its CID: qmuxd-lite hands a CID's indications
 * to whoever spoke on it last, so another qmicli on it silences the
 * follower. Manual LOC commands must allocate their own CID.
 *
 * Supervision (from the 2026-09-18 ride image, which restarted only the
 * follower): an owned child that exits, 60 s without an NMEA line, or the
 * modem owner going away or being replaced tears the whole owned set down
 * as one unit. While a lease remains it then retries; with no lease it never
 * does. A run is healthy once NMEA has flowed for 120 s since the pipeline
 * started (gaps are bounded by the 60 s silence rule); reaching healthy
 * resets the fast-failure streak to 0. A failure before healthy is fast and
 * adds one to the streak; the retry waits 2, 4, 8, 16 s for streak 1..4 (2 s
 * after a slow failure), and a streak of 5 parks the manager in FAILED until
 * every lease is released or a client sends `retry` (both reset the streak).
 * A lost or replaced owner is not counted: FAILED waits for an owner.
 *
 * Respawn: init restarts this daemon if it dies. One instance at a time: an
 * flock on DIR/lock is taken before anything else and held for life; a
 * second instance exits. Children survive a SIGKILL of the manager (gpsd
 * drops privileges, which clears PR_SET_PDEATHSIG) and a CID stays allocated
 * in qmuxd-lite with LOC session 1 running until someone stops it, so
 * DIR/state (0600, atomic rename) records the CID with the owner that
 * served it as soon as the CID is known, each child's pid + start time
 * right after its fork, and the manager's own pid (for shells and tests,
 * which signal recorded pids only). A new instance kills the recorded
 * follower, broker and gpsd whose start time and name still match (others
 * are skipped and logged), waits for them, and then stops and releases the
 * recorded CID as soon as the same owner is ready, before any bring-up; if
 * the owner was replaced the CID died with the old bridge and is dropped.
 * Clients of the dead instance lost their leases with their connections
 * and lease again.
 *
 * Usage: gps-manager [-S SOCK] [-d DIR] [-q QMUX_SOCK] [-o OWNER_LOCK]
 *                    [-b BINDIR] [-s STATE_CMD] [-g GRACE_S] [-v]   daemon
 *        gps-manager [-S SOCK] status | watch | retry | enable | disable |
 *                    hold KIND...
 *   -b  run qmicli, gpsd and nmea-broker from BINDIR (tests); default PATH
 *   -s  the chef-state command for the switch (default chef-state; tests)
 * Logs: "gps-manager:" lines through chef_log (kmsg; transitions, not
 * polls); the children's output in DIR (default /run/chef/gps-manager):
 * steps.log, gpsd.log, follow.log and broker.log, each rotated to .1 at a
 * bring-up so the run that failed is kept next to its retry. RAM only.
 *
 * Files: core.c the state machine with injected operations, server.c the
 * lease server and its protocol, owner.c the owner check, /proc start
 * times and the state-file record, cli.c the command-line client, this
 * file the process shell (children, the switch, the poll loop, main);
 * gps-manager.h is their internal header. tests/test_gps-manager.c links
 * core.c, server.c and owner.c and drives them through fake operations;
 * tests/test_gps-manager.sh runs the real binary with stub qmicli/gpsd,
 * the real nmea-broker, kill -9 and a respawn.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common/chef_log.h"
#include "common/linesrv.h"
#include "gps-manager.h"

static bool g_verbose;
static volatile sig_atomic_t g_stop;
static int g_sigpipe[2] = { -1, -1 };
static pid_t g_self;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct shell {
	struct gm m;
	struct gm_server srv;
	struct gm_paths paths;
	const char *dir;
	char bin[3][256];	/* qmicli, gpsd, nmea-broker */
	pid_t pid[CH_COUNT];
	unsigned long long start[CH_COUNT];
	int step_fd;		/* step stdout+stderr, -1 when closed */
	bool step_exited;
	int step_rc;
	char step_out[4096];
	size_t step_len;
	int nmea_fd;		/* broker tee, -1 when closed */
	char nmea_in[512];
	size_t nmea_len;
	/* the switch: chef-state gps-load at start, gps-save on a change */
	const char *state_cmd;
	pid_t load_pid;
	int load_fd;		/* its stdout, -1 when closed */
	bool load_exited;
	int64_t load_until;
	char load_out[64];
	size_t load_len;
	pid_t save_pid;
	int64_t save_until;
	bool save_pending;	/* changed again while a save ran */
	bool save_value;
};

static void shell_save(struct shell *sh)
{
	struct gm_record r;
	char buf[256], path[256], tmp[256];
	int fd, i;

	memset(&r, 0, sizeof(r));
	/* a pending stale CID stays recorded until it is handled */
	if (sh->m.stale_cid > 0) {
		r.cid = sh->m.stale_cid;
		r.owner = sh->m.stale_owner;
	} else {
		r.cid = sh->m.cid;
		if (sh->m.cid > 0)
			r.owner = sh->m.cid_owner;
	}
	for (i = 0; i < CH_STEP; i++) {
		r.pid[i] = sh->pid[i];
		r.start[i] = sh->start[i];
	}
	if (gm_record_format(&r, buf, sizeof(buf)))
		return;
	/* for tests and shells: signal the manager by this pid, never by name */
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "manager %d\n", (int)g_self);
	snprintf(path, sizeof(path), "%s/state", sh->dir);
	snprintf(tmp, sizeof(tmp), "%s/state.tmp", sh->dir);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		chef_log("cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf) || fsync(fd) != 0) {
		chef_log("cannot write %s: %s; %s keeps its previous contents", tmp, strerror(errno), path);
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) != 0) {
		chef_log("cannot rename %s: %s", tmp, strerror(errno));
		unlink(tmp);
	}
}

static void op_log(void *ctx, const char *msg)
{
	(void)ctx;
	chef_log("%s", msg);
}

static bool op_owner(void *ctx, struct gm_owner *o)
{
	struct shell *sh = ctx;

	return gm_owner_check(&sh->paths, o);
}

static bool op_bridge(void *ctx)
{
	struct shell *sh = ctx;

	return gm_bridge_present(&sh->paths);
}

static int open_log(struct shell *sh, const char *name, bool trunc)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", sh->dir, name);
	return open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (trunc ? O_TRUNC : O_APPEND), 0600);
}

static void op_rotate(void *ctx)
{
	static const char *const logs[] = { "steps.log", "gpsd.log", "follow.log", "broker.log" };
	struct shell *sh = ctx;
	char a[256], b[256];
	size_t i;

	for (i = 0; i < sizeof(logs) / sizeof(logs[0]); i++) {
		snprintf(a, sizeof(a), "%s/%s", sh->dir, logs[i]);
		snprintf(b, sizeof(b), "%s/%s.1", sh->dir, logs[i]);
		rename(a, b);
	}
}

/*
 * fork + exec, the given fds as stdin/stdout/stderr (-1 = /dev/null). Every
 * fd of ours is O_CLOEXEC, so the child inherits only its stdio. The child
 * dies with us (PR_SET_PDEATHSIG; if we died before the prctl it exits).
 */
static pid_t spawn(const char *path, char *const argv[], int in, int out, int err)
{
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != g_self)
			_exit(127);
		setpgid(0, 0);
		dup2(in >= 0 ? in : nul, 0);
		dup2(out >= 0 ? out : nul, 1);
		dup2(err >= 0 ? err : nul, 2);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execvp(path, argv);
		_exit(127);
	}
	setpgid(pid, pid);	/* also from here: no window before the child's own */
	return pid;
}

static void shell_started(struct shell *sh, enum gm_child c, pid_t pid)
{
	unsigned long long start = 0;

	sh->pid[c] = pid;
	gm_proc_start(sh->paths.proc, pid, &start, NULL, NULL, 0);
	sh->start[c] = start;
	if (c != CH_STEP)
		shell_save(sh);
}

static void log_line(struct shell *sh, const char *name, const char *text, size_t len)
{
	int fd = open_log(sh, name, false);

	if (fd < 0)
		return;
	if (write(fd, text, len) < 0) {
		/* best effort */
	}
	close(fd);
}

static int op_run_step(void *ctx, char *const argv[])
{
	struct shell *sh = ctx;
	char line[512];
	size_t off = 0;
	int p[2], i;
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0)
		return -errno;
	pid = spawn(sh->bin[0], argv, -1, p[1], p[1]);
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return (int)pid;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	sh->step_fd = p[0];
	sh->step_exited = false;
	sh->step_rc = 0;
	sh->step_len = 0;
	sh->step_out[0] = '\0';
	shell_started(sh, CH_STEP, pid);
	for (i = 0; argv[i] && off < sizeof(line) - 2; i++)
		off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "$ ", argv[i]);
	if (off > sizeof(line) - 2)
		off = sizeof(line) - 2;
	line[off++] = '\n';
	log_line(sh, "steps.log", line, off);
	return 0;
}

static int op_spawn_gpsd(void *ctx)
{
	struct shell *sh = ctx;
	char *argv[] = { "gpsd", "-N", "-n", "-b", GM_GPSD_SOURCE, NULL };
	int logfd = open_log(sh, "gpsd.log", true);
	pid_t pid = spawn(sh->bin[1], argv, -1, logfd, logfd);

	if (logfd >= 0)
		close(logfd);
	if (pid < 0)
		return (int)pid;
	shell_started(sh, CH_GPSD, pid);
	return 0;
}

static int op_spawn_pipeline(void *ctx, int cid)
{
	struct shell *sh = ctx;
	char cidarg[32];
	char *follow[] = { "qmicli", "-d", (char *)sh->m.qmux_sock, cidarg, "--client-no-release-cid",
			   "--loc-follow-nmea", NULL };
	char *broker[] = { "nmea-broker", "-n", "-t", NULL };
	int a[2], b[2], flog, blog;
	pid_t fp = -1, bp;

	snprintf(cidarg, sizeof(cidarg), "--client-cid=%d", cid);
	if (pipe2(a, O_CLOEXEC) < 0)
		return -errno;
	if (pipe2(b, O_CLOEXEC) < 0) {
		int e = -errno;

		close(a[0]);
		close(a[1]);
		return e;
	}
	flog = open_log(sh, "follow.log", true);
	blog = open_log(sh, "broker.log", true);
	bp = spawn(sh->bin[2], broker, a[0], b[1], blog);
	if (bp > 0) {
		shell_started(sh, CH_BROKER, bp);
		fp = spawn(sh->bin[0], follow, -1, a[1], flog);
		if (fp > 0)
			shell_started(sh, CH_FOLLOW, fp);
	}
	close(a[0]);
	close(a[1]);
	close(b[1]);
	if (flog >= 0)
		close(flog);
	if (blog >= 0)
		close(blog);
	fcntl(b[0], F_SETFL, O_NONBLOCK);
	sh->nmea_fd = b[0];	/* drained even if the follower failed to start */
	sh->nmea_len = 0;
	if (bp < 0)
		return (int)bp;
	if (fp < 0) {
		/* the broker is recorded and reaped like any other child; the
		 * core only marks it alive once both started, so tell it now */
		sh->m.alive[CH_BROKER] = true;
		return (int)fp;
	}
	return 0;
}

static void op_signal(void *ctx, enum gm_child c, int sig)
{
	struct shell *sh = ctx;

	if (sh->pid[c] > 0)
		kill(sh->pid[c], sig);
}

static void op_save(void *ctx, const struct gm *m)
{
	(void)m;
	shell_save(ctx);
}

static void op_changed(void *ctx, const struct gm *m)
{
	struct shell *sh = ctx;

	(void)m;
	gm_server_broadcast(&sh->srv);
}

/* Save the switch with chef-state in a child; one at a time, the last
 * value wins. A failure is logged by chef-state itself. */
static void shell_save_switch(struct shell *sh)
{
	char *argv[] = { (char *)sh->state_cmd, "gps-save", sh->save_value ? "1" : "0", NULL };
	pid_t pid;

	if (sh->save_pid > 0) {
		sh->save_pending = true;
		return;
	}
	sh->save_pending = false;
	pid = spawn(sh->state_cmd, argv, -1, -1, -1);
	if (pid < 0) {
		chef_log("cannot run %s gps-save: %s; the switch is not saved", sh->state_cmd, strerror(-pid));
	} else {
		sh->save_pid = pid;
		sh->save_until = now_ms() + GM_SAVE_TIMEOUT_MS;
	}
}

/* A save that hangs (storage stuck) is killed after its bound, so a later
 * change still gets its turn; reaped as any child. */
static void shell_save_check(struct shell *sh)
{
	if (sh->save_pid > 0 && sh->save_until && now_ms() >= sh->save_until) {
		chef_log("%s gps-save still running after %d s: killed; the switch may not be saved",
		      sh->state_cmd, GM_SAVE_TIMEOUT_MS / 1000);
		kill(-sh->save_pid, SIGKILL);	/* its group: a lock holder dies too */
		sh->save_until = 0;
	}
}

static void op_persist(void *ctx, bool on)
{
	struct shell *sh = ctx;

	sh->save_value = on;
	shell_save_switch(sh);
}

/* The saved switch, read once at start (bounded). */
static void shell_load_start(struct shell *sh)
{
	char *argv[] = { (char *)sh->state_cmd, "gps-load", NULL };
	int p[2];
	pid_t pid;

	sh->load_fd = -1;
	if (pipe2(p, O_CLOEXEC) < 0) {
		chef_log("pipe: %s; GPS switch taken as enabled", strerror(errno));
		gm_server_set_enabled(&sh->srv, true, false);
		return;
	}
	pid = spawn(sh->state_cmd, argv, -1, p[1], -1);
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		chef_log("cannot run %s gps-load: %s; GPS switch taken as enabled", sh->state_cmd, strerror(-pid));
		gm_server_set_enabled(&sh->srv, true, false);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	sh->load_pid = pid;
	sh->load_fd = p[0];
	sh->load_until = now_ms() + GM_LOAD_TIMEOUT_MS;
	sh->m.loaded = false;
	gm_set(&sh->m, ST_OFF, "reading the GPS switch");
}

/* Done once the loader exited and its pipe is at EOF (or it timed out). */
static void shell_load_check(struct shell *sh, bool timed_out)
{
	bool on = true;

	if (!timed_out && (!sh->load_exited || sh->load_fd >= 0))
		return;
	sh->load_until = 0;
	if (sh->load_fd >= 0) {
		close(sh->load_fd);
		sh->load_fd = -1;
	}
	sh->load_exited = false;
	if (timed_out)
		chef_log("%s gps-load gave no answer in %d s: GPS switch taken as enabled", sh->state_cmd,
		      GM_LOAD_TIMEOUT_MS / 1000);
	else if (strcmp(sh->load_out, "enabled 0\n") == 0)
		on = false;
	else if (strcmp(sh->load_out, "enabled 1\n") != 0)
		chef_log("%s gps-load: unexpected answer; GPS switch taken as enabled", sh->state_cmd);
	if (!sh->m.loaded)	/* a client's enable/disable since start wins */
		gm_server_set_enabled(&sh->srv, on, false);
}

static void shell_load_read(struct shell *sh)
{
	char buf[64];
	ssize_t n = read(sh->load_fd, buf, sizeof(buf));

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		close(sh->load_fd);
		sh->load_fd = -1;
		shell_load_check(sh, false);
		return;
	}
	if (sh->load_len + (size_t)n < sizeof(sh->load_out)) {
		memcpy(sh->load_out + sh->load_len, buf, (size_t)n);
		sh->load_len += (size_t)n;
		sh->load_out[sh->load_len] = '\0';
	}
}

/* A step is finished once it has exited and its output pipe is at EOF. */
static void shell_step_check(struct shell *sh)
{
	if (!sh->step_exited || sh->step_fd >= 0)
		return;
	sh->step_exited = false;
	gm_step_done(&sh->m, now_ms(), sh->step_rc, sh->step_out);
}

static void shell_step_read(struct shell *sh)
{
	char buf[1024];
	ssize_t n = read(sh->step_fd, buf, sizeof(buf));

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		close(sh->step_fd);
		sh->step_fd = -1;
		shell_step_check(sh);
		return;
	}
	log_line(sh, "steps.log", buf, (size_t)n);
	if (sh->step_len < sizeof(sh->step_out) - 1) {
		size_t k = (size_t)n;

		if (k > sizeof(sh->step_out) - 1 - sh->step_len)
			k = sizeof(sh->step_out) - 1 - sh->step_len;
		memcpy(sh->step_out + sh->step_len, buf, k);
		sh->step_len += k;
		sh->step_out[sh->step_len] = '\0';
	}
}

/* Drain the broker's tee completely: it must never block on it. */
static void shell_nmea_read(struct shell *sh)
{
	char buf[1024];

	for (;;) {
		ssize_t n = read(sh->nmea_fd, buf, sizeof(buf));
		ssize_t i;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			close(sh->nmea_fd);
			sh->nmea_fd = -1;
			return;
		}
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				sh->nmea_in[sh->nmea_len] = '\0';
				gm_nmea_line(&sh->m, now_ms(), sh->nmea_in);
				sh->nmea_len = 0;
			} else if (sh->nmea_len < sizeof(sh->nmea_in) - 1) {
				sh->nmea_in[sh->nmea_len++] = buf[i];
			}	/* else: overlong junk, truncated */
		}
	}
}

static void shell_reap(struct shell *sh)
{
	int status, c;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

		if (pid == sh->save_pid) {
			sh->save_pid = 0;
			if (rc)
				chef_log("%s gps-save exited %d: the switch may not be saved", sh->state_cmd, rc);
			if (sh->save_pending)
				shell_save_switch(sh);
			continue;
		}
		if (pid == sh->load_pid) {
			sh->load_pid = 0;
			sh->load_exited = true;
			shell_load_check(sh, false);
			continue;
		}
		for (c = 0; c < CH_COUNT; c++)
			if (sh->pid[c] == pid)
				break;
		if (c == CH_COUNT)
			continue;
		sh->pid[c] = 0;
		sh->start[c] = 0;
		if (c == CH_STEP) {
			sh->step_exited = true;
			sh->step_rc = rc;
			shell_step_check(sh);
		} else {
			chef_log("%s (pid %d) exited %d", gm_child_names[c], (int)pid, rc);
			shell_save(sh);
			gm_child_exited(&sh->m, now_ms(), (enum gm_child)c, rc);
		}
	}
}

/*
 * A previous instance died: kill its recorded follower, broker and gpsd
 * (only if pid, start time and name all still match), wait for them, and
 * hand its CID to the core as stale, to be stopped and released once the
 * same owner is ready.
 */
static void shell_cleanup_previous(struct shell *sh)
{
	char path[256], buf[512];
	struct gm_record r;
	ssize_t n;
	int fd, c, i;

	snprintf(path, sizeof(path), "%s/state", sh->dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = '\0';
	gm_record_parse(&r, buf);
	for (c = 0; c < CH_STEP; c++) {
		if (!r.pid[c])
			continue;
		if (!gm_record_match(sh->paths.proc, &r, c)) {
			chef_log("previous %s pid %d: gone or no longer ours, not signalled", gm_child_names[c], r.pid[c]);
			continue;
		}
		chef_log("stopping the previous instance's %s (pid %d)", gm_child_names[c], r.pid[c]);
		kill(r.pid[c], c == CH_FOLLOW ? SIGINT : SIGTERM);
		for (i = 0; i < 30 && gm_record_match(sh->paths.proc, &r, c); i++)
			usleep(100000);
		if (gm_record_match(sh->paths.proc, &r, c)) {
			kill(r.pid[c], SIGKILL);
			for (i = 0; i < 30 && gm_record_match(sh->paths.proc, &r, c); i++)
				usleep(100000);
		}
	}
	if (r.cid > 0) {
		sh->m.stale_cid = r.cid;
		sh->m.stale_owner = r.owner;
		chef_log("previous instance held LOC CID %d (owner pid %d): releasing it when that owner is ready",
		      r.cid, r.owner.pid);
	}
}

static void wake(char c)
{
	int e = errno;

	if (g_sigpipe[1] >= 0 && write(g_sigpipe[1], &c, 1) < 0) {
		/* full: a wakeup is already pending */
	}
	errno = e;
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
	wake('s');
}

static void on_chld(int sig)
{
	(void)sig;
	wake('c');
}

static struct shell g_sh;

static int64_t srv_now(void)
{
	return now_ms();
}

/* One poll round: signals, children, step output, NMEA, clients, tick.
 * SERVING false: the socket is closed already (shutdown). */
static void shell_round(struct shell *sh, bool serving)
{
	struct pollfd pfd[4 + 1 + GM_MAX_CLIENTS];
	int n = 0, nsrv = 0, istep = -1, inmea = -1, iload = -1, isrv;

	pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
	if (sh->step_fd >= 0) {
		istep = n;
		pfd[n++] = (struct pollfd){ .fd = sh->step_fd, .events = POLLIN };
	}
	if (sh->nmea_fd >= 0) {
		inmea = n;
		pfd[n++] = (struct pollfd){ .fd = sh->nmea_fd, .events = POLLIN };
	}
	if (sh->load_fd >= 0) {
		iload = n;
		pfd[n++] = (struct pollfd){ .fd = sh->load_fd, .events = POLLIN };
	}
	isrv = n;
	if (serving) {
		nsrv = linesrv_pollfds(sh->srv.srv, pfd + n, 1 + GM_MAX_CLIENTS);
		n += nsrv;
	}
	if (poll(pfd, (nfds_t)n, serving ? GM_TICK_MS : 200) < 0 && errno != EINTR)
		return;
	if (pfd[0].revents & POLLIN) {
		char junk[64];

		while (read(g_sigpipe[0], junk, sizeof(junk)) > 0)
			;
	}
	if (inmea >= 0 && sh->nmea_fd >= 0 && (pfd[inmea].revents & (POLLIN | POLLHUP | POLLERR)))
		shell_nmea_read(sh);
	if (istep >= 0 && sh->step_fd >= 0 && (pfd[istep].revents & (POLLIN | POLLHUP | POLLERR)))
		shell_step_read(sh);
	if (iload >= 0 && sh->load_fd >= 0 && (pfd[iload].revents & (POLLIN | POLLHUP | POLLERR)))
		shell_load_read(sh);
	shell_reap(sh);
	/* bounded even when a client set the switch first: the loader goes */
	if (sh->load_until && now_ms() >= sh->load_until) {
		if (sh->load_pid > 0)
			kill(-sh->load_pid, SIGKILL);	/* its group; reaped as any child */
		shell_load_check(sh, true);
	}
	shell_save_check(sh);
	if (nsrv)
		linesrv_handle(sh->srv.srv, pfd + isrv, nsrv);
	gm_tick(&sh->m, now_ms());
}

int main(int argc, char **argv)
{
	struct shell *sh = &g_sh;
	const char *sock_path = CHEF_GPS_MANAGER_SOCK, *bindir = NULL;
	struct gm_ops ops = {
		.ctx = sh, .log = op_log, .owner = op_owner, .bridge = op_bridge,
		.run_step = op_run_step, .spawn_gpsd = op_spawn_gpsd, .spawn_pipeline = op_spawn_pipeline,
		.signal = op_signal, .save = op_save, .rotate = op_rotate, .changed = op_changed,
		.persist = op_persist,
	};
	struct sigaction sa;
	char path[256];
	int rc, lockfd, opt, grace_s = GM_GRACE_MS / 1000;

	memset(sh, 0, sizeof(*sh));
	sh->dir = CHEF_GPS_MANAGER_DIR;
	sh->paths.proc = "/proc";
	sh->paths.owner_lock = CHEF_GPS_UP_LOCK;
	sh->paths.qmux_sock = CHEF_QMUX_SOCK;
	sh->state_cmd = "chef-state";
	while ((opt = getopt(argc, argv, "S:d:q:o:b:s:g:v")) != -1) {
		switch (opt) {
		case 'S': sock_path = optarg; break;
		case 'd': sh->dir = optarg; break;
		case 'q': sh->paths.qmux_sock = optarg; break;
		case 'o': sh->paths.owner_lock = optarg; break;
		case 'b': bindir = optarg; break;
		case 's': sh->state_cmd = optarg; break;
		case 'g': grace_s = atoi(optarg); break;
		case 'v': g_verbose = true; break;
		default: gm_usage(); return 64;
		}
	}
	if (optind < argc)
		return gm_cli_main(sock_path, argc - optind, argv + optind);
	chef_log_init("gps-manager", g_verbose);
	if (grace_s < 0) {
		gm_usage();
		return 64;
	}
	snprintf(sh->bin[0], sizeof(sh->bin[0]), "%s%sqmicli", bindir ? bindir : "", bindir ? "/" : "");
	snprintf(sh->bin[1], sizeof(sh->bin[1]), "%s%sgpsd", bindir ? bindir : "", bindir ? "/" : "");
	snprintf(sh->bin[2], sizeof(sh->bin[2]), "%s%snmea-broker", bindir ? bindir : "", bindir ? "/" : "");
	g_self = getpid();

	/* one instance: the lock comes before the state file and the socket */
	mkdir(sh->dir, 0700);
	snprintf(path, sizeof(path), "%s/lock", sh->dir);
	lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		int e = errno;

		chef_log("%s: %s; another gps-manager is running, exiting in 5 s", path, strerror(e));
		fprintf(stderr, "gps-manager: already running (%s: %s)\n", path, strerror(e));
		sleep(5);	/* keeps a respawning init from spinning */
		return 1;
	}

	sh->step_fd = sh->nmea_fd = sh->load_fd = -1;
	gm_init(&sh->m, &ops);
	sh->m.qmicli = "qmicli";
	sh->m.qmux_sock = sh->paths.qmux_sock;
	sh->m.grace_ms = grace_s * 1000;
	if (gm_server_init(&sh->srv, &sh->m, srv_now, NULL)) {
		chef_log("out of memory; exiting in 5 s");
		sleep(5);
		return 1;
	}

	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		chef_log("pipe: %s; exiting in 5 s", strerror(errno));
		sleep(5);
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	shell_cleanup_previous(sh);
	shell_save(sh);
	shell_load_start(sh);

	/* the stale path is unlinked only ever while holding the instance lock */
	rc = linesrv_listen(sh->srv.srv, sock_path);
	if (rc == 0 && chmod(sock_path, 0660) < 0)
		rc = -errno;
	if (rc < 0) {
		chef_log("listen %s: %s; exiting in 5 s", sock_path, strerror(-rc));
		sleep(5);
		return 1;
	}
	chef_log("ready on %s, idle until leased", sock_path);

	while (!g_stop)
		shell_round(sh, true);

	/* orderly shutdown (init's SIGTERM): everything down, bounded */
	chef_log("exiting: tearing down");
	gm_server_fini(&sh->srv);	/* clients closed, the socket unlinked */
	sh->m.leases = 0;
	sh->m.grace_at = 0;
	sh->m.failing = false;
	gm_teardown(&sh->m, now_ms());
	{
		int64_t until = now_ms() + 3 * GM_STEP_TIMEOUT_MS;

		while (sh->m.phase != PH_IDLE && now_ms() < until)
			shell_round(sh, false);
	}
	/* the last switch change is saved before we go (bounded): a saver
	 * killed by our exit would leave the old value on disk */
	{
		int64_t until = now_ms() + GM_SAVE_DRAIN_MS;

		while ((sh->save_pid > 0 || sh->save_pending) && now_ms() < until) {
			if (sh->save_pid <= 0)
				shell_save_switch(sh);
			if (sh->save_pid > 0)
				shell_round(sh, false);
		}
		if (sh->save_pid > 0 || sh->save_pending) {
			chef_log("exiting with the GPS switch save unfinished: the saved value may be stale");
			if (sh->save_pid > 0)
				kill(-sh->save_pid, SIGKILL);
		}
	}
	if (sh->load_pid > 0)
		kill(-sh->load_pid, SIGKILL);
	if (sh->m.phase == PH_IDLE && sh->m.stale_cid < 0 && sh->m.cid < 0) {
		snprintf(path, sizeof(path), "%s/state", sh->dir);
		unlink(path);
	}
	chef_log("exited %s (%s)", gm_state_names[sh->m.state], gm_phase_names[sh->m.phase]);
	return 0;
}
