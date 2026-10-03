/*
 * plan.h - which shadow areas a frame copies into which fb page.
 * No LVGL, no syscalls: covered by tests/test_plan.c.
 *
 * Two pages (yres_virtual >= 2 * yres), panned with FBIOPAN_DISPLAY.
 * The back page last held the frame before the previous one, so bringing
 * it up to date takes this frame's dirty areas plus the previous frame's.
 * A page whose content is unknown (first open, after an UNBLANK, another
 * client drew on it) is "stale" and gets one full copy. An area list
 * overflow or an area covering the whole screen also means a full copy,
 * and then the next frame's "previous" areas are unknown too, so it
 * copies the full screen as well.
 *
 * Areas are logical (rotated) coordinates; the copy transforms them.
 */
#ifndef CHEFUI_PLAN_H
#define CHEFUI_PLAN_H

#include <stdbool.h>
#include "copy.h"

#define CU_PLAN_MAX_AREAS 32	/* per frame; more forces a full copy */

struct cu_plan {
	int32_t lw, lh;			/* logical screen */
	uint32_t yres;			/* physical rows per page */
	int back;			/* page the next frame is written to */
	bool stale[2];
	struct cu_rect cur[CU_PLAN_MAX_AREAS];
	int ncur;
	bool cur_full;
	struct cu_rect prev[CU_PLAN_MAX_AREAS];
	int nprev;
	bool prev_full;
	/* result of cu_plan_finish(), valid until the next call */
	struct cu_rect out[2 * CU_PLAN_MAX_AREAS];
	int nout;
	bool out_full;
};

/* Both pages stale, page 1 is the first back page (fblog shows page 0). */
void cu_plan_init(struct cu_plan *p, int32_t lw, int32_t lh, uint32_t yres);

/* Forget both pages' content (after UNBLANK or reopening the fb). */
void cu_plan_invalidate_pages(struct cu_plan *p);

/* One flushed area of the current frame. */
void cu_plan_add(struct cu_plan *p, const struct cu_rect *a);

/*
 * The frame is complete: compute the copy list for the back page into
 * p->out/p->nout (or p->out_full = a single full-screen copy, also
 * listed as one out entry), and roll this frame's areas into "previous".
 * Returns the page to copy into; the caller copies, pans to it with
 * cu_plan_pan_yoffset(), and then calls cu_plan_flipped().
 */
int cu_plan_finish(struct cu_plan *p);

/* yoffset for FBIOPAN_DISPLAY to show `page`. */
uint32_t cu_plan_pan_yoffset(const struct cu_plan *p, int page);

/* The pan to the page cu_plan_finish() returned succeeded: swap pages. */
void cu_plan_flipped(struct cu_plan *p);

/* The pan failed: the written page was not shown, and the shown page never
 * got this frame's areas (they already rolled into "previous"), so both
 * pages are stale and the next frames copy in full. */
void cu_plan_pan_failed(struct cu_plan *p);

/* True if a fully contains b. */
bool cu_rect_contains(const struct cu_rect *a, const struct cu_rect *b);

#endif /* CHEFUI_PLAN_H */
