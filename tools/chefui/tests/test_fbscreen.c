/* Host tests for fbscreen.c with the fb ioctls wrapped (-Wl,--wrap=ioctl)
 * and temporary lock/flag/backlight/modes files: the ordering of
 * POWERDOWN, flag write and UNBLANK; start-dark (fb0 never opened until
 * the first screen-on, size from sysfs modes and validated); layout
 * rejection without FBIOPUT_VSCREENINFO; clean exit with the screen on
 * and off; brightness commits; and handoff lock inheritance across a real
 * exec with no unlocked gap (the test binary re-executes itself in
 * --adopt mode), dark handoff and exec failure. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "../fbscreen.h"

#define FW 64
#define FH 40
#define FSTRIDE (FW * 4 + 64)

struct rec {
	unsigned long req;
	long arg;
	bool flag;		/* flag present at the call */
	char bl[8];		/* backlight file content at the call */
};

static struct rec g_rec[128];
static int g_nrec;
static int g_bpp = 32;		/* what FBIOGET_VSCREENINFO reports */
static int g_fb_yres = FH;
static char g_dir[64], g_fb[96], g_lock[96], g_flag[96], g_bl[96], g_modes[96];

static void read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;

	buf[n > 0 ? n : 0] = '\0';
	if (fd >= 0)
		close(fd);
}

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	CHECK(fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s));
	close(fd);
}

int __real_ioctl(int fd, unsigned long req, ...);

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	switch (req) {
	case FBIOGET_VSCREENINFO: case FBIOGET_FSCREENINFO: case FBIOBLANK:
	case FBIOPAN_DISPLAY: case FBIOPUT_VSCREENINFO:
		break;
	default:
		return __real_ioctl(fd, req, arg);
	}
	if (g_nrec < 128) {
		struct rec *r = &g_rec[g_nrec++];

		r->req = req;
		r->arg = req == FBIOBLANK ? (long)arg :
			 req == FBIOPAN_DISPLAY ? (long)((struct fb_var_screeninfo *)arg)->yoffset : 0;
		r->flag = access(g_flag, F_OK) == 0;
		read_file(g_bl, r->bl, sizeof(r->bl));
	}
	if (req == FBIOGET_VSCREENINFO) {
		struct fb_var_screeninfo v;

		memset(&v, 0, sizeof(v));
		v.xres = FW;
		v.yres = (uint32_t)g_fb_yres;
		v.xres_virtual = FW;
		v.yres_virtual = 2 * FH;
		v.bits_per_pixel = (uint32_t)g_bpp;
		v.red.offset = 0; v.red.length = 8;
		v.green.offset = 8; v.green.length = 8;
		v.blue.offset = 16; v.blue.length = 8;
		v.transp.offset = 24; v.transp.length = 8;
		memcpy(arg, &v, sizeof(v));
	} else if (req == FBIOGET_FSCREENINFO) {
		struct fb_fix_screeninfo f;

		memset(&f, 0, sizeof(f));
		f.line_length = FSTRIDE;
		f.smem_len = FSTRIDE * 2 * FH;
		memcpy(arg, &f, sizeof(f));
	} else if (req == FBIOPUT_VSCREENINFO) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static int count(unsigned long req, long arg)
{
	int i, n = 0;

	for (i = 0; i < g_nrec; i++)
		if (g_rec[i].req == req && (arg < 0 || g_rec[i].arg == arg))
			n++;
	return n;
}

static int find(unsigned long req, long arg)
{
	int i;

	for (i = 0; i < g_nrec; i++)
		if (g_rec[i].req == req && (arg < 0 || g_rec[i].arg == arg))
			return i;
	return -1;
}

static bool fb_is_open(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	bool found = false;

	while (d && (e = readdir(d))) {
		char link[300], path[300];
		ssize_t n;

		snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
		n = readlink(link, path, sizeof(path) - 1);
		if (n > 0) {
			path[n] = '\0';
			if (strcmp(path, g_fb) == 0)
				found = true;
		}
	}
	if (d)
		closedir(d);
	return found;
}

/* Can someone else (fblog) take the lock right now? */
static bool lock_free(void)
{
	int fd = open(g_lock, O_RDWR | O_CLOEXEC);
	bool ok = fd >= 0 && flock(fd, LOCK_SH | LOCK_NB) == 0;

	if (fd >= 0)
		close(fd);
	return ok;
}

static void paths(struct cu_fbscreen *s)
{
	cu_fb_init(s, NULL);
	s->p.fb = g_fb;
	s->p.lock = g_lock;
	s->p.flag = g_flag;
	s->p.backlight = g_bl;
	s->p.modes = g_modes;
}

