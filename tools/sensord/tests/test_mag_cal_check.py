#!/usr/bin/env python3
"""Host tests for tools/mag-cal-check.py on synthetic captures with a
known answer: a phone model (device frame x right, y top, z out of the
screen, as sensord publishes) is put through the guided run's six faces,
a figure-8 and a slow flat turn in an Earth field of 0.25 G north and
0.40 G down; the accelerometer and gyro are consistent with the motion,
and the magnetometer sees D (field) + offset + noise for a chosen
soft-iron matrix D and hard-iron offset. Checks: the fits recover the
offset and undo D, the verdicts (FAIL as delivered with an offset, PASS
after the fit), the face and heading checks catch a sign error, steps
in both the new and the 2026-09-26 shape, QMAG bias fields and the
between-capture difference.
"""
import json
import math
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", "..", "mag-cal-check.py"))
FAILS = []
B_WORLD = np.array([0.0, 0.25, -0.40])	# ENU: north 0.25 G, down 0.40 G
G = 9.80665


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: %s" % what, file=sys.stderr)


def rot(axis, ang):
    axis = np.asarray(axis, float)
    axis = axis / np.linalg.norm(axis)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * K @ K


# Device -> world rotations for the faces, before a heading turn about up.
FACES = [
    ("up", "FACE UP on the table", np.eye(3)),
    ("down", "FACE DOWN on the table", rot([0, 1, 0], math.pi)),
    ("top", "PORTRAIT, top edge up", rot([1, 0, 0], math.pi / 2)),
    ("bottom", "PORTRAIT, top edge down", rot([1, 0, 0], -math.pi / 2)),
    ("left", "LANDSCAPE, left edge down", rot([0, 1, 0], -math.pi / 2)),
    ("right", "LANDSCAPE, right edge down", rot([0, 1, 0], math.pi / 2)),
]


class Phone:
    """Records orientation R(t) (device -> world) and body rates."""

    def __init__(self, rng):
        self.rng = rng
        self.t = 100.0
        self.segs = []	# (t0, t1, fn(t) -> (R, w_dev))
        self.steps = []

    def hold(self, R, dur):
        t0 = self.t
        self.segs.append((t0, t0 + dur, lambda t, R=R: (R, np.zeros(3))))
        self.t += dur

    def turn(self, R0, dur, total):
        """Rotate about world up at a constant rate, starting at R0."""
        t0 = self.t
        w = total / dur

        def f(t, R0=R0, t0=t0, w=w):
            R = rot([0, 0, 1], w * (t - t0)) @ R0
            return R, R.T @ np.array([0, 0, w])
        self.segs.append((t0, t0 + dur, f))
        self.t += dur

    def wave(self, R0, dur):
        """Figure-8-like tumbling: body rates from a few sinusoids,
        integrated so the gyro and the orientation agree."""
        t0 = self.t
        dt = 0.002
        ts = np.arange(0, dur + dt, dt)
        ph = self.rng.uniform(0, 2 * math.pi, 6)
        Rs, ws = [], []
        R = R0
        for tt in ts:
            w = np.array([2.2 * math.sin(1.3 * tt + ph[0]) + 0.8 * math.sin(3.1 * tt + ph[1]),
                          2.0 * math.sin(0.9 * tt + ph[2]) + 0.7 * math.sin(2.3 * tt + ph[3]),
                          1.5 * math.sin(0.7 * tt + ph[4]) + 0.9 * math.sin(1.9 * tt + ph[5])])
            Rs.append(R)
            ws.append(w)
            R = R @ rot(w, np.linalg.norm(w) * dt) if np.linalg.norm(w) > 0 else R
        Rs, ws = np.array(Rs), np.array(ws)

        def f(t, t0=t0):
            i = min(int(round((t - t0) / dt)), len(Rs) - 1)
            return Rs[i], ws[i]
        self.segs.append((t0, t0 + dur, f))
        self.t += dur

    def step(self, sid, text, dur, fn, *args):
        self.steps.append("%.2f STEP %s: %s (%d s)" % (self.t, sid, text, dur))
        fn(*args)

    def at(self, t):
        for t0, t1, f in self.segs:
            if t0 <= t < t1:
                return f(t)
        return None


