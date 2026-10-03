/* Host tests for copy.c: swizzle and alpha, stride handling, clipping and
 * pixel placement for every rotation. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "../copy.h"

#define SENTINEL 0xdeadbeefu

/* Shadow pixel (XRGB8888 word) that encodes its own logical position:
 * R = ly, G = lx, B = 0x5a, X = junk (must not reach the fb). */
static uint32_t enc(int32_t lx, int32_t ly)
{
	return 0x7f000000u | ((uint32_t)ly << 16) | ((uint32_t)lx << 8) | 0x5au;
}

/* What the copy must write for that shadow pixel: R and B swapped,
 * alpha forced -> 0xff, B=0x5a, G=lx, R=ly in RGBA byte order. */
static uint32_t want(int32_t lx, int32_t ly)
{
	return 0xff000000u | (0x5au << 16) | ((uint32_t)lx << 8) | (uint32_t)ly;
}

struct bufs {
	struct cu_geom g;
	uint8_t *src, *dst;
	size_t src_stride, dst_stride;
};

static void bufs_init(struct bufs *b, int rot, int32_t pw, int32_t ph)
{
	int32_t x, y;

	CHECK(cu_geom_init(&b->g, rot, pw, ph) == 0);
	b->src_stride = (size_t)b->g.lw * 4 + 8;	/* padded rows */
	b->dst_stride = (size_t)pw * 4 + 12;		/* like fb0's 4352 for 1080 px */
	b->src = malloc(b->src_stride * (size_t)b->g.lh);
	b->dst = malloc(b->dst_stride * (size_t)ph);
	for (y = 0; y < b->g.lh; y++)
		for (x = 0; x < b->g.lw; x++)
			((uint32_t *)(b->src + (size_t)y * b->src_stride))[x] = enc(x, y);
	for (size_t i = 0; i < b->dst_stride * (size_t)ph / 4; i++)
		((uint32_t *)b->dst)[i] = SENTINEL;
}

static uint32_t dpx(const struct bufs *b, int32_t px, int32_t py)
{
	return ((const uint32_t *)(b->dst + (size_t)py * b->dst_stride))[px];
}

static void bufs_free(struct bufs *b)
{
	free(b->src);
	free(b->dst);
}

static void test_swizzle(void)
{
	CHECK(cu_swizzle(0x00112233u) == 0xff332211u);
	CHECK(cu_swizzle(0x7f112233u) == 0xff332211u);	/* X byte never leaks */
	CHECK(cu_swizzle(0xffff0000u) == 0xff0000ffu);	/* red -> byte 0 (R) */
	CHECK(cu_swizzle(0x0000ff00u) == 0xff00ff00u);	/* green stays */
	CHECK(cu_swizzle(0x000000ffu) == 0xffff0000u);	/* blue -> byte 2 (B) */
}

static void test_geom(void)
{
	struct cu_geom g;
	int rots[] = { 0, 90, 180, 270 }, i;
	int32_t x, y, px, py, lx, ly;

	CHECK(cu_geom_init(&g, 45, 1080, 2246) == -1);
	CHECK(cu_geom_init(&g, 0, 1080, 2246) == 0 && g.lw == 1080 && g.lh == 2246);
	CHECK(cu_geom_init(&g, 90, 1080, 2246) == 0 && g.lw == 2246 && g.lh == 1080);
	CHECK(cu_geom_init(&g, 180, 1080, 2246) == 0 && g.lw == 1080 && g.lh == 2246);
	CHECK(cu_geom_init(&g, 270, 1080, 2246) == 0 && g.lw == 2246 && g.lh == 1080);

	/* 90: the logical top edge is the physical right edge */
	cu_geom_init(&g, 90, 1080, 2246);
	cu_log_to_phys(&g, 0, 0, &px, &py);
	CHECK(px == 1079 && py == 0);
	cu_log_to_phys(&g, 2245, 1079, &px, &py);
	CHECK(px == 0 && py == 2245);
	cu_geom_init(&g, 270, 1080, 2246);
	cu_log_to_phys(&g, 0, 0, &px, &py);
	CHECK(px == 0 && py == 2245);
	cu_geom_init(&g, 180, 1080, 2246);
	cu_log_to_phys(&g, 0, 0, &px, &py);
	CHECK(px == 1079 && py == 2245);

	/* phys_to_log is the exact inverse, every point, every rotation */
	for (i = 0; i < 4; i++) {
		int ok = 1;

		cu_geom_init(&g, rots[i], 7, 5);
		for (y = 0; y < g.lh; y++)
			for (x = 0; x < g.lw; x++) {
				cu_log_to_phys(&g, x, y, &px, &py);
				cu_phys_to_log(&g, px, py, &lx, &ly);
				if (px < 0 || px >= 7 || py < 0 || py >= 5 || lx != x || ly != y)
					ok = 0;
			}
		CHECK(ok);
	}
}

/* Full-screen copy: every physical pixel holds the logical pixel that
 * maps to it, and the destination stride padding is untouched. */
