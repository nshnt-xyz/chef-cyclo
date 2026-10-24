/*
 * drmtest - DRM/KMS probe for the SDE experiment on chef-cyclo.
 *
 * A static client that talks to /dev/dri/card0 with raw ioctls (no libdrm):
 *
 *   drmtest info              resources, connectors with their modes,
 *                             encoders, CRTCs and planes
 *   drmtest show [-t SECS] [-c CYCLES]
 *                             draw a test pattern into one dumb buffer and
 *                             SETCRTC it on the first connected connector
 *   drmtest flip [-n FRAMES]  page flip two dumb buffers with a moving bar
 *                             and report the flip event timing
 *
 * show picks the preferred mode (else the first), the connector's encoder's
 * CRTC (else the first CRTC in the encoder's possible_crtcs), holds the
 * pattern for -t seconds (default 20), then -c times (default 0) disables
 * the CRTC, waits 3 s and sets the same mode again. Every SETCRTC is
 * bracketed by CLOCK_MONOTONIC timestamps so the run can be lined up with
 * dmesg. The mode stays set until the process exits (SIGINT/SIGTERM exit
 * cleanly); closing the fd lets the kernel drop the framebuffer.
 *
 * The pattern, in panel pixels: a 24 px white border; eight vertical colour
 * bars (white, yellow, cyan, green, magenta, red, blue, black) in the upper
 * half; a black-to-white gradient in the lower half; 120 px squares in the
 * corners inside the border (red top left, green top right, blue bottom
 * left, white bottom right).
 *
 * flip uses the same target as show. Each of FRAMES frames (default 600)
 * draws a black frame with the white border and a 120 px white bar that
 * moves 8 px per frame (wrapping) into the back buffer, queues a page flip
 * with an event and waits up to 100 ms for the event, so a panel refreshing
 * at 60 Hz shows smooth motion if the flips complete on every vblank. The
 * summary has the event counts, the interval between consecutive event
 * timestamps and the draw time per frame.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define CARD		"/dev/dri/card0"
#define BORDER		24
#define CORNER		120
#define OFF_SECS	3
#define BAR_WIDTH	120
#define BAR_STEP	8
#define FLIP_WAIT_MS	100
#define LATE_US		25000
#define CONNECTED	1	/* drm_connector_status, libdrm only */

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int ioc(int fd, unsigned long req, void *arg, const char *name)
{
	if (ioctl(fd, req, arg) < 0) {
		fprintf(stderr, "drmtest: %s: %s\n", name, strerror(errno));
		return -1;
	}
	return 0;
}

