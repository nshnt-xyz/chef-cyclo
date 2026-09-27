#!/usr/bin/env python3
"""Magnetometer calibration check for sensord captures (host only; stdlib
+ numpy). docs/next-steps/sensors-plan.md section 7 is the experiment
this analyses; sensors-magcal-run on the phone records the input.

    python3 tools/mag-cal-check.py DIR [--steps FILE] [--spin NAME] [--json OUT]

DIR holds sensord JSON-lines captures: every *accel*.jsonl and
*anglvel*.jsonl file is merged into one accelerometer and one gyro
series (they share CLOCK_MONOTONIC stamps), and every *magn*.jsonl file
is one magnetometer capture, analysed on its own. The steps file (the
guided run's announcements, "UPTIME STEP ID: TEXT (N s)" per line; the
2026-09-26 g-steps.txt shape also works) is optional: with it, face
statistics come only from the face steps and the flat-turn window is
found by name. Without it faces come from the accelerometer alone and
there is no heading check.

Per magn capture it reports:
 - sphere fit (hard-iron offset, radius) and ellipsoid fit (offset plus
   soft-iron correction matrix W: corrected = W (m - offset)); the
   ellipsoid is used only when it is positive definite, the data cover at
   least 6 of the 8 octants around the offset (each with at least
   max(10, 1 %) of the samples), W's eigenvalues stay within 0.8..1.25,
   its centre is within 0.05 G of the sphere's, and it makes neither the
   |B| spread nor the face up+down z sum worse than the sphere; else the
   sphere, with the reasons printed;
 - gross outliers dropped first: after a first sphere fit, samples whose
   |m - centre| is outside the median +-50 % (count reported); the fits
   and the |B| statistics use the rest;
 - |B| mean/std/min/max and spread (the larger of p98 and p2 distance
   from the median, over the median) raw, after the sphere and after the
   ellipsoid correction, and over all samples for comparison;
 - the stationary |B| per resting face and its range (how much |B|
   changes with orientation alone);
 - face up vs face down z (stationary samples, accel within 18 deg of
   +z / -z): the sum should be 0;
 - the magnetic dip after the fit (information only: it exposes a z
   sign error, which the two checks below cannot see);
 - tilt-compensated heading vs gyro-integrated yaw about the gravity
   axis (gyro bias from stationary samples) over the flat-turn window,
   less its first second;
 - the acceptance of section 7 step 3, both for the data as delivered
   and after the fitted correction: |B| spread < 5 %, |z_up + z_down| <
   0.03 gauss, heading error < 5 deg over the turn. PASS needs all
   three; a missing part (no face-down samples, no turn) is INCOMPLETE.
   When the |B| spread is the only failure, a note line says so.
Also, when a capture carries sensord -Q's "bias"/"accuracy" fields, the
QMAG_CAL bias and accuracy history, and for captures recorded at the same
time (full vs factory vs raw) the median per-axis difference, over the
whole overlap and over its last 30 % (a dynamic bias converges), which
is the bias SMGR applied between them.

Exit status 0 when the analysis ran (whatever the verdicts), 2 on bad
input.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

import numpy as np

SPREAD_MAX = 0.05	# |B| spread, fraction of the median
FACE_Z_MAX = 0.03	# |z_up + z_down|, gauss
HEADING_MAX = 5.0	# degrees
FACE_COS = math.cos(math.radians(18))
ELL_EIG = (0.8, 1.25)	# W's eigenvalues (det 1): an ellipsoid more squashed is not trusted
ELL_CENTRE_MAX = 0.05	# gauss between the ellipsoid and sphere centres
OUTLIER_FRAC = 0.5	# |B - centre| outside median * (1 +- this) is a gross outlier
STILL_ACCEL_STD = 0.08	# m/s^2 over the 0.5 s window around a sample
STILL_GYRO = 0.2	# rad/s: stationary face samples
G = 9.80665

FACE_IDS = {
    "up": "face_up", "down": "face_down", "top": "top_up", "bottom": "top_down",
    "left": "left_down", "right": "right_down",
}


class InputError(Exception):
    pass


def load_jsonl(path, sensor):
    """(t[s], rows) of one sensor's sample lines; other lines skipped."""
    t, rows, extra = [], [], []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if d.get("sensor") != sensor or "t" not in d:
                continue
            try:
                rows.append((float(d["x"]), float(d["y"]), float(d["z"])))
            except (KeyError, TypeError, ValueError):
                continue
            t.append(d["t"] / 1e9)
            extra.append({k: d[k] for k in ("bias", "bias_raw", "accuracy") if k in d})
    return np.array(t), np.array(rows).reshape(-1, 3), extra