static void fixture(void)
{
	int fd;

	g_nrec = 0;
	g_bpp = 32;
	g_fb_yres = FH;
	unlink(g_flag);
	write_file(g_bl, "");
	write_file(g_modes, "U:64x40p-60\n");
	fd = open(g_fb, O_RDWR | O_CREAT, 0600);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)FSTRIDE * 2 * FH) == 0);
	close(fd);
}

static void test_parse_mode(void)
{
	uint32_t x = 0, y = 0;

	CHECK(cu_fb_parse_mode("U:1080x2246p-60\n", &x, &y) == 0 && x == 1080 && y == 2246);
	CHECK(cu_fb_parse_mode("720x1600", &x, &y) == 0 && x == 720 && y == 1600);
	CHECK(cu_fb_parse_mode("U:", &x, &y) == -EINVAL);
	CHECK(cu_fb_parse_mode("U:1080", &x, &y) == -EINVAL);
	CHECK(cu_fb_parse_mode("", &x, &y) == -EINVAL);
	CHECK(cu_fb_parse_mode("U:0x5p-60", &x, &y) == -EINVAL);
}

static void test_start_off_on_exit(void)
{
	struct cu_fbscreen s;
	bool dark = true;
	char bl[16];
	int i;

	fixture();
	paths(&s);
	CHECK(cu_fb_start(&s, 120, &dark) == 0);
	CHECK(!dark && s.on && !s.lock_inherited);
	CHECK(!lock_free());
	CHECK(s.xres == FW && s.yres == FH);
	CHECK(count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);
	read_file(g_bl, bl, sizeof(bl));
	CHECK(strcmp(bl, "120\n") == 0);
	CHECK(fcntl(s.fb.fd, F_GETFD) & FD_CLOEXEC);
	CHECK(fcntl(s.lockfd, F_GETFD) & FD_CLOEXEC);

	/* off: POWERDOWN first, then the flag */
	g_nrec = 0;
	CHECK(cu_fb_screen_off(&s) == 0);
	CHECK(g_nrec == 1 && g_rec[0].req == FBIOBLANK && g_rec[0].arg == FB_BLANK_POWERDOWN);
	CHECK(!g_rec[0].flag);
	CHECK(access(g_flag, F_OK) == 0);
	CHECK(!s.on && fb_is_open());		/* fd stays open while off */
	CHECK(cu_fb_screen_off(&s) == 0 && g_nrec == 1);	/* idempotent */
	CHECK(cu_fb_brightness_set(&s, 50) == 0 && g_nrec == 1);	/* no pan while off */

	/* on: flag removed, then UNBLANK, then the backlight */
	write_file(g_bl, "");
	g_nrec = 0;
	CHECK(cu_fb_screen_on(&s) == 0);
	i = find(FBIOBLANK, FB_BLANK_UNBLANK);
	CHECK(g_nrec == 1 && i == 0);
	CHECK(!g_rec[0].flag);			/* the flag was already gone */
	CHECK(g_rec[0].bl[0] == '\0');		/* backlight written after UNBLANK */
	read_file(g_bl, bl, sizeof(bl));
	CHECK(strcmp(bl, "50\n") == 0);
	CHECK(access(g_flag, F_OK) != 0 && s.on);

	/* brightness: write then commit (pan to the front page) */
	cu_fb_pan(&s, FH, 1);
	g_nrec = 0;
	CHECK(cu_fb_brightness_set(&s, 300) == 0);
	read_file(g_bl, bl, sizeof(bl));
	CHECK(strcmp(bl, "255\n") == 0);
	CHECK(g_nrec == 1 && g_rec[0].req == FBIOPAN_DISPLAY && g_rec[0].arg == FH);
	CHECK(cu_fb_brightness_set(&s, 0) == 0 && s.brightness == 1);

	/* clean exit with the screen on: POWERDOWN, close, unlock, no flag */
	g_nrec = 0;
	cu_fb_shutdown(&s);
	CHECK(g_nrec == 1 && g_rec[0].arg == FB_BLANK_POWERDOWN);
	CHECK(!fb_is_open() && lock_free());
	CHECK(access(g_flag, F_OK) != 0);
	CHECK(count(FBIOPUT_VSCREENINFO, -1) == 0);
}

static void test_exit_screen_off(void)
{
	struct cu_fbscreen s;
	bool dark;

	fixture();
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0 && !dark);
	CHECK(cu_fb_screen_off(&s) == 0);
	g_nrec = 0;
	cu_fb_shutdown(&s);
	CHECK(count(FBIOBLANK, FB_BLANK_POWERDOWN) == 1);	/* harmless again */
	CHECK(count(FBIOBLANK, FB_BLANK_UNBLANK) == 0);
	CHECK(!fb_is_open() && lock_free());
	CHECK(access(g_flag, F_OK) == 0);	/* left: fblog stays dark */
}

