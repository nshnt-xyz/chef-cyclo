#!/usr/bin/env python3
"""Compass check for sensord captures (host only; stdlib + numpy).
docs/next-steps/sensors-plan.md section 8 is the plan; the guided
sensors-compass-run on the phone records the input.

    python3 tools/compass-check.py DIR [--ref DEG] [--replay] [--replay-bin PATH]
                                       [--mount MOUNT] [--calibrated-from STEP] [--json OUT]

--calibrated-from STEP treats every heading line from that step on as
calibrated (for runs whose flag was wrong, e.g. the 2026-10-01 one whose
registry signal lagged the ADSP), and says so in the report.

DIR is a capture directory with a steps.txt ("UPTIME STEP ID: TEXT (N s)"
lines and a "# clock uptime U sample_t T" line) and sensord JSON-lines
captures: *heading*.jsonl (sensord's heading channel), *rotvec*.jsonl
(the ADSP rotation vector, sensord -V), *accel*, *anglvel* and one
magnetometer capture (*magn.jsonl, else *magn-full*.jsonl).

Heading source: the recorded heading channel when there is one; with
--replay, or when there is none (the 2026-09-27 magcal captures), the
filter is run over the raw captures by compass-replay (the same compass.c
sensord runs; built by make -C tools/sensord). Both are reported when
both exist, with their difference.

Per step: circular mean, std, spread of the heading, the disturbed
fraction, the median accuracy, pitch; for the rotation vector the mean
difference against the heading. Over flat-turn windows ("turn" or
"TURN"/"circle" in the step, as in the magcal run): the heading change
against the gyro yaw integrated about the smoothed gravity axis (bias
from stationary samples), first second skipped, like mag-cal-check.py.

Acceptance (section 8, for a sensors-compass-run directory):
 1. turns: the means of holds turn.0 .. turn.360 step by 90 +- 5 deg
    (the gyro's yaw between the same holds is printed next to it, to tell
    the person's turning error from the compass's);
 2. tilt: the heading change since the tilt.flat hold minus the change
    of a gyro-propagated reference (below) within +-5 deg in each of
    tilt.up, tilt.bar and tilt.roll, scored only where the pose was
    reached: tilt.up pitch up to >= 70 deg, tilt.bar median pitch 45..75
    (aim 60), tilt.roll median pitch >= 60 and |roll| up to >= 20; n/a
    when none was;
 3. shake: the standard deviation of heading change minus reference
    change over the shake step under 3 deg;
 4. quick turn: heading change minus reference change within 2 deg of
    its final value (mean of the last 3 s) no later than 1 s after the
    turn ends (gyro yaw rate back under 10 deg/s);
 Real motion is allowed in 2-4: a hand-held phone yaws while it tilts or
 shakes (live 2026-10-01 the roll step yawed 95 deg), so the yardstick is
 not a constant heading but the reference: the attitude of the first
 heading line of the window (its heading, pitch and roll, screen not
 facing down) propagated by the gyro (stationary-median bias removed),
 its heading taken with the same mount rule;
 5. absolute: |mean(turn.0) - REF| <= 10 deg with --ref (the bearing of
    the reference direction; magnetic, or give the true bearing and the
    declination separately in your head: the heading here is magnetic).
Each is PASS / FAIL / n/a with the numbers, or NOT CALIBRATED (a
failure) when any heading line in the windows it scores is flagged
"calibrated":false: the acceptance holds after the ADSP has learned its
magnetometer bias. The per-step table shows the calibrated fraction. Exit status 0 when the
analysis ran, 2 on bad input.
"""
import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys

import numpy as np

TURN_TOL = 5.0
TILT_TOL = 5.0
TILT_POSE = {"tilt.up": "pitch up to >= 70", "tilt.bar": "median pitch 45..75, about 60",
             "tilt.roll": "median pitch >= 60, |roll| up to >= 20"}
MOUNTS = {"portrait": (True, (1.0, 0, 0)), "landscape-left": (True, (0, -1.0, 0)),
          "landscape-right": (True, (0, 1.0, 0)), "flat": (False, (0, 1.0, 0)),
          "upright": (False, (0, 0, -1.0))}