def load_series(d, sensor):
    t_all, v_all = [], []
    for p in sorted(glob.glob(os.path.join(d, "*%s*.jsonl" % sensor))):
        t, v, _ = load_jsonl(p, sensor)
        t_all.append(t)
        v_all.append(v)
    if not t_all:
        return np.zeros(0), np.zeros((0, 3))
    t = np.concatenate(t_all)
    v = np.concatenate(v_all)
    t, idx = np.unique(t, return_index=True)	# sorted, duplicates merged
    return t, v[idx]


CLOCK_RE = re.compile(r"^#\s*clock uptime\s+(\S+)\s+sample_t\s+(\d+)")
STEP_RE = re.compile(r"^(\S+)\s+STEP\s+([^:]+):\s*(.*?)(?:\s*\((\d+(?:\.\d+)?)\s*s\))?\s*$")


def load_steps(path):
    """Steps with times moved onto the samples' CLOCK_MONOTONIC when the
    file has a "# clock uptime U sample_t T" line (sensors-magcal-run):
    uptime is BOOTTIME, which runs ahead by any time spent suspended.
    Offsets under 0.5 s are sample age and ignored."""
    steps = []
    shift = 0.0
    with open(path, errors="replace") as f:
        for line in f:
            c = CLOCK_RE.match(line.strip())
            if c:
                off = float(c.group(1)) - int(c.group(2)) / 1e9
                if abs(off) >= 0.5:
                    shift = off
                    print("mag-cal-check: step times are %.1f s ahead of the sample clock "
                          "(suspend?); shifted" % off, file=sys.stderr)
                continue
            m = STEP_RE.match(line.strip())
            if not m:
                continue
            steps.append({"t0": float(m.group(1)) - shift, "id": m.group(2).strip(), "text": m.group(3),
                          "dur": float(m.group(4)) if m.group(4) else None})
    for i, s in enumerate(steps):
        nxt = steps[i + 1]["t0"] if i + 1 < len(steps) else None
        end = s["t0"] + s["dur"] if s["dur"] else nxt
        if nxt is not None and end is not None:
            end = min(end, nxt)
        s["t1"] = end if end is not None else s["t0"] + 10
        s["face"] = step_face(s)
    return steps


def step_face(s):
    sid = s["id"].split(".")[-1].lower()
    if sid in FACE_IDS:
        return FACE_IDS[sid]
    txt = s["text"].upper()
    if any(w in txt for w in ("SPIN", "TURN", "COVER", "HAND", "FIGURE")):
        return None
    if "FACE UP" in txt:
        return "face_up"
    if "FACE DOWN" in txt:
        return "face_down"
    return None


def is_spin(s):
    sid = s["id"].split(".")[-1].lower()
    txt = s["text"].upper()
    return sid in ("turn", "spin") or ("SPIN" in txt or "TURN" in txt) and "FIGURE" not in txt


def interp3(t, ts, vs):
    if len(ts) < 2:
        return np.full((len(t), 3), np.nan)
    return np.stack([np.interp(t, ts, vs[:, k], left=np.nan, right=np.nan) for k in range(3)], 1)


def moving_mean(t, v, half):
    """Mean over [t-half, t+half] per sample (O(n) via cumulative sums)."""
    c = np.vstack([np.zeros((1, v.shape[1])), np.cumsum(v, 0)])
    c2 = np.vstack([np.zeros((1, v.shape[1])), np.cumsum(v * v, 0)])
    lo = np.searchsorted(t, t - half, "left")
    hi = np.searchsorted(t, t + half, "right")
    n = (hi - lo)[:, None]
    mean = (c[hi] - c[lo]) / n
    var = np.maximum((c2[hi] - c2[lo]) / n - mean * mean, 0)
    return mean, np.sqrt(var.sum(1))


# ------------------------------------------------------------------ fits