static uint64_t ptr(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

static void stamp(const char *what)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("[%ld.%03ld] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, what);
}

/* Sleep up to SECS, returning early when a signal asked us to stop. */
static void nap(int secs)
{
	struct timespec ts = { .tv_sec = secs };

	while (!stop && nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ---- info ---- */

static int print_connector(int fd, uint32_t id)
{
	struct drm_mode_get_connector c = { .connector_id = id };
	struct drm_mode_modeinfo *modes;
	uint32_t i;

	if (ioc(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR"))
		return -1;
	modes = calloc(c.count_modes ? c.count_modes : 1, sizeof(*modes));
	if (!modes)
		return -1;
	c.modes_ptr = ptr(modes);
	c.count_props = 0;
	c.count_encoders = 0;
	if (ioc(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR")) {
		free(modes);
		return -1;
	}
	printf("connector %u: type %u-%u connection %u encoder %u modes %u\n",
	       c.connector_id, c.connector_type, c.connector_type_id,
	       c.connection, c.encoder_id, c.count_modes);
	for (i = 0; i < c.count_modes; i++) {
		struct drm_mode_modeinfo *m = &modes[i];

		printf("  mode %s clock %u h %u %u %u %u v %u %u %u %u vrefresh %u flags 0x%x preferred %d\n",
		       m->name, m->clock, m->hdisplay, m->hsync_start,
		       m->hsync_end, m->htotal, m->vdisplay, m->vsync_start,
		       m->vsync_end, m->vtotal, m->vrefresh, m->flags,
		       !!(m->type & DRM_MODE_TYPE_PREFERRED));
	}
	free(modes);
	return 0;
}

static int print_encoder(int fd, uint32_t id)
{
	struct drm_mode_get_encoder e = { .encoder_id = id };

	if (ioc(fd, DRM_IOCTL_MODE_GETENCODER, &e, "GETENCODER"))
		return -1;
	printf("encoder %u: crtc %u possible_crtcs 0x%x\n", e.encoder_id,
	       e.crtc_id, e.possible_crtcs);
	return 0;
}

static int print_crtc(int fd, uint32_t id)
{
	struct drm_mode_crtc c = { .crtc_id = id };

	if (ioc(fd, DRM_IOCTL_MODE_GETCRTC, &c, "GETCRTC"))
		return -1;
	printf("crtc %u: fb %u mode_valid %u mode %s\n", c.crtc_id, c.fb_id,
	       c.mode_valid, c.mode_valid ? c.mode.name : "-");
	return 0;
}

static int print_plane(int fd, uint32_t id)
{
	struct drm_mode_get_plane p = { .plane_id = id };

	if (ioc(fd, DRM_IOCTL_MODE_GETPLANE, &p, "GETPLANE"))
		return -1;
	printf("plane %u: possible_crtcs 0x%x formats %u crtc %u fb %u\n",
	       p.plane_id, p.possible_crtcs, p.count_format_types, p.crtc_id,
	       p.fb_id);
	return 0;
}

static int cmd_info(int fd)
{
	struct drm_mode_card_res r = { 0 };
	struct drm_mode_get_plane_res pr = { 0 };
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	uint32_t *fbs, *crtcs, *conns, *encs, *planes;
	uint32_t i;
	int rc = 0;

	if (ioc(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap, "SET_CLIENT_CAP"))
		return 1;
	if (ioc(fd, DRM_IOCTL_MODE_GETRESOURCES, &r, "GETRESOURCES"))
		return 1;
	fbs = calloc(r.count_fbs + 1, sizeof(*fbs));
	crtcs = calloc(r.count_crtcs + 1, sizeof(*crtcs));
	conns = calloc(r.count_connectors + 1, sizeof(*conns));
	encs = calloc(r.count_encoders + 1, sizeof(*encs));
	if (!fbs || !crtcs || !conns || !encs)
		return 1;
	r.fb_id_ptr = ptr(fbs);
	r.crtc_id_ptr = ptr(crtcs);
	r.connector_id_ptr = ptr(conns);
	r.encoder_id_ptr = ptr(encs);
	if (ioc(fd, DRM_IOCTL_MODE_GETRESOURCES, &r, "GETRESOURCES"))
		return 1;
	printf("resources: fbs %u crtcs %u connectors %u encoders %u size %ux%u..%ux%u\n",
	       r.count_fbs, r.count_crtcs, r.count_connectors, r.count_encoders,
	       r.min_width, r.min_height, r.max_width, r.max_height);
	for (i = 0; i < r.count_fbs; i++)
		printf("fb %u\n", fbs[i]);
	for (i = 0; i < r.count_connectors; i++)
		rc |= print_connector(fd, conns[i]);
	for (i = 0; i < r.count_encoders; i++)
		rc |= print_encoder(fd, encs[i]);
	for (i = 0; i < r.count_crtcs; i++)
		rc |= print_crtc(fd, crtcs[i]);

	if (ioc(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES"))
		return 1;
	planes = calloc(pr.count_planes + 1, sizeof(*planes));
	if (!planes)
		return 1;
	pr.plane_id_ptr = ptr(planes);
	if (ioc(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES"))
		return 1;
	printf("planes: %u\n", pr.count_planes);
	for (i = 0; i < pr.count_planes; i++)
		rc |= print_plane(fd, planes[i]);
	return rc ? 1 : 0;
}

/* ---- show ---- */

static uint32_t bar_colour(int bar)
{
	static const uint32_t colours[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000,
	};

	return colours[bar];
}

static uint32_t pattern_pixel(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t iw = w - 2 * BORDER, g;

	if (x < BORDER || y < BORDER || x >= w - BORDER || y >= h - BORDER)
		return 0xffffff;
	if (x < BORDER + CORNER && y < BORDER + CORNER)
		return 0xff0000;
	if (x >= w - BORDER - CORNER && y < BORDER + CORNER)
		return 0x00ff00;
	if (x < BORDER + CORNER && y >= h - BORDER - CORNER)
		return 0x0000ff;
	if (x >= w - BORDER - CORNER && y >= h - BORDER - CORNER)
		return 0xffffff;
	if (y < h / 2)
		return bar_colour((x - BORDER) * 8 / iw);
	g = (x - BORDER) * 255 / (iw - 1);
	return g << 16 | g << 8 | g;
}

static void draw_pattern(uint32_t *map, uint32_t pitch, uint32_t w, uint32_t h)
{
	uint32_t x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			map[y * (pitch / 4) + x] = pattern_pixel(x, y, w, h);
}

struct target {
	uint32_t conn, crtc, fb;
	struct drm_mode_modeinfo mode;
};

/* First connected connector, its preferred (else first) mode and its CRTC. */
static int pick_target(int fd, struct target *t)
{
	struct drm_mode_card_res r = { 0 };
	uint32_t *crtcs, *conns, i, j;
	struct drm_mode_get_connector c;
	struct drm_mode_modeinfo *modes;
	struct drm_mode_get_encoder e;

	if (ioc(fd, DRM_IOCTL_MODE_GETRESOURCES, &r, "GETRESOURCES"))
		return -1;
	crtcs = calloc(r.count_crtcs + 1, sizeof(*crtcs));
	conns = calloc(r.count_connectors + 1, sizeof(*conns));
	if (!crtcs || !conns)
		return -1;
	r.count_fbs = 0;
	r.count_encoders = 0;
	r.crtc_id_ptr = ptr(crtcs);
	r.connector_id_ptr = ptr(conns);
	if (ioc(fd, DRM_IOCTL_MODE_GETRESOURCES, &r, "GETRESOURCES"))
		return -1;

	for (i = 0; i < r.count_connectors; i++) {
		memset(&c, 0, sizeof(c));
		c.connector_id = conns[i];
		if (ioc(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR"))
			return -1;
		if (c.connection != CONNECTED || !c.count_modes)
			continue;
		modes = calloc(c.count_modes, sizeof(*modes));
		if (!modes)
			return -1;
		c.modes_ptr = ptr(modes);
		c.count_props = 0;
		c.count_encoders = 0;
		if (ioc(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR"))
			return -1;
		t->mode = modes[0];
		for (j = 0; j < c.count_modes; j++)
			if (modes[j].type & DRM_MODE_TYPE_PREFERRED) {
				t->mode = modes[j];
				break;
			}
		free(modes);
		t->conn = c.connector_id;
		t->crtc = 0;
		memset(&e, 0, sizeof(e));
		e.encoder_id = c.encoder_id;
		if (c.encoder_id &&
		    ioc(fd, DRM_IOCTL_MODE_GETENCODER, &e, "GETENCODER"))
			return -1;
		if (e.crtc_id) {
			t->crtc = e.crtc_id;
		} else {
			for (j = 0; j < r.count_crtcs; j++)
				if (e.possible_crtcs & 1u << j) {
					t->crtc = crtcs[j];
					break;
				}
		}
		if (!t->crtc && r.count_crtcs)
			t->crtc = crtcs[0];
		if (!t->crtc) {
			fprintf(stderr, "drmtest: no CRTC for connector %u\n",
				t->conn);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "drmtest: no connected connector with modes\n");
	return -1;
}

struct buf {
	uint32_t fb, pitch;
	uint32_t *map;
};

/* A dumb buffer of the mode size, mapped and registered as XRGB8888. */
static int new_buf(int fd, const struct target *t, struct buf *b)
{
	struct drm_mode_create_dumb cd = {
		.width = t->mode.hdisplay,
		.height = t->mode.vdisplay,
		.bpp = 32,
	};
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_fb_cmd2 fb = { 0 };
	void *map;

	if (ioc(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB"))
		return -1;
	md.handle = cd.handle;
	if (ioc(fd, DRM_IOCTL_MODE_MAP_DUMB, &md, "MAP_DUMB"))
		return -1;
	map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   md.offset);
	if (map == MAP_FAILED) {
		fprintf(stderr, "drmtest: mmap: %s\n", strerror(errno));
		return -1;
	}
	fb.width = cd.width;
	fb.height = cd.height;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = cd.handle;
	fb.pitches[0] = cd.pitch;
	if (ioc(fd, DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2"))
		return -1;
	b->fb = fb.fb_id;
	b->pitch = cd.pitch;
	b->map = map;
	return 0;
}

static int make_fb(int fd, struct target *t)
{
	struct buf b;

	if (new_buf(fd, t, &b))
		return -1;
	draw_pattern(b.map, b.pitch, t->mode.hdisplay, t->mode.vdisplay);
	t->fb = b.fb;
	return 0;
}

/* FB 0 with no mode and no connectors turns the CRTC off. */
static int set_crtc(int fd, const struct target *t, bool on)
{
	struct drm_mode_crtc c = { .crtc_id = t->crtc };
	uint32_t conn = t->conn;
	int rc;

	if (on) {
		c.set_connectors_ptr = ptr(&conn);
		c.count_connectors = 1;
		c.fb_id = t->fb;
		c.mode_valid = 1;
		c.mode = t->mode;
	}
	stamp(on ? "SETCRTC on begin" : "SETCRTC off begin");
	rc = ioc(fd, DRM_IOCTL_MODE_SETCRTC, &c, "SETCRTC");
	stamp(on ? "SETCRTC on end" : "SETCRTC off end");
	printf("SETCRTC %s rc=%d\n", on ? "on" : "off", rc);
	return rc;
}

static int cmd_show(int fd, int secs, int cycles)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct target t = { 0 };
	int i;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	if (pick_target(fd, &t))
		return 1;
	printf("target: connector %u crtc %u mode %s %ux%u@%u\n", t.conn,
	       t.crtc, t.mode.name, t.mode.hdisplay, t.mode.vdisplay,
	       t.mode.vrefresh);
	if (make_fb(fd, &t))
		return 1;
	if (set_crtc(fd, &t, true))
		return 1;
	nap(secs);
	for (i = 0; i < cycles && !stop; i++) {
		if (set_crtc(fd, &t, false))
			return 1;
		nap(OFF_SECS);
		if (stop)
			break;
		if (set_crtc(fd, &t, true))
			return 1;
		nap(secs);
	}
	return 0;
}

/* ---- flip ---- */

struct flip_stats {
	unsigned long events, timeouts, ebusy, late, intervals;
	int64_t min_us, max_us, sum_us, last_us;
	int64_t draw_max_us, draw_sum_us;
	uint32_t shown;
};

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Black frame, white border, and the white bar at BAR_STEP px per frame. */
static void draw_frame(const struct buf *b, uint32_t w, uint32_t h,
		       unsigned long frame)
{
	uint32_t x0 = frame * BAR_STEP % w, x, y;

	for (y = 0; y < h; y++) {
		uint32_t *row = b->map + y * (b->pitch / 4);

		if (y < BORDER || y >= h - BORDER) {
			for (x = 0; x < w; x++)
				row[x] = 0xffffff;
			continue;
		}
		memset(row, 0, w * 4);
		for (x = 0; x < BORDER; x++)
			row[x] = row[w - 1 - x] = 0xffffff;
		for (x = 0; x < BAR_WIDTH; x++)
			row[(x0 + x) % w] = 0xffffff;
	}
}

static void note_event(struct flip_stats *fs, const struct drm_event_vblank *ev)
{
	int64_t us = (int64_t)ev->tv_sec * 1000000 + ev->tv_usec, d;

	fs->events++;
	fs->shown = ev->user_data;
	if (fs->events > 1) {
		d = us - fs->last_us;
		if (!fs->intervals || d < fs->min_us)
			fs->min_us = d;
		if (!fs->intervals || d > fs->max_us)
			fs->max_us = d;
		fs->sum_us += d;
		fs->intervals++;
		if (d > LATE_US)
			fs->late++;
	}
	fs->last_us = us;
}

/* Wait for a flip event; 1 if at least one was read, 0 on timeout. */
static int wait_event(int fd, struct flip_stats *fs)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	uint64_t storage[128];
	char *buf = (char *)storage;
	ssize_t n, off;
	int got = 0;

	if (poll(&pfd, 1, FLIP_WAIT_MS) <= 0)
		return 0;
	n = read(fd, buf, sizeof(storage));
	for (off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(buf + off);

		if (e->length < sizeof(*e) || off + (ssize_t)e->length > n)
			break;
		if (e->type == DRM_EVENT_FLIP_COMPLETE &&
		    e->length >= sizeof(struct drm_event_vblank)) {
			note_event(fs, (struct drm_event_vblank *)e);
			got = 1;
		}
		off += e->length;
	}
	return got;
}

/* Queue a flip to buffer INDEX; 1 queued, 0 skipped after EBUSY, -1 error. */
static int queue_flip(int fd, const struct target *t, const struct buf *bufs,
		      uint32_t index, struct flip_stats *fs)
{
	struct drm_mode_crtc_page_flip pf = {
		.crtc_id = t->crtc,
		.fb_id = bufs[index].fb,
		.flags = DRM_MODE_PAGE_FLIP_EVENT,
		.user_data = index,
	};
	bool retried = false;

	for (;;) {
		if (!ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf))
			return 1;
		if (errno != EBUSY) {
			fprintf(stderr, "drmtest: PAGE_FLIP: %s\n",
				strerror(errno));
			return -1;
		}
		fs->ebusy++;
		if (retried)
			return 0;
		if (!wait_event(fd, fs)) {
			if (!stop)
				fs->timeouts++;
			return 0;
		}
		retried = true;
	}
}

static void print_flip_summary(const struct flip_stats *fs, unsigned long want,
			       unsigned long issued, int64_t wall_us)
{
	printf("flip: frames requested %lu issued %lu events %lu timeouts %lu ebusy %lu\n",
	       want, issued, fs->events, fs->timeouts, fs->ebusy);
	if (fs->intervals)
		printf("flip: event interval us min %lld avg %lld max %lld, over %d ms: %lu of %lu\n",
		       (long long)fs->min_us,
		       (long long)(fs->sum_us / (int64_t)fs->intervals),
		       (long long)fs->max_us, LATE_US / 1000, fs->late,
		       fs->intervals);
	else
		printf("flip: fewer than two events, no intervals\n");
	if (issued)
		printf("flip: draw us avg %lld max %lld\n",
		       (long long)(fs->draw_sum_us / (int64_t)issued),
		       (long long)fs->draw_max_us);
	printf("flip: wall time %lld ms\n", (long long)(wall_us / 1000));
}

static int cmd_flip(int fd, unsigned long frames)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct flip_stats fs = { 0 };
	struct target t = { 0 };
	struct buf bufs[2];
	unsigned long i, issued = 0;
	int64_t start, d;
	int rc = 0, q;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	if (pick_target(fd, &t))
		return 1;
	printf("target: connector %u crtc %u mode %s %ux%u@%u\n", t.conn,
	       t.crtc, t.mode.name, t.mode.hdisplay, t.mode.vdisplay,
	       t.mode.vrefresh);
	if (new_buf(fd, &t, &bufs[0]) || new_buf(fd, &t, &bufs[1]))
		return 1;
	draw_frame(&bufs[0], t.mode.hdisplay, t.mode.vdisplay, 0);
	t.fb = bufs[0].fb;
	if (set_crtc(fd, &t, true))
		return 1;

	start = now_us();
	for (i = 0; i < frames && !stop; i++) {
		uint32_t back = 1 - fs.shown;

		d = now_us();
		draw_frame(&bufs[back], t.mode.hdisplay, t.mode.vdisplay, i + 1);
		d = now_us() - d;
		fs.draw_sum_us += d;
		if (d > fs.draw_max_us)
			fs.draw_max_us = d;
		q = queue_flip(fd, &t, bufs, back, &fs);
		if (q < 0) {
			rc = 1;
			break;
		}
		issued++;
		if (q && !wait_event(fd, &fs) && !stop)
			fs.timeouts++;
	}
	print_flip_summary(&fs, frames, issued, now_us() - start);
	return rc;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: drmtest info\n"
		"       drmtest show [-t secs] [-c cycles]\n"
		"       drmtest flip [-n frames]\n");
}

int main(int argc, char **argv)
{
	int secs = 20, cycles = 0, frames = 600, opt, fd, rc;
	const char *cmd;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		usage();
		return 64;
	}
	cmd = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "t:c:n:")) != -1) {
		switch (opt) {
		case 't': secs = atoi(optarg); break;
		case 'c': cycles = atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		default:
			usage();
			return 64;
		}
	}
	if (optind != argc || frames < 1) {
		usage();
		return 64;
	}
	fd = open(CARD, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "drmtest: %s: %s\n", CARD, strerror(errno));
		return 1;
	}
	if (!strcmp(cmd, "info"))
		rc = cmd_info(fd);
	else if (!strcmp(cmd, "show"))
		rc = cmd_show(fd, secs, cycles);
	else if (!strcmp(cmd, "flip"))
		rc = cmd_flip(fd, frames);
	else {
		usage();
		rc = 64;
	}
	close(fd);
	return rc;
}
