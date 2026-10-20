/*
 * blesensd's records: the sensors saved and loaded through chef-state
 * children in their own process group, bounded. See main.c.
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
#include <unistd.h>

#include "blesensd.h"

/* ------------------------------------------------------------ records */

/* chef-state children: their own process group, killed as a group. */
static pid_t spawn_state(const char *verb, int in, int out)
{
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != bd_self)
			_exit(127);
		setpgid(0, 0);
		dup2(in >= 0 ? in : nul, 0);
		dup2(out >= 0 ? out : nul, 1);
		dup2(nul, 2);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execlp(D.state_cmd, D.state_cmd, verb, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	return pid;
}

void bd_child_kill(struct child *c)
{
	if (c->pgid > 0)
		kill(-c->pgid, SIGKILL);
}

/* The record lines: "sensor ADDR KINDS DATE|- FLAG NAME" */
static size_t records_text(char *out, size_t n)
{
	size_t o = 0;

	out[0] = 0;
	for (int i = 0; i < MAX_SENSORS; i++) {
		const struct sensor *s = &D.sensors[i];
		char kinds[32];
		int w;

		if (!s->used || s->removal_confirmed)
			continue;
		bd_kinds_str(s->kinds, kinds, sizeof(kinds));
		w = snprintf(out + o, n - o, "sensor %s %s %s %s %s\n", s->addr, kinds,
			     s->date[0] ? s->date : "-", s->forgetting ? "forget" : "-",
			     s->name[0] ? s->name : s->addr);
		if (w < 0 || (size_t)w >= n - o)
			break;
		o += (size_t)w;
	}
	return o;
}

void bd_save_records(void)
{
	D.record_revision++;
	D.save_pending = true;
	bd_save_start();
}

void bd_save_start(void)
{
	char text[2048];
	int p[2];
	size_t len;
	pid_t pid;

	if (!D.save_pending || D.save.pgid > 0 || bd_now_ms() < D.save_retry_ms)
		return;
	D.save_pending = false;
	len = records_text(text, sizeof(text));
	if (pipe2(p, O_CLOEXEC) < 0) {
		chef_log("pipe: %s; sensors not saved", strerror(errno));
		D.save_pending = true;
		D.save_retry_ms = bd_now_ms() + RETRY_MS;
		return;
	}
	pid = spawn_state("blesens-save", p[0], -1);
	close(p[0]);
	if (pid < 0) {
		close(p[1]);
		chef_log("cannot run %s blesens-save: %s; sensors not saved", D.state_cmd, strerror(-pid));
		D.save_pending = true;
		D.save_retry_ms = bd_now_ms() + RETRY_MS;
		return;
	}
	/* at most 2 KiB into an empty pipe: never blocks */
	D.save_write_ok = write(p[1], text, len) == (ssize_t)len;
	if (!D.save_write_ok)
		chef_log("blesens-save: short write");
	close(p[1]);
	D.save_revision = D.record_revision;
	D.save = (struct child){ .pid = pid, .pgid = pid, .until = bd_now_ms() + CHILD_SAVE_MS };
}

static void save_finished(bool success)
{
	if (success && D.save_write_ok) {
		D.saved_revision = D.save_revision;
		D.save_retry_ms = 0;
	} else {
		D.save_pending = true;
		D.save_retry_ms = bd_now_ms() + RETRY_MS;
	}
	bd_child_kill(&D.save);	/* nothing of it outlives the leader */
	D.save = (struct child){ 0 };
	if (D.save_pending)
		bd_save_start();
}

static void load_parse(void)
{
	char *save = NULL, *line;
	int n = 0;

	D.load_buf[D.load_len] = 0;
	for (line = strtok_r(D.load_buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char addr[18], kinds[32], date[16], flag[8], name[64];
		int off = 0;
		struct sensor *s;

		if (sscanf(line, "sensor %17s %31s %15s %7s %n", addr, kinds, date, flag, &off) != 4 || !off ||
		    !bd_addr_ok(addr) || !bd_kinds_parse(kinds) || (strcmp(flag, "-") && strcmp(flag, "forget")) ||
		    n >= MAX_SENSORS || bd_sensor_by_addr(addr))
			continue;
		bd_name_clean(name, sizeof(name), line + off);
		s = &D.sensors[n++];
		bd_sensor_init(s, addr, bd_kinds_parse(kinds), strcmp(date, "-") ? date : "", name[0] ? name : addr);
		s->forgetting = !strcmp(flag, "forget");
	}
	chef_log("%d sensor%s", n, n == 1 ? "" : "s");
}

static void load_finish(bool timed_out)
{
	if (D.load_fd >= 0) {
		close(D.load_fd);
		D.load_fd = -1;
	}
	bd_child_kill(&D.load);
	D.load = (struct child){ 0 };
	if (timed_out)
		chef_log("%s blesens-load gave no answer: no sensors this time", D.state_cmd);
	else
		load_parse();
	D.loaded = true;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			bd_sensor_event(&D.sensors[i], true);
}

void bd_load_start(void)
{
	int p[2];
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0) {
		load_finish(true);
		return;
	}
	pid = spawn_state("blesens-load", -1, p[1]);
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		chef_log("cannot run %s blesens-load: %s", D.state_cmd, strerror(-pid));
		load_finish(true);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	D.load = (struct child){ .pid = pid, .pgid = pid, .until = bd_now_ms() + CHILD_LOAD_MS };
	D.load_fd = p[0];
}

/* Read what is in the pipe now; EOF closes it. */
void bd_load_read(void)
{
	size_t room = D.load_len < sizeof(D.load_buf) - 1 ? sizeof(D.load_buf) - 1 - D.load_len : 0;
	ssize_t n = room ? read(D.load_fd, D.load_buf + D.load_len, room) : 0;

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0)
		D.load_len += (size_t)n;
	if (n <= 0 || D.load_len >= sizeof(D.load_buf) - 1) {
		close(D.load_fd);
		D.load_fd = -1;
	}
}

void bd_reap(void)
{
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

		if (pid == D.save.pid) {
			if (rc)
				chef_log("%s blesens-save exited %d: sensors may not be saved", D.state_cmd, rc);
			save_finished(rc == 0);
		} else if (pid == D.load.pid) {
			D.load.pid = 0;
			/* its output is complete: what a descendant still holds
			 * open is not waited for */
			bd_child_kill(&D.load);
		}
	}
	if (!D.loaded && D.load.pgid > 0 && !D.load.pid) {
		/* the leader is done and its writes are in the pipe: take
		 * them, never wait for a killed descendant's end */
		size_t before;

		do {
			before = D.load_len;
			if (D.load_fd >= 0)
				bd_load_read();
		} while (D.load_fd >= 0 && D.load_len > before);
		load_finish(false);
	}
}

void bd_children_tick(int64_t now)
{
	if (D.load.pgid > 0 && now >= D.load.until)
		load_finish(true);	/* kills the group */
	if (D.save.pgid > 0 && now >= D.save.until) {
		chef_log("%s blesens-save still running after %d s: killed", D.state_cmd, CHILD_SAVE_MS / 1000);
		bd_child_kill(&D.save);	/* the leader's exit (reaped next) finishes it */
		D.save.until = INT64_MAX;
	}
}
