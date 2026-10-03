/*
 * copy.h - shadow-to-page pixel copy for the fbdev backend, and the
 * rotation transform it shares with the touch reader. No LVGL, no
 * syscalls: covered by tests/test_copy.c.
 *
 * LVGL renders XRGB8888 (bytes B,G,R,X) into a cached RAM shadow at the
 * logical (rotated) size. MDSS scans out RGBA8888 (bytes R,G,B,A). The
 * copy swaps R and B, forces alpha to 0xff and writes each destination
 * row left to right, top to bottom: the fb mapping is write-combined, so
 * it is only ever written, never read, and sequential writes keep the
 * write-combining buffers full.
 *
 * Rotation r is how far the logical picture is turned clockwise on the
 * physical panel (PW x PH = 1080 x 2246 portrait):
 *   0    px = lx              py = ly               logical = PW x PH
 *   90   px = PW - 1 - ly     py = lx               logical = PH x PW
 *   180  px = PW - 1 - lx     py = PH - 1 - ly      logical = PW x PH
 *   270  px = ly              py = PH - 1 - lx      logical = PH x PW
 * so at 90 the logical top edge is the physical right edge.
 */
#ifndef CHEFUI_COPY_H
#define CHEFUI_COPY_H

#include <stddef.h>
#include <stdint.h>

/* Inclusive rectangle, like lv_area_t (kept separate so these modules
 * build and test without LVGL). */
struct cu_rect {
	int32_t x1, y1, x2, y2;
};

struct cu_geom {
	int rot;			/* 0, 90, 180, 270 */
	int32_t pw, ph;			/* physical panel size */
	int32_t lw, lh;			/* logical size (swapped for 90/270) */
};

/* Fill g for rotation rot on a pw x ph panel; -1 for an invalid rotation. */
int cu_geom_init(struct cu_geom *g, int rot, int32_t pw, int32_t ph);

/* Logical point -> physical point, and the inverse (for touch). */
void cu_log_to_phys(const struct cu_geom *g, int32_t lx, int32_t ly, int32_t *px, int32_t *py);
void cu_phys_to_log(const struct cu_geom *g, int32_t px, int32_t py, int32_t *lx, int32_t *ly);

/* The physical rectangle a logical rectangle lands on. */
struct cu_rect cu_rect_to_phys(const struct cu_geom *g, const struct cu_rect *l);

/* XRGB8888 (B,G,R,X in memory, i.e. 0xXXRRGGBB as a little-endian word)
 * to RGBA8888 with alpha 0xff (0xffBBGGRR). */
static inline uint32_t cu_swizzle(uint32_t v)
{
	return 0xff000000u | (v & 0x0000ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
}

/*
 * Copy logical area `a` (clipped to the logical size) from the shadow
 * (logical lw x lh, src_stride bytes per row) into a physical page
 * (dst_stride bytes per row). Returns the number of pixels written.
 */
size_t cu_copy_area(const struct cu_geom *g, uint8_t *dst, size_t dst_stride,
		    const uint8_t *src, size_t src_stride, const struct cu_rect *a);

/* Logical rectangle excluding a physical-top strip; inclusive bounds. */
struct cu_rect cu_safe_area(const struct cu_geom *g, int32_t top);

#endif /* CHEFUI_COPY_H */
