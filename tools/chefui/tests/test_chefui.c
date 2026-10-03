/*
 * End-to-end host tests for libchefui's fbdev backend and main loop,
 * linked with a headless LVGL build. Each scenario forks a child that runs
 * a small chefui application against a temporary directory:
 *   fb0       a regular file standing in for the two fb pages (the fb
 *             ioctls are wrapped: -Wl,--wrap=ioctl, logged to ioctl.log)
 *   input/event0  a FIFO the parent writes input_events into (the evdev
 *             ioctls are wrapped to describe a 720x1600 MT touchscreen)
 *   buttond.sock  a listener played by the parent
 *   fb0.lock, fblog.off, brightness, modes
 * Covered: first frame (R/B swap, alpha, page 1 pan), taps delivered as
 * press+release batches reaching LVGL as separate clicks, signal exit
 * (POWERDOWN, close, unlock, flag untouched), buttond claims and the
 * default power.short toggle (flag/ioctl ordering), start-dark, buttond
 * restart re-claim, static-page wakeups, and handoff to a re-executed
 * peer with no unlocked gap.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "check.h"
#include "../chefui_internal.h"
#include "lvgl_private.h"

#define W 120
#define H 200
#define STRIDE (W * 4 + 64)

static int g_pan_delay_us;
static unsigned g_pan_count;
static int g_pan_brightness;

static char g_self[300];
static char D[64], P_FB[96], P_LOCK[96], P_FLAG[96], P_BL[96], P_MODES[96], P_INPUT[96],
	    P_EV[112], P_SOCK[96], P_LOG[96];

/* ------------------------------------------------------- wrapped ioctl */

int __real_ioctl(int fd, unsigned long req, ...);

static void log_ioctl(unsigned long req, long arg)
{
	char line[96];
	int fd = open(P_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644), n;

	if (fd < 0)
		return;
	n = snprintf(line, sizeof(line), "%s %ld %d\n",
		     req == FBIOGET_VSCREENINFO ? "GETV" : req == FBIOGET_FSCREENINFO ? "GETF" :
		     req == FBIOBLANK ? (arg == FB_BLANK_UNBLANK ? "UNBLANK" : "POWERDOWN") :
		     req == FBIOPAN_DISPLAY ? "PAN" : "PUT",
		     arg, access(P_FLAG, F_OK) == 0);
	(void)!write(fd, line, (size_t)n);
	close(fd);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	switch (req) {
	case FBIOGET_VSCREENINFO: {
		struct fb_var_screeninfo v;

		log_ioctl(req, 0);
		memset(&v, 0, sizeof(v));
		v.xres = W; v.yres = H; v.xres_virtual = W; v.yres_virtual = 2 * H;
		v.bits_per_pixel = 32;
		v.red.offset = 0; v.red.length = 8;
		v.green.offset = 8; v.green.length = 8;
		v.blue.offset = 16; v.blue.length = 8;
		v.transp.offset = 24; v.transp.length = 8;
		memcpy(arg, &v, sizeof(v));
		return 0;
	}
	case FBIOGET_FSCREENINFO: {
		struct fb_fix_screeninfo f;

		log_ioctl(req, 0);
		memset(&f, 0, sizeof(f));
		f.line_length = STRIDE;
		f.smem_len = STRIDE * 2 * H;
		memcpy(arg, &f, sizeof(f));
		return 0;
	}
	case FBIOBLANK:
		log_ioctl(req, (long)arg);
		return 0;
	case FBIOPAN_DISPLAY:
		if (g_pan_delay_us) usleep(g_pan_delay_us);
		g_pan_count++;
		{
			char level[16] = {0};
			int bl = open(P_BL, O_RDONLY);
			if (bl >= 0) { (void)!read(bl, level, sizeof(level) - 1); close(bl); }
			g_pan_brightness = atoi(level);
		}
		log_ioctl(req, (long)((struct fb_var_screeninfo *)arg)->yoffset);
		return 0;
	case FBIOPUT_VSCREENINFO:
		log_ioctl(req, 0);
		errno = EPERM;
		return -1;
	default:
		break;
	}
	if (_IOC_TYPE(req) == 'E') {
		if (_IOC_NR(req) == _IOC_NR(EVIOCGPROP(0))) {
			uint8_t *b = arg;

			memset(b, 0, _IOC_SIZE(req));
			b[INPUT_PROP_DIRECT / 8] |= 1u << (INPUT_PROP_DIRECT % 8);
			return 0;
		}
		if (_IOC_NR(req) == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
			uint8_t *b = arg;

			memset(b, 0, _IOC_SIZE(req));
			b[ABS_MT_POSITION_X / 8] |= 1u << (ABS_MT_POSITION_X % 8);
			b[ABS_MT_POSITION_Y / 8] |= 1u << (ABS_MT_POSITION_Y % 8);
			return 0;
		}
		if (_IOC_NR(req) == _IOC_NR(EVIOCGMTSLOTS(0))) {
			struct { __u32 code; __s32 v[64]; } *m = arg;
			int n = (int)((_IOC_SIZE(req) - 4) / 4), i;

			for (i = 0; i < n; i++)
				m->v[i] = m->code == ABS_MT_TRACKING_ID ? -1 : 0;
			return 0;
		}
		if (_IOC_NR(req) >= 0x40 && _IOC_NR(req) < 0x40 + ABS_CNT) {
			struct input_absinfo *a = arg;
			unsigned code = _IOC_NR(req) - 0x40;

			memset(a, 0, sizeof(*a));
			a->maximum = code == ABS_MT_POSITION_X ? 720 : code == ABS_MT_POSITION_Y ? 1600 :
				     code == ABS_MT_SLOT ? 9 : 0;
			return 0;
		}
	}
	return __real_ioctl(fd, req, arg);
}