def fit_sphere(m):
    A = np.hstack([2 * m, np.ones((len(m), 1))])
    b = (m * m).sum(1)
    sol, *_ = np.linalg.lstsq(A, b, rcond=None)
    c = sol[:3]
    r = math.sqrt(max(sol[3] + c @ c, 0))
    return c, r


def fit_ellipsoid(m, c0=None):
    """General quadric fit. Returns (offset, W, radius) with corrected =
    W (m - offset) on a sphere of that radius, or None if not an
    ellipsoid. The quadric is normalised to 1, which needs the origin
    inside the ellipsoid: fit around c0 (the sphere centre) so a hard-iron
    offset larger than the field (factory/raw data) still works."""
    c0 = np.zeros(3) if c0 is None else np.asarray(c0, float)
    x, y, z = (m - c0).T
    D = np.stack([x * x, y * y, z * z, 2 * y * z, 2 * x * z, 2 * x * y, 2 * x, 2 * y, 2 * z], 1)
    v, *_ = np.linalg.lstsq(D, np.ones(len(m)), rcond=None)
    A = np.array([[v[0], v[5], v[4]], [v[5], v[1], v[3]], [v[4], v[3], v[2]]])
    bv = v[6:9]
    try:
        c = -np.linalg.solve(A, bv)
    except np.linalg.LinAlgError:
        return None
    k = 1 + c @ A @ c
    if k <= 0:
        return None
    M = A / k
    w, V = np.linalg.eigh(M)
    if np.any(w <= 0):
        return None
    radius = float(np.prod(w) ** (-1 / 6))	# geometric mean semi-axis
    W = radius * (V @ np.diag(np.sqrt(w)) @ V.T)
    return c + c0, W, radius