static void test_start_dark(void)
{
	struct cu_fbscreen s;
	bool dark = false;

	fixture();
	write_file(g_flag, "");
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0);
	CHECK(dark && !s.on);
	CHECK(g_nrec == 0 && !fb_is_open());	/* fb0 not opened: it would light the panel */
	CHECK(!lock_free());			/* but the lock is held */
	CHECK(s.xres == FW && s.yres == FH);	/* from sysfs modes */
	CHECK(cu_fb_screen_off(&s) == 0 && g_nrec == 0);

	CHECK(cu_fb_screen_on(&s) == 0);	/* the first open */
	CHECK(s.on && fb_is_open() && access(g_flag, F_OK) != 0);
	CHECK(find(FBIOGET_VSCREENINFO, -1) == 0);
	CHECK(count(FBIOBLANK, FB_BLANK_UNBLANK) == 1 && !g_rec[find(FBIOBLANK, FB_BLANK_UNBLANK)].flag);
	cu_fb_shutdown(&s);

	/* sysfs and the fb disagree: clear failure, still dark, flag back */
	fixture();
	write_file(g_flag, "");
	write_file(g_modes, "U:64x41p-60\n");
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0 && dark && s.yres == 41);
	g_nrec = 0;
	CHECK(cu_fb_screen_on(&s) == -EINVAL);
	CHECK(!s.on && !fb_is_open() && access(g_flag, F_OK) == 0);
	CHECK(count(FBIOBLANK, FB_BLANK_POWERDOWN) == 1);	/* before the close */
	cu_fb_shutdown(&s);

	/* no modes file: cannot start dark, nothing held */
	fixture();
	write_file(g_flag, "");
	unlink(g_modes);
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) < 0);
	CHECK(s.lockfd == -1 && lock_free() && !fb_is_open());
}

static void test_layout_rejected(void)
{
	struct cu_fbscreen s;
	bool dark;

	fixture();
	g_bpp = 16;
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == -EINVAL);
	CHECK(count(FBIOPUT_VSCREENINFO, -1) == 0);	/* never "fixed" by a format change */
	CHECK(count(FBIOBLANK, FB_BLANK_POWERDOWN) == 1);
	CHECK(!fb_is_open() && lock_free());

	fixture();
	g_fb_yres = FH + 1;			/* yres_virtual < 2 * yres: no second page */
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == -EINVAL && lock_free());
}

static void test_lock_busy(void)
{
	struct cu_fbscreen s;
	bool dark;
	int other;

	fixture();
	other = open(g_lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	CHECK(flock(other, LOCK_EX) == 0);
	paths(&s);
	/* (shortened wait: fb_lock_exclusive polls; use the env-less path) */
	{
		int fd = fb_lock_open(g_lock);

		CHECK(fb_lock_exclusive(fd, 40) == -ETIMEDOUT);
		close(fd);
	}
	close(other);
	CHECK(cu_fb_start(&s, 96, &dark) == 0);
	cu_fb_shutdown(&s);
	/* a stale/foreign CHEFUI_LOCK_FD is ignored */
	setenv(CU_FB_LOCK_ENV, "0", 1);
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0 && !s.lock_inherited);
	CHECK(getenv(CU_FB_LOCK_ENV) == NULL);
	cu_fb_shutdown(&s);
}

/* --- handoff across exec --- */

static char g_self[300];

/* The exec'd side: adopt, report on fd `rep`, hold for a while, exit. */
static int adopt_main(int rep, int hold_ms)
{
	struct cu_fbscreen s;
	bool dark = false;
	char msg[4] = "x";
	struct timespec ts = { hold_ms / 1000, (long)(hold_ms % 1000) * 1000000L };

	paths(&s);
	if (cu_fb_start(&s, 96, &dark) == 0 && s.lock_inherited && !getenv(CU_FB_LOCK_ENV) &&
	    (fcntl(s.lockfd, F_GETFD) & FD_CLOEXEC))
		msg[0] = dark ? 'D' : 'A';
	(void)!write(rep, msg, 1);
	nanosleep(&ts, NULL);
	(void)!write(rep, "R", 1);		/* about to release: gaps after this are fine */
	cu_fb_shutdown(&s);
	return 0;
}