static void test_full_copy_all_rotations(void)
{
	int rots[] = { 0, 90, 180, 270 }, i;

	for (i = 0; i < 4; i++) {
		struct bufs b;
		struct cu_rect all;
		int32_t px, py, lx, ly;
		int ok = 1, pad_ok = 1;

		bufs_init(&b, rots[i], 7, 5);
		all = (struct cu_rect){ 0, 0, b.g.lw - 1, b.g.lh - 1 };
		CHECK(cu_copy_area(&b.g, b.dst, b.dst_stride, b.src, b.src_stride, &all) == 35);
		for (py = 0; py < 5; py++) {
			for (px = 0; px < 7; px++) {
				cu_phys_to_log(&b.g, px, py, &lx, &ly);
				if (dpx(&b, px, py) != want(lx, ly))
					ok = 0;
			}
			for (px = 7; px < 10; px++)	/* 12 bytes of padding */
				if (dpx(&b, px, py) != SENTINEL)
					pad_ok = 0;
		}
		CHECK(ok);
		CHECK(pad_ok);
		bufs_free(&b);
	}
}

/* A partial logical area lands exactly on its transformed rectangle. */
static void test_partial_copy_all_rotations(void)
{
	int rots[] = { 0, 90, 180, 270 }, i;

	for (i = 0; i < 4; i++) {
		struct bufs b;
		struct cu_rect a = { 1, 1, 3, 2 }, p;
		int32_t px, py, lx, ly;
		int ok = 1;

		bufs_init(&b, rots[i], 7, 5);
		CHECK(cu_copy_area(&b.g, b.dst, b.dst_stride, b.src, b.src_stride, &a) == 6);
		p = cu_rect_to_phys(&b.g, &a);
		CHECK((p.x2 - p.x1 + 1) * (p.y2 - p.y1 + 1) == 6);
		for (py = 0; py < 5; py++)
			for (px = 0; px < 7; px++) {
				bool inside = px >= p.x1 && px <= p.x2 && py >= p.y1 && py <= p.y2;

				cu_phys_to_log(&b.g, px, py, &lx, &ly);
				if (inside != (lx >= a.x1 && lx <= a.x2 && ly >= a.y1 && ly <= a.y2))
					ok = 0;
				if (dpx(&b, px, py) != (inside ? want(lx, ly) : SENTINEL))
					ok = 0;
			}
		CHECK(ok);
		bufs_free(&b);
	}
}

static void test_clipping(void)
{
	struct bufs b;
	struct cu_rect over = { -5, -5, 100, 1 }, none = { 10, 10, 20, 20 }, inv = { 3, 3, 2, 2 };

	bufs_init(&b, 90, 7, 5);	/* logical 5 x 7 */
	CHECK(cu_copy_area(&b.g, b.dst, b.dst_stride, b.src, b.src_stride, &over) == 10);
	CHECK(cu_copy_area(&b.g, b.dst, b.dst_stride, b.src, b.src_stride, &none) == 0);
	CHECK(cu_copy_area(&b.g, b.dst, b.dst_stride, b.src, b.src_stride, &inv) == 0);
	bufs_free(&b);
}

/* The real panel geometry: a full copy writes 1080 x 2246 pixels into a
 * 4352-byte stride page, and the corners land where they should. */
static void test_panel_size(void)
{
	struct cu_geom g;
	size_t sstride = 1080 * 4, dstride = 4352;
	uint32_t *src = calloc(1080 * 2246, 4), *dst = calloc(dstride / 4 * 2246, 4);
	struct cu_rect all = { 0, 0, 1079, 2245 };

	cu_geom_init(&g, 0, 1080, 2246);
	src[0] = 0x00ff0000u;			/* red at the top-left */
	src[1080 * 2246 - 1] = 0x000000ffu;	/* blue at the bottom-right */
	CHECK(cu_copy_area(&g, (uint8_t *)dst, dstride, (uint8_t *)src, sstride, &all) == 1080u * 2246u);
	CHECK(dst[0] == 0xff0000ffu);
	CHECK(dst[2245 * (dstride / 4) + 1079] == 0xffff0000u);
	CHECK(dst[1080] == 0);			/* stride padding untouched */
	free(src);
	free(dst);
}

/* Exhaustively compare membership with the physical copy transform. */
static void test_safe_area(void)
{
	for (int rot = 0; rot < 360; rot += 90) {
		struct cu_geom g;
		cu_geom_init(&g, rot, 7, 9);
		for (int top = 0; top < 9; top++) {
			struct cu_rect r = cu_safe_area(&g, top);
			struct cu_rect phys = cu_rect_to_phys(&g, &r);
			CHECK(phys.x1 == 0 && phys.x2 == 6 && phys.y1 == top && phys.y2 == 8);
			for (int y = 0; y < g.lh; y++)
				for (int x = 0; x < g.lw; x++) {
					int32_t px, py;
					cu_log_to_phys(&g, x, y, &px, &py);
					CHECK((x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2) == (py >= top));
				}
		}
	}
}

int main(void)
{
	test_safe_area();
	test_swizzle();
	test_geom();
	test_full_copy_all_rotations();
	test_partial_copy_all_rotations();
	test_clipping();
	test_panel_size();
	return check_report("test-copy");
}
