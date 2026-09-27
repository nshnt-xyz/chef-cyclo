/* Unit tests for sensord.c's pure parts: sample conversion and axis
 * mapping, DSP tick -> CLOCK_MONOTONIC conversion (with wrap), the
 * JSON lines, claim/release/get bookkeeping and rate aggregation, the
 * bounded per-client queue and its drop accounting, and the request
 * parser. sensord.c is included with SENSORD_NO_MAIN; the SMGR side is
 * held "down" here (so nothing is sent) and exercised end to end by
 * tests/test_e2e.sh instead. */
#define SENSORD_NO_MAIN
#include "../sensord.c"

#include <math.h>

static int failures;
static int64_t fake_now;
static char last_log[512];

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int64_t test_now(void)
{
	return fake_now;
}

static char all_logs[8192];

static void test_log(const char *fmt, ...)
{
	va_list ap;
	size_t n = strlen(all_logs);

	va_start(ap, fmt);
	vsnprintf(last_log, sizeof(last_log), fmt, ap);
	va_end(ap);
	if (n + strlen(last_log) + 2 < sizeof(all_logs))
		snprintf(all_logs + n, sizeof(all_logs) - n, "%s\n", last_log);
}

static bool near(double a, double b)
{
	return fabs(a - b) < 1e-6;
}

static void test_convert(void)
{
	int32_t d[3] = { 0x8000, 0x10000, -0x9ce80 };	/* 0.5, 1.0, -9.80664 */
	double v[3];

	/* SMGR -> Android frame, as sensors.ssc.so: x=d1, y=d0, z=-d2 */
	convert_sample(CH_VEC3, d, v);
	CHECK(near(v[0], 1.0) && near(v[1], 0.5) && near(v[2], 0x9ce80 / 65536.0));
	convert_sample(CH_SCALAR, d, v);
	CHECK(near(v[0], 0.5));
	convert_sample(CH_PROX, (int32_t[3]){ 0, 42, 0 }, v);
	CHECK(v[0] == 0 && near(v[1], 42));
	convert_sample(CH_PROX, (int32_t[3]){ 0x10000, 0, 0 }, v);
	CHECK(v[0] == 1);
	/* near = trunc(d0 / 2^16) != 0, like the HAL's fcvtzs */
	convert_sample(CH_PROX, (int32_t[3]){ 0xffff, 0, 0 }, v);
	CHECK(v[0] == 0);
	convert_sample(CH_PROX, (int32_t[3]){ -0x10000, 0, 0 }, v);
	CHECK(v[0] == 1);
}

static void test_ticks(void)
{
	/* one second before the anchor */
	CHECK(ticks_to_ns(1000, 1000 + 32768, 5000000000LL) == 4000000000LL);
	/* stamp just before the u32 counter wrapped, anchor just after */
	CHECK(ticks_to_ns(0xffffffffu - 32767, 1, 10000000000LL) ==
	      10000000000LL - 1000000000LL - 1000000000LL / 32768);
	/* a stamp slightly in the future stays in the future */
	CHECK(ticks_to_ns(32768 + 16384, 32768, 0) == 500000000LL);
}

static void test_format(void)
{
	struct channel ch[CH_COUNT];
	char buf[256];

	memcpy(ch, channel_defaults, sizeof(ch));
	format_sample(buf, sizeof(buf), &ch[CH_ACCEL], 123, (int32_t[3]){ 0, 0x10000, 0x20000 });
	CHECK(strcmp(buf, "{\"sensor\":\"accel\",\"t\":123,\"x\":1,\"y\":0,\"z\":-2}\n") == 0);
	format_sample(buf, sizeof(buf), &ch[CH_ILLUM], 5, (int32_t[3]){ 0x7b8000, 0, 0 });
	CHECK(strcmp(buf, "{\"sensor\":\"illuminance\",\"t\":5,\"illuminance\":123.5}\n") == 0);
	format_sample(buf, sizeof(buf), &ch[CH_PROX_], 5, (int32_t[3]){ 0x10000, 42, 0 });
	CHECK(strcmp(buf, "{\"sensor\":\"proximity\",\"t\":5,\"near\":1,\"proximity\":42}\n") == 0);

	json_escape(buf, sizeof(buf), "a\"b\\c\x01\xffz");
	CHECK(strcmp(buf, "a\\\"b\\\\cz") == 0);
	CHECK(channel_from_name("magn") == CH_MAGN && channel_from_name("gyro") == -1);
}

