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
	all_logs[0] = '\0';
	smgr_reset(&s, "test");
	CHECK(s.st == SMGR_DOWN && s.ch[CH_ACCEL].rate == 0 && !s.ch[CH_ACCEL].busy &&
	      !s.pend[0].used && s.smgr_resets == 1);
	CHECK(strstr(all_logs, "lost (test)"));
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
	CHECK(strcmp(sam_state_name(&s.qmag), "on") == 0);

	/* from another port: ignored */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, sizeof(ind));
	sam_rx(&s, &s.qmag, &pkt, 5, 10);
	CHECK(!s.qmag.have && s.qmag.inds == 0);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.have && s.qmag.inds == 1 && s.qmag.accuracy == 2);
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
		sam_rx(&s, &s.qmag, &pkt, 5, 9);
		free(b);
		CHECK(s.qmag.inds == 1 && s.qmag.accuracy == 2);
		CHECK(strstr(last_log, "qmag report for instance 8 ignored"));
	}
	/* truncated: the bias TLV claims 12 bytes, the packet ends first */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, 20);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.inds == 1);
	CHECK(strstr(last_log, "qmag report indication did not decode"));
	/* error indication */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND,
		  (const uint8_t[]){ 0x01, 0x01, 0x00, 0x04, 0x02, 0x01, 0x00, 0x07 }, 8);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && s.qmag.last_error == 4 &&
	      strstr(last_log, "qmag error indication: error 4 for our instance 7"));
	/* another instance's error is not ours */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND,
		  (const uint8_t[]){ 0x01, 0x01, 0x00, 0x05, 0x02, 0x01, 0x00, 0x09 }, 8);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && strstr(last_log, "instance 9 (not ours"));
	/* mandatory TLVs missing (libqrtr would decode zeros): ignored */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, 4);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.inds == 1 && strstr(last_log, "without all of TLVs"));
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_ERROR_IND, (const uint8_t[]){ 0x01, 0x01, 0x00, 0x04 }, 4);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.errors == 1 && strstr(last_log, "malformed"));
	/* a late ENABLE response for another instance: disabled at once
	 * (the send fails here: no socket, which is logged) */
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_ENABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x0b }, 9);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(strstr(last_log, "qmag late enable response created instance 11: disabling it"));
	/* ... but a duplicate response for our own instance is left alone */
	last_log[0] = '\0';
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_ENABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x07 }, 9);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(!strstr(last_log, "late enable"));
	/* a response nobody asked for changes nothing */
	b = mkpkt(&pkt, QMI_RESPONSE, SNS_SAM_DISABLE,
		  (const uint8_t[]){ 0x02, 0x02, 0x00, 0x00, 0x00 }, 5);
	sam_rx(&s, &s.qmag, &pkt, 5, 9);
	free(b);
	CHECK(s.qmag.on);

	/* without -Q, or with SMGR down, nothing is wanted */
	CHECK(!sam_wanted(&s, &s.qmag));
	s.qmag.opt = false;
	CHECK(strcmp(sam_state_name(&s.qmag), "off") == 0);
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

/* The heading channel: internal claims (rate max with other clients),
 * the filter fed from published samples, decimated heading lines, a get,
 * declination, list/status fields. SMGR is down, so nothing is sent. */
static int32_t q16(double v)
{
	return (int32_t)lround(v * 65536.0);
}

/* One accel, gyro and magn sample each at t (ms), phone flat with the
 * top edge pointing east (heading 90): device magn (-0.3, 0, -0.2) G. */
static void feed_flat_east(struct server *s, int64_t ms)
{
	int64_t t = ms * 1000000LL;
	/* SMGR frame: device x = d[1], y = d[0], z = -d[2] */
	int32_t a[3] = { 0, 0, q16(-9.80665) }, g[3] = { 0, 0, 0 };
	int32_t m[3] = { 0, q16(-0.3), q16(0.2) };

	server_publish(s, CH_ANGLVEL, t, g);
	server_publish(s, CH_ACCEL, t + 1, a);
	if (ms % 50 == 0)
		server_publish(s, CH_MAGN, t + 2, m);
	server_flush_all(s);
}