def guided(rng, turn_deg=360):
    ph = Phone(rng)
    for sid, text, R in FACES:
        ph.step("a." + sid, text, 6, ph.hold, rot([0, 0, 1], rng.uniform(0, 2 * math.pi)) @ R, 6)
    ph.step("a.fig8", "FIGURE-8 in the air", 20, ph.wave, np.eye(3), 20)
    ph.step("a.turn", "FACE UP, turn one slow circle", 20, ph.turn,
            rot([0, 0, 1], 0.3), 20, math.radians(turn_deg))
    ph.hold(np.eye(3), 1)
    return ph


def write(d, ph, rng, D, off, magn_name="m-magn-full.jsonl", noise=0.002, steps=True,
          qmag=None, axis_bug=False, old_steps=False, clock_shift=0.0, outliers=0, gain=None,
          extra=None):
    os.makedirs(d, exist_ok=True)
    t_end = ph.t
    gb = np.array([0.004, -0.003, 0.006])	# gyro bias, rad/s

    def lines(sensor, hz, fn):
        out = []
        for t in np.arange(100.0, t_end, 1.0 / hz):
            s = ph.at(t)
            if s is None:
                continue
            v = fn(*s)
            rec = {"sensor": sensor, "t": int(round(t * 1e9)), "x": v[0], "y": v[1], "z": v[2]}
            if sensor == "magn" and qmag:
                rec.update(qmag(t))
            out.append(json.dumps(rec))
        return out

    acc = lines("accel", 50, lambda R, w: R.T @ np.array([0, 0, G]) + rng.normal(0, 0.02, 3))
    gyr = lines("anglvel", 50, lambda R, w: w + gb + rng.normal(0, 0.002, 3))

    def magf(R, w, t=None):
        g = gain(t, R) if gain and t is not None else 1.0
        bw = g * B_WORLD + (extra(t) if extra and t is not None else 0)
        b = D @ (R.T @ bw) + off + rng.normal(0, noise, 3)
        if axis_bug:	# x and y swapped: a left-handed frame
            b = np.array([b[1], b[0], b[2]])
        return b
    mag = []
    for t in np.arange(100.0, t_end, 1.0 / 20):
        st = ph.at(t)
        if st is None:
            continue
        v = magf(*st, t=t)
        rec = {"sensor": "magn", "t": int(round(t * 1e9)), "x": v[0], "y": v[1], "z": v[2]}
        if qmag:
            rec.update(qmag(t))
        mag.append(json.dumps(rec))
    # gross outliers: a few samples at three times the field (a magnet)
    for k in range(outliers):
        rec = json.loads(mag[400 + 37 * k])
        for a in "xyz":
            rec[a] = rec[a] * 3 + 0.5
        mag[400 + 37 * k] = json.dumps(rec)
    with open(os.path.join(d, "m-accel.jsonl"), "w") as f:
        f.write("\n".join(['{"hello":"sensord","proto":1}'] + acc[:5] + ['{"dropped":3}'] + acc[5:]) + "\n")
    with open(os.path.join(d, "m-anglvel.jsonl"), "w") as f:
        f.write("\n".join(gyr) + "\n")
    with open(os.path.join(d, magn_name), "w") as f:
        f.write("\n".join(mag) + "\n")
    if steps:
        st = ph.steps
        if old_steps:	# the 2026-09-26 g-steps.txt shape
            st = [s.replace("STEP a.up:", "STEP 1: FLAT").replace("STEP a.down:", "STEP 2: FLAT")
                  .replace("STEP a.turn: FACE UP, turn one slow circle", "STEP 7: FLAT FACE UP, slowly spin one full turn on the table")
                  for s in st if "a.up" in s or "a.down" in s or "a.turn" in s]
        if clock_shift:	# uptime ahead of the sample clock (a suspend)
            st = ["%.2f%s" % (float(x.split(" ", 1)[0]) + clock_shift, " " + x.split(" ", 1)[1]) for x in st]
            st.insert(0, "# clock uptime %.3f sample_t %d" % (100.0 + clock_shift, int(100.0 * 1e9)))
        with open(os.path.join(d, "m-steps.txt"), "w") as f:
            f.write("99.00 starting in 1\n" + "\n".join(st) + "\n")