/* ------------------------------------------------------------ fixture */

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		(void)!write(fd, s, strlen(s));
		close(fd);
	}
}

static void read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;

	buf[n > 0 ? n : 0] = '\0';
	if (fd >= 0)
		close(fd);
}

static void set_paths(const char *dir)
{
	snprintf(D, sizeof(D), "%s", dir);
	snprintf(P_FB, sizeof(P_FB), "%s/fb0", D);
	snprintf(P_LOCK, sizeof(P_LOCK), "%s/fb0.lock", D);
	snprintf(P_FLAG, sizeof(P_FLAG), "%s/fblog.off", D);
	snprintf(P_BL, sizeof(P_BL), "%s/brightness", D);
	snprintf(P_MODES, sizeof(P_MODES), "%s/modes", D);
	snprintf(P_INPUT, sizeof(P_INPUT), "%s/input", D);
	snprintf(P_EV, sizeof(P_EV), "%s/event0", P_INPUT);
	snprintf(P_SOCK, sizeof(P_SOCK), "%s/buttond.sock", D);
	snprintf(P_LOG, sizeof(P_LOG), "%s/ioctl.log", D);
}

static void use_paths(void)
{
	struct cu_test_paths p = {
		.fb = P_FB, .lock = P_LOCK, .flag = P_FLAG, .backlight = P_BL, .modes = P_MODES,
		.input_dir = P_INPUT, .buttond_sock = P_SOCK, .kmsg = "/dev/null",
	};

	cu_set_test_paths(&p);
}

/* Fresh files; returns the parent's read-write fd on the input FIFO (it
 * keeps a writer present, so the child never reads EOF). */
static int fixture(void)
{
	int fd;

	unlink(P_FLAG);
	unlink(P_LOG);
	unlink(P_SOCK);
	write_file(P_BL, "");
	write_file(P_MODES, "U:120x200p-60\n");
	fd = open(P_FB, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)STRIDE * 2 * H) == 0);
	close(fd);
	mkdir(P_INPUT, 0700);
	unlink(P_EV);
	CHECK(mkfifo(P_EV, 0600) == 0);
	fd = open(P_EV, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	return fd;
}

static int listen_sock(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", P_SOCK);
	CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 4) == 0);
	return fd;
}

static int accept_timeout(int l, int ms)
{
	struct pollfd p = { .fd = l, .events = POLLIN };

	if (poll(&p, 1, ms) <= 0)
		return -1;
	return accept4(l, NULL, NULL, SOCK_CLOEXEC);
}

