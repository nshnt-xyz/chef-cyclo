/* copy.c - see copy.h. */
#include "copy.h"

int cu_geom_init(struct cu_geom *g, int rot, int32_t pw, int32_t ph)
{
	if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
		return -1;
	g->rot = rot;
	g->pw = pw;
	g->ph = ph;
	if (rot == 90 || rot == 270) {
		g->lw = ph;
		g->lh = pw;
	} else {
		g->lw = pw;
		g->lh = ph;
	}
	return 0;
}

void cu_log_to_phys(const struct cu_geom *g, int32_t lx, int32_t ly, int32_t *px, int32_t *py)
{
	switch (g->rot) {
	case 90:  *px = g->pw - 1 - ly; *py = lx; break;
	case 180: *px = g->pw - 1 - lx; *py = g->ph - 1 - ly; break;
	case 270: *px = ly; *py = g->ph - 1 - lx; break;
	default:  *px = lx; *py = ly; break;
	}
}

void cu_phys_to_log(const struct cu_geom *g, int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
	switch (g->rot) {
	case 90:  *lx = py; *ly = g->pw - 1 - px; break;
	case 180: *lx = g->pw - 1 - px; *ly = g->ph - 1 - py; break;
	case 270: *lx = g->ph - 1 - py; *ly = px; break;
	default:  *lx = px; *ly = py; break;
	}
}

static int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

struct cu_rect cu_rect_to_phys(const struct cu_geom *g, const struct cu_rect *l)
{
	struct cu_rect p;
	int32_t ax, ay, bx, by;

	cu_log_to_phys(g, l->x1, l->y1, &ax, &ay);
	cu_log_to_phys(g, l->x2, l->y2, &bx, &by);
	p.x1 = min32(ax, bx);
	p.x2 = max32(ax, bx);
	p.y1 = min32(ay, by);
	p.y2 = max32(ay, by);
	return p;
}

size_t cu_copy_area(const struct cu_geom *g, uint8_t *dst, size_t dst_stride,
		    const uint8_t *src, size_t src_stride, const struct cu_rect *area)
{
	struct cu_rect a = *area, p;
	int32_t x, y, w, h;
	/* source step per destination pixel along a row, and per row, in
	 * pixels; the source start pixel is the logical point that lands on
	 * the physical rectangle's top-left corner */
	ptrdiff_t sstride = (ptrdiff_t)(src_stride / 4), step_x, step_y;
	int32_t sx, sy;

	a.x1 = max32(a.x1, 0);
	a.y1 = max32(a.y1, 0);
	a.x2 = min32(a.x2, g->lw - 1);
	a.y2 = min32(a.y2, g->lh - 1);
	if (a.x1 > a.x2 || a.y1 > a.y2)
		return 0;
	p = cu_rect_to_phys(g, &a);
	w = p.x2 - p.x1 + 1;
	h = p.y2 - p.y1 + 1;
	cu_phys_to_log(g, p.x1, p.y1, &sx, &sy);
	switch (g->rot) {
	case 90:  step_x = -sstride; step_y = 1; break;		/* px+1 -> ly-1; py+1 -> lx+1 */
	case 180: step_x = -1; step_y = -sstride; break;
	case 270: step_x = sstride; step_y = -1; break;		/* px+1 -> ly+1; py+1 -> lx-1 */
	default:  step_x = 1; step_y = sstride; break;
	}

	if (g->rot == 0) {
		for (y = 0; y < h; y++) {
			const uint32_t *s = (const uint32_t *)(src + (size_t)(sy + y) * src_stride) + sx;
			uint32_t *d = (uint32_t *)(dst + (size_t)(p.y1 + y) * dst_stride) + p.x1;

			for (x = 0; x < w; x++)
				d[x] = cu_swizzle(s[x]);
		}
	} else {
		const uint32_t *s0 = (const uint32_t *)src + (ptrdiff_t)sy * sstride + sx;

		for (y = 0; y < h; y++) {
			const uint32_t *s = s0 + y * step_y;
			uint32_t *d = (uint32_t *)(dst + (size_t)(p.y1 + y) * dst_stride) + p.x1;

			for (x = 0; x < w; x++, s += step_x)
				d[x] = cu_swizzle(*s);
		}
	}
	return (size_t)w * (size_t)h;
}

struct cu_rect cu_safe_area(const struct cu_geom *g, int32_t top)
{
	int32_t ax, ay, bx, by;
	cu_phys_to_log(g, 0, top, &ax, &ay);
	cu_phys_to_log(g, g->pw - 1, g->ph - 1, &bx, &by);
	return (struct cu_rect){ min32(ax, bx), min32(ay, by), max32(ax, bx), max32(ay, by) };
}