/* A server with one connected client over a socketpair; returns the
 * client's own end in *peer. SMGR stays down, so no request is sent. */
static struct client *setup(struct server *s, int *peer)
{
	int sv[2];
	struct client *c;
	int i;

	server_init(s);
	s->now_ms = test_now;
	s->log = test_log;
	for (i = 0; i < CH_COUNT; i++) {
		s->ch[i].present = true;
		s->ch[i].max_hz = i == CH_MAGN ? 50 : 200;
	}
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	c = server_add_client(s, sv[0]);
	*peer = sv[1];
	return c;
}

static void send_line(struct server *s, struct client *c, const char *line)
{
	char buf[SENSORD_LINE_MAX + 64];

	snprintf(buf, sizeof(buf), "%s", line);
	server_handle_line(s, c, buf);
}

static size_t drain(int fd, char *buf, size_t len)
{
	size_t got = 0;
	ssize_t n;

	while (got < len - 1 && (n = read(fd, buf + got, len - 1 - got)) > 0)
		got += (size_t)n;
	buf[got] = '\0';
	return got;
}

static void test_claims(void)
{
	static struct server s;
	struct client *a, *b;
	char buf[4096];
	int pa, pb, sv[2];

	a = setup(&s, &pa);
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	b = server_add_client(&s, sv[0]);
	pb = sv[1];

	send_line(&s, a, "claim accel 50");
	drain(pa, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"ok\":\"claim\",\"sensor\":\"accel\",\"rate\":50}\n") == 0);
	send_line(&s, b, "claim accel");			/* default 10 Hz */
	send_line(&s, b, "claim magn 500");			/* clamped to 50 */
	drain(pb, buf, sizeof(buf));
	CHECK(strstr(buf, "\"rate\":10") && strstr(buf, "\"sensor\":\"magn\",\"rate\":50"));
	CHECK(channel_want(&s, CH_ACCEL) == 50 && channel_claims(&s, CH_ACCEL) == 2);
	CHECK(channel_want(&s, CH_MAGN) == 50);
	send_line(&s, a, "release accel");
	CHECK(channel_want(&s, CH_ACCEL) == 10);

	/* errors */
	send_line(&s, a, "claim gyro");
	send_line(&s, a, "claim accel 0");
	send_line(&s, a, "claim accel fast");
	send_line(&s, a, "frobnicate");
	drain(pa, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"err\":\"unknown sensor\",\"sensor\":\"gyro\"}\n"));
	CHECK(strstr(buf, "{\"err\":\"bad rate\",\"sensor\":\"accel\"}\n"));
	CHECK(strstr(buf, "{\"err\":\"unknown command\",\"command\":\"frobnicate\"}\n"));

	/* a sensor the DSP did not list is refused once SMGR is ready */
	s.st = SMGR_READY;
	s.ch[CH_ANGLVEL].present = false;
	s.smgr_sock = -1;
	/* nothing to sync: every channel already at its wanted rate */
	s.ch[CH_ACCEL].rate = 10;
	s.ch[CH_MAGN].rate = 50;
	send_line(&s, a, "claim anglvel");
	drain(pa, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"err\":\"sensor not present\",\"sensor\":\"anglvel\"}\n") == 0);
	s.st = SMGR_DOWN;

	/* disconnect drops that client's claims */
	close(pb);
	server_client_input(&s, b);
	CHECK(b->fd < 0 && channel_want(&s, CH_ACCEL) == 0 && channel_want(&s, CH_MAGN) == 0);

	/* list and status are single JSON lines */
	send_line(&s, a, "list");
	drain(pa, buf, sizeof(buf));
	CHECK(strncmp(buf, "{\"ok\":\"list\",\"smgr\":\"down\",\"sensors\":[{\"sensor\":\"accel\"", 50) == 0);
	CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1);
	send_line(&s, a, "status");
	drain(pa, buf, sizeof(buf));
	CHECK(strstr(buf, "\"api\":\"buffering\"") && strstr(buf, "\"clients\":1"));
	close(pa);
	server_drop_client(&s, a);
}