SHAKE_STD = 3.0
SETTLE_S = 1.0
SETTLE_TOL = 2.0
ABS_TOL = 10.0
MOVING_RATE = math.radians(10)
STILL_ACCEL_STD = 0.08
STILL_GYRO = 0.05	# rad/s

CLOCK_RE = re.compile(r"^#\s*clock uptime\s+(\S+)\s+sample_t\s+(\d+)")
STEP_RE = re.compile(r"^(\S+)\s+STEP\s+([^:]+):\s*(.*?)(?:\s*\((\d+(?:\.\d+)?)\s*s\))?\s*$")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_REPLAY = os.path.join(HERE, "sensord", "compass-replay")


class InputError(Exception):
    pass


def load_lines(path, sensor):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if d.get("sensor") == sensor and "t" in d:
                out.append(d)
    out.sort(key=lambda d: d["t"])
    return out


def load_vec(paths, sensor):
    t, v = [], []
    for p in paths:
        for d in load_lines(p, sensor):
            try:
                v.append((float(d["x"]), float(d["y"]), float(d["z"])))
            except (KeyError, TypeError, ValueError):
                continue
            t.append(d["t"] / 1e9)
    if not t:
        return np.zeros(0), np.zeros((0, 3))
    t = np.array(t)
    v = np.array(v)
    t, idx = np.unique(t, return_index=True)
    return t, v[idx]


def load_steps(path):
    steps, shift = [], 0.0
    with open(path, errors="replace") as f:
        for line in f:
            c = CLOCK_RE.match(line.strip())
            if c:
                off = float(c.group(1)) - int(c.group(2)) / 1e9
                if abs(off) >= 0.5:
                    shift = off
                continue
            m = STEP_RE.match(line.strip())
            if m:
                steps.append({"t0": float(m.group(1)) - shift, "id": m.group(2).strip(),
                              "text": m.group(3), "dur": float(m.group(4)) if m.group(4) else None})
    for i, s in enumerate(steps):
        nxt = steps[i + 1]["t0"] if i + 1 < len(steps) else None
        end = s["t0"] + s["dur"] if s["dur"] else nxt
        if nxt is not None and end is not None:
            end = min(end, nxt)
        s["t1"] = end if end is not None else s["t0"] + 10
    return steps


def pick(d, pattern, exclude=()):
    out = []
    for p in sorted(glob.glob(os.path.join(d, pattern))):
        if not any(x in os.path.basename(p) for x in exclude):
            out.append(p)
    return out


def magn_file(d):
    plain = pick(d, "*-magn.jsonl") + pick(d, "magn.jsonl")
    if plain:
        return plain[0]
    full = pick(d, "*magn-full*.jsonl")
    return full[0] if full else None


