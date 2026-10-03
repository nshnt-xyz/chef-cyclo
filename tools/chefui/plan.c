/* plan.c - see plan.h. */
#include <string.h>

#include "plan.h"

void cu_plan_init(struct cu_plan *p, int32_t lw, int32_t lh, uint32_t yres)
{
	memset(p, 0, sizeof(*p));
	p->lw = lw;
	p->lh = lh;
	p->yres = yres;
	p->back = 1;
	p->stale[0] = p->stale[1] = true;
}

void cu_plan_invalidate_pages(struct cu_plan *p)
{
	p->stale[0] = p->stale[1] = true;
}

bool cu_rect_contains(const struct cu_rect *a, const struct cu_rect *b)
{
	return a->x1 <= b->x1 && a->y1 <= b->y1 && a->x2 >= b->x2 && a->y2 >= b->y2;
}

static bool covers_screen(const struct cu_plan *p, const struct cu_rect *a)
{
	return a->x1 <= 0 && a->y1 <= 0 && a->x2 >= p->lw - 1 && a->y2 >= p->lh - 1;
}

void cu_plan_add(struct cu_plan *p, const struct cu_rect *a)
{
	if (a->x1 > a->x2 || a->y1 > a->y2)
		return;
	if (covers_screen(p, a))
		p->cur_full = true;
	if (p->cur_full)
		return;
	if (p->ncur == CU_PLAN_MAX_AREAS) {
		p->cur_full = true;	/* overflow: the area list is lost */
		return;
	}
	p->cur[p->ncur++] = *a;
}

/* Append a to out unless an area already there contains it; drop the
 * ones it contains. Equal areas keep the first. */
static void out_add(struct cu_plan *p, const struct cu_rect *a)
{
	int i, j;

	for (i = 0; i < p->nout; i++)
		if (cu_rect_contains(&p->out[i], a))
			return;
	for (i = 0, j = 0; i < p->nout; i++)
		if (!cu_rect_contains(a, &p->out[i]))
			p->out[j++] = p->out[i];
	p->nout = j;
	p->out[p->nout++] = *a;
}

int cu_plan_finish(struct cu_plan *p)
{
	int page = p->back, i;

	p->nout = 0;
	p->out_full = p->stale[page] || p->cur_full || p->prev_full;
	if (p->out_full) {
		p->out[0] = (struct cu_rect){ 0, 0, p->lw - 1, p->lh - 1 };
		p->nout = 1;
	} else {
		for (i = 0; i < p->nprev; i++)
			out_add(p, &p->prev[i]);
		for (i = 0; i < p->ncur; i++)
			out_add(p, &p->cur[i]);
	}
	p->stale[page] = false;
	memcpy(p->prev, p->cur, sizeof(p->cur[0]) * (size_t)p->ncur);
	p->nprev = p->ncur;
	p->prev_full = p->cur_full;
	p->ncur = 0;
	p->cur_full = false;
	return page;
}

uint32_t cu_plan_pan_yoffset(const struct cu_plan *p, int page)
{
	return (uint32_t)page * p->yres;
}

void cu_plan_flipped(struct cu_plan *p)
{
	p->back ^= 1;
}

void cu_plan_pan_failed(struct cu_plan *p)
{
	cu_plan_invalidate_pages(p);
}