static void test_heading(void)
{
	static struct server s;
	struct client *c, *o;
	char buf[8192], *l;
	int p, po, sv[2];
	int64_t ms;

	c = setup(&s, &p);
	s.ch[CH_HEADING].max_hz = SENSORD_HEADING_MAX_HZ;
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	o = server_add_client(&s, sv[0]);
	po = sv[1];
	CHECK(channel_want(&s, CH_ACCEL) == 0 && !s.heading.active);
	send_line(&s, c, "claim heading 5");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"ok\":\"claim\",\"sensor\":\"heading\",\"rate\":5}"));
	CHECK(s.heading.active && strstr(all_logs, "heading on: accel 50 Hz, anglvel 50 Hz, magn 20 Hz"));
	CHECK(channel_want(&s, CH_ACCEL) == 50 && channel_want(&s, CH_ANGLVEL) == 50 &&
	      channel_want(&s, CH_MAGN) == 20 && channel_want(&s, CH_ILLUM) == 0);
	CHECK(channel_used(&s, CH_MAGN) == 1 && channel_claims(&s, CH_MAGN) == 0);
	/* rate max with another client, both ways */
	send_line(&s, o, "claim accel 100");
	send_line(&s, o, "claim magn 10");
	CHECK(channel_want(&s, CH_ACCEL) == 100 && channel_want(&s, CH_MAGN) == 20);
	send_line(&s, o, "release accel");
	CHECK(channel_want(&s, CH_ACCEL) == 50 && channel_used(&s, CH_MAGN) == 2);
	/* heading over 50 Hz is clamped */
	send_line(&s, o, "claim heading 400");
	drain(po, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"ok\":\"claim\",\"sensor\":\"heading\",\"rate\":50}"));
	send_line(&s, o, "release heading");
	send_line(&s, o, "release magn");
	drain(po, buf, sizeof(buf));
	/* 2 s of samples: decimated to 5 Hz, flat, heading 90 */
	for (ms = 1000; ms < 3000; ms += 20)
		feed_flat_east(&s, ms);
	drain(p, buf, sizeof(buf));
	{
		int n = 0;

		for (l = buf; (l = strstr(l, "{\"sensor\":\"heading\"")); l++)
			n++;
		CHECK(n >= 9 && n <= 11);
	}
	l = strrchr(buf, '{');
	if (l && !strstr(l, "\"heading\":90.00,\"pitch\":0.00,"))
		fprintf(stderr, "last heading line: %s", l);
	CHECK(l && strstr(l, "\"heading\":90.00,\"pitch\":0.00,\"roll\":0.00,\"calibrated\":false,"
			     "\"disturbed\":false,\"accuracy\":180.0,\"cal_source\":\"none\",\"mag_bias\":0.000}\n"));
	/* status and list */
	{
		char st[4096];

		server_status_line(&s, st, sizeof(st));
		CHECK(strstr(st, "\"compass\":\"on\",\"compass_clients\":1,\"mount\":\"portrait\","
				 "\"calibrated\":false,\"cal_source\":\"none\",\"mag_bias\":null,"
				 "\"mag_bias_pairs\":0,\"mag_bias_reg\":null,\"disturbed\":false,"
				 "\"accuracy\":180.0,\"rotvec_accuracy\":null,\"declination\":null,"));
		CHECK(strstr(st, "\"compass_gyro_lost\":0,"));
		CHECK(strstr(st, "\"rotvec\":\"off\",\"rotvec_instance\":-1,"));
		CHECK(strstr(st, ",\"qmag\":\"off\","));
		server_list_line(&s, st, sizeof(st));
		CHECK(strstr(st, "{\"sensor\":\"heading\",\"unit\":\"deg\",\"virtual\":true,"
				 "\"inputs\":\"accel 50 Hz, anglvel 50 Hz, magn 20 Hz\",\"present\":true,"
				 "\"max_hz\":50,\"rate\":5,\"claims\":1,\"mount\":\"portrait\",\"calibrated\":false}"));
		CHECK(strstr(st, "{\"sensor\":\"rotvec\",\"unit\":\"quaternion\",\"virtual\":true,"
				 "\"present\":false,"));
		CHECK(st[strlen(st) - 1] == '\n' && strstr(st, "}]}\n"));
	}
	/* declination: true_heading, then off; bad values refused */
	send_line(&s, c, "declination 2.5");
	send_line(&s, c, "declination x");
	send_line(&s, c, "declination 181");
	send_line(&s, c, "declination");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"ok\":\"declination\",\"declination\":2.50}\n{\"err\":\"bad declination\"}\n"
			  "{\"err\":\"bad declination\"}\n{\"ok\":\"declination\",\"declination\":2.50}\n"));
	for (; ms < 3500; ms += 20)
		feed_flat_east(&s, ms);
	drain(p, buf, sizeof(buf));
	l = strrchr(buf, '{');
	CHECK(l && strstr(l, "\"accuracy\":180.0,\"cal_source\":\"none\",\"mag_bias\":0.000,"
			     "\"true_heading\":92.50,\"declination\":2.50}\n"));
	s.heading.decl = -95;
	for (; ms < 3700; ms += 20)
		feed_flat_east(&s, ms);
	drain(p, buf, sizeof(buf));
	l = strrchr(buf, '{');
	CHECK(l && strstr(l, "\"true_heading\":355.00,\"declination\":-95.00}\n"));
	send_line(&s, c, "declination off");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"ok\":\"declination\",\"declination\":null}"));
	/* get heading from the other client: one line, the claim ends */
	send_line(&s, o, "get heading");
	CHECK(o->rate[CH_HEADING] && o->get_only[CH_HEADING]);
	for (; ms < 3800; ms += 20)
		feed_flat_east(&s, ms);
	drain(po, buf, sizeof(buf));
	l = strstr(buf, "{\"sensor\":\"heading\"");
	CHECK(l && !o->rate[CH_HEADING]);
	CHECK(l && !strstr(l + 1, "{\"sensor\":\"heading\""));
	/* anglvel stops: not silent, logged, lines go on from accel + magn */
	{
		int32_t a[3] = { 0, 0, q16(-9.80665) }, m[3] = { 0, q16(-0.3), q16(0.2) };
		unsigned long lines = s.heading.lines;
		int64_t t0 = ms;

		all_logs[0] = '\0';
		for (; ms < t0 + 4000; ms += 20) {
			server_publish(&s, CH_ACCEL, ms * 1000000LL + 1, a);
			if (ms % 100 == 0)
				server_publish(&s, CH_MAGN, ms * 1000000LL + 2, m);
		}
		CHECK(strstr(all_logs, "heading: anglvel stopped (accel/magn more than 2 s past the last gyro sample): filter re-initialised, running on accel + magn until it is back (1 so far)"));
		CHECK(s.heading.c.gyro_lost == 1 && s.heading.lines > lines + 50);
		{
			char st[4096];

			server_status_line(&s, st, sizeof(st));
			CHECK(strstr(st, "\"compass_gyro_lost\":1,"));
		}
		server_flush_all(&s);
		drain(p, buf, sizeof(buf));
	}
	/* the internal factory magn report: wanted at 5 Hz while heading is
	 * claimed, not listed, not claimable */
	CHECK(channel_want(&s, CH_MAGF) == SENSORD_HEADING_FAC_HZ && channel_from_name("magn_factory") < 0);
	{
		char st[4096];

		server_list_line(&s, st, sizeof(st));
		CHECK(!strstr(st, "magn_factory"));
	}
	/* live calibration: factory == full for 3 s: known, not calibrated;
	 * then the ADSP applies a 0.53 G bias (factory - full = (0.401,
	 * 0.229, 0.254) G, device frame): calibrated, source live */
	{
		int32_t m0[3] = { 0, q16(-0.3), q16(0.2) };	/* the feed's full magn */
		int32_t mb[3] = { q16(0.229), q16(-0.3 + 0.401), q16(0.2 - 0.254) };
		int64_t t0 = ms;

		all_logs[0] = '\0';
		for (; ms < t0 + 4000; ms += 20) {
			feed_flat_east(&s, ms);
			if (ms % 200 == 0)
				server_publish(&s, CH_MAGF, ms * 1000000LL + 3, m0);
		}
		CHECK(s.heading.mc.known && !s.heading.calibrated && !strcmp(s.heading.cal_source, "live"));
		CHECK(strstr(all_logs, "magnetometer NOT calibrated (source live, live): live bias 0.0000 0.0000 0.0000 gauss"));
		for (t0 = ms; ms < t0 + 4000; ms += 20) {
			feed_flat_east(&s, ms);
			if (ms % 200 == 0)
				server_publish(&s, CH_MAGF, ms * 1000000LL + 3, mb);
		}
		CHECK(s.heading.calibrated && s.heading.c.calibrated && !strcmp(s.heading.cal_source, "live"));
		CHECK(strstr(all_logs, "magnetometer calibrated (source live, live): live bias 0.4010 0.2290 0.2540 gauss"));
		drain(p, buf, sizeof(buf));
		l = strrchr(buf, '{');
		/* |(0.401, 0.229, 0.254)| = 0.5275 */
		CHECK(l && strstr(l, "\"calibrated\":true,") && (strstr(l, ",\"cal_source\":\"live\",\"mag_bias\":0.527}\n") ||
								 strstr(l, ",\"cal_source\":\"live\",\"mag_bias\":0.528}\n")));
		{
			char st[4096];

			server_status_line(&s, st, sizeof(st));
			CHECK(strstr(st, "\"calibrated\":true,\"cal_source\":\"live\",\"mag_bias\":[0.4010,0.2290,0.2540],"));
		}
		/* SMGR lost (the ADSP may restart and forget): learned again */
		smgr_reset(&s, "test");
		CHECK(!s.heading.mc.known && !s.heading.calibrated && !strcmp(s.heading.cal_source, "none"));
		CHECK(strstr(last_log, "magnetometer NOT calibrated (source none, smgr lost)") ||
		      strstr(all_logs, "magnetometer NOT calibrated (source none, smgr lost)"));
	}
	/* release: inputs no longer wanted, filter off */
	send_line(&s, c, "release heading");
	CHECK(!s.heading.active && channel_want(&s, CH_ACCEL) == 0 && channel_used(&s, CH_MAGN) == 0);
	CHECK(strstr(last_log, "heading off (no claims)"));
	/* the heading's input refused: its claimants hear about it */
	send_line(&s, c, "claim heading");
	drain(p, buf, sizeof(buf));
	notify_claimants(&s, CH_MAGN, "smgr refused report");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"err\":\"smgr refused report\",\"sensor\":\"heading\",\"input\":\"magn\"}"));
	/* -M: mount name, parse */
	CHECK(compass_mount_parse("upright") == COMPASS_UPRIGHT);
	close(p);
	close(po);
	close(o->fd);
}

