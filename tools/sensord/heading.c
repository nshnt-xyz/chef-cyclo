/*
 * sensord's compass glue: the heading channel fed from the SMGR samples
 * (compass.c), its lines and its part of status, the calibrated flag
 * (live, from the registry, or none) and the heading's on/off sync. See
 * main.c, section 5.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensord.h"

/* The compass part of status. */
int sn_format_heading_status(const struct server *s, char *buf, size_t len)
{
	const struct heading *h = &s->heading;
	const struct compass *c = &h->c;
	struct compass o_c = *c;	/* compass_output updates the held heading */
	struct compass_out o;
	bool out = h->active && compass_output(&o_c, &o);
	char bias[96] = "null", acc[32] = "null", decl[32] = "null", live[96] = "null";
	char rva[16] = "null";

	if (h->mc.known)
		snprintf(live, sizeof(live), "[%.4f,%.4f,%.4f]", h->mc.bias[0], h->mc.bias[1],
			 h->mc.bias[2]);
	if (s->rotvec.on && s->rotvec.have)
		snprintf(rva, sizeof(rva), "%" PRIu32, s->rotvec.accuracy);
	if (h->bias_known)
		snprintf(bias, sizeof(bias), "[%.6g,%.6g,%.6g]", h->bias_raw[0] / 65536.0,
			 h->bias_raw[1] / 65536.0, h->bias_raw[2] / 65536.0);
	if (out)
		snprintf(acc, sizeof(acc), "%.1f", o.accuracy);
	if (h->have_decl)
		snprintf(decl, sizeof(decl), "%.2f", h->decl);
	return snprintf(buf, len,
			",\"compass\":\"%s\",\"compass_clients\":%d,\"mount\":\"%s\",\"calibrated\":%s,"
			"\"cal_source\":\"%s\",\"mag_bias\":%s,\"mag_bias_pairs\":%lu,"
			"\"mag_bias_reg\":%s,\"disturbed\":%s,\"accuracy\":%s,\"rotvec_accuracy\":%s,"
			"\"declination\":%s,"
			"\"compass_lines\":%lu,\"compass_dropped\":%lu,\"compass_gaps\":%lu,"
			"\"compass_resets\":%lu,\"compass_gyro_lost\":%lu,\"compass_disturbances\":%lu,"
			"\"compass_reacquired\":%lu,\"gyro_bias\":[%.5f,%.5f,%.5f]",
			!h->active ? "off" : c->st == COMPASS_RUN ? "on" : "init",
			sn_channel_claims(s, CH_HEADING), compass_mount_name(h->mount),
			h->calibrated ? "true" : "false", h->cal_source ? h->cal_source : "none", live,
			h->mc.pairs, bias, out && o.disturbed ? "true" : "false", acc, rva,
			decl, h->lines, c->stale, c->gaps, c->resets, c->gyro_lost, c->disturbances,
			c->reacquired, c->bias[0], c->bias[1], c->bias[2]);
}


/* The compass output to the heading clients, after a sample changed it. */
static bool heading_emit(struct server *s)
{
	struct heading *h = &s->heading;
	struct compass_out o;
	char line[384], extra[128];
	int n;

	if (!compass_output(&h->c, &o))
		return false;
	/* where the calibrated flag comes from and the live bias (|factory -
	 * full|, gauss); with -V the ADSP rotation vector's accuracy (0..3) */
	n = snprintf(extra, sizeof(extra), ",\"cal_source\":\"%s\",\"mag_bias\":%.3f",
		     h->cal_source ? h->cal_source : "none",
		     h->mc.known ? sqrt(h->mc.bias[0] * h->mc.bias[0] + h->mc.bias[1] * h->mc.bias[1] +
					h->mc.bias[2] * h->mc.bias[2]) : 0.0);
	if (s->rotvec.on && s->rotvec.have && n > 0 && (size_t)n < sizeof(extra))
		snprintf(extra + n, sizeof(extra) - (size_t)n, ",\"rotvec_accuracy\":%" PRIu32,
			 s->rotvec.accuracy);
	n = compass_format(line, sizeof(line), &o, h->have_decl, h->decl, extra);
	if (n <= 0 || (size_t)n >= sizeof(line))
		return false;
	h->lines++;
	return sn_deliver(s, CH_HEADING, o.t, line, (size_t)n);
}


/* An SMGR sample into the compass while heading is claimed; full and
 * factory magn samples also into the live calibration state. */
bool sn_heading_feed(struct server *s, int chn, int64_t t, const int32_t d[3])
{
	struct heading *h = &s->heading;
	unsigned long lost = h->c.gyro_lost;
	double v[3];
	int rc;

	if (!h->active || (chn != CH_ACCEL && chn != CH_ANGLVEL && chn != CH_MAGN && chn != CH_MAGF))
		return false;
	sn_convert_sample(CH_VEC3, d, v);
	if (chn == CH_MAGF) {
		if (compass_magcal_factory(&h->mc, t, v))
			sn_cal_apply(s, "live");
		return false;
	}
	if (chn == CH_MAGN && s->ch[CH_MAGN].cal == SNS_SMGR_CAL_FULL &&
	    compass_magcal_full(&h->mc, t, v))
		sn_cal_apply(s, "live");
	rc = chn == CH_ACCEL ? compass_accel(&h->c, t, v)
	     : chn == CH_ANGLVEL ? compass_gyro(&h->c, t, v) : compass_magn(&h->c, t, v);
	if (h->c.gyro_lost != lost)
		s->log("heading: anglvel stopped (accel/magn more than 2 s past the last gyro sample): filter re-initialised, running on accel + magn until it is back (%lu so far)",
		       h->c.gyro_lost);
	return rc > 0 && heading_emit(s);
}