static void test_publish_and_get(void)
{
	static struct server s;
	struct client *fast, *slow_, *g;
	char buf[8192];
	int pf, ps, pg, sv[2], i;
	int64_t t;

	fast = setup(&s, &pf);
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	slow_ = server_add_client(&s, sv[0]);
	ps = sv[1];
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	g = server_add_client(&s, sv[0]);
	pg = sv[1];

	send_line(&s, fast, "claim accel 100");
	send_line(&s, slow_, "claim accel 10");
	send_line(&s, g, "get accel");
	CHECK(g->rate[CH_ACCEL] == 10 && g->get_only[CH_ACCEL]);
	CHECK(channel_want(&s, CH_ACCEL) == 100);
	drain(pf, buf, sizeof(buf));
	drain(ps, buf, sizeof(buf));

	/* one second of 100 Hz samples */
	for (i = 0, t = 1000000000LL; i < 100; i++, t += 10000000LL)
		server_publish(&s, CH_ACCEL, t, (int32_t[3]){ 0, 0x10000, 0 });
	server_flush_all(&s);
	{
		size_t n = drain(pf, buf, sizeof(buf));
		int lines = 0;

		for (i = 0; i < (int)n; i++)
			lines += buf[i] == '\n';
		CHECK(lines == 100);
		n = drain(ps, buf, sizeof(buf));
		for (i = 0, lines = 0; i < (int)n; i++)
			lines += buf[i] == '\n';
		CHECK(lines == 10);	/* decimated to its own 10 Hz */
		n = drain(pg, buf, sizeof(buf));
		CHECK(strcmp(buf, "{\"sensor\":\"accel\",\"t\":1000000000,\"x\":1,\"y\":0,\"z\":-0}\n") == 0 ||
		      strcmp(buf, "{\"sensor\":\"accel\",\"t\":1000000000,\"x\":1,\"y\":0,\"z\":0}\n") == 0);
	}
	/* the get-only claim is gone after its sample */
	CHECK(g->rate[CH_ACCEL] == 0 && !g->get[CH_ACCEL]);

	/* a get nobody answers times out */
	fake_now = 1000;
	send_line(&s, g, "get illuminance");
	server_tick_clients(&s);
	CHECK(g->get[CH_ILLUM]);
	fake_now += SENSORD_GET_TIMEOUT_MS;
	server_tick_clients(&s);
	drain(pg, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"err\":\"timeout\",\"sensor\":\"illuminance\"}\n") == 0);
	CHECK(!g->get[CH_ILLUM] && g->rate[CH_ILLUM] == 0);
	close(pf);
	close(ps);
	close(pg);
}

/* A client that never reads: samples are dropped and counted, the
 * daemon never blocks, and the count is reported once there is room. */