def run(d, *extra):
    js = os.path.join(d, "out.json")
    p = subprocess.run([sys.executable, TOOL, d, "--json", js] + list(extra),
                       capture_output=True, text=True)
    res = json.load(open(js)) if p.returncode == 0 and os.path.exists(js) else None
    return p.returncode, p.stdout + p.stderr, res


def cap(res, name):
    for c in res["captures"]:
        if c["capture"] == name:
            return c
    return None


def main():
    rng = np.random.default_rng(7)
    tmp = tempfile.mkdtemp(prefix="magcal.")
    off = np.array([0.05, -0.10, 0.25])

    # 1. hard iron only: the fit recovers the offset; as delivered fails the
    #    face check, after the fit everything passes
    d = os.path.join(tmp, "hard")
    ph = guided(rng)
    write(d, ph, rng, np.eye(3), off)
    rc, out, res = run(d)
    check(rc == 0, "hard: exit %d: %s" % (rc, out))
    if res:
        c = cap(res, "m-magn-full")
        check(np.allclose(c["sphere"]["offset"], off, atol=0.005), "hard: sphere offset %s" % c["sphere"]["offset"])
        check(abs(c["sphere"]["radius"] - np.linalg.norm(B_WORLD)) < 0.005, "hard: radius")
        check(c["correction"] == "ellipsoid" and c["octants"] == 8, "hard: ellipsoid used (%s, %d octants)" %
              (c["correction"], c["octants"]))
        check(np.allclose(c["ellipsoid"]["offset"], off, atol=0.005), "hard: ellipsoid offset")
        check(np.allclose(np.array(c["ellipsoid"]["W"]), np.eye(3), atol=0.03), "hard: W not identity")
        check(c["verdict_as_delivered"]["verdict"] == "FAIL", "hard: as delivered %s" % c["verdict_as_delivered"])
        check(any("z up+down +0.500" in w for w in c["verdict_as_delivered"]["why"]),
              "hard: face sum reason %s" % c["verdict_as_delivered"]["why"])
        check(c["verdict_after_fit"]["verdict"] == "PASS", "hard: after fit %s" % c["verdict_after_fit"])
        f = c["faces_fit"]
        check(abs(f["face_up"]["mean"][2] + 0.40) < 0.01 and abs(f["sum"]) < 0.01, "hard: faces %s" % f)
        h = c["heading_fit"]
        # 18.7 s of the 20 s turn are compared (the first second is skipped)
        check(h and abs(h["gyro_yaw_deg"] - (-360 * 18.7 / 20)) < 3 and h["max_err_deg"] < 3,
              "hard: heading %s" % h)
        check(abs(c["dip_deg"]["median"] - math.degrees(math.atan2(0.40, 0.25))) < 1, "hard: dip %s" % c["dip_deg"])
        check(c.get("spin_step") == "a.turn", "hard: spin step %s" % c.get("spin_step"))
        check(len(c["steps"]) == 8, "hard: per-step table")
        check("VERDICT m-magn-full after fit: PASS" in out, "hard: verdict line")

    # 2. soft iron as well: only the ellipsoid undoes it
    d = os.path.join(tmp, "soft")
    D = np.array([[1.15, 0.05, 0.0], [0.05, 0.90, 0.02], [0.0, 0.02, 1.05]])
    write(d, guided(rng), rng, D, off)
    rc, out, res = run(d)
    check(rc == 0, "soft: exit %d" % rc)
    if res:
        c = cap(res, "m-magn-full")
        check(c["norm_sphere"]["spread"] > 0.08, "soft: sphere spread %s" % c["norm_sphere"]["spread"])
        check(c["norm_ellipsoid"]["spread"] < 0.02, "soft: ellipsoid spread %s" % c["norm_ellipsoid"]["spread"])
        check(np.allclose(c["ellipsoid"]["offset"], off, atol=0.01), "soft: offset %s" % c["ellipsoid"]["offset"])
        WD = np.array(c["ellipsoid"]["W"]) @ D
        WD /= np.cbrt(np.linalg.det(WD))
        # W undoes D up to a rotation and scale: (W D)^T (W D) ~ I
        check(np.allclose(WD.T @ WD, np.eye(3), atol=0.03), "soft: W does not undo D")
        check(c["verdict_after_fit"]["verdict"] == "PASS", "soft: after fit %s" % c["verdict_after_fit"])

    # 3. x/y swapped (an axis-map bug): no fit can mend a left-handed
    #    frame; the heading turns the wrong way
    d = os.path.join(tmp, "axis")
    write(d, guided(rng), rng, np.eye(3), off, axis_bug=True)
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        v = c["verdict_after_fit"]
        check(v["verdict"] == "FAIL" and any("heading" in w for w in v["why"]) and
              c["heading_fit"]["heading_change_deg"] > 300, "axis: %s %s" % (v, c["heading_fit"]))

    # 4. half a turn only: heading still checked over what there is; no
    #    steps file: faces from the accelerometer, heading n/a -> INCOMPLETE
    d = os.path.join(tmp, "nosteps")
    write(d, guided(rng), rng, np.eye(3), off, steps=False)
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        check(c["faces_fit"]["face_up"]["n"] > 50 and c["faces_fit"]["face_down"]["n"] > 50,
              "nosteps: faces from accel %s" % c["faces_fit"])
        check(c["verdict_after_fit"]["verdict"] == "INCOMPLETE" and
              c["verdict_after_fit"]["why"] == ["no flat-turn window"], "nosteps: %s" % c["verdict_after_fit"])

    # 5. the 2026-09-26 steps shape, --spin by text; QMAG fields; a second
    #    capture of the same motion minus a known bias
    d = os.path.join(tmp, "old")
    ph = guided(rng)

    def q(t):
        return {"bias": [0.01, 0.02, 0.03], "bias_raw": [1310, 655, -1966],
                "accuracy": 0 if t < 130 else 3}
    write(d, ph, np.random.default_rng(1), np.eye(3), off, old_steps=True, qmag=q)
    write(d, ph, np.random.default_rng(1), np.eye(3), off + np.array([0.01, 0.02, 0.03]),
          magn_name="m-magn-factory.jsonl", old_steps=True)
    rc, out, res = run(d, "--spin", "spin one full turn")
    check(rc == 0, "old: exit %d %s" % (rc, out))
    if res:
        c = cap(res, "m-magn-full")
        check(c.get("spin_step") == "7" and c["heading_fit"] and c["heading_fit"]["max_err_deg"] < 3,
              "old: spin %s %s" % (c.get("spin_step"), c["heading_fit"]))
        check(c["faces_fit"]["face_up"]["n"] > 50, "old: face up from FLAT FACE UP step")
        check(c["qmag"]["last_bias"] == [0.01, 0.02, 0.03] and
              [a for _, a in c["qmag"]["accuracy_changes"]] == [0, 3], "old: qmag %s" % c.get("qmag"))
        p = res["pairs"]
        check(len(p) == 1 and p[0]["a"] == "m-magn-factory" and
              np.allclose(p[0]["median_a_minus_b"], [0.01, 0.02, 0.03], atol=0.003) and
              np.allclose(p[0]["late_median_a_minus_b"], [0.01, 0.02, 0.03], atol=0.003), "old: pair diff %s" % p)

    # 6. step times on a clock 250 s ahead (suspended earlier): shifted back
    d = os.path.join(tmp, "clock")
    write(d, guided(rng), rng, np.eye(3), off, clock_shift=250.0)
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        check("250.0 s ahead" in out and c.get("spin_step") == "a.turn" and
              c["verdict_after_fit"]["verdict"] == "PASS", "clock: %s %s" % (c["verdict_after_fit"], out[:300]))

    # 7. gross outliers: dropped (and counted), the fit and verdict unhurt;
    #    stationary |B| per face all equal after the fit
    d = os.path.join(tmp, "outl")
    write(d, guided(rng), rng, np.eye(3), off, outliers=8)
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        check(c["outliers"] == 8, "outl: count %s" % c["outliers"])
        check(np.allclose(c["sphere"]["offset"], off, atol=0.005), "outl: offset %s" % c["sphere"]["offset"])
        check(c["verdict_after_fit"]["verdict"] == "PASS", "outl: %s" % c["verdict_after_fit"])
        check(c["norm_fit_all"]["max"] > 1.0, "outl: all-sample stats keep them")
        fn = c["face_norms_fit"]
        check(len(fn["faces"]) == 6 and fn["range_frac"] < 0.01, "outl: face norms %s" % fn)
        check(c["face_norms_raw"]["range_frac"] > 0.3, "outl: raw face norms %s" % c["face_norms_raw"])
        check("8 of " in out and "stationary per face fit" in out, "outl: report lines")

    # 8. the field strength wanders during the figure-8 only (an indoor
    #    gradient): faces and heading pass, |B| spread alone fails -> note
    d = os.path.join(tmp, "grad")
    ph = guided(rng)
    fig = [x for x in ph.steps if "a.fig8" in x][0]
    t8 = float(fig.split()[0])
    write(d, ph, rng, np.eye(3), off,
          gain=lambda t, R: 1 + (0.15 * math.sin(2 * t) if t8 <= t < t8 + 20 else 0))
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        v = c["verdict_after_fit"]
        check(v["verdict"] == "FAIL" and all(w.startswith("|B| spread") for w in v["why"]), "grad: %s" % v)
        check("note: only the |B| spread fails after fit" in out, "grad: no note line")
    check("note: only" not in run(os.path.join(tmp, "hard"))[1], "note line on a PASS")

    # 9. a field gradient over the movement volume: during the figure-8 a
    #    world-vertical field that comes and goes (the phone swung nearer a
    #    source) smears the cloud; a free ellipsoid fits it squashed and
    #    off-centre (as live on 2026-09-27). It must be rejected, with the
    #    reasons, and the sphere kept because it is the better of the two.
    d = os.path.join(tmp, "bend")
    brng = np.random.default_rng(7)
    ph = guided(brng)
    t8 = float([x for x in ph.steps if "a.fig8" in x][0].split()[0])
    write(d, ph, brng, np.eye(3), off,
          extra=lambda t: np.array([0, 0, -0.35]) * (0.5 + 0.5 * math.sin(0.3 * t))
          if t8 <= t < t8 + 20 else np.zeros(3))
    rc, out, res = run(d)
    if res:
        c = cap(res, "m-magn-full")
        why = c["ellipsoid_rejected"]
        check(c["correction"] == "sphere", "bend: ellipsoid accepted: %s" % c.get("ellipsoid"))
        check(any("W eigenvalues" in w for w in why) and any("centre" in w for w in why) and
              any("face up+down" in w for w in why), "bend: reasons %s" % why)
        check("correction used: sphere (ellipsoid rejected: W eigenvalues" in out, "bend: reason not printed")
        # the faces with the sphere are better than with the rejected ellipsoid
        W, ce = np.array(c["ellipsoid"]["W"]), np.array(c["ellipsoid"]["offset"])
        fu, fd = c["faces_raw"]["face_up"]["mean"], c["faces_raw"]["face_down"]["mean"]
        ell_sum = (W @ (np.array(fu) - ce))[2] + (W @ (np.array(fd) - ce))[2]
        check(abs(c["faces_fit"]["sum"]) < abs(ell_sum), "bend: sphere faces %s vs ellipsoid %s" % (
            c["faces_fit"]["sum"], ell_sum))
    # ... while a clean capture still uses the ellipsoid (soft iron, case 2)
    check(cap(run(os.path.join(tmp, "soft"))[2], "m-magn-full")["correction"] == "ellipsoid",
          "soft: ellipsoid no longer used")

    # 10. bad input
    rc, out, _ = run(os.path.join(tmp, "missing"))
    check(rc == 2 and "not a directory" in out, "missing dir: %d %s" % (rc, out))
    os.makedirs(os.path.join(tmp, "empty"))
    rc, out, _ = run(os.path.join(tmp, "empty"))
    check(rc == 2 and "no *magn*.jsonl" in out, "empty dir: %d %s" % (rc, out))

    if FAILS:
        print("test_mag_cal_check.py: %d failure(s) (data in %s)" % (len(FAILS), tmp))
        return 1
    subprocess.run(["rm", "-rf", tmp])
    print("test_mag_cal_check.py: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