/* Registry group 2980: calibrated when items 3903..3905 are nonzero, at
 * load and after a DSP group write; not with magn at calibration factory. */
static void test_mag_cal(void)
{
	static struct server s;
	static struct sns_reg_span groups[] = { { 2980, 26, 0 } };
	static struct sns_reg_span items[] = {
		{ 3900, 4, 0 }, { 3901, 1, 4 }, { 3902, 1, 5 },
		{ 3903, 4, 6 }, { 3904, 4, 10 }, { 3905, 4, 14 },
	};
	static uint8_t data[32];
	static struct sns_reg2_group_write_req gw;
	DEFINE_QRTR_PACKET(out, 512);
	int p;

	setup(&s, &p);
	/* no registry at all (-R) */
	s.serve_reg2 = false;
	mag_cal_check(&s, "no registry (-R)");
	CHECK(!s.heading.calibrated && !s.heading.bias_known &&
	      strstr(all_logs, "magnetometer calibration in the registry unknown (no registry (-R)") &&
	      strstr(last_log, "magnetometer NOT calibrated (source none, no registry (-R))"));
	s.serve_reg2 = true;
	s.reg.data = data;
	s.reg.size = sizeof(data);
	s.reg.groups = groups;
	s.reg.ngroups = 1;
	s.reg.items = items;
	s.reg.nitems = 6;
	mag_cal_check(&s, "registry loaded");
	CHECK(s.heading.bias_known && !s.heading.calibrated && !s.heading.c.calibrated);
	CHECK(strstr(all_logs, "magnetometer bias in the registry (registry loaded): 0.0000 0.0000 0.0000 gauss (SMGR frame, group 2980) -> not calibrated"));
	/* the DSP writes group 2980 with the live bias words */
	memset(&gw, 0, sizeof(gw));
	gw.id = 2980;
	gw.data_len = 26;
	gw.data[0] = 1;
	gw.data[4] = 1;
	{
		int32_t b[3] = { -15790, -25877, 16690 };

		memcpy(gw.data + 6, b, sizeof(b));
	}
	CHECK(qmi_encode_message(&out, QMI_REQUEST, SNS_REG2_GROUP_WRITE, 3, &gw,
				 sns_reg2_group_write_req_ei) > 0);
	s.reg_sock = -1;
	all_logs[0] = '\0';
	reg2_rx(&s, &out, 5, 81);
	CHECK(s.reg.writes == 1 && s.heading.calibrated && s.heading.c.calibrated);
	CHECK(s.heading.bias_raw[0] == -15790 && s.heading.bias_raw[2] == 16690);
	CHECK(strstr(all_logs, "magnetometer bias in the registry (DSP write): -0.2409 -0.3949 0.2547 gauss (SMGR frame, group 2980) -> calibrated"));
	CHECK(strstr(all_logs, "magnetometer calibrated (source registry, DSP write)"));
	{
		char st[4096];

		server_status_line(&s, st, sizeof(st));
		CHECK(strstr(st, "\"calibrated\":true,\"cal_source\":\"registry\",\"mag_bias\":null,"
				 "\"mag_bias_pairs\":0,\"mag_bias_reg\":[-0.240936,-0.394852,0.254669],"));
	}
	/* a known live state wins over the registry: the ADSP is not
	 * applying a bias right now (e.g. it restarted), whatever 2980 says */
	s.heading.mc.known = true;
	s.heading.mc.calibrated = false;
	cal_apply(&s, "live");
	CHECK(!s.heading.calibrated && !strcmp(s.heading.cal_source, "live"));
	compass_magcal_init(&s.heading.mc);
	cal_apply(&s, "test");
	CHECK(s.heading.calibrated && !strcmp(s.heading.cal_source, "registry"));
	/* the same bias again: no new log */
	all_logs[0] = '\0';
	mag_cal_check(&s, "DSP write");
	CHECK(all_logs[0] == '\0');
	/* magn at calibration factory: SMGR does not apply the bias */
	s.ch[CH_MAGN].cal = SNS_SMGR_CAL_FACTORY;
	mag_cal_check(&s, "test");
	CHECK(!s.heading.calibrated && strstr(last_log, "magnetometer NOT calibrated (source registry, test; magn calibration not full)"));
	CHECK(internal_want(&s, CH_MAGF) == 0);
	s.ch[CH_MAGN].cal = SNS_SMGR_CAL_FULL;
	/* a map without the items */
	s.reg.nitems = 3;
	mag_cal_check(&s, "test");
	CHECK(!s.heading.calibrated && !s.heading.bias_known);
	s.reg.data = NULL;
	s.reg.groups = s.reg.items = NULL;
	s.reg.ngroups = s.reg.nitems = 0;
	close(p);
}