static void test_slow_client(void)
{
	static struct server s;
	struct client *c;
	char buf[65536];
	int p, i, sz = 4096;
	unsigned long dropped;
	char *d;

	c = setup(&s, &p);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	send_line(&s, c, "claim accel 200");
	for (i = 0; i < 5000; i++) {
		server_publish(&s, CH_ACCEL, 1000000000LL + i * 5000000LL, (int32_t[3]){ 1, 2, 3 });
		server_flush_all(&s);
	}
	CHECK(c->fd >= 0 && c->outlen > sizeof(c->out) - 200);
	dropped = c->dropped;
	CHECK(dropped > 0 && c->dropped_total == dropped);
	drain(p, buf, sizeof(buf));
	server_flush_all(&s);
	drain(p, buf, sizeof(buf));
	server_flush_all(&s);
	drain(p, buf, sizeof(buf));
	server_publish(&s, CH_ACCEL, 99000000000LL, (int32_t[3]){ 1, 2, 3 });
	server_flush_all(&s);
	drain(p, buf, sizeof(buf));
	d = strstr(buf, "{\"dropped\":");
	CHECK(d && strtoul(d + 11, NULL, 10) == dropped);
	CHECK(d && strstr(d, "\n{\"sensor\":\"accel\",\"t\":99000000000,"));
	CHECK(c->dropped == 0);

	/* a reply that cannot be queued disconnects the client */
	c->outlen = sizeof(c->out) - 10;
	send_line(&s, c, "status");
	CHECK(c->fd < 0);
	CHECK(strstr(all_logs, "client output full, disconnecting it"));
	close(p);
}

static void test_long_line(void)
{
	static struct server s;
	struct client *c;
	char buf[1024], junk[300];
	int p;

	c = setup(&s, &p);
	memset(junk, 'x', sizeof(junk));
	CHECK(write(p, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
	CHECK(write(p, "\nclaim accel 20\n", 16) == 16);
	server_client_input(&s, c);
	server_client_input(&s, c);
	server_client_input(&s, c);
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"err\":\"line too long\"}\n"));
	CHECK(strstr(buf, "{\"ok\":\"claim\",\"sensor\":\"accel\",\"rate\":20}\n"));
	close(p);
}

static void test_deadline(void)
{
	static struct server s;
	struct client *c;
	int p;

	c = setup(&s, &p);
	fake_now = 10000;
	s.st = SMGR_READY;
	s.next_check = 15000;
	CHECK(server_deadline_ms(&s) == 5000);
	s.pend[0].used = true;
	s.pend[0].sent = 9000;
	CHECK(server_deadline_ms(&s) == 2000);	/* request timeout at 12000 */
	c->get[CH_ACCEL] = true;
	c->get_deadline[CH_ACCEL] = 10500;
	CHECK(server_deadline_ms(&s) == 500);
	c->get_deadline[CH_ACCEL] = 9000;
	CHECK(server_deadline_ms(&s) == 0);
	s.pend[0].used = false;
	c->get[CH_ACCEL] = false;
	s.next_check = 10000000;
	CHECK(server_deadline_ms(&s) == 60000);
	close(p);
}

static void test_reset(void)
{
	static struct server s;
	int p;

	setup(&s, &p);
	s.st = SMGR_READY;
	s.ch[CH_ACCEL].rate = 50;
	s.ch[CH_ACCEL].busy = true;
	s.pend[0].used = true;
	smgr_reset(&s, "test");
	CHECK(s.st == SMGR_DOWN && s.ch[CH_ACCEL].rate == 0 && !s.ch[CH_ACCEL].busy &&
	      !s.pend[0].used && s.smgr_resets == 1);
	CHECK(strstr(last_log, "lost (test)"));
	close(p);
}

/* REG2 write whose count the decoder rejects: the log must not read the
 * rejected count's worth of bytes (ASan build), nothing is written. */
static void test_reg2_bad_write(void)
{
	static struct server s;
	static const uint8_t msg[] = { 0x00, 0x01, 0x00, 0x03, 0x00, 0x11, 0x00,
				       0x01, 0x02, 0x00, 0xbc, 0x02,
				       0x02, 0x09, 0x00, 0xc8, 1, 2, 3, 4, 5, 6, 7, 8 };
	struct qrtr_packet pkt;
	uint8_t *buf = malloc(sizeof(msg));
	int p;

	setup(&s, &p);
	memcpy(buf, msg, sizeof(msg));
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = buf;
	pkt.data_len = sizeof(msg);
	all_logs[0] = '\0';
	reg2_rx(&s, &pkt, 5, 77);
	if (!strstr(all_logs, "item write 700 from 5:77 (RAM, /run copy) rejected (0 bytes)"))
		fprintf(stderr, "logs: %s\n", all_logs);
	CHECK(strstr(all_logs, "item write 700 from 5:77 (RAM, /run copy) rejected (0 bytes)"));
	CHECK(s.reg.writes == 0);
	free(buf);
	close(p);
}