def octants(m, c):
    """Octants around c holding at least max(10, 1 %) of the samples, so
    a stray noisy sample does not count as coverage."""
    d = (m - c > 0).astype(int)
    idx = d[:, 0] * 4 + d[:, 1] * 2 + d[:, 2]
    need = max(10, len(m) // 100)
    return int((np.bincount(idx, minlength=8) >= need).sum())


def norm_stats(v):
    n = np.linalg.norm(v, axis=1)
    if not len(n):
        return None
    med = float(np.median(n))
    p2, p98 = np.percentile(n, [2, 98])
    return {"mean": float(n.mean()), "std": float(n.std()), "min": float(n.min()),
            "max": float(n.max()), "median": med,
            "spread": float(max(p98 - med, med - p2) / med) if med > 0 else float("inf")}


# ------------------------------------------------------------- analysis

def still_mask(tm, acc_t, acc_v, gyr_t, gyr_v):
    """Stationary at each magn time: accel steady and gyro quiet."""
    if len(acc_t) < 10:
        return np.zeros(len(tm), bool)
    _, sd = moving_mean(acc_t, acc_v, 0.25)
    sd_m = np.interp(tm, acc_t, sd, left=np.inf, right=np.inf)
    ok = sd_m < STILL_ACCEL_STD
    if len(gyr_t) > 10:
        g = np.linalg.norm(interp3(tm, gyr_t, gyr_v), axis=1)
        ok &= np.nan_to_num(g, nan=np.inf) < STILL_GYRO
    return ok


def gyro_bias(acc_t, acc_v, gyr_t, gyr_v):
    if len(gyr_t) < 10 or len(acc_t) < 10:
        return np.zeros(3), 0
    _, sd = moving_mean(acc_t, acc_v, 0.25)
    sd_g = np.interp(gyr_t, acc_t, sd, left=np.inf, right=np.inf)
    still = sd_g < STILL_ACCEL_STD
    if still.sum() < 20:
        return np.zeros(3), int(still.sum())
    return np.median(gyr_v[still], 0), int(still.sum())


def faces(tm, mag, up, still, steps):
    """Mean z (and vector) of stationary face-up / face-down samples."""
    cosz = up[:, 2]
    sel = still & ~np.isnan(cosz)
    if steps:
        inwin = np.zeros(len(tm), bool)
        for s in steps:
            if s["face"] in ("face_up", "face_down"):
                inwin |= (tm >= s["t0"] + 1.0) & (tm <= s["t1"] - 0.3)
        sel &= inwin
    out = {}
    for name, m in (("face_up", sel & (cosz > FACE_COS)), ("face_down", sel & (cosz < -FACE_COS))):
        out[name] = {"n": int(m.sum()), "mean": mag[m].mean(0).tolist() if m.any() else None}
    return out


def heading_track(tm, mag, acc_t, acc_v, gyr_t, gyr_v, bias, win):
    # skip the first second (the phone is still being put down after the
    # announcement) and the smoothing edge at the end
    t0, t1 = win[0] + 1.0, win[1] - 0.3
    sel = (tm >= t0) & (tm <= t1)
    if sel.sum() < 10 or len(gyr_t) < 10 or len(acc_t) < 10:
        return None
    acc_s, _ = moving_mean(acc_t, acc_v, 0.25)
    # gyro yaw about the (smoothed) gravity axis, integrated over the window
    gs = (gyr_t >= t0 - 0.1) & (gyr_t <= t1 + 0.1)
    gt, gv = gyr_t[gs], gyr_v[gs] - bias
    if len(gt) < 10:
        return None
    ug = interp3(gt, acc_t, acc_s)
    ug /= np.linalg.norm(ug, axis=1, keepdims=True)
    w_up = np.nan_to_num((gv * ug).sum(1))
    yaw = np.concatenate([[0], np.cumsum(0.5 * (w_up[1:] + w_up[:-1]) * np.diff(gt))])
    tt, mm = tm[sel], mag[sel]
    U = interp3(tt, acc_t, acc_s)
    U /= np.linalg.norm(U, axis=1, keepdims=True)
    N = mm - (mm * U).sum(1, keepdims=True) * U
    E = np.cross(N, U)
    yd = np.array([0.0, 1.0, 0.0])
    yh = yd - (U @ yd)[:, None] * U
    head = np.arctan2((yh * E).sum(1), (yh * N).sum(1))
    ok = ~np.isnan(head)
    if ok.sum() < 10:
        return None
    tt, head = tt[ok], np.unwrap(head[ok])
    dh = np.degrees(head - head[0])
    # compass heading grows clockwise seen from above, gyro yaw anticlockwise
    dg = -np.degrees(np.interp(tt, gt, yaw) - np.interp(tt[0], gt, yaw))
    err = (dh - dg + 180) % 360 - 180
    return {"window": [t0, t1], "n": int(len(tt)), "gyro_yaw_deg": float(dg[-1]),
            "heading_change_deg": float(dh[-1]), "max_err_deg": float(np.abs(err).max()),
            "rms_err_deg": float(np.sqrt((err ** 2).mean()))}


def verdict(stats, fc, head):
    why, missing = [], []
    if stats["spread"] >= SPREAD_MAX:
        why.append("|B| spread %.1f%% >= %.0f%%" % (100 * stats["spread"], 100 * SPREAD_MAX))
    if fc.get("sum") is None:
        missing.append("no %s samples" % " or ".join(
            k.replace("_", "-") for k in ("face_up", "face_down") if not fc[k]["n"]))
    elif abs(fc["sum"]) >= FACE_Z_MAX:
        why.append("z up+down %+.3f G (|.| >= %.2f)" % (fc["sum"], FACE_Z_MAX))
    if head is None:
        missing.append("no flat-turn window")
    elif head["max_err_deg"] >= HEADING_MAX:
        why.append("heading error %.1f deg >= %.0f" % (head["max_err_deg"], HEADING_MAX))
    if why:
        return "FAIL", why + missing
    if missing:
        return "INCOMPLETE", missing
    return "PASS", []


FACE_AXES = (("face_up", 2, 1), ("face_down", 2, -1), ("top_up", 1, 1), ("top_down", 1, -1),
             ("left_down", 0, 1), ("right_down", 0, -1))


def face_norms(tm, mag, up, still, steps):
    """Mean |B| of stationary samples per resting face (from the gravity
    direction; with steps, only inside the face steps). The spread of
    these says how much |B| changes with orientation alone."""
    sel = still & ~np.isnan(up).any(1)
    if steps:
        inwin = np.zeros(len(tm), bool)
        for s in steps:
            if s["face"] or s["id"].split(".")[-1].lower() in FACE_IDS:
                inwin |= (tm >= s["t0"] + 1.0) & (tm <= s["t1"] - 0.3)
        sel &= inwin
    n = np.linalg.norm(mag, axis=1)
    out = {}
    for name, ax, sign in FACE_AXES:
        m = sel & (sign * up[:, ax] > FACE_COS)
        if m.sum() >= 5:
            out[name] = {"n": int(m.sum()), "mean": float(n[m].mean())}
    if not out:
        return {"faces": {}}
    v = [f["mean"] for f in out.values()]
    return {"faces": out, "min": min(v), "max": max(v),
            "range_frac": (max(v) - min(v)) / float(np.median(v))}


def face_sum(f, W=None, c=None):
    r = {"face_up": dict(f["face_up"]), "face_down": dict(f["face_down"])}
    zs = []
    for k in ("face_up", "face_down"):
        m = r[k]["mean"]
        if m is not None and W is not None:
            m = (W @ (np.array(m) - c)).tolist()
            r[k]["mean"] = m
        zs.append(None if m is None else m[2])
    r["sum"] = None if None in zs else zs[0] + zs[1]
    return r


def ell_reject(ell, oct_, c_s, mi, f):
    """Why the ellipsoid must not replace the sphere ([] = use it). Indoors
    a field gradient over the movement volume bends the point cloud, and
    a free quadric happily fits that: live, one capture's ellipsoid moved
    the centre 0.11 G and turned a -0.02 G face sum into +0.22 G. So it
    has to stay close to the sphere and must not make |B| or the faces
    worse."""
    c_e, W_e, _ = ell
    why = []
    if oct_ < 6:
        why.append("%d/8 octants covered" % oct_)
    ev = np.linalg.eigvalsh((W_e + W_e.T) / 2)
    if ev.min() < ELL_EIG[0] or ev.max() > ELL_EIG[1]:
        why.append("W eigenvalues %.3f..%.3f outside %.2f..%.2f" % (ev.min(), ev.max(), *ELL_EIG))
    dc = float(np.linalg.norm(c_e - c_s))
    if dc > ELL_CENTRE_MAX:
        why.append("centre %.3f G from the sphere's (> %.2f)" % (dc, ELL_CENTRE_MAX))
    sp_s = norm_stats(mi - c_s)["spread"]
    sp_e = norm_stats((mi - c_e) @ W_e.T)["spread"]
    if sp_e > sp_s:
        why.append("|B| spread %.1f%% vs sphere %.1f%%" % (100 * sp_e, 100 * sp_s))
    fs, fe = face_sum(f, np.eye(3), c_s)["sum"], face_sum(f, W_e, c_e)["sum"]
    if fs is not None and fe is not None and abs(fe) > abs(fs) + 0.005:
        why.append("face up+down z %+.3f G vs sphere %+.3f" % (fe, fs))
    return why


def analyse(path, acc, gyr, steps, spin_name):
    name = os.path.basename(path)[:-len(".jsonl")]
    tm, mag, extra = load_jsonl(path, "magn")
    res = {"capture": name, "n": int(len(tm))}
    if len(tm) < 30:
        res["error"] = "only %d magn samples" % len(tm)
        return res, tm, mag
    acc_t, acc_v = acc
    gyr_t, gyr_v = gyr
    up = interp3(tm, acc_t, acc_v)
    up /= np.linalg.norm(up, axis=1, keepdims=True)
    still = still_mask(tm, acc_t, acc_v, gyr_t, gyr_v)
    bias, nstill = gyro_bias(acc_t, acc_v, gyr_t, gyr_v)
    res["duration_s"] = float(tm[-1] - tm[0])
    res["gyro_bias"] = bias.tolist()

    # Gross outliers (live: a few figure-8 samples next to the laptop or
    # the USB connector, |B| far off) are dropped before fitting: a first
    # sphere fit, then |m - centre| outside median +-50 % goes.
    c0, _ = fit_sphere(mag)
    r0 = np.linalg.norm(mag - c0, axis=1)
    med0 = np.median(r0)
    inl = (r0 >= (1 - OUTLIER_FRAC) * med0) & (r0 <= (1 + OUTLIER_FRAC) * med0)
    if inl.sum() < 30:
        inl = np.ones(len(mag), bool)
    res["outliers"] = int((~inl).sum())
    mi = mag[inl]
    c_s, r_s = fit_sphere(mi)
    res["sphere"] = {"offset": c_s.tolist(), "radius": r_s}
    ell = fit_ellipsoid(mi, c_s)
    oct_ = octants(mi, c_s)
    res["octants"] = oct_
    if ell is not None:
        c_e, W_e, r_e = ell
        res["ellipsoid"] = {"offset": c_e.tolist(), "W": W_e.tolist(), "radius": r_e}
    I = np.eye(3)
    f = faces(tm, mag, up, still, steps)
    why = ell_reject(ell, oct_, c_s, mi, f) if ell is not None else ["not an ellipsoid"]
    use_ell = not why
    c_fit, W_fit = (c_e, W_e) if use_ell else (c_s, I)
    res["correction"] = "ellipsoid" if use_ell else "sphere"
    res["ellipsoid_rejected"] = why
    corrected = (mag - c_fit) @ W_fit.T

    # |B| statistics without the outliers (these feed the verdict), and
    # over every sample for comparison
    res["norm_raw"] = norm_stats(mi)
    res["norm_sphere"] = norm_stats(mi - c_s)
    if ell is not None:
        res["norm_ellipsoid"] = norm_stats((mi - c_e) @ W_e.T)
    res["norm_raw_all"] = norm_stats(mag)
    res["norm_fit_all"] = norm_stats(corrected)
    res["face_norms_raw"] = face_norms(tm, mag, up, still & inl, steps)
    res["face_norms_fit"] = face_norms(tm, corrected, up, still & inl, steps)

    res["faces_raw"] = face_sum(f)
    res["faces_fit"] = face_sum(f, W_fit, c_fit)
    # magnetic dip (positive: the field points down, northern hemisphere);
    # a z sign error flips it and the face and heading checks cannot see that
    if still.any():
        cu = corrected[still]
        dip = np.degrees(np.arcsin(np.clip(-(cu * up[still]).sum(1) / np.linalg.norm(cu, axis=1), -1, 1)))
        res["dip_deg"] = {"median": float(np.median(dip)), "std": float(dip.std()), "n": int(still.sum())}

    win = None
    if steps:
        cands = [s for s in steps if (spin_name and (spin_name == s["id"] or spin_name in s["id"]
                                                     or spin_name.upper() in s["text"].upper()))
                 or (not spin_name and is_spin(s))]
        for s in cands:
            if (tm >= s["t0"]).any() and (tm <= s["t1"]).any() and tm[0] < s["t1"] and tm[-1] > s["t0"]:
                win = (s["t0"], s["t1"])
                res["spin_step"] = s["id"]
                break
    res["heading_raw"] = heading_track(tm, mag, acc_t, acc_v, gyr_t, gyr_v, bias, win) if win else None
    res["heading_fit"] = heading_track(tm, corrected, acc_t, acc_v, gyr_t, gyr_v, bias, win) if win else None

    v_raw, why_raw = verdict(res["norm_raw"], res["faces_raw"], res["heading_raw"])
    v_fit, why_fit = verdict(norm_stats(corrected[inl]), res["faces_fit"], res["heading_fit"])
    res["verdict_as_delivered"] = {"verdict": v_raw, "why": why_raw}
    res["verdict_after_fit"] = {"verdict": v_fit, "why": why_fit}

    q = [(t, e) for t, e in zip(tm, extra) if "bias" in e]
    if q:
        acc_hist, last = [], None
        for t, e in q:
            if e.get("accuracy") != last:
                acc_hist.append([float(t - tm[0]), e.get("accuracy")])
                last = e.get("accuracy")
        res["qmag"] = {"samples": len(q), "first_bias": q[0][1]["bias"], "last_bias": q[-1][1]["bias"],
                       "last_bias_raw": q[-1][1].get("bias_raw"), "accuracy_changes": acc_hist}
    if steps:
        per = []
        for s in steps:
            sel = (tm >= s["t0"]) & (tm <= s["t1"])
            if sel.sum() < 3:
                continue
            per.append({"step": s["id"], "n": int(sel.sum()), "raw": mag[sel].mean(0).tolist(),
                        "fit": corrected[sel].mean(0).tolist(),
                        "norm_fit": float(np.linalg.norm(corrected[sel], axis=1).mean())})
        res["steps"] = per
    return res, tm, mag


def pair_diffs(caps):
    out = []
    for i in range(len(caps)):
        for j in range(i + 1, len(caps)):
            (na, ta, ma), (nb, tb, mb) = caps[i], caps[j]
            if len(ta) < 30 or len(tb) < 30:
                continue
            lo, hi = max(ta[0], tb[0]), min(ta[-1], tb[-1])
            if hi - lo < 5:
                continue
            sel = (ta >= lo) & (ta <= hi)
            d = ma[sel] - interp3(ta[sel], tb, mb)
            d = d[~np.isnan(d).any(1)]
            if len(d) < 10:
                continue
            ts = ta[sel][~np.isnan(ma[sel] - interp3(ta[sel], tb, mb)).any(1)]
            late = d[ts >= hi - 0.3 * (hi - lo)]
            out.append({"a": na, "b": nb, "overlap_s": float(hi - lo),
                        "median_a_minus_b": np.median(d, 0).tolist(),
                        "iqr": (np.percentile(d, 75, 0) - np.percentile(d, 25, 0)).tolist(),
                        "late_median_a_minus_b": np.median(late, 0).tolist() if len(late) else None})
    return out


# --------------------------------------------------------------- report

def v3(v, fmt="%+.4f"):
    return "(" + ", ".join(fmt % x for x in v) + ")" if v is not None else "n/a"


def ns(s):
    return ("mean %.4f std %.4f min %.4f max %.4f spread %.1f%%" %
            (s["mean"], s["std"], s["min"], s["max"], 100 * s["spread"])) if s else "n/a"


def report(results, pairs):
    L = []
    for r in results:
        L.append("== %s: %d samples" % (r["capture"], r["n"]))
        if "error" in r:
            L.append("  " + r["error"])
            continue
        L.append("  sphere fit: offset %s G, radius %.4f G; %d/8 octants covered" %
                 (v3(r["sphere"]["offset"]), r["sphere"]["radius"], r["octants"]))
        if "ellipsoid" in r:
            e = r["ellipsoid"]
            L.append("  ellipsoid fit: offset %s G, radius %.4f G, W rows %s %s %s" %
                     (v3(e["offset"]), e["radius"], v3(e["W"][0], "%.4f"), v3(e["W"][1], "%.4f"),
                      v3(e["W"][2], "%.4f")))
        else:
            L.append("  ellipsoid fit: not an ellipsoid (too little coverage)")
        L.append("  correction used: %s%s" % (r["correction"], " (ellipsoid rejected: %s)" % "; ".join(
            r["ellipsoid_rejected"]) if r["ellipsoid_rejected"] else ""))
        L.append("  outliers: %d of %d samples dropped (|B - centre| outside median +-%d%%); fits and |B| below use the rest" %
                 (r["outliers"], r["n"], 100 * OUTLIER_FRAC))
        L.append("  |B| raw:       " + ns(r["norm_raw"]))
        L.append("  |B| sphere:    " + ns(r["norm_sphere"]))
        if "norm_ellipsoid" in r:
            L.append("  |B| ellipsoid: " + ns(r["norm_ellipsoid"]))
        L.append("  |B| all samples: raw " + ns(r["norm_raw_all"]))
        L.append("  |B| all samples: fit " + ns(r["norm_fit_all"]))
        for k, lab in (("face_norms_raw", "raw"), ("face_norms_fit", "fit")):
            f = r[k]
            if f["faces"]:
                L.append("  |B| stationary per face %s: %.4f..%.4f G (range %.1f%% of median): %s" % (
                    lab, f["min"], f["max"], 100 * f["range_frac"],
                    ", ".join("%s %.4f (n %d)" % (nm, v["mean"], v["n"]) for nm, v in f["faces"].items())))
            else:
                L.append("  |B| stationary per face %s: n/a (no stationary face samples)" % lab)
        for k, lab in (("faces_raw", "raw"), ("faces_fit", "fit")):
            f = r[k]
            L.append("  faces %s: up n=%d z %s, down n=%d z %s, sum %s" % (
                lab, f["face_up"]["n"], "%+.4f" % f["face_up"]["mean"][2] if f["face_up"]["mean"] else "n/a",
                f["face_down"]["n"], "%+.4f" % f["face_down"]["mean"][2] if f["face_down"]["mean"] else "n/a",
                "%+.4f G" % f["sum"] if f["sum"] is not None else "n/a"))
        for k, lab in (("heading_raw", "raw"), ("heading_fit", "fit")):
            h = r[k]
            if h:
                L.append("  heading %s over %s (%.1f s): gyro yaw %+.1f deg, heading %+.1f deg, max err %.1f, rms %.1f" % (
                    lab, r.get("spin_step", "?"), h["window"][1] - h["window"][0], h["gyro_yaw_deg"],
                    h["heading_change_deg"], h["max_err_deg"], h["rms_err_deg"]))
            else:
                L.append("  heading %s: n/a (no flat-turn window with data)" % lab)
        L.append("  gyro bias %s rad/s" % v3(r["gyro_bias"]))
        if "dip_deg" in r:
            L.append("  dip after fit (stationary samples, + = field points down): median %+.1f deg, std %.1f, n %d" % (
                r["dip_deg"]["median"], r["dip_deg"]["std"], r["dip_deg"]["n"]))
        if "qmag" in r:
            q = r["qmag"]
            L.append("  QMAG_CAL: %d samples with bias; first %s last %s G (raw %s); accuracy changes (s, value) %s" % (
                q["samples"], v3(q["first_bias"]), v3(q["last_bias"]), q["last_bias_raw"], q["accuracy_changes"]))
        for s in r.get("steps", []):
            L.append("    step %-10s n=%4d raw %s fit %s |fit| %.4f" % (s["step"], s["n"], v3(s["raw"]), v3(s["fit"]),
                                                                    s["norm_fit"]))
        for k, lab in (("verdict_as_delivered", "as delivered"), ("verdict_after_fit", "after fit")):
            v = r[k]
            L.append("  VERDICT %s %s: %s%s" % (r["capture"], lab, v["verdict"],
                                                 " (" + "; ".join(v["why"]) + ")" if v["why"] else ""))
            if v["verdict"] == "FAIL" and all(w.startswith("|B| spread") for w in v["why"]):
                fn = r["face_norms_fit" if k == "verdict_after_fit" else "face_norms_raw"]
                L.append("  note: only the |B| spread fails %s (faces and heading pass)%s; indoors a field "
                         "gradient over the movement volume or soft iron can do this" % (
                             lab, "; stationary faces %.4f..%.4f G" % (fn["min"], fn["max"]) if fn["faces"] else ""))
    for p in pairs:
        L.append("== %s minus %s over %.0f s: median %s G, IQR %s, last 30%% median %s G" % (
            p["a"], p["b"], p["overlap_s"], v3(p["median_a_minus_b"]), v3(p["iqr"], "%.4f"),
            v3(p["late_median_a_minus_b"])))
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir", help="directory of sensord *.jsonl captures")
    ap.add_argument("--steps", help="steps file (default: the one *steps.txt in DIR, if any)")
    ap.add_argument("--spin", help="flat-turn step: id or text substring (default: a step named turn/spin)")
    ap.add_argument("--json", help="also write all numbers here")
    a = ap.parse_args(argv)
    try:
        if not os.path.isdir(a.dir):
            raise InputError("%s: not a directory" % a.dir)
        steps_path = a.steps
        if not steps_path:
            c = sorted(glob.glob(os.path.join(a.dir, "*steps.txt")))
            if len(c) > 1:
                raise InputError("several steps files, pick one with --steps: %s" % ", ".join(c))
            steps_path = c[0] if c else None
        steps = load_steps(steps_path) if steps_path else []
        acc = load_series(a.dir, "accel")
        gyr = load_series(a.dir, "anglvel")
        mags = sorted(glob.glob(os.path.join(a.dir, "*magn*.jsonl")))
        if not mags:
            raise InputError("%s: no *magn*.jsonl captures" % a.dir)
        if len(acc[0]) < 10:
            raise InputError("%s: no accel samples (*accel*.jsonl)" % a.dir)
    except (InputError, OSError) as e:
        print("mag-cal-check: %s" % e, file=sys.stderr)
        return 2
    results, caps = [], []
    for p in mags:
        r, tm, mag = analyse(p, acc, gyr, steps, a.spin)
        results.append(r)
        caps.append((r["capture"], tm, mag))
    pairs = pair_diffs(caps)
    print("mag-cal-check: %s, %d magn capture(s), %d accel, %d gyro samples, steps: %s" % (
        a.dir, len(mags), len(acc[0]), len(gyr[0]), steps_path or "none"))
    print(report(results, pairs))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"captures": results, "pairs": pairs}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
