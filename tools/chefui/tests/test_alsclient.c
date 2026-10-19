/*
 * test_alsclient.c - the sensord illuminance client (sys/alsclient.c) against a
 * fake sensord on a short unix socket, on fakeloop with a test clock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "common/json.h"
#include "fakeloop.h"
#include "settings.h"
#include "sys.h"
#include "sysstate.h"

static char dir[64], sock[96];
static unsigned changes;
static int bright_cb, theme_cb, last_bright;
static bool last_light;
static int64_t clock_ms;

static struct {
	int lfd, cfd, accepts, status_reqs, claims;
	bool drop_on_accept, ack, smgr, status_silent, eof, mute;
	const char *status_line;	/* the status reply, NULL: the short one */
	char in[1024];
	size_t n;
	int64_t accept_at[16];
} S = { .lfd = -1, .cfd = -1, .ack = true, .smgr = true };

static void on_state(unsigned what, void *user)
{
	(void)user;
	changes |= what;
}

static void on_als(const struct als_event *ev, void *user)
{
	(void)user;
	if (ev->bright_changed) {
		bright_cb++;
		last_bright = ev->bright;
	}
	if (ev->theme_changed) {
		theme_cb++;
		last_light = ev->light;
	}
}

static void say(const char *t)
{
	if (S.cfd >= 0)
		(void)!write(S.cfd, t, strlen(t));	/* the client may be gone */
}

/* Answer the complete request lines in S.in. */
static void serve_requests(void)
{
	char *nl;

	while ((nl = memchr(S.in, '\n', S.n))) {
		size_t len = (size_t)(nl - S.in) + 1;

		if (!strncmp(S.in, "claim illuminance 5\n", len)) {
			S.claims++;
			if (S.ack)
				say("{\"ok\":\"claim\",\"sensor\":\"illuminance\",\"rate\":5}\n");
		} else if (!strncmp(S.in, "status\n", len)) {
			S.status_reqs++;
			if (!S.status_silent && S.status_line)
				say(S.status_line);
			else if (!S.status_silent)
				say(S.smgr ? "{\"ok\":\"status\",\"proto\":1,\"smgr\":\"ready\",\"clients\":2}\n"
					   : "{\"ok\":\"status\",\"proto\":1,\"smgr\":\"down\",\"clients\":2}\n");
		}
		memmove(S.in, S.in + len, S.n - len);
		S.n -= len;
		S.in[S.n] = 0;
	}
}