static void handoff(bool dark)
{
	int p[2], gaps = 0, status = 0;
	pid_t child;
	char c = 0, adopted = 0;

	fixture();
	CHECK(pipe(p) == 0);			/* no CLOEXEC: the exec'd side reports on it */
	child = fork();
	if (child == 0) {
		struct cu_fbscreen s;
		bool d;
		char fdarg[16], *argv[5];

		close(p[0]);
		paths(&s);
		if (cu_fb_start(&s, 96, &d) != 0)
			_exit(2);
		(void)!write(p[1], "L", 1);
		snprintf(fdarg, sizeof(fdarg), "%d", p[1]);
		argv[0] = g_self;
		argv[1] = "--adopt";
		argv[2] = fdarg;
		argv[3] = g_dir;
		argv[4] = NULL;
		cu_fb_handoff_exec(&s, g_self, argv, dark);
		_exit(3);
	}
	close(p[1]);
	CHECK(read(p[0], &c, 1) == 1 && c == 'L');
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	/* hammer the lock like fblog would, from the first owner's start
	 * until the adopter announces its release */
	for (;;) {
		char m;

		if (read(p[0], &m, 1) == 1) {
			if (m == 'R')
				break;
			adopted = m;
		}
		if (waitpid(child, &status, WNOHANG) == child)
			break;		/* died early: the checks below fail */
		if (lock_free()) {
			/* 'R' is written before the release: free with no 'R'
			 * in the pipe yet is a real gap, free after it is not */
			if (read(p[0], &m, 1) == 1) {
				if (m == 'R')
					break;
				adopted = m;
			}
			gaps++;
		}
	}
	CHECK(adopted == (dark ? 'D' : 'A'));
	CHECK(waitpid(child, &status, 0) == child || errno == ECHILD);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(gaps == 0);			/* no unlocked moment across the exec */
	CHECK(lock_free());			/* released when the adopter exited */
	CHECK((access(g_flag, F_OK) == 0) == dark);
	close(p[0]);
}

static void test_handoff(void)
{
	handoff(false);
	handoff(true);
}

static void test_handoff_exec_fails(void)
{
	struct cu_fbscreen s;
	bool dark;
	char *argv[] = { "nope", NULL };

	fixture();
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0);
	g_nrec = 0;
	CHECK(cu_fb_handoff_exec(&s, "/nonexistent/chefui-peer", argv, false) == -ENOENT);
	CHECK(getenv(CU_FB_LOCK_ENV) == NULL);
	CHECK(fcntl(s.lockfd, F_GETFD) & FD_CLOEXEC);
	CHECK(!lock_free());
	/* POWERDOWN + close happened, then the screen came back */
	CHECK(find(FBIOBLANK, FB_BLANK_POWERDOWN) >= 0 &&
	      find(FBIOBLANK, FB_BLANK_UNBLANK) > find(FBIOBLANK, FB_BLANK_POWERDOWN));
	CHECK(s.on && fb_is_open() && access(g_flag, F_OK) != 0);
	cu_fb_shutdown(&s);

	/* failing from an off screen: stays off, flag present */
	fixture();
	paths(&s);
	CHECK(cu_fb_start(&s, 96, &dark) == 0 && cu_fb_screen_off(&s) == 0);
	CHECK(cu_fb_handoff_exec(&s, "/nonexistent/chefui-peer", argv, false) == -ENOENT);
	CHECK(!s.on && access(g_flag, F_OK) == 0);
	cu_fb_shutdown(&s);
}

static void set_paths(const char *dir)
{
	snprintf(g_dir, sizeof(g_dir), "%s", dir);
	snprintf(g_fb, sizeof(g_fb), "%s/fb0", g_dir);
	snprintf(g_lock, sizeof(g_lock), "%s/fb0.lock", g_dir);
	snprintf(g_flag, sizeof(g_flag), "%s/fblog.off", g_dir);
	snprintf(g_bl, sizeof(g_bl), "%s/brightness", g_dir);
	snprintf(g_modes, sizeof(g_modes), "%s/modes", g_dir);
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/chefui-fb-XXXXXX";
	ssize_t n;

	if (argc == 4 && strcmp(argv[1], "--adopt") == 0) {
		set_paths(argv[3]);
		return adopt_main(atoi(argv[2]), 150);
	}
	n = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);
	if (n <= 0 || !mkdtemp(dir))
		return 1;
	g_self[n] = '\0';
	set_paths(dir);
	test_parse_mode();
	test_start_off_on_exit();
	test_exit_screen_off();
	test_start_dark();
	test_layout_rejected();
	test_lock_busy();
	test_handoff();
	test_handoff_exec_fails();
	unlink(g_fb);
	unlink(g_lock);
	unlink(g_flag);
	unlink(g_bl);
	unlink(g_modes);
	rmdir(dir);
	return check_report("test-fbscreen");
}