/* Rotation vector indications (-V): our instance and port only, the
 * result TLV checked for its exact length, lines with heading. */
static void test_rotvec(void)
{
	static struct server s;
	/* heading 90 flat: body to ENU is -90 deg about up: x 0 y 0 z -0.7071 w 0.7071 */
	static const uint8_t ind[] = {
		0x01, 0x01, 0x00, 0x04,
		0x02, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00,
		0x03, 0x12, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x04, 0x35, 0xbf,
		0xf3, 0x04, 0x35, 0x3f, 0x02, 0x01 };
	uint8_t shortr[sizeof(ind) - 1];
	struct qrtr_packet pkt;
	struct client *c;
	char buf[4096];
	uint8_t *b;
	int p;

	c = setup(&s, &p);
	/* without -V: refused */
	send_line(&s, c, "claim rotvec");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"err\":\"not enabled (sensord -V)\",\"sensor\":\"rotvec\"}"));
	CHECK(!c->rate[CH_ROTVEC]);
	s.rotvec.opt = true;
	send_line(&s, c, "claim rotvec 20");
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"ok\":\"claim\",\"sensor\":\"rotvec\",\"rate\":20}"));
	CHECK(channel_used(&s, CH_ROTVEC) == 1 && !sam_wanted(&s, &s.rotvec));	/* SMGR down */
	s.rotvec.on = true;
	s.rotvec.instance = 4;
	s.rotvec.node = 5;
	s.rotvec.port = 12;
	CHECK(strcmp(sam_state_name(&s.rotvec), "on") == 0);
	/* another port: ignored */
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, ind, sizeof(ind));
	sam_rx(&s, &s.rotvec, &pkt, 5, 13);
	CHECK(s.rotvec.inds == 0);
	sam_rx(&s, &s.rotvec, &pkt, 5, 12);
	free(b);
	CHECK(s.rotvec.inds == 1 && s.rotvec.accuracy == 2 && s.rotvec.coord == 1);
	server_flush_all(&s);
	CHECK(strstr(last_log, "rotvec report 1: instance 4 ts 16 q x 0.00000 y 0.00000 z -0.70711 w 0.70711 (|q| 1.0000) accuracy 2 coord 1"));
	drain(p, buf, sizeof(buf));
	CHECK(strstr(buf, "{\"sensor\":\"rotvec\",\"t\":"));
	CHECK(strstr(buf, ",\"x\":0,\"y\":0,\"z\":-0.707107,\"w\":0.707107,\"accuracy\":2,\"coord\":1,"
			  "\"heading\":90.00,\"pitch\":0.00,\"roll\":0.00}\n"));
	/* the result one byte short (TLV length 17): ignored */
	memcpy(shortr, ind, sizeof(shortr));
	shortr[12] = 0x11;
	b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, shortr, sizeof(shortr));
	sam_rx(&s, &s.rotvec, &pkt, 5, 12);
	free(b);
	CHECK(s.rotvec.inds == 1 && strstr(last_log, "with a 17-byte result, ignored"));
	/* another instance: ignored */
	{
		uint8_t other[sizeof(ind)];

		memcpy(other, ind, sizeof(ind));
		other[3] = 5;
		b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, other, sizeof(other));
		sam_rx(&s, &s.rotvec, &pkt, 5, 12);
		free(b);
		CHECK(s.rotvec.inds == 1 && strstr(last_log, "rotvec report for instance 5 ignored"));
	}
	/* not a unit quaternion as floats: line without heading, logged once */
	{
		uint8_t bad[sizeof(ind)];

		memcpy(bad, ind, sizeof(ind));
		memset(bad + 14, 0, 16);
		bad[29] = 0x40;		/* w = 2.0 */
		c->next_t[CH_ROTVEC] = 0;	/* stamps are receive times here: not decimated away */
		b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, bad, sizeof(bad));
		all_logs[0] = '\0';
		sam_rx(&s, &s.rotvec, &pkt, 5, 12);
		free(b);
		CHECK(strstr(all_logs, "rotvec quaternion norm 2.0000: not a unit quaternion"));
		server_flush_all(&s);
		drain(p, buf, sizeof(buf));
		CHECK(strstr(buf, "\"w\":2,\"accuracy\":2,\"coord\":1}\n"));
	}
	/* words that are no finite floats (exponent 0xff): null, never nan/inf */
	{
		uint8_t nan_q[sizeof(ind)];

		memcpy(nan_q, ind, sizeof(ind));
		memcpy(nan_q + 14, "\x00\x00\xc0\x7f\x00\x00\x80\x7f", 8);	/* x nan, y inf */
		b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, nan_q, sizeof(nan_q));
		c->next_t[CH_ROTVEC] = 0;
		sam_rx(&s, &s.rotvec, &pkt, 5, 12);
		free(b);
		server_flush_all(&s);
		drain(p, buf, sizeof(buf));
		CHECK(strstr(buf, "\"x\":null,\"y\":null,\"z\":-0.707107,\"w\":0.707107,\"accuracy\":2,\"coord\":1}\n"));
		CHECK(!strstr(buf, "nan") && !strstr(buf, "inf"));
	}
	/* reports for a foreign instance (20 Hz): logged first, then at most
	 * once per 5 s */
	{
		uint8_t other[sizeof(ind)];
		int k, n = 0;
		char *l;

		memcpy(other, ind, sizeof(ind));
		other[3] = 6;
		fake_now += 5000;	/* past the earlier foreign report's log */
		all_logs[0] = '\0';
		for (k = 0; k < 40; k++) {
			fake_now += 50;
			b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, other, sizeof(other));
			sam_rx(&s, &s.rotvec, &pkt, 5, 12);
			free(b);
		}
		for (l = all_logs; (l = strstr(l, "rotvec report for instance 6 ignored")); l++)
			n++;
		CHECK(n == 1);
		fake_now += 5000;
		b = mkpkt(&pkt, QMI_INDICATION, SNS_SAM_REPORT_IND, other, sizeof(other));
		sam_rx(&s, &s.rotvec, &pkt, 5, 12);
		free(b);
		CHECK(strstr(last_log, "rotvec report for instance 6 ignored (ours is 4; 42 such reports so far)"));
	}
	{
		char st[4096];

		server_status_line(&s, st, sizeof(st));
		CHECK(strstr(st, "\"rotvec\":\"on\",\"rotvec_instance\":4,\"rotvec_enables\":0,"
				 "\"rotvec_inds\":3,\"rotvec_errors\":0,\"rotvec_last_error\":-1,"));
	}
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
	test_heading();
	test_mag_cal();
	test_rotvec();
	if (failures) {
		fprintf(stderr, "test-sensord: %d failure(s)\n", failures);
		return 1;
	}
	printf("test-sensord: all passed\n");
	return 0;
}