static void server_io(void)
{
	struct pollfd p;

	if (S.lfd >= 0 && S.cfd < 0) {
		int fd = accept4(S.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

		if (fd >= 0) {
			S.cfd = fd;
			S.n = 0;
			S.in[0] = 0;
			S.eof = false;
			if (S.accepts < 16)
				S.accept_at[S.accepts] = clock_ms;
			S.accepts++;
			if (S.drop_on_accept) {
				close(S.cfd);
				S.cfd = -1;
				return;
			}
			if (!S.mute)
				say("{\"hello\":\"sensord\",\"proto\":1}\n");
		}
	}
	if (S.cfd >= 0) {
		p = (struct pollfd){ S.cfd, POLLIN, 0 };
		if (poll(&p, 1, 0) > 0) {
			char buf[128];
			ssize_t r = read(S.cfd, buf, sizeof(buf));

			if (r > 0 && S.n + (size_t)r < sizeof(S.in)) {
				memcpy(S.in + S.n, buf, (size_t)r);
				S.n += (size_t)r;
				S.in[S.n] = 0;
				serve_requests();
			} else if (r == 0) {
				S.eof = true;
				close(S.cfd);
				S.cfd = -1;
			}
		}
	}
}

/* Advance the test clock in 10 ms steps, serving the fake sensord. */
static void run(int ms)
{
	for (int i = 0; i < ms; i += 10) {
		fakeloop_advance(10);
		clock_ms += 10;
		server_io();
	}
}

static void listen_on(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	unlink(sock);
	S.lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	CHECK(bind(S.lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(S.lfd, 4) == 0);
}

static void drop_conn(void)
{
	if (S.cfd >= 0)
		close(S.cfd);
	S.cfd = -1;
}

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* a sample stamped age_ms before now (CLOCK_MONOTONIC, as sensord does) */
static void sample_aged(double lux, int age_ms)
{
	char line[160];

	snprintf(line, sizeof(line), "{\"sensor\":\"illuminance\",\"t\":%lld,\"illuminance\":%g}\n",
		 (long long)(mono_ns() - (int64_t)age_ms * 1000000), lux);
	say(line);
}

static void sample(double lux)
{
	sample_aged(lux, 0);
}

/* A live sensord status line (2026-09-26 sensors evidence): 50 members. */
#define STATUS_50(smgr) "{\"ok\":\"status\",\"proto\":1,\"smgr\":\"" smgr "\",\"smgr_node\":5,\"smgr_port\":90," \
	"\"smgr_resets\":0,\"api\":\"buffering\",\"reg2\":true,\"reg2_reads\":75,\"reg2_writes\":4,\"reg2_misses\":0," \
	"\"reg2_saves\":3,\"reg2_save_errors\":0,\"time2\":false,\"time2_reqs\":0,\"unknown_reqs\":0,\"ts\":\"dsps\"," \
	"\"clients\":1,\"compass\":\"off\",\"compass_clients\":0,\"mount\":\"portrait\",\"calibrated\":true," \
	"\"cal_source\":\"live\",\"mag_bias\":[0.2120,0.3383,0.0427],\"mag_bias_pairs\":2700," \
	"\"mag_bias_reg\":[-0.339203,-0.213211,0.042511],\"disturbed\":false,\"accuracy\":null," \
	"\"rotvec_accuracy\":null,\"declination\":null,\"compass_lines\":57551,\"compass_dropped\":0," \
	"\"compass_gaps\":0,\"compass_resets\":1,\"compass_gyro_lost\":0,\"compass_disturbances\":14," \
	"\"compass_reacquired\":11,\"gyro_bias\":[-0.00005,0.00060,-0.00201],\"rotvec\":\"off\"," \
	"\"rotvec_instance\":-1,\"rotvec_enables\":0,\"rotvec_inds\":0,\"rotvec_errors\":0," \
	"\"rotvec_last_error\":-1,\"qmag\":\"off\",\"qmag_instance\":-1,\"qmag_enables\":0,\"qmag_inds\":0," \
	"\"qmag_errors\":0,\"qmag_last_error\":-1}\n"

static void test_parse(void)
{
	double lux = -1;

	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":312.5}", &lux) == 0 && lux == 312.5);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":0}", &lux) == 0 && lux == 0);
	CHECK(sys_als_parse_sample("{\"sensor\":\"proximity\",\"t\":5,\"illuminance\":3}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":-1}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":nan}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":inf}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"sensor\":\"illuminance\",\"t\":5}", &lux) == -1);
	{
		int64_t t = 0;

		CHECK(sys_als_parse_sample_t("{\"sensor\":\"illuminance\",\"t\":1500000000,\"illuminance\":7}", &lux, &t) == 0 &&
		      lux == 7 && t == 1500000000);
		CHECK(sys_als_parse_sample_t("{\"sensor\":\"illuminance\",\"illuminance\":7}", &lux, &t) == 0 && t == -1);
	}
	/* stale: stamped more than the slack before the claim; the boundary is fresh */
	CHECK(sys_als_sample_stale(1000000000 - 200000001, 1000000000));
	CHECK(!sys_als_sample_stale(1000000000 - 200000000, 1000000000));
	CHECK(!sys_als_sample_stale(1000000000, 1000000000) && !sys_als_sample_stale(2000000000, 1000000000));
	CHECK(!sys_als_sample_stale(-1, 1000000000) && !sys_als_sample_stale(5, 0));
	CHECK(sys_als_parse_sample("{\"dropped\":3}", &lux) == -1);
	CHECK(sys_als_parse_sample("{\"ok\":\"claim\",\"sensor\":\"illuminance\",\"rate\":5}", &lux) == -1);
	CHECK(sys_als_parse_sample("junk", &lux) == -1);
}

/* Run until the n-th accept (since S.accepts was zeroed) or max_ms. */
static bool until_accept(int n, int max_ms)
{
	for (int i = 0; i < max_ms && S.accepts < n; i += 10)
		run(10);
	return S.accepts >= n;
}