/* Build an exact-size heap packet (ASan catches over-reads). */
static uint8_t *mkpkt(struct qrtr_packet *pkt, uint8_t type, uint16_t msg, const uint8_t *tlv,
		      size_t n)
{
	uint8_t *b = malloc(7 + n);

	b[0] = type;
	b[1] = 1;
	b[2] = 0;
	b[3] = (uint8_t)msg;
	b[4] = (uint8_t)(msg >> 8);
	b[5] = (uint8_t)n;
	b[6] = (uint8_t)(n >> 8);
	memcpy(b + 7, tlv, n);
	memset(pkt, 0, sizeof(*pkt));
	pkt->data = b;
	pkt->data_len = 7 + n;
	return b;
}

/* QMAG_CAL indications: stored only for our instance and port, bounds
 * checked, and the latest bias rides along on magn lines and status. */
static void test_qmag(void)
{
	static struct server s;
	static const uint8_t ind[] = {
		0x01, 0x01, 0x00, 0x07,
		0x02, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00,
		0x03, 0x0c, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0x00, 0x60, 0x00, 0x00,
		0x04, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 };
	struct qrtr_packet pkt;
	struct client *c;
	char buf[4096];
	uint8_t *b;
	int p;

	c = setup(&s, &p);
	s.qmag.opt = true;
	s.qmag.on = true;
	s.qmag.instance = 7;
	s.qmag.node = 5;
	s.qmag.port = 9;
	c->rate[CH_MAGN] = 10;
	CHECK(strcmp(qmag_state_name(&s.qmag), "on") == 0);

	/* from another port: ignored */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, sizeof(ind));
	qmag_rx(&s, &pkt, 5, 10);
	CHECK(!s.qmag.have_bias && s.qmag.inds == 0);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.have_bias && s.qmag.inds == 1 && s.qmag.accuracy == 2);
	CHECK(s.qmag.bias[0] == 0x2000 && s.qmag.bias[1] == -0x4000 && s.qmag.bias[2] == 0x6000);
	CHECK(strstr(last_log, "qmag report 1: instance 7 ts 16 bias raw 8192 -16384 24576"));
	CHECK(strstr(last_log, "device x -0.25000 y 0.12500 z -0.37500 gauss, accuracy 2"));

	server_publish(&s, CH_MAGN, 5, (int32_t[3]){ 0x10000, 0x20000, 0x30000 });
	server_flush_all(&s);
	drain(p, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"sensor\":\"magn\",\"t\":5,\"x\":2,\"y\":1,\"z\":-3,\"bias\":[-0.25,0.125,-0.375],"
			  "\"bias_raw\":[8192,-16384,24576],\"accuracy\":2}\n") == 0);
	/* other channels are untouched */
	c->rate[CH_ACCEL] = 10;
	server_publish(&s, CH_ACCEL, 5, (int32_t[3]){ 0, 0, 0 });
	server_flush_all(&s);
	drain(p, buf, sizeof(buf));
	CHECK(!strstr(buf, "bias"));

	server_status_line(&s, buf, sizeof(buf));
	CHECK(strstr(buf, "\"qmag\":\"on\",\"qmag_instance\":7,\"qmag_enables\":0,\"qmag_inds\":1,"
			  "\"qmag_errors\":0,\"qmag_last_error\":-1,\"bias\":[-0.25,0.125,-0.375]"));

	/* another instance: logged, not stored */
	{
		uint8_t other[sizeof(ind)];

		memcpy(other, ind, sizeof(ind));
		other[3] = 8;
		other[29] = 3;
		b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, other, sizeof(other));
		qmag_rx(&s, &pkt, 5, 9);
		free(b);
		CHECK(s.qmag.inds == 1 && s.qmag.accuracy == 2);
		CHECK(strstr(last_log, "qmag report for instance 8 ignored"));
	}
	/* truncated: the bias TLV claims 12 bytes, the packet ends first */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, 20);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.inds == 1);
	CHECK(strstr(last_log, "qmag report indication did not decode"));
	/* error indication */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND,
		  (const uint8_t[]){ 0x01, 0x01, 0x00, 0x04, 0x02, 0x01, 0x00, 0x07 }, 8);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && s.qmag.last_error == 4 &&
	      strstr(last_log, "qmag error indication: error 4 for our instance 7"));
	/* another instance's error is not ours */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND,
		  (const uint8_t[]){ 0x01, 0x01, 0x00, 0x05, 0x02, 0x01, 0x00, 0x09 }, 8);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && strstr(last_log, "instance 9 (not ours"));
	/* mandatory TLVs missing (libqrtr would decode zeros): ignored */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, 4);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.inds == 1 && strstr(last_log, "without all of TLVs"));
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND, (const uint8_t[]){ 0x01, 0x01, 0x00, 0x04 }, 4);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && strstr(last_log, "malformed"));
	/* a late ENABLE response for another instance: disabled at once
	 * (the send fails here: no socket, which is logged) */
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_ENABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x0b }, 9);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(strstr(last_log, "qmag late enable response created instance 11: disabling it"));
	/* ... but a duplicate response for our own instance is left alone */
	last_log[0] = '\0';
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_ENABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x07 }, 9);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(!strstr(last_log, "late enable"));
	/* a response nobody asked for changes nothing */
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_DISABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00 }, 5);
	qmag_rx(&s, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.on);

	/* without -Q, or with SMGR down, nothing is wanted */
	CHECK(!qmag_wanted(&s));
	s.qmag.opt = false;
	CHECK(strcmp(qmag_state_name(&s.qmag), "off") == 0);
	close(p);
}