/* The heading's registry-derived calibration: group 2980's bias items
 * (3903..3905, what the DSP learns and writes over REG2) nonzero, with
 * magn delivered at calibration "full" (SMGR applies the bias only then). */
void sn_mag_cal_check(struct server *s, const char *why)
{
	struct heading *h = &s->heading;
	int32_t b[3] = { 0, 0, 0 };
	bool known = s->serve_reg2 && s->reg.data, cal;
	int i;

	for (i = 0; known && i < 3; i++) {
		const struct sns_reg_span *sp = sns_reg_find(s->reg.items, s->reg.nitems,
							     (uint16_t)(SNS_REG2_MAG_BIAS_ITEM + i));

		if (!sp || sp->size != 4 || sp->offset + 4 > s->reg.size) {
			known = false;
			break;
		}
		memcpy(&b[i], s->reg.data + sp->offset, 4);	/* little endian, as the DSP writes it */
	}
	cal = known && (b[0] || b[1] || b[2]);
	if (h->cal_logged && known == h->bias_known && cal == h->reg_calibrated &&
	    !memcmp(b, h->bias_raw, sizeof(b))) {
		sn_cal_apply(s, why);	/* quiet unless the flag in use changes */
		return;
	}
	if (!known)
		s->log("magnetometer calibration in the registry unknown (%s: no registry items %d..%d)",
		       why, SNS_REG2_MAG_BIAS_ITEM, SNS_REG2_MAG_BIAS_ITEM + 2);
	else
		s->log("magnetometer bias in the registry (%s): %.4f %.4f %.4f gauss (SMGR frame, group %d)%s -> %s",
		       why, b[0] / 65536.0, b[1] / 65536.0, b[2] / 65536.0, SNS_REG2_MAG_CAL_GROUP,
		       s->ch[CH_MAGN].cal != SNS_SMGR_CAL_FULL ? ", but magn calibration is not full (-c)" : "",
		       cal ? "calibrated" : "not calibrated");
	h->cal_logged = true;
	h->bias_known = known;
	memcpy(h->bias_raw, b, sizeof(b));
	h->reg_calibrated = cal;
	sn_cal_apply(s, why);
}

/* The calibrated flag in use: the live state (full - factory) once it is
 * known, it is what SMGR applies right now (live 2026-10-01 the ADSP
 * applied its bias about 75 s into the holds but wrote group 2980 only
 * when the magn report was deleted, 8 minutes later); before that the
 * registry (a /run copy reused in the same boot carries the DSP's
 * earlier writes). Never calibrated when magn is not delivered at "full". */
void sn_cal_apply(struct server *s, const char *why)
{
	struct heading *h = &s->heading;
	bool full = s->ch[CH_MAGN].cal == SNS_SMGR_CAL_FULL, cal;
	const char *src;

	if (h->mc.known && full) {
		src = "live";
		cal = h->mc.calibrated;
	} else if (h->bias_known) {
		src = "registry";
		cal = h->reg_calibrated;
	} else {
		src = "none";
		cal = false;
	}
	if (!full)
		cal = false;
	if (cal != h->calibrated || !h->cal_source || strcmp(src, h->cal_source))
		s->log("magnetometer %s (source %s, %s%s): live bias %.4f %.4f %.4f gauss (factory - full, device frame, %s)",
		       cal ? "calibrated" : "NOT calibrated", src, why, full ? "" : "; magn calibration not full",
		       h->mc.bias[0], h->mc.bias[1], h->mc.bias[2], h->mc.known ? "known" : "not known yet");
	else if (cal && !strcmp(why, "live"))
		/* still calibrated, but the ADSP's bias moved by > 0.05 G */
		s->log("magnetometer bias changed (live): %.4f %.4f %.4f gauss; heading re-learns the field",
		       h->mc.bias[0], h->mc.bias[1], h->mc.bias[2]);
	if (cal == h->calibrated && cal && !strcmp(why, "live"))
		compass_field_jump(&h->c);
	h->calibrated = cal;
	h->cal_source = src;
	compass_set_calibrated(&h->c, cal);
}

/* Heading on while claimed (its inputs are then wanted internally),
 * off otherwise; each start is a fresh filter. */
void sn_heading_sync(struct server *s)
{
	struct heading *h = &s->heading;
	bool want = sn_channel_claims(s, CH_HEADING) > 0;

	if (want == h->active)
		return;
	h->active = want;
	compass_reset(&h->c);
	if (want)
		s->log("heading on: accel %d Hz, anglvel %d Hz, magn %d Hz (calibration %s) and magn %d Hz (factory) claimed internally%s; mount %s; magnetometer %s (source %s)",
		       SENSORD_HEADING_IMU_HZ, SENSORD_HEADING_IMU_HZ, SENSORD_HEADING_MAG_HZ,
		       sn_cal_name(s->ch[CH_MAGN].cal), sn_internal_want(s, CH_MAGF),
		       s->rotvec.opt ? ", rotation vector (-V)" : "", compass_mount_name(h->mount),
		       h->calibrated ? "calibrated" : "NOT calibrated yet",
		       h->cal_source ? h->cal_source : "none");
	else
		s->log("heading off (no claims): %lu lines, %lu disturbances, %lu resets", h->lines,
		       h->c.disturbances, h->c.resets);
}