static void test_client(void)
{
	snprintf(sock, sizeof(sock), "%s/s.sock", dir);

	/* wanted with no sensord: retries on the backoff, no fds kept, not available */
	sys_als_start(sock, NULL);
	sys_als_on_change(on_als, NULL);
	sys_on_change(on_state, NULL);
	sys_als_want(true);
	run(20000);
	CHECK(!sys_state.als.available && fakeloop_watched() == 0);
	CHECK(!(changes & SYS_ALS));

	/* sensord appears: hello, one claim, then a status; available without any sample */
	listen_on();
	CHECK(until_accept(1, 35000));
	run(100);
	CHECK(S.accepts == 1 && S.claims == 1 && S.status_reqs == 1);
	CHECK(sys_state.als.available && (changes & SYS_ALS));
	CHECK(bright_cb == 0 && theme_cb == 0);

	/* dropped, malformed and foreign lines change nothing */
	changes = 0;
	say("{\"dropped\":4}\n{\"ok\":\"claim\",\"sensor\":\"illuminance\",\"rate\":5}\n");
	say("garbage\n{\"sensor\":\"illuminance\",\"t\":1,\"illuminance\":-3}\n");
	say("{\"sensor\":\"illuminance\",\"t\":1,\"illuminance\":nan}\n{\"sensor\":\"accel\",\"t\":1,\"x\":1}\n");
	run(100);
	CHECK(sys_state.als.available && bright_cb == 0 && theme_cb == 0 && !changes);
	CHECK(fakeloop_log_count("als:") == 0 && S.status_reqs == 1);

	/* the cached reading of a fresh claim (old stamp) is dropped with one log line per claim */
	sample_aged(5000, 59900);
	sample_aged(5000, 20700);
	run(100);
	CHECK(bright_cb == 0 && theme_cb == 0 && sys_state.als.lux != 5000);
	CHECK(fakeloop_log_count("als: dropped a cached sample") == 1);

	/* a first sample: brightness at once; long silence changes nothing and arms no work */
	sample(300);
	run(200);
	CHECK(sys_state.als.lux == 300 && bright_cb == 1 && last_bright == 30 && theme_cb == 0);
	run(60000);
	CHECK(bright_cb == 1 && theme_cb == 0 && sys_state.als.available);
	CHECK(S.status_reqs >= 2 && S.status_reqs <= 3);	/* the 30 s probe */

	/* one 3000 lux sample, then silence: Light at sample time + 5 s through the tick */
	sample(3000);
	run(4800);
	CHECK(theme_cb == 0);
	run(400);
	CHECK(theme_cb == 1 && last_light && sys_als_theme_light());
	CHECK(bright_cb > 1);	/* the level followed through ticks, with no further sample */

	/* smgr down on a probe: unavailable, connection and claim kept; ready again: available */
	{
		int acc = S.accepts, claims = S.claims;

		S.smgr = false;
		run(31000);
		CHECK(!sys_state.als.available && S.accepts == acc && S.claims == claims && S.cfd >= 0);
		S.smgr = true;
		run(31000);
		CHECK(sys_state.als.available && S.accepts == acc && S.claims == claims);

		/* the real 50-member status line, read with the JSON reader: smgr is its
		 * top-level member, wherever it stands; one nested in another member is
		 * not it */
		{
			struct json_obj o;

			CHECK(json_parse(&o, STATUS_50("ready"), strlen(STATUS_50("ready")) - 1) == JSON_OK && o.n == 50);
		}
		S.status_line = STATUS_50("down");
		run(31000);
		CHECK(!sys_state.als.available && S.cfd >= 0);
		S.status_line = STATUS_50("ready");
		run(31000);
		CHECK(sys_state.als.available && S.accepts == acc);
		S.status_line = "{\"x\":{\"smgr\":\"ready\"},\"ok\":\"status\",\"proto\":1,\"smgr\":\"down\"}\n";
		run(31000);
		CHECK(!sys_state.als.available && S.cfd >= 0);
		S.status_line = NULL;
		run(31000);
		CHECK(sys_state.als.available && S.accepts == acc && S.claims == claims);
	}

	/* a status that never answers: reconnect after the reply timeout, logged */
	S.accepts = 0;
	S.status_silent = true;
	for (int i = 0; i < 40000 && fakeloop_log_count("no answer from sensord") == 0; i += 10)
		run(10);
	CHECK(fakeloop_log_count("no answer from sensord") == 1 && !sys_state.als.available);
	S.status_silent = false;
	CHECK(until_accept(1, 3000));
	run(500);
	CHECK(S.accepts == 1 && sys_state.als.available);	/* new connection: claim, status ready */
	sample_aged(5000, 30000);	/* the second claim's cached line: dropped, logged again */
	run(100);
	CHECK(fakeloop_log_count("als: dropped a cached sample") == 2);

	/* an err line: logged once per distinct text, connection closed, retried */
	S.accepts = 0;
	say("{\"err\":\"unknown sensor\",\"sensor\":\"illuminance\"}\n");
	run(100);
	CHECK(fakeloop_log_count("als: sensord error: unknown sensor") == 1 && !sys_state.als.available);
	CHECK(until_accept(1, 3000));
	run(100);
	say("{\"err\":\"unknown sensor\",\"sensor\":\"illuminance\"}\n");
	run(100);
	CHECK(fakeloop_log_count("als: sensord error: unknown sensor") == 1);
	CHECK(until_accept(2, 5000));
	run(100);
	say("{\"err\":\"other\"}\n");
	run(100);
	CHECK(fakeloop_log_count("als: sensord error: other") == 1);
	/* the err text is a JSON string: escapes decoded */
	CHECK(until_accept(3, 9000));
	run(100);
	say("{\"err\":\"bad \\\"rate\\\" \\u0041\",\"sensor\":\"illuminance\"}\n");
	run(100);
	CHECK(fakeloop_log_count("als: sensord error: bad \"rate\" A") == 1 && !sys_state.als.available);

	/* a server that drops every connection at once: the delays double to the 30 s cap */
	S.accepts = 0;
	S.drop_on_accept = true;
	if (S.cfd >= 0) {
		close(S.cfd);
		S.cfd = -1;
	}
	CHECK(until_accept(6, 200000));
	{
		int gaps[5];

		for (int i = 1; i < 6; i++)
			gaps[i - 1] = (int)(S.accept_at[i] - S.accept_at[i - 1]);
		for (int i = 1; i < 5; i++)
			CHECK(gaps[i] >= gaps[i - 1] - 100 && gaps[i] <= 30100);
		CHECK(gaps[4] >= 29900 && gaps[4] <= 30100);	/* the cap */
	}
	S.drop_on_accept = false;

	/* a ready status resets the backoff to 1 s */
	S.accepts = 0;
	CHECK(until_accept(1, 35000));
	run(200);
	CHECK(sys_state.als.available);
	drop_conn();
	CHECK(until_accept(2, 3000));
	CHECK(S.accept_at[1] - S.accept_at[0] < 1500);

	/* a claim that is never acknowledged: reconnect 5 s after each connect (L4), no
	 * status probes, never available */
	sys_als_want(false);
	run(200);
	S.ack = false;
	S.status_reqs = 0;
	S.accepts = 0;
	fakeloop_log_clear();
	sys_als_want(true);
	CHECK(until_accept(1, 3000));
	run(4500);
	CHECK(S.accepts == 1 && fakeloop_log_count("no answer from sensord") == 0);
	run(1000);
	CHECK(fakeloop_log_count("no answer from sensord") == 1);
	run(100000);
	CHECK(S.status_reqs == 0 && !sys_state.als.available && S.accepts >= 4 && S.accepts <= 9);
	S.ack = true;

	/* a sensord that accepts and says nothing at all (no hello): the same deadline */
	sys_als_want(false);
	run(200);
	S.mute = true;
	S.claims = 0;
	S.accepts = 0;
	fakeloop_log_clear();
	sys_als_want(true);
	CHECK(until_accept(1, 3000));
	run(6000);
	CHECK(S.claims == 0 && fakeloop_log_count("no answer from sensord") == 1 && !sys_state.als.available);
	S.mute = false;

	/* not wanted: the connection closes and nothing reconnects */
	sys_als_want(false);
	run(100);
	CHECK(S.eof && !sys_state.als.available && fakeloop_watched() == 0);
	S.accepts = 0;
	run(120000);
	CHECK(S.accepts == 0 && !sys_als_wanted());

	/* off again during a connect in progress: cancelled cleanly, no claim sent */
	S.claims = 0;
	sys_als_want(true);
	sys_als_want(false);
	run(60000);
	CHECK(S.claims == 0 && S.accepts <= 1 && fakeloop_watched() == 0);

	/* wanted again: claims again, with a fresh stream, wake grace rules */
	S.accepts = 0;
	sys_als_want(true);
	CHECK(until_accept(1, 3000));
	run(200);
	CHECK(S.claims == 1 && sys_state.als.available);
	theme_cb = 0;
	sample(700);
	run(200);
	CHECK(theme_cb == 0 && sys_als_theme_light());	/* the wake grace: a low reading decides nothing */
	sample(3000);
	run(200);
	CHECK(theme_cb == 0 && sys_als_theme_light());	/* Light stays Light */
	sys_als_stop();
	run(100);
	CHECK(fakeloop_watched() == 0);
	drop_conn();
	close(S.lfd);
	unlink(sock);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	fakeloop_init(true);
	snprintf(dir, sizeof(dir), "/tmp/als.XXXXXX");
	if (!mkdtemp(dir))
		return 1;
	test_parse();
	test_client();
	rmdir(dir);
	return check_report("test-alsclient");
}