static void read_quiet(int fd, char *buf, size_t cap, int ms)
{
	size_t n = 0;
	struct pollfd p = { .fd = fd, .events = POLLIN };

	while (n < cap - 1 && poll(&p, 1, ms) > 0) {
		ssize_t r = read(fd, buf + n, cap - 1 - n);

		if (r <= 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static bool lock_free(void)
{
	int fd = open(P_LOCK, O_RDWR | O_CLOEXEC);
	bool ok = fd >= 0 && flock(fd, LOCK_SH | LOCK_NB) == 0;

	if (fd >= 0)
		close(fd);
	return ok;
}

/* Wait until the ioctl log contains `needle` (up to ms). */
static bool log_wait(const char *needle, int ms)
{
	char buf[8192];

	for (; ms > 0; ms -= 20) {
		read_file(P_LOG, buf, sizeof(buf));
		if (strstr(buf, needle))
			return true;
		sleep_ms(20);
	}
	return false;
}

static int count_lines(const char *buf, const char *prefix)
{
	int n = 0;
	const char *p = buf;
	size_t l = strlen(prefix);

	while (p && *p) {
		if (strncmp(p, prefix, l) == 0)
			n++;
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return n;
}

static const char *last_line(const char *buf)
{
	static char line[96];
	size_t n = strlen(buf);
	const char *s;

	while (n && buf[n - 1] == '\n')
		n--;
	s = buf + n;
	while (s > buf && s[-1] != '\n')
		s--;
	snprintf(line, sizeof(line), "%.*s", (int)(buf + n - s), s);
	return line;
}

/* ----------------------------------------------------- child programs */

static int g_clicks;
static int g_report = -1;

static void click_cb(lv_event_t *e)
{
	(void)e;
	g_clicks++;
}

static void quit_cb(lv_timer_t *t)
{
	(void)t;
	chefui_quit(7);
}

static void tick_label(lv_timer_t *t)
{
	static int n;

	lv_label_set_text_fmt(lv_timer_get_user_data(t), "%d", ++n);
}

/* A red screen with a big button in the middle. */
static lv_display_t *app_init(const char *const *claims)
{
	struct chefui_config cfg;
	lv_display_t *d;
	lv_obj_t *b;

	memset(&cfg, 0, sizeof(cfg));
	cfg.app_name = "test";
	cfg.claims = claims;
	use_paths();
	d = chefui_init(&cfg);
	if (!d)
		return NULL;
	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0xff0000), 0);
	b = lv_button_create(lv_screen_active());
	lv_obj_set_size(b, 80, 80);
	lv_obj_center(b);
	lv_obj_add_event_cb(b, click_cb, LV_EVENT_CLICKED, NULL);
	return d;
}

static void report(const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	(void)!write(g_report, buf, (size_t)n);
}

/* ------------------------------------------------------------ helpers */

struct child {
	pid_t pid;
	int rep;	/* parent's read end of the report pipe */
};

typedef int (*child_fn)(void *arg);

static struct child spawn(child_fn fn, void *arg, int fifo_fd)
{
	struct child c;
	int p[2];

	CHECK(pipe(p) == 0);
	c.pid = fork();
	if (c.pid == 0) {
		close(p[0]);
		if (fifo_fd >= 0)
			close(fifo_fd);
		g_report = p[1];
		_exit(fn(arg));
	}
	close(p[1]);
	c.rep = p[0];
	return c;
}

static int finish(struct child *c, char *out, size_t cap)
{
	int status = 0;

	read_quiet(c->rep, out, cap, 6000);
	close(c->rep);
	CHECK(waitpid(c->pid, &status, 0) == c->pid);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

static void tap(int fifo, int x, int y)
{
	struct input_event e[6];

	memset(e, 0, sizeof(e));
	e[0].type = EV_ABS; e[0].code = ABS_MT_TRACKING_ID; e[0].value = 1;
	e[1].type = EV_ABS; e[1].code = ABS_MT_POSITION_X; e[1].value = x;
	e[2].type = EV_ABS; e[2].code = ABS_MT_POSITION_Y; e[2].value = y;
	e[3].type = EV_SYN; e[3].code = SYN_REPORT;
	e[4].type = EV_ABS; e[4].code = ABS_MT_TRACKING_ID; e[4].value = -1;
	e[5].type = EV_SYN; e[5].code = SYN_REPORT;
	/* press and release in one write: they arrive in one read */
	CHECK(write(fifo, e, sizeof(e)) == (ssize_t)sizeof(e));
}

/* ------------------------------------------------- scenario: taps, exit */

static int child_taps(void *arg)
{
	int code;

	(void)arg;
	if (!app_init(NULL))
		return 99;
	report("R");
	code = chefui_run();
	report("clicks=%d code=%d", g_clicks, code);
	return 0;
}

static void test_frame_taps_signal_exit(void)
{
	int fifo = fixture(), l = listen_sock(), s, i;
	struct child c;
	char out[128], log[8192], claims[128];
	uint32_t px;
	int fd;

	c = spawn(child_taps, NULL, fifo);
	read_quiet(c.rep, out, 2, 3000);
	CHECK(strcmp(out, "R") == 0);
	s = accept_timeout(l, 2000);
	CHECK(s >= 0);
	read_quiet(s, claims, sizeof(claims), 100);
	CHECK(strcmp(claims, "claim power.short\n") == 0);	/* power.long never by default */
	CHECK(log_wait("PAN 200 0", 2000));	/* first frame: page 1 */
	CHECK(!lock_free());

	for (i = 0; i < 5; i++) {
		tap(fifo, 360, 800);		/* the screen centre in touch units */
		sleep_ms(120);
	}
	tap(fifo, 10, 10);			/* outside the button */
	sleep_ms(200);
	kill(c.pid, SIGTERM);
	CHECK(finish(&c, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "clicks=5 code=143") == 0);

	/* the first frame: full copy into page 1, R/B swapped, alpha forced */
	fd = open(P_FB, O_RDONLY);
	CHECK(pread(fd, &px, 4, (off_t)STRIDE * H) == 4);	/* page 1, (0,0) */
	CHECK(px == 0xff0000ffu);				/* RGBA bytes ff 00 00 ff */
	CHECK(pread(fd, &px, 4, (off_t)STRIDE * (2 * H - 1) + (W - 1) * 4) == 4);
	CHECK(px == 0xff0000ffu);
	CHECK(pread(fd, &px, 4, (off_t)STRIDE * H + W * 4) == 4);
	CHECK(px == 0);						/* stride padding untouched */
	close(fd);

	read_file(P_LOG, log, sizeof(log));
	CHECK(strncmp(log, "GETV 0 0\nGETF 0 0\nUNBLANK 0 0\n", 30) == 0);
	CHECK(count_lines(log, "PUT") == 0);
	CHECK(strcmp(last_line(log), "POWERDOWN 4 0") == 0);	/* signal exit: POWERDOWN last */
	CHECK(lock_free());
	CHECK(access(P_FLAG, F_OK) != 0);			/* flag left alone */
	read_file(P_BL, out, sizeof(out));
	CHECK(strcmp(out, "96\n") == 0);
	close(s);
	close(l);
	close(fifo);
}

/* ------------------------------- scenario: start dark, buttond toggle */

static int child_run(void *arg)
{
	lv_obj_t *lbl;
	int code;

	(void)arg;
	if (!app_init(NULL))
		return 99;
	/* an application timer that keeps invalidating, also while dark:
	 * nothing may reach the fb then (no copy into a missing mapping, no
	 * pan into a powered-down panel) */
	lbl = lv_label_create(lv_screen_active());
	lv_timer_create(tick_label, 100, lbl);
	report("R");
	code = chefui_run();
	report("code=%d", code);
	return 0;
}

static void test_dark_start_toggle_restart(void)
{
	int fifo = fixture(), l = listen_sock(), s, s2;
	struct child c;
	char out[128], log[8192], claims[128];

	write_file(P_FLAG, "");
	c = spawn(child_run, NULL, fifo);
	read_quiet(c.rep, out, 2, 3000);
	CHECK(strcmp(out, "R") == 0);
	s = accept_timeout(l, 2000);
	CHECK(s >= 0);
	read_quiet(s, claims, sizeof(claims), 100);
	sleep_ms(400);				/* ~4 label updates while dark */
	read_file(P_LOG, log, sizeof(log));
	CHECK(log[0] == '\0');			/* dark start: fb0 never opened, nothing panned */
	CHECK(!lock_free());

	/* power.short: on. Flag removed before the first open's UNBLANK */
	CHECK(write(s, "ok claim power.short\nevent power.short\n", 39) == 39);
	CHECK(log_wait("PAN", 2000));
	CHECK(access(P_FLAG, F_OK) != 0);
	read_file(P_LOG, log, sizeof(log));
	CHECK(strncmp(log, "GETV 0 0\nGETF 0 0\nUNBLANK 0 0\n", 30) == 0);

	/* power.short again: off. POWERDOWN with the flag still absent, then the flag */
	CHECK(write(s, "event power.short\n", 18) == 18);
	CHECK(log_wait("POWERDOWN 4 0", 2000));
	sleep_ms(400);				/* label keeps changing while off */
	CHECK(access(P_FLAG, F_OK) == 0);
	read_file(P_LOG, log, sizeof(log));
	CHECK(strcmp(last_line(log), "POWERDOWN 4 0") == 0);	/* no PAN after POWERDOWN */

	/* buttond restarts: the client reconnects and claims again */
	close(s);
	s2 = accept_timeout(l, 2500);
	CHECK(s2 >= 0);
	read_quiet(s2, claims, sizeof(claims), 100);
	CHECK(strcmp(claims, "claim power.short\n") == 0);
	CHECK(write(s2, "event power.short\n", 18) == 18);	/* on again over the new link */
	CHECK(log_wait("UNBLANK 0 0\nPAN", 2000));
	CHECK(write(s2, "event power.short\n", 18) == 18);	/* and off */
	sleep_ms(200);

	/* clean exit with the screen off: flag stays, nothing turned on */
	kill(c.pid, SIGINT);
	CHECK(finish(&c, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "code=130") == 0);
	read_file(P_LOG, log, sizeof(log));
	CHECK(strcmp(last_line(log), "POWERDOWN 4 1") == 0);
	CHECK(access(P_FLAG, F_OK) == 0);
	CHECK(lock_free());
	CHECK(count_lines(log, "PUT") == 0);
	close(s2);
	close(l);
	close(fifo);
}

/* ----------------------------------------- scenario: static wakeups */

static int child_static(void *arg)
{
	lv_obj_t *lbl;
	int code;

	(void)arg;
	if (!app_init(NULL))
		return 99;
	lbl = lv_label_create(lv_screen_active());
	lv_timer_create(tick_label, 1000, lbl);	/* a 1 Hz clock */
	lv_timer_create(quit_cb, 3000, NULL);
	code = chefui_run();
	report("code=%d wakeups=%lu", code, cu_total_wakeups);
	return 0;
}

static void test_static_wakeups(void)
{
	int fifo = fixture(), l = listen_sock(), s;
	struct child c;
	char out[128];
	unsigned long wk = 0;
	int code = 0;

	c = spawn(child_static, NULL, fifo);
	s = accept_timeout(l, 2000);		/* buttond present: no reconnect wakeups */
	CHECK(finish(&c, out, sizeof(out)) == 0);
	CHECK(sscanf(out, "code=%d wakeups=%lu", &code, &wk) == 2);
	CHECK(code == 7);
	/* 3 s with a 1 Hz label: about 2 wakeups per second (timer, then the
	 * refresh it triggers), plus startup */
	CHECK(wk >= 3 && wk <= 12);
	printf("test-chefui: static page with a 1 Hz label: %lu wakeups in 3 s\n", wk);
	if (s >= 0)
		close(s);
	close(l);
	close(fifo);
}

/* ------------------------------ scenario: screen off from touch handlers */

static int g_released_a, g_clicked_b, g_clicked_a;

static void off_on_released(lv_event_t *e)
{
	(void)e;
	g_released_a++;
	chefui_screen_set(false);
}

static void count_clicked_a(lv_event_t *e)
{
	(void)e;
	g_clicked_a++;
}

static void off_on_clicked(lv_event_t *e)
{
	(void)e;
	g_clicked_b++;
	chefui_screen_set(false);
}

static int child_off_handlers(void *arg)
{
	struct chefui_config cfg;
	lv_obj_t *a, *b;
	int code;

	(void)arg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.app_name = "test";
	use_paths();
	if (!chefui_init(&cfg))
		return 99;
	a = lv_button_create(lv_screen_active());
	lv_obj_set_size(a, 40, 80);
	lv_obj_set_pos(a, 10, 60);
	lv_obj_add_event_cb(a, off_on_released, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(a, count_clicked_a, LV_EVENT_CLICKED, NULL);
	b = lv_button_create(lv_screen_active());
	lv_obj_set_size(b, 40, 80);
	lv_obj_set_pos(b, 70, 60);
	lv_obj_add_event_cb(b, off_on_clicked, LV_EVENT_CLICKED, NULL);
	report("R");
	code = chefui_run();
	report("relA=%d clkA=%d clkB=%d code=%d", g_released_a, g_clicked_a, g_clicked_b, code);
	return 0;
}

/* A button that turns the screen off from its RELEASED or CLICKED
 * handler: the release must not be delivered a second time from inside
 * that handler (double click, NULL indev_act in LVGL's outer read). */
static void test_screen_off_from_handlers(void)
{
	int fifo = fixture(), l = listen_sock(), s;
	struct child c;
	char out[128], log[8192];
	int ra = -1, ca = -1, cb = -1, code = 0;

	c = spawn(child_off_handlers, NULL, fifo);
	read_quiet(c.rep, out, 2, 3000);
	s = accept_timeout(l, 2000);
	CHECK(s >= 0);
	CHECK(log_wait("PAN", 2000));
	tap(fifo, 180, 800);			/* button A (x 10..49) */
	CHECK(log_wait("POWERDOWN 4 0", 2000));
	sleep_ms(150);
	CHECK(write(s, "event power.short\n", 18) == 18);	/* back on */
	CHECK(log_wait("UNBLANK 0 0\nPAN", 2000));
	sleep_ms(100);
	tap(fifo, 540, 800);			/* button B (x 70..109) */
	sleep_ms(300);
	read_file(P_LOG, log, sizeof(log));
	CHECK(count_lines(log, "POWERDOWN") == 2);
	CHECK(strcmp(last_line(log), "POWERDOWN 4 0") == 0);
	kill(c.pid, SIGTERM);
	CHECK(finish(&c, out, sizeof(out)) == 0);
	CHECK(sscanf(out, "relA=%d clkA=%d clkB=%d code=%d", &ra, &ca, &cb, &code) == 4);
	CHECK(ra == 1 && ca <= 1 && cb == 1);	/* once each, no crash */
	CHECK(code == 143);
	if (s >= 0)
		close(s);
	close(l);
	close(fifo);
}

/* ------------------------------------------------- scenario: pinch */

static int g_pinch, g_rotate, g_swipe;
static float g_pinch_scale;

static void gesture_cb(lv_event_t *e)
{
	if (lv_event_get_gesture_type(e) == LV_INDEV_GESTURE_PINCH) {
		g_pinch++;
		g_pinch_scale = lv_event_get_pinch_scale(e);
	} else if (lv_event_get_gesture_type(e) == LV_INDEV_GESTURE_ROTATE) {
		g_rotate++;
	} else if (lv_event_get_gesture_type(e) == LV_INDEV_GESTURE_TWO_FINGERS_SWIPE) {
		g_swipe++;
	}
}

static int child_pinch(void *arg)
{
	int code;

	(void)arg;
	if (!app_init(NULL))
		return 99;
	lv_obj_add_event_cb(lv_screen_active(), gesture_cb, LV_EVENT_GESTURE, NULL);
	report("R");
	code = chefui_run();
	report("pinch=%d scale=%.2f rotate=%d swipe=%d code=%d", g_pinch, (double)g_pinch_scale, g_rotate, g_swipe, code);
	return 0;
}

static void two_xy(int fifo, int id0, int x0, int y0, int id1, int x1, int y1)
{
	struct input_event e[9];
	int n = 0;

	memset(e, 0, sizeof(e));
#define EV(t, c, v) (e[n].type = (t), e[n].code = (c), e[n].value = (v), n++)
	EV(EV_ABS, ABS_MT_SLOT, 0);
	EV(EV_ABS, ABS_MT_TRACKING_ID, id0);
	EV(EV_ABS, ABS_MT_POSITION_X, x0);
	EV(EV_ABS, ABS_MT_POSITION_Y, y0);
	EV(EV_ABS, ABS_MT_SLOT, 1);
	EV(EV_ABS, ABS_MT_TRACKING_ID, id1);
	EV(EV_ABS, ABS_MT_POSITION_X, x1);
	EV(EV_ABS, ABS_MT_POSITION_Y, y1);
	EV(EV_SYN, SYN_REPORT, 0);
#undef EV
	CHECK(write(fifo, e, sizeof(e)) == (ssize_t)sizeof(e));
}

/* Noisy pinch and translated swipe must not be stolen by tiny rotation;
 * an intentional rotation must still work. Each uses real input frames
 * through the backend/LVGL loop, including release and gesture reporting. */
static void test_gestures(void)
{
	for (int mode = 0; mode < 3; mode++) {
		int fifo = fixture(), l = listen_sock(), s;
		struct child c = spawn(child_pinch, NULL, fifo);
		char out[128];
		int pinch = 0, rotate = 0, swipe = 0, code = 0;
		float scale = 0;
		read_quiet(c.rep, out, 2, 3000);
		s = accept_timeout(l, 2000);
		sleep_ms(100);
		for (int i = 0; i <= 10; i++) {
			if (mode == 0)
				two_xy(fifo, 11, 300 - 20 * i, 1300, 12, 420 + 20 * i, 1300 + (i ? 8 : 0));
			else if (mode == 1)
				two_xy(fifo, 11, 80 + 45 * i, 1300, 12, 260 + 45 * i, 1300 + (i ? 8 : 0));
			else
				two_xy(fifo, 11, 200, 1300 - 8 * i, 12, 500, 1300 + 8 * i);
			sleep_ms(40);
		}
		two_xy(fifo, -1, 100, 1300, -1, 600, 1300);
		sleep_ms(150);
		kill(c.pid, SIGTERM);
		CHECK(finish(&c, out, sizeof(out)) == 0);
		CHECK(sscanf(out, "pinch=%d scale=%f rotate=%d swipe=%d code=%d",
			&pinch, &scale, &rotate, &swipe, &code) == 5);
		CHECK(code == 143);
		if (mode == 0) {
			CHECK(pinch > 0 && scale > 1.4f);
			CHECK(rotate == 0 && swipe == 0);
		} else if (mode == 1) {
			CHECK(swipe > 0);
			CHECK(rotate == 0 && pinch == 0);
		} else {
			CHECK(rotate > 0);
			CHECK(pinch == 0 && swipe == 0);
		}
		if (s >= 0) close(s);
		close(l);
		close(fifo);
	}
}

static int child_safe_area(void *arg)
{
	int rotation = *(int *)arg;
	struct chefui_config cfg = { .app_name = "safe-test", .rotation = rotation,
		.refresh_ms = rotation == 180 ? 33 : 0 };
	lv_obj_t *root, *content, *other;
	lv_area_t a;
	int before = g_failures;
	use_paths();
	CHECK(chefui_init(&cfg) != NULL);
	lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
	CHECK(refresh->period == (rotation == 180 ? 33u : 16u));
	CHECK(lv_anim_get_timer()->period == refresh->period);
	chefui_refresh_period_set(33);
	CHECK(refresh->period == 33);
	CHECK(lv_anim_get_timer()->period == 33);
	chefui_refresh_period_set(0);
	CHECK(refresh->period == 16);
	CHECK(lv_anim_get_timer()->period == 16);
	root = chefui_root();
	CHECK(root == lv_screen_active());
	CHECK(chefui_safe_top_get() == 96);
	content = chefui_content_root();
	CHECK(content != root && lv_obj_get_parent(content) == root);
	CHECK(chefui_content_root() == content);
	CHECK(!lv_obj_is_scrollable(content));
	CHECK(lv_obj_get_style_bg_opa(content, 0) == LV_OPA_TRANSP);
	CHECK(chefui_safe_top_set(24) == 0);
	a = chefui_safe_area();
	lv_obj_update_layout(root);
	CHECK(lv_obj_get_x(content) == a.x1 && lv_obj_get_y(content) == a.y1);
	CHECK(lv_obj_get_width(content) == a.x2 - a.x1 + 1);
	CHECK(lv_obj_get_height(content) == a.y2 - a.y1 + 1);
	CHECK(lv_display_get_horizontal_resolution(lv_display_get_default()) == (rotation % 180 ? H : W));
	CHECK(lv_display_get_vertical_resolution(lv_display_get_default()) == (rotation % 180 ? W : H));
	CHECK(chefui_safe_top_set(H) == -EINVAL);
	CHECK(chefui_safe_top_set(-2) == -EINVAL);
	CHECK(chefui_safe_top_get() == 24);
	other = lv_obj_create(NULL);
	lv_screen_load(other);
	CHECK(chefui_content_root() != content);
	lv_screen_load(root);
	CHECK(chefui_content_root() == content);
	lv_obj_delete(other);
	lv_obj_delete(content);
	CHECK(chefui_content_root() != NULL);
	CHECK(chefui_safe_top_set(-1) == 0);
	a = chefui_safe_area();
	CHECK(a.x1 == 0 && a.y1 == 0);
	CHECK(a.x2 == (rotation % 180 ? H : W) - 1 && a.y2 == (rotation % 180 ? W : H) - 1);
	CHECK(chefui_safe_top_set(0) == 0 && chefui_safe_top_get() == 96);
	report("safe=%d", g_failures - before);
	chefui_quit(0);
	chefui_run();
	return g_failures != before;
}

/* A failed init must relinquish button claims even if its caller stays alive. */
static int child_bad_inset(void *arg)
{
	struct chefui_config cfg = { .app_name = "bad-inset", .safe_top_px = *(int *)arg };
	use_paths();
	if (chefui_init(&cfg)) {
		chefui_quit(0);
		chefui_run();
		return 99;
	}
	report("R");
	sleep_ms(1000); /* Socket EOF must precede process exit. */
	report("done");
	return 0;
}

static void test_failed_init_claim_release(void)
{
	for (int i = 0; i < 2; i++) {
		int inset = i ? -2 : H;
		int fifo = fixture(), l = listen_sock(), s, status;
		struct child c = spawn(child_bad_inset, &inset, fifo);
		char out[128];
		bool eof = false;
		read_quiet(c.rep, out, 2, 3000);
		CHECK(strcmp(out, "R") == 0);
		s = accept_timeout(l, 200);
		CHECK(s >= 0);
		if (s >= 0) {
			/* Drain the already-sent claim, then observe peer closure. */
			struct pollfd pfd = { .fd = s, .events = POLLIN };
			while (poll(&pfd, 1, 200) > 0) {
				ssize_t n = recv(s, out, sizeof(out), MSG_DONTWAIT);
				if (n == 0) { eof = true; break; }
				if (n < 0) break;
			}
			close(s);
		}
		CHECK(eof);
		CHECK(waitpid(c.pid, &status, WNOHANG) == 0);
		CHECK(lock_free());
		CHECK(finish(&c, out, sizeof(out)) == 0);
		CHECK(strcmp(out, "done") == 0);
		close(l);
		close(fifo);
	}
}

static void test_safe_api(void)
{
	for (int rotation = 0; rotation < 360; rotation += 90) {
		int fifo = fixture();
		struct child c = spawn(child_safe_area, &rotation, fifo);
		char out[128];
		CHECK(finish(&c, out, sizeof(out)) == 0);
		CHECK(strcmp(out, "safe=0") == 0);
		CHECK(lock_free());
		close(fifo);
	}
}

/* Real LVGL animation + a pan that blocks for one 60Hz scanout period.
 * Check effective fps, not merely timer configuration, and bound wakeups. */
static void pacing_anim(void *object, int32_t value)
{
	lv_obj_set_x(object, value);
}

static void pacing_quit(lv_timer_t *timer)
{
	(void)timer;
	chefui_quit(0);
}

static int child_pacing(void *arg)
{
	int mode = *(int *)arg;
	struct chefui_config cfg = { .app_name = "pacing", .refresh_ms = mode == 1 ? 33 : 16 };
	lv_anim_t animation;
	lv_obj_t *object;
	int64_t start;
	use_paths();
	g_pan_delay_us = 16000;
	g_pan_count = 0;
	if (!chefui_init(&cfg)) return 99;
	/* Mode2 reproduces the old animation timer's hardwired33ms even
	 * though the display requests16ms. */
	if (mode == 2) lv_timer_set_period(lv_anim_get_timer(), 33);
	object = lv_obj_create(chefui_root());
	lv_obj_set_size(object, 10, 10);
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, object);
	lv_anim_set_exec_cb(&animation, pacing_anim);
	lv_anim_set_values(&animation, 0, 100);
	lv_anim_set_duration(&animation, 500);
	lv_anim_set_reverse_duration(&animation, 500);
	lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&animation);
	lv_timer_create(pacing_quit, 1500, NULL);
	start = cu_now_ms();
	chefui_run();
	report("frames=%u elapsed=%lld wake=%lu", g_pan_count,
		(long long)(cu_now_ms() - start), cu_total_wakeups);
	return 0;
}

static void test_blocking_pan_pacing(void)
{
	for (int mode = 0; mode < 3; mode++) {
		int fifo = fixture(), l = listen_sock();
		struct child c = spawn(child_pacing, &mode, fifo);
		char out[128];
		unsigned frames;
		long long elapsed;
		unsigned long wake;
		CHECK(finish(&c, out, sizeof(out)) == 0);
		CHECK(sscanf(out, "frames=%u elapsed=%lld wake=%lu", &frames, &elapsed, &wake) == 3);
		double fps = 1000.0 * frames / elapsed;
		fprintf(stderr, "pacing mode%d: %.1f fps, %lu wakes in %lldms\n", mode, fps, wake, elapsed);
		CHECK(fps >= (mode == 0 ? 48 : 23) && fps <= (mode == 0 ? 65 : 36));
		CHECK(wake < (unsigned long)(elapsed / 3)); /* no zero-timeout spin */
		close(l);
		close(fifo);
	}
}

static unsigned g_bright_requests, g_bright_sync_pans;
static int64_t g_bright_max_ms;

static void brightness_event(lv_event_t *event)
{
	chefui_brightness_set(lv_slider_get_value(lv_event_get_target(event)));
	g_bright_requests++;
}

static void brightness_burst(lv_timer_t *timer)
{
	lv_obj_t *slider = lv_timer_get_user_data(timer);
	unsigned before = g_pan_count;
	int64_t start = cu_now_ms();
	for (int i = 0; i < 32; i++) {
		lv_slider_set_value(slider, i == 31 ? 217 : 20 + 5 * i, LV_ANIM_OFF);
		lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	}
	g_bright_sync_pans += g_pan_count - before;
	int64_t elapsed = cu_now_ms() - start;
	if (elapsed > g_bright_max_ms) g_bright_max_ms = elapsed;
}

static void bright_screen_on(lv_timer_t *timer)
{
	(void)timer;
	chefui_screen_set(true);
}

static int child_brightness(void *arg)
{
	int mode = *(int *)arg;
	struct chefui_config cfg = { .app_name = "brightness-test" };
	lv_obj_t *slider;
	lv_anim_t animation;
	lv_timer_t *burst;
	int64_t start;
	use_paths();
	g_pan_delay_us = 16000;
	g_pan_count = g_bright_requests = g_bright_sync_pans = 0;
	g_bright_max_ms = 0;
	if (!chefui_init(&cfg)) return 99;
	slider = lv_slider_create(chefui_root());
	lv_slider_set_range(slider, 1, 255);
	lv_obj_add_event_cb(slider, brightness_event, LV_EVENT_VALUE_CHANGED, NULL);
	if (mode == 1) {
		lv_obj_t *object = lv_obj_create(chefui_root());
		lv_obj_set_size(object, 10, 10);
		lv_anim_init(&animation);
		lv_anim_set_var(&animation, object);
		lv_anim_set_exec_cb(&animation, pacing_anim);
		lv_anim_set_values(&animation, 0, 100);
		lv_anim_set_duration(&animation, 500);
		lv_anim_set_reverse_duration(&animation, 500);
		lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&animation);
	}
	if (mode == 2) {
		chefui_screen_set(false);
		lv_timer_t *on = lv_timer_create(bright_screen_on, 300, NULL);
		lv_timer_set_repeat_count(on, 1);
	}
	burst = lv_timer_create(brightness_burst, mode == 1 ? 16 : 100, slider);
	if (mode != 1) lv_timer_set_repeat_count(burst, 1);
	lv_timer_create(pacing_quit, mode == 1 ? 1500 : 500, NULL);
	start = cu_now_ms();
	chefui_run();
	report("pan=%u frames=%u elapsed=%lld requests=%u sync=%u max=%lld latest=%d", g_pan_count, cu_stats.frames,
		(long long)(cu_now_ms() - start), g_bright_requests, g_bright_sync_pans,
		(long long)g_bright_max_ms, g_pan_brightness);
	return 0;
}

static void test_brightness_bursts(void)
{
	for (int mode = 0; mode < 3; mode++) {
		int fifo = fixture(), l = listen_sock();
		struct child c = spawn(child_brightness, &mode, fifo);
		char out[128];
		unsigned pans, frames, requests, sync;
		long long elapsed, max;
		int latest;
		CHECK(finish(&c, out, sizeof(out)) == 0);
		CHECK(sscanf(out, "pan=%u frames=%u elapsed=%lld requests=%u sync=%u max=%lld latest=%d",
			&pans, &frames, &elapsed, &requests, &sync, &max, &latest) == 7);
		fprintf(stderr, "brightness mode%d: %s\n", mode, out);
		CHECK(sync == 0 && max < 50); /* no per-input blocking vsync */
		CHECK(latest == 217); /* latest level was written BEFORE a pan */
		if (mode == 1) {
			CHECK(1000.0 * frames / elapsed >= 48);
			CHECK(requests > 500 && pans < requests / 8);
		} else {
			CHECK(requests == 32);
			CHECK(pans >= 1 && pans <= 3); /* static/off -> one committed update */
		}
		close(l);
		close(fifo);
	}
}

/* -------------------------------------------------- scenario: handoff */

static void peer_quit(lv_timer_t *t)
{
	(void)t;
	report("R");			/* the lock is released after this */
	chefui_quit(0);
}

static int peer_main(void)
{
	lv_display_t *d = app_init(NULL);

	if (!d)
		return 99;
	report("P%s", getenv("CHEFUI_LOCK_FD") ? "env" : "");
	lv_timer_create(peer_quit, 300, NULL);
	chefui_run();
	return 0;
}

static void handoff_timer(lv_timer_t *t)
{
	static char rep[16];
	char *argv[] = { g_self, "--peer", D, rep, NULL };

	(void)t;
	snprintf(rep, sizeof(rep), "%d", g_report);
	chefui_handoff_exec(g_self, argv, false);
	report("handoff failed");
	chefui_quit(3);
}

static int child_handoff(void *arg)
{
	(void)arg;
	/* the report fd must survive exec */
	(void)fcntl(g_report, F_SETFD, 0);
	if (!app_init(NULL))
		return 99;
	report("L");
	lv_timer_create(handoff_timer, 200, NULL);
	return chefui_run();
}

static void test_handoff(void)
{
	int fifo = fixture(), l = listen_sock(), s1, s2, gaps = 0, status = 0;
	struct child c;
	char m, seen[32] = "", log[8192];
	size_t ns = 0;
	bool released = false;

	c = spawn(child_handoff, NULL, fifo);
	s1 = accept_timeout(l, 2000);
	CHECK(read(c.rep, &m, 1) == 1 && m == 'L');
	fcntl(c.rep, F_SETFL, O_NONBLOCK);
	for (;;) {
		if (read(c.rep, &m, 1) == 1) {
			if (ns < sizeof(seen) - 1)
				seen[ns++] = m;
			if (m == 'R')
				released = true;
		}
		if (released || waitpid(c.pid, &status, WNOHANG) == c.pid)
			break;
		if (lock_free()) {
			/* 'R' precedes the release: only free-before-'R' is a gap */
			if (read(c.rep, &m, 1) == 1) {
				if (ns < sizeof(seen) - 1)
					seen[ns++] = m;
				if (m == 'R') {
					released = true;
					break;
				}
			}
			gaps++;
		}
	}
	if (released)
		CHECK(waitpid(c.pid, &status, 0) == c.pid);
	CHECK(released);
	CHECK(strcmp(seen, "PR") == 0);		/* adopted, CHEFUI_LOCK_FD consumed */
	CHECK(gaps == 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	s2 = accept_timeout(l, 500);		/* the peer claimed on its own connection */
	CHECK(s2 >= 0);
	read_file(P_LOG, log, sizeof(log));
	/* first instance: on; handoff: POWERDOWN; peer: open + UNBLANK; exit: POWERDOWN */
	CHECK(count_lines(log, "UNBLANK") == 2 && count_lines(log, "POWERDOWN") == 2);
	CHECK(strcmp(last_line(log), "POWERDOWN 4 0") == 0);
	CHECK(lock_free());
	close(c.rep);
	if (s1 >= 0)
		close(s1);
	if (s2 >= 0)
		close(s2);
	close(l);
	close(fifo);
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/chefui-e2e-XXXXXX";
	ssize_t n = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);

	if (n <= 0)
		return 1;
	g_self[n] = '\0';
	if (argc == 4 && strcmp(argv[1], "--peer") == 0) {
		set_paths(argv[2]);
		g_report = atoi(argv[3]);
		return peer_main();
	}
	if (!mkdtemp(dir))
		return 1;
	set_paths(dir);
	signal(SIGPIPE, SIG_IGN);
	test_frame_taps_signal_exit();
	test_dark_start_toggle_restart();
	test_static_wakeups();
	test_screen_off_from_handlers();
	test_gestures();
	test_safe_api();
	test_failed_init_claim_release();
	test_blocking_pan_pacing();
	test_brightness_bursts();
	test_handoff();
	{
		char cmd[128];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		(void)!system(cmd);
	}
	return check_report("test-chefui");
}