/* Registry write-back timing: 3 s after the last DSP write, but never
 * more than 30 s after the first unsaved one. The path does not exist,
 * so each attempt shows up as a save error (and a failure is not fatal). */
static void test_reg_debounce(void)
{
	static struct server s;
	int p, t;

	setup(&s, &p);
	s.reg_path = "/nonexistent/sns.reg";
	fake_now = 1000;
	s.reg_dirty = true;
	s.reg_dirty_since = s.reg_dirty_at = fake_now;
	fake_now += 2999;
	reg_tick(&s);
	CHECK(s.reg_dirty && s.reg_save_errors == 0);
	fake_now += 1;
	reg_tick(&s);
	CHECK(!s.reg_dirty && s.reg_save_errors == 1 && strstr(last_log, "write-back"));
	/* a write every 2 s: saved at 30 s, not postponed for ever */
	s.reg_dirty = true;
	s.reg_dirty_since = fake_now;
	for (t = 0; t < 40; t++) {
		fake_now += 1000;
		if (t % 2 == 0)
			s.reg_dirty_at = fake_now;
		reg_tick(&s);
		if (!s.reg_dirty)
			break;
	}
	CHECK(!s.reg_dirty && s.reg_save_errors == 2 && fake_now - s.reg_dirty_since == 30000);
	/* no path (-R): nothing happens */
	s.reg_path = NULL;
	s.reg_dirty = true;
	fake_now += 60000;
	reg_tick(&s);
	CHECK(s.reg_save_errors == 2);
	close(p);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	test_convert();
	test_ticks();
	test_format();
	test_claims();
	test_publish_and_get();
	test_slow_client();
	test_long_line();
	test_deadline();
	test_reset();
	test_reg2_bad_write();
	test_qmag();
	test_reg_debounce();
	if (failures) {
		fprintf(stderr, "test-sensord: %d failure(s)\n", failures);
		return 1;
	}
	printf("test-sensord: all passed\n");
	return 0;
}