def replay(d, binary, mount, prefix=None):
    """Heading lines from compass-replay over DIR's raw captures (per
    phase prefix when the captures are split, e.g. a-/q-)."""
    if not os.access(binary, os.X_OK):
        raise InputError("%s not built (make -C tools/sensord compass-replay)" % binary)
    pre = (prefix + "-") if prefix else ""
    acc = pick(d, pre + "*accel*.jsonl") if prefix else pick(d, "*accel*.jsonl")
    gyr = pick(d, pre + "*anglvel*.jsonl") if prefix else pick(d, "*anglvel*.jsonl")
    if prefix:
        mg = pick(d, pre + "magn.jsonl") or pick(d, pre + "magn-full*.jsonl")
        mg = mg[0] if mg else None
    else:
        mg = magn_file(d)
    if not acc or not gyr or not mg:
        return []
    out = subprocess.run([binary, "-r", "0", "-m", mount] + acc + gyr + [mg],
                         capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        try:
            res.append(json.loads(line))
        except ValueError:
            pass
    return res


def phases(d):
    """Capture prefixes ("a", "q") when the raw captures are split by
    phase (the magcal run), else [None]."""
    pre = sorted({os.path.basename(p).split("-")[0] for p in pick(d, "*-accel.jsonl")})
    return pre if len(pre) > 1 or (pre and not os.path.exists(os.path.join(d, "accel.jsonl"))
                                   and pick(d, "*-magn-full*.jsonl")) else [None]


def circ_mean(deg):
    r = np.radians(deg)
    return math.degrees(math.atan2(np.sin(r).mean(), np.cos(r).mean())) % 360


def circ_dev(deg, ref):
    return (np.asarray(deg) - ref + 180) % 360 - 180


def hstats(rows, t0, t1, key="heading"):
    sel = [r for r in rows if t0 <= r["t"] / 1e9 <= t1 and key in r]
    if len(sel) < 3:
        return None
    h = np.array([r[key] for r in sel], float)
    m = circ_mean(h)
    dv = circ_dev(h, m)
    out = {"n": len(sel), "mean": m, "std": float(dv.std()), "min_dev": float(dv.min()),
           "max_dev": float(dv.max())}
    if "disturbed" in sel[0]:
        out["disturbed"] = float(np.mean([bool(r.get("disturbed")) for r in sel]))
        out["accuracy"] = float(np.median([r.get("accuracy", 180) for r in sel]))
        out["calibrated"] = float(np.mean([bool(r.get("calibrated")) for r in sel]))
    if "pitch" in sel[0]:
        out["pitch"] = float(np.median([r["pitch"] for r in sel]))
    return out


def moving_mean(t, v, half):
    c = np.vstack([np.zeros((1, v.shape[1])), np.cumsum(v, 0)])
    c2 = np.vstack([np.zeros((1, v.shape[1])), np.cumsum(v * v, 0)])
    lo = np.searchsorted(t, t - half, "left")
    hi = np.searchsorted(t, t + half, "right")
    n = (hi - lo)[:, None]
    mean = (c[hi] - c[lo]) / n
    var = np.maximum((c2[hi] - c2[lo]) / n - mean * mean, 0)
    return mean, np.sqrt(var.sum(1))


class Gyro:
    """Yaw about the smoothed gravity axis, integrated from the gyro."""

    def __init__(self, acc_t, acc_v, gyr_t, gyr_v):
        self.ok = len(acc_t) > 10 and len(gyr_t) > 10
        if not self.ok:
            return
        acc_s, sd = moving_mean(acc_t, acc_v, 0.25)
        sd_g = np.interp(gyr_t, acc_t, sd, left=np.inf, right=np.inf)
        # a steady flat turn keeps the accelerometer still too: only quiet
        # gyro samples estimate the bias
        still = (sd_g < STILL_ACCEL_STD) & (np.linalg.norm(gyr_v, axis=1) < STILL_GYRO)
        bias = np.median(gyr_v[still], 0) if still.sum() >= 20 else np.zeros(3)
        ug = np.stack([np.interp(gyr_t, acc_t, acc_s[:, k]) for k in range(3)], 1)
        ug /= np.linalg.norm(ug, axis=1, keepdims=True)
        w_up = ((gyr_v - bias) * ug).sum(1)
        # compass heading grows clockwise seen from above, gyro yaw anticlockwise
        self.t = gyr_t
        self.rate = -np.degrees(w_up)
        self.yaw = np.concatenate([[0], np.cumsum(0.5 * (self.rate[1:] + self.rate[:-1]) * np.diff(gyr_t))])
        self.bias = bias
        self.w = gyr_v - bias

    def at(self, t):
        return np.interp(t, self.t, self.yaw)


def turn_track(rows, gyro, t0, t1):
    t0, t1 = t0 + 1.0, t1 - 0.3
    sel = [r for r in rows if t0 <= r["t"] / 1e9 <= t1]
    if len(sel) < 10 or not gyro.ok:
        return None
    tt = np.array([r["t"] / 1e9 for r in sel])
    h = np.unwrap(np.radians([r["heading"] for r in sel]))
    dh = np.degrees(h - h[0])
    dg = gyro.at(tt) - gyro.at(tt[0])
    err = (dh - dg + 180) % 360 - 180
    return {"window": [t0, t1], "n": len(sel), "gyro_yaw": float(dg[-1]), "heading_change": float(dh[-1]),
            "max_err": float(np.abs(err).max()), "rms_err": float(np.sqrt((err ** 2).mean()))}


def settle(rows, gyro, t0, t1, mount="portrait"):
    """Quick-turn settling: the turn end from the gyro rate, then the
    first time after which heading change minus reference change stays
    within SETTLE_TOL of its final value (mean of the last 3 s)."""
    if not gyro.ok:
        return None
    gs = (gyro.t >= t0) & (gyro.t <= t1)
    gt, gr = gyro.t[gs], gyro.rate[gs]
    fast = np.abs(gr) > math.degrees(MOVING_RATE) * 3
    if not fast.any():
        return {"turned": False}
    moving = np.abs(gr) > math.degrees(MOVING_RATE)
    i0 = int(np.argmax(fast))
    k = i0
    while k < len(moving) - 1 and moving[k]:
        k += 1
    t_end = float(gt[k])
    turned = float(gyro.at(t_end) - gyro.at(float(gt[max(i0 - 5, 0)])))
    tr = ref_track(rows, gyro, t0, t1, mount)
    if tr is None or t1 - 3.0 < t_end:
        return {"turned": True, "turn_deg": turned, "t_end": t_end, "settle_s": None}
    tt, err = tr
    tail = tt >= t1 - 3.0
    if tail.sum() < 3:
        return {"turned": True, "turn_deg": turned, "t_end": t_end, "settle_s": None}
    final = float(err[tail].mean())
    off = np.abs(err - final) > SETTLE_TOL
    bad = np.where(off & (tt >= t_end))[0]
    ts = float(tt[bad[-1]] - t_end) if len(bad) else 0.0
    return {"turned": True, "turn_deg": turned, "t_end": t_end, "final_err": final,
            "max_err": float(err[np.argmax(np.abs(err))]), "settle_s": max(ts, 0.0)}


def win(s, lead=1.0, trail=0.3):
    """A step's analysis window: the first second (the phone is still
    being put down after the buzz) and the last 0.3 s dropped, less for
    short steps (the host test's)."""
    d = s["t1"] - s["t0"]
    return s["t0"] + min(lead, 0.2 * d), s["t1"] - min(trail, 0.1 * d)


def rodrigues(th):
    a = float(np.linalg.norm(th))
    if a < 1e-15:
        return np.eye(3)
    k = th / a
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K


def attitude_from_line(r, mount):
    """Body -> ENU rotation from a heading line (heading, pitch, roll),
    the screen taken as not facing down (up.z >= 0)."""
    cross, v = MOUNTS[mount]
    v = np.array(v)
    ux, uy = -math.sin(math.radians(r["roll"])), math.sin(math.radians(r["pitch"]))
    u = np.array([ux, uy, math.sqrt(max(0.0, 1 - ux * ux - uy * uy))])
    u /= np.linalg.norm(u)
    f = np.cross(u, v) if cross else v - (v @ u) * u
    if np.linalg.norm(f) < 1e-6:
        return None
    f /= np.linalg.norm(f)
    rr = np.cross(f, u)
    h = math.radians(r["heading"])
    north = math.cos(h) * f - math.sin(h) * rr
    east = math.sin(h) * f + math.cos(h) * rr
    return np.array([east, north, u])


def heading_of(R, mount):
    cross, v = MOUNTS[mount]
    vw = R @ np.array(v)
    if cross:
        return math.degrees(math.atan2(-vw[1], vw[0])) % 360
    return math.degrees(math.atan2(vw[0], vw[1])) % 360


def ref_track(rows, gyro, t_a, t_end, mount):
    """Heading change minus the change of the gyro-propagated reference,
    from the first heading line at or after t_a: (times, errors) in
    seconds / degrees, or None."""
    sel = [r for r in rows if t_a <= r["t"] / 1e9 <= t_end and "pitch" in r and "roll" in r]
    if len(sel) < 3 or not gyro.ok:
        return None
    R = attitude_from_line(sel[0], mount)
    if R is None:
        return None
    t0 = sel[0]["t"] / 1e9
    gs = np.where((gyro.t > t0) & (gyro.t <= t_end + 0.05))[0]
    gt = [t0] + [float(gyro.t[i]) for i in gs]
    refs = [heading_of(R, mount)]
    prev_w = None
    for k, i in enumerate(gs):
        w = gyro.w[i]
        dt = gt[k + 1] - gt[k]
        wm = w if prev_w is None else 0.5 * (w + prev_w)
        R = R @ rodrigues(wm * dt)
        prev_w = w
        refs.append(heading_of(R, mount))
    gt = np.array(gt)
    ru = np.degrees(np.unwrap(np.radians(refs)))
    tt = np.array([r["t"] / 1e9 for r in sel])
    hu = np.degrees(np.unwrap(np.radians([r["heading"] for r in sel])))
    rr = np.interp(tt, gt, ru)
    err = ((hu - hu[0]) - (rr - rr[0]) + 180) % 360 - 180
    return tt, err


def pose(rows, w0, w1):
    sel = [r for r in rows if w0 <= r["t"] / 1e9 <= w1 and "pitch" in r]
    if not sel:
        return None
    p = np.array([r["pitch"] for r in sel])
    ro = np.array([r.get("roll", 0) for r in sel])
    return {"pitch_max": float(p.max()), "pitch_med": float(np.median(p)), "roll_max": float(np.abs(ro).max())}


def pose_ok(sid, ps):
    if ps is None:
        return False
    if sid == "tilt.up":
        return ps["pitch_max"] >= 70
    if sid == "tilt.bar":
        return 45 <= ps["pitch_med"] <= 75
    if sid == "tilt.roll":
        return ps["pitch_med"] >= 60 and ps["roll_max"] >= 20
    return True


def by_id(steps, sid):
    for s in steps:
        if s["id"] == sid:
            return s
    return None


def acceptance(steps, rows, gyro, ref, mount="portrait"):
    acc = {}
    # 1. turns
    ids = ["turn.0", "turn.90", "turn.180", "turn.270", "turn.360"]
    holds = [by_id(steps, i) for i in ids]
    if all(holds):
        means, gy = [], []
        for s in holds:
            st = hstats(rows, *win(s))
            means.append(st["mean"] if st else None)
            gy.append(gyro.at((s["t0"] + s["t1"]) / 2) if gyro.ok else None)
        if all(m is not None for m in means):
            diffs = [float((means[i + 1] - means[i]) % 360) for i in range(4)]
            gdiffs = [float(gy[i + 1] - gy[i]) if gyro.ok else None for i in range(4)]
            ok = all(abs(x - 90) <= TURN_TOL for x in diffs)
            acc["turns"] = {"verdict": "PASS" if ok else "FAIL", "means": means, "diffs": diffs,
                            "gyro_diffs": gdiffs}
    acc.setdefault("turns", {"verdict": "n/a"})
    # 2. tilt: heading minus the gyro reference, from the tilt.flat hold on
    flat = by_id(steps, "tilt.flat")
    tilts = [s for s in steps if s["id"].startswith("tilt.")]
    if flat and tilts:
        tr = ref_track(rows, gyro, win(flat)[0], max(win(s, 0.5)[1] for s in tilts), mount)
        if tr is not None:
            tt, err = tr
            sub, scored, worst = {}, 0, 0.0
            for s in tilts:
                if s["id"] == "tilt.flat":
                    continue
                w0, w1 = win(s, 0.5)
                m = (tt >= w0) & (tt <= w1)
                ps = pose(rows, w0, w1)
                e = {"pose": ps, "pose_ok": pose_ok(s["id"], ps), "aim": TILT_POSE.get(s["id"], "")}
                if m.any():
                    e["max_err"] = float(err[m][np.argmax(np.abs(err[m]))])
                    e["rms_err"] = float(np.sqrt((err[m] ** 2).mean()))
                if e["pose_ok"] and "max_err" in e:
                    scored += 1
                    if abs(e["max_err"]) > abs(worst):
                        worst = e["max_err"]
                sub[s["id"]] = e
            v = ("PASS" if abs(worst) <= TILT_TOL else "FAIL") if scored else "n/a"
            acc["tilt"] = {"verdict": v, "max_err": worst, "steps": sub, "scored": scored,
                           "why": "" if scored else "pose not reached in any tilt step"}
    acc.setdefault("tilt", {"verdict": "n/a"})
    # 3. shake: heading minus the gyro reference
    sh = by_id(steps, "shake")
    if sh:
        w0, w1 = win(sh, 0.5)
        tr = ref_track(rows, gyro, w0, w1, mount)
        if tr is not None and len(tr[1]) >= 3:
            err = tr[1]
            st = hstats(rows, w0, w1)
            acc["shake"] = {"verdict": "PASS" if err.std() < SHAKE_STD else "FAIL", "std": float(err.std()),
                            "max_err": float(err[np.argmax(np.abs(err))]),
                            "heading_std": st["std"] if st else None,
                            "disturbed": st.get("disturbed") if st else None}
    acc.setdefault("shake", {"verdict": "n/a"})
    # 4. quick turn: heading minus the gyro reference settles
    qk = by_id(steps, "quick")
    if qk:
        se = settle(rows, gyro, qk["t0"], qk["t1"], mount)
        if se and se.get("settle_s") is not None:
            se["verdict"] = "PASS" if se["settle_s"] <= SETTLE_S else "FAIL"
        elif se:
            se["verdict"] = "n/a"
            se["why"] = "no turn seen" if not se.get("turned") else "turn ended too late in the step"
        acc["quick"] = se or {"verdict": "n/a"}
    acc.setdefault("quick", {"verdict": "n/a"})
    # 5. absolute
    r0 = by_id(steps, "turn.0")
    if r0 is not None and ref is not None:
        st = hstats(rows, *win(r0))
        if st:
            d = float(circ_dev(st["mean"], ref))
            acc["absolute"] = {"verdict": "PASS" if abs(d) <= ABS_TOL else "FAIL", "heading": st["mean"],
                               "ref": ref, "error": d}
    acc.setdefault("absolute", {"verdict": "n/a", "why": "no --ref" if ref is None else "no turn.0 hold"})
    # Section 8's acceptance holds after the ADSP has learned its bias: a
    # criterion scored on heading lines that are not all calibrated is
    # NOT CALIBRATED (a failure), whatever its numbers say.
    windows = {
        "turns": [win(s) for s in holds if s],
        "tilt": [win(s, 0.5) for s in tilts
                 if acc["tilt"].get("steps", {}).get(s["id"], {}).get("pose_ok")],
        "shake": [win(sh, 0.5)] if sh else [],
        "quick": [(qk["t0"], qk["t1"])] if qk else [],
        "absolute": [win(r0)] if r0 is not None else [],
    }
    for k, ws in windows.items():
        frac = cal_frac(rows, ws)
        if frac is None or acc[k]["verdict"] == "n/a":
            continue
        acc[k]["calibrated"] = frac
        if frac < 1.0:
            acc[k]["verdict"] = "NOT CALIBRATED"
    return acc


def cal_frac(rows, windows):
    """Fraction of the heading lines in the windows flagged calibrated
    (None without lines or without the flag)."""
    n = c = 0
    for w0, w1 in windows:
        for r in rows:
            if w0 <= r["t"] / 1e9 <= w1 and "calibrated" in r:
                n += 1
                c += bool(r["calibrated"])
    return c / n if n else None


def rotvec_diff(rows, rv, t0, t1):
    if not rv:
        return None
    sel = [r for r in rows if t0 <= r["t"] / 1e9 <= t1]
    rsel = [r for r in rv if t0 <= r["t"] / 1e9 <= t1 and "heading" in r]
    if len(sel) < 3 or len(rsel) < 3:
        return None
    ht = np.array([r["t"] / 1e9 for r in sel])
    hu = np.unwrap(np.radians([r["heading"] for r in sel]))
    rt = np.array([r["t"] / 1e9 for r in rsel])
    rh = np.array([r["heading"] for r in rsel])
    d = circ_dev(rh, np.degrees(np.interp(rt, ht, hu)) % 360)
    return {"n": len(rsel), "mean": float(np.mean(d)), "max": float(np.abs(d).max())}


def fmt_st(st):
    if not st:
        return "no data"
    s = "mean %6.1f std %4.1f dev %+5.1f..%+5.1f" % (st["mean"], st["std"], st["min_dev"], st["max_dev"])
    if "disturbed" in st:
        s += " cal %3.0f%% dist %3.0f%% acc %5.1f" % (100 * st["calibrated"], 100 * st["disturbed"],
                                                   st["accuracy"])
    if "pitch" in st:
        s += " pitch %+5.1f" % st["pitch"]
    return s


def is_turn(s):
    t = s["text"].upper()
    return s["id"].endswith(".turn") or ("CIRCLE" in t and "FIGURE" not in t)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--ref", type=float, help="bearing of the reference direction (deg)")
    ap.add_argument("--replay", action="store_true", help="also replay the raw captures")
    ap.add_argument("--replay-bin", default=DEFAULT_REPLAY)
    ap.add_argument("--mount", default="portrait")
    ap.add_argument("--calibrated-from", metavar="STEP",
                    help="treat heading lines from this step on as calibrated")
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        steps_path = os.path.join(a.dir, "steps.txt")
        if not os.path.isfile(steps_path):
            raise InputError("no steps.txt in %s" % a.dir)
        steps = load_steps(steps_path)
        live = []
        for p in pick(a.dir, "*heading*.jsonl"):
            live += load_lines(p, "heading")
        live.sort(key=lambda d: d["t"])
        rv = []
        for p in pick(a.dir, "*rotvec*.jsonl"):
            rv += load_lines(p, "rotvec")
        rv.sort(key=lambda d: d["t"])
        sources = {}
        if live:
            sources["live"] = live
        if a.replay or not live:
            rep = []
            for pre in phases(a.dir):
                rep += replay(a.dir, a.replay_bin, a.mount, pre)
            rep.sort(key=lambda d: d["t"])
            if rep:
                sources["replay"] = rep
        if not sources:
            raise InputError("no heading captures and nothing to replay in %s" % a.dir)
        cal_note = None
        if a.calibrated_from:
            cs = by_id(steps, a.calibrated_from)
            if cs is None:
                raise InputError("--calibrated-from: no step %s" % a.calibrated_from)
            n = 0
            for rows in sources.values():
                for r in rows:
                    if r["t"] / 1e9 >= cs["t0"] and r.get("calibrated") is False:
                        r["calibrated"] = True
                        n += 1
            cal_note = "calibrated flag overridden: %d heading lines from %s (%.2f) on taken as calibrated (--calibrated-from)" % (
                n, a.calibrated_from, cs["t0"])
        acc_t, acc_v = load_vec(pick(a.dir, "*accel*.jsonl"), "accel")
        gyr_t, gyr_v = load_vec(pick(a.dir, "*anglvel*.jsonl"), "anglvel")
        gyro = Gyro(acc_t, acc_v, gyr_t, gyr_v)
    except (InputError, OSError, subprocess.CalledProcessError) as e:
        print("compass-check: %s" % e, file=sys.stderr)
        return 2

    L, res = [], {"dir": a.dir, "sources": {}}
    L.append("compass-check %s: %d steps; heading from %s%s" % (
        a.dir, len(steps), " and ".join(sources), "; rotation vector" if rv else ""))
    if cal_note:
        L.append(cal_note)
    if gyro.ok:
        L.append("gyro bias (stationary median) %s rad/s" % " ".join("%+.5f" % x for x in gyro.bias))
    for name, rows in sources.items():
        r = {"steps": {}, "turns": {}}
        L.append("")
        L.append("== heading: %s (%d lines)" % (name, len(rows)))
        for s in steps:
            st = hstats(rows, *win(s, 0.5))
            d = rotvec_diff(rows, rv, *win(s, 0.5))
            r["steps"][s["id"]] = {"stats": st, "rotvec": d}
            line = "  %-12s %s" % (s["id"], fmt_st(st))
            if d:
                line += " | rotvec-heading %+5.1f (max %4.1f)" % (d["mean"], d["max"])
            L.append(line)
        for s in steps:
            if is_turn(s):
                tr = turn_track(rows, gyro, s["t0"], s["t1"])
                r["turns"][s["id"]] = tr
                if tr:
                    L.append("  flat turn %s (%.1f s): gyro yaw %+.1f deg, heading %+.1f deg, max err %.1f, rms %.1f" % (
                        s["id"], tr["window"][1] - tr["window"][0], tr["gyro_yaw"], tr["heading_change"],
                        tr["max_err"], tr["rms_err"]))
        acc = acceptance(steps, rows, gyro, a.ref, a.mount)
        r["acceptance"] = acc
        if any(v.get("verdict") != "n/a" for v in acc.values()) or by_id(steps, "turn.0"):
            n0 = len(L)
            L.append("  acceptance:")
            t = acc["turns"]
            if t["verdict"] != "n/a":
                L.append("   1 turns    %s: holds %s; steps %s (gyro %s)" % (
                    t["verdict"], " ".join("%.1f" % m for m in t["means"]),
                    " ".join("%.1f" % x for x in t["diffs"]),
                    " ".join("%.1f" % x if x is not None else "-" for x in t["gyro_diffs"])))
            else:
                L.append("   1 turns    n/a")
            t = acc["tilt"]
            if t.get("steps"):
                parts = []
                for sid, e in t["steps"].items():
                    ps = e.get("pose") or {}
                    pz = "pitch max %.0f med %.0f, |roll| max %.0f" % (
                        ps.get("pitch_max", 0), ps.get("pitch_med", 0), ps.get("roll_max", 0))
                    if e["pose_ok"] and "max_err" in e:
                        parts.append("%s %+.1f rms %.1f (%s)" % (sid, e["max_err"], e["rms_err"], pz))
                    else:
                        parts.append("%s n/a: pose not reached (%s; aim %s)%s" % (
                            sid, pz, e["aim"], ", heading-gyro max %+.1f" % e["max_err"] if "max_err" in e else ""))
                L.append("   2 tilt     %s: heading change minus gyro reference within +-%.0f; %s" % (
                    t["verdict"], TILT_TOL, "; ".join(parts)))
            else:
                L.append("   2 tilt     n/a")
            t = acc["shake"]
            L.append("   3 shake    %s%s" % (t["verdict"], ": heading minus gyro reference std %.2f deg (max %+.1f); heading std %.2f" % (
                t["std"], t["max_err"], t["heading_std"] if t["heading_std"] is not None else float("nan"))
                if t["verdict"] != "n/a" else ""))
            t = acc["quick"]
            if t.get("settle_s") is not None:
                L.append("   4 quick    %s: turned %+.1f deg (gyro), heading minus gyro reference settled within %.2f s of the turn end (tol %.0f deg), final %+.1f, max %+.1f" % (
                    t["verdict"], t["turn_deg"], t["settle_s"], SETTLE_TOL, t["final_err"], t["max_err"]))
            else:
                L.append("   4 quick    n/a%s" % (": " + t["why"] if t.get("why") else ""))
            t = acc["absolute"]
            L.append("   5 absolute %s%s" % (t["verdict"], ": heading %.1f vs ref %.1f, error %+.1f" % (
                t["heading"], t["ref"], t["error"]) if t["verdict"] != "n/a" else " (%s)" % t.get("why", "")))
            for i, k in enumerate(("turns", "tilt", "shake", "quick", "absolute")):
                if acc[k].get("calibrated", 1.0) < 1.0:
                    L[n0 + 1 + i] += " (only %.0f%% of the scored heading lines calibrated)" % (
                        100 * acc[k]["calibrated"])
        res["sources"][name] = r
    if "live" in sources and "replay" in sources:
        a_ = sources["live"]
        b_ = sources["replay"]
        bt = np.array([r["t"] / 1e9 for r in b_])
        bu = np.unwrap(np.radians([r["heading"] for r in b_]))
        at = np.array([r["t"] / 1e9 for r in a_])
        ah = np.array([r["heading"] for r in a_])
        ok = (at >= bt[0]) & (at <= bt[-1])
        if ok.any():
            d = circ_dev(ah[ok], np.degrees(np.interp(at[ok], bt, bu)) % 360)
            L.append("")
            L.append("live - replay heading: median %+.2f, p95 |.| %.2f, max |.| %.2f deg over %d lines" % (
                float(np.median(d)), float(np.percentile(np.abs(d), 95)), float(np.abs(d).max()), int(ok.sum())))
            res["live_minus_replay"] = {"median": float(np.median(d)), "max": float(np.abs(d).max())}
    print("\n".join(L))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1, default=float)
    return 0


if __name__ == "__main__":
    sys.exit(main())
