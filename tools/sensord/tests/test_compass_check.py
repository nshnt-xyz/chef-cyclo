#!/usr/bin/env python3
"""Host tests for tools/compass-check.py and, through it, the replay of
raw captures by compass-replay (compass.c, the filter sensord runs).

A phone model (device frame x right, y top, z out of the screen; world
East-North-Up; heading psi of the top edge / back of the phone, pitch
theta about x, roll phi about the horizontal forward axis) goes through
the sensors-compass-run steps. From it the test writes:

 1. a run directory with sensord-style heading lines equal to the truth
    plus chosen errors: the acceptance verdicts must come out PASS for a
    good compass and FAIL, with the right numbers, for a bad one;
 2. a run directory with only raw captures (accel/anglvel/magn at
    50/50/20 Hz with noise and a gyro bias), which compass-check replays
    with compass-replay: the recovered heading must pass the acceptance
    (replay-based regression of the filter on realistic data);
 3. a magcal-style directory (a-/q- captures, *-magn-full) with a slow
    flat turn: the replay's heading must track the gyro yaw.

With COMPASS_EVIDENCE set to a recorded magcal capture directory (the
2026-09-27 one), it is also replayed and the flat turns must track the
gyro within 12 deg (opt-in: the captures are not in the repository).
"""
import json
import math
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", "..", "compass-check.py"))
REPLAY = os.path.normpath(os.path.join(HERE, "..", "compass-replay"))
FAILS = []
B_WORLD = np.array([0.0, 0.32, -0.20])
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


def attitude(psi, theta, phi):
    """Device -> world."""
    th = math.radians(theta)
    f = [0, math.cos(th), -math.sin(th)]
    return rot([0, 0, 1], -math.radians(psi)) @ rot([1, 0, 0], th) @ rot(f, math.radians(phi))


class Run:
    """Piecewise-linear angles over time; steps recorded like the script."""

    def __init__(self):
        self.t = 1000.0
        self.keys = [(self.t, 0.0, 0.0, 0.0)]
        self.steps = []
        self.shake = []	# (t0, t1, amplitude deg)

    def to(self, dur, psi=None, theta=None, phi=None):
        _, p, th, ph = self.keys[-1]
        self.t += dur
        self.keys.append((self.t, p if psi is None else psi, th if theta is None else theta,
                          ph if phi is None else phi))

    def step(self, sid, text, dur, gap=2.0, **move):
        """gap s to move (to the step's start pose, given as move), then
        the hold, during which nothing moves unless hold_to is given."""
        hold_to = move.pop("hold_to", None)
        self.to(gap, **move)
        self.steps.append((self.t, sid, text, dur))
        if hold_to:
            for d, kw in hold_to:
                self.to(d, **kw)
            rest = dur - sum(d for d, _ in hold_to)
            if rest > 0:
                self.to(rest)
        else:
            self.to(dur)

    def angles(self, t):
        k = np.array(self.keys)
        out = [np.interp(t, k[:, 0], k[:, i]) for i in (1, 2, 3)]
        return out

    def R(self, t):
        p, th, ph = self.angles(t)
        R = attitude(p, th, ph)
        for t0, t1, a in self.shake:
            if t0 <= t <= t1:
                # smooth on and off over 0.5 s: an attitude step would be
                # an infinite rate for the gyro
                a *= min(1.0, (t - t0) / 0.5, (t1 - t) / 0.5)
                R = R @ rot([1, 0, 0], math.radians(a * math.sin(2 * math.pi * 7 * t))) \
                      @ rot([0, 0, 1], math.radians(a * math.sin(2 * math.pi * 5 * t + 1)))
        return R


def standard_run(ref=30.0, quick_dur=0.3, handheld=False):
    """The script's steps. handheld: as live 2026-10-01, the person yaws
    while tilting and shaking, holds the handlebar pose at 29 deg and
    rolls at 23 deg pitch (poses not reached), with a 95 deg yaw."""
    if handheld:
        return handheld_run(ref, quick_dur)
    r = Run()
    r.step("turn.0", "FLAT, top edge at your REFERENCE mark, still", 10, psi=ref)
    for i, sid in enumerate(["turn.90", "turn.180", "turn.270", "turn.360"]):
        r.step(sid, "turn 90 deg CLOCKWISE to the next mark, flat, still", 10, psi=ref + 90 * (i + 1))
    r.step("tilt.flat", "FLAT at the reference, still", 10)
    r.step("tilt.up", "same way: SLOWLY tilt up to UPRIGHT and back, twice", 20,
           hold_to=[(5, {"theta": 90}), (5, {"theta": 0}), (5, {"theta": 90}), (5, {"theta": 0})])
    r.step("tilt.bar", "same way: HANDLEBAR POSE, still", 8, theta=60)
    r.step("tilt.roll", "same way, UPRIGHT: roll SLOWLY left and right", 15, theta=90,
           hold_to=[(4, {"phi": 30}), (7, {"phi": -30}), (4, {"phi": 0})])
    r.step("shake", "same way: TAP and SHAKE the phone", 10, theta=0, phi=0)
    r.shake.append((r.steps[-1][0], r.steps[-1][0] + 10, 1.0))
    # the quick turn: 90 deg in quick_dur with a smooth (smoothstep) rate
    # profile, as a hand turns
    prof = [(0.5, {})]
    for i in range(1, 11):
        x = i / 10
        prof.append((quick_dur / 10, {"psi": ref + 360 + 90 * (3 * x * x - 2 * x * x * x)}))
    r.step("quick", "FLAT at the reference; at the LONG BUZZ turn 90 deg CLOCKWISE QUICKLY, then hold still", 8,
           hold_to=prof)
    r.to(2)
    return r


def handheld_run(ref, quick_dur):
    r = Run()
    r.step("turn.0", "FLAT, top edge at your REFERENCE mark, still", 10, psi=ref)
    for i, sid in enumerate(["turn.90", "turn.180", "turn.270", "turn.360"]):
        r.step(sid, "turn 90 deg CLOCKWISE to the next mark, flat, still", 10, psi=ref + 90 * (i + 1))
    r.step("tilt.flat", "FLAT at the reference, still", 10)
    base = ref + 360
    r.step("tilt.up", "same way: SLOWLY tilt up to UPRIGHT and back, twice", 20,
           hold_to=[(5, {"theta": 88, "psi": base + 15}), (5, {"theta": 0, "psi": base - 10}),
                    (5, {"theta": 85, "psi": base + 20}), (5, {"theta": 0, "psi": base})])
    r.step("tilt.bar", "same way: HANDLEBAR POSE, still", 8, theta=29, psi=base + 4)
    r.step("tilt.roll", "same way, UPRIGHT: roll SLOWLY left and right", 15, theta=23,
           hold_to=[(5, {"phi": 15, "psi": base + 50}), (5, {"phi": -15, "psi": base + 95}),
                    (5, {"phi": 0, "psi": base + 60})])
    r.step("shake", "same way: TAP and SHAKE the phone", 10, theta=0, phi=0, psi=base,
           hold_to=[(2.5, {"psi": base + 12}), (2.5, {"psi": base - 8}), (2.5, {"psi": base + 10}),
                    (2.5, {"psi": base})])
    r.shake.append((r.steps[-1][0], r.steps[-1][0] + 10, 1.0))
    prof = [(0.5, {})]
    for i in range(1, 11):
        x = i / 10
        prof.append((quick_dur / 10, {"psi": base + 90 * (3 * x * x - 2 * x * x * x)}))
    r.step("quick", "FLAT at the reference; at the LONG BUZZ turn 90 deg CLOCKWISE QUICKLY, then hold still", 8,
           hold_to=prof)
    r.to(2)
    return r


def write_steps(d, r):
    with open(os.path.join(d, "steps.txt"), "w") as f:
        f.write("# sensors-compass-run test; step times: /proc/uptime\n")
        f.write("# clock uptime %.2f sample_t %d\n" % (r.keys[0][0], int(r.keys[0][0] * 1e9)))
        for t, sid, text, dur in r.steps:
            f.write("%.2f STEP %s: %s (%g s)\n" % (t, sid, text, dur))


def raw(d, r, rng, prefix="", magn_name="magn.jsonl", gbias=(0.002, -0.001, 0.003)):
    t0, t1 = r.keys[0][0], r.keys[-1][0]
    fa = open(os.path.join(d, prefix + "accel.jsonl"), "w")
    fg = open(os.path.join(d, prefix + "anglvel.jsonl"), "w")
    fm = open(os.path.join(d, prefix + magn_name), "w")
    t = t0
    k = 0
    while t < t1:
        R = r.R(t)
        a = R.T @ np.array([0, 0, G]) + rng.normal(0, 0.03, 3)
        h = 0.0005
        dR = r.R(t - h).T @ r.R(t + h)
        w = np.array([dR[2, 1] - dR[1, 2], dR[0, 2] - dR[2, 0], dR[1, 0] - dR[0, 1]]) / (4 * h)
        w = w + np.array(gbias) + rng.normal(0, 0.002, 3)
        ns = int(round(t * 1e9))
        fa.write(json.dumps({"sensor": "accel", "t": ns + 1, "x": a[0], "y": a[1], "z": a[2]}) + "\n")
        fg.write(json.dumps({"sensor": "anglvel", "t": ns, "x": w[0], "y": w[1], "z": w[2]}) + "\n")
        if k % 5 == 0:
            m = R.T @ B_WORLD + rng.normal(0, 0.002, 3)
            fm.write(json.dumps({"sensor": "magn", "t": ns + 2, "x": m[0], "y": m[1], "z": m[2]}) + "\n")
        t += 0.02
        k += 1
    for f in (fa, fg, fm):
        f.close()


def truth_heading(R):
    fw = np.cross([0, 0, 1], R @ np.array([1.0, 0, 0]))
    return math.degrees(math.atan2(fw[0], fw[1])) % 360


def live(d, r, rng, err=lambda t, sid: 0.0, noise=0.2, lag=0.0, cal=lambda t, sid: True):
    """Heading lines: truth (lag s late) + err(t, step) + noise, the
    calibrated flag from cal(t, step)."""
    t0, t1 = r.keys[0][0], r.keys[-1][0]
    sid_at = []
    for t, sid, _, dur in r.steps:
        sid_at.append((t, t + dur, sid))
    with open(os.path.join(d, "heading.jsonl"), "w") as f:
        t = t0
        while t < t1:
            R = r.R(max(t - lag, t0))
            u = R.T @ np.array([0, 0, 1.0])
            sid = next((s for a, b, s in sid_at if a <= t <= b), "")
            h = (truth_heading(R) + err(t, sid) + rng.normal(0, noise)) % 360
            f.write(json.dumps({"sensor": "heading", "t": int(round(t * 1e9)), "heading": round(h, 2),
                                "pitch": round(math.degrees(math.asin(u[1])), 2),
                                "roll": round(math.degrees(math.asin(max(-1.0, min(1.0, -u[0])))), 2),
                                "calibrated": bool(cal(t, sid)), "disturbed": False,
                                "accuracy": 3.0 if cal(t, sid) else 180.0}) + "\n")
            t += 0.04


def run_tool(d, *args):
    p = subprocess.run([sys.executable, TOOL, d, "--json", os.path.join(d, "out.json")] + list(args),
                       capture_output=True, text=True)
    out = p.stdout + p.stderr
    res = None
    if os.path.exists(os.path.join(d, "out.json")):
        with open(os.path.join(d, "out.json")) as f:
            res = json.load(f)
    return p.returncode, out, res


def test_live_good(tmp, rng):
    d = os.path.join(tmp, "good")
    os.mkdir(d)
    r = standard_run(ref=30.0)
    write_steps(d, r)
    raw(d, r, rng)
    live(d, r, rng, err=lambda t, sid: 2.0 if sid == "turn.180" else 0.0)
    rc, out, res = run_tool(d, "--ref", "33")
    check(rc == 0, "good: exit %d: %s" % (rc, out[-400:]))
    acc = res["sources"]["live"]["acceptance"]
    check(acc["turns"]["verdict"] == "PASS", "good: turns %s" % acc["turns"])
    diffs = acc["turns"]["diffs"]
    check(abs(diffs[1] - 92) < 0.5 and abs(diffs[2] - 88) < 0.5, "good: turn diffs %s" % diffs)
    check(all(abs(g - 90) < 1.5 for g in acc["turns"]["gyro_diffs"]), "good: gyro diffs %s" % acc["turns"]["gyro_diffs"])
    check(acc["tilt"]["verdict"] == "PASS" and abs(acc["tilt"]["max_err"]) < 1.5 and acc["tilt"]["scored"] == 3,
          "good: tilt %s" % acc["tilt"])
    check(acc["tilt"]["steps"]["tilt.up"]["pose"]["pitch_max"] > 85, "good: tilt.up pose %s" % acc["tilt"]["steps"])
    check(acc["shake"]["verdict"] == "PASS" and acc["shake"]["std"] < 1.5, "good: shake %s" % acc["shake"])
    check(acc["quick"]["verdict"] == "PASS" and acc["quick"]["settle_s"] < 0.2 and
          abs(acc["quick"]["turn_deg"] - 90) < 3 and abs(acc["quick"]["final_err"]) < 1.5, "good: quick %s" % acc["quick"])
    check(acc["absolute"]["verdict"] == "PASS" and abs(acc["absolute"]["error"] + 3) < 0.5,
          "good: absolute %s" % acc["absolute"])
    check("   1 turns    PASS" in out and "   4 quick    PASS" in out, "good: report text")
    # without --ref: n/a
    rc, out, res = run_tool(d)
    check(res["sources"]["live"]["acceptance"]["absolute"]["verdict"] == "n/a", "good: no --ref")
    # --replay too: both sources, and their difference
    rc, out, res = run_tool(d, "--replay", "--ref", "30")
    check(rc == 0 and "replay" in res["sources"], "good --replay: %s" % out[-300:])
    check("live - replay heading" in out, "good --replay: difference line")


def test_not_calibrated(tmp, rng):
    """Section 8's acceptance holds after the bias is learned: heading
    lines flagged not calibrated in a scored window make that criterion
    NOT CALIBRATED, even with perfect numbers."""
    d = os.path.join(tmp, "nocal")
    os.mkdir(d)
    r = standard_run(ref=30.0)
    write_steps(d, r)
    raw(d, r, rng)
    live(d, r, rng, cal=lambda t, sid: False)
    rc, out, res = run_tool(d, "--ref", "30")
    acc = res["sources"]["live"]["acceptance"]
    for k in ("turns", "tilt", "shake", "quick", "absolute"):
        check(acc[k]["verdict"] == "NOT CALIBRATED" and acc[k]["calibrated"] == 0.0,
              "nocal: %s %s" % (k, acc[k]))
    check("PASS" not in out and "(only 0% of the scored heading lines calibrated)" in out, "nocal: report text")
    check(res["sources"]["live"]["steps"]["turn.0"]["stats"]["calibrated"] == 0.0, "nocal: per-step fraction")
    # calibrated only from tilt.flat on (the turns were not): turns and
    # absolute flagged, the rest scored normally
    d = os.path.join(tmp, "halfcal")
    os.mkdir(d)
    write_steps(d, r)
    raw(d, r, rng)
    t_flat = next(t for t, sid, _, _ in r.steps if sid == "tilt.flat")
    live(d, r, rng, cal=lambda t, sid: t >= t_flat - 1)
    rc, out, res = run_tool(d, "--ref", "30")
    acc = res["sources"]["live"]["acceptance"]
    check(acc["turns"]["verdict"] == "NOT CALIBRATED" and acc["absolute"]["verdict"] == "NOT CALIBRATED",
          "halfcal: turns %s absolute %s" % (acc["turns"]["verdict"], acc["absolute"]["verdict"]))
    for k in ("tilt", "shake", "quick"):
        check(acc[k]["verdict"] == "PASS" and acc[k]["calibrated"] == 1.0, "halfcal: %s %s" % (k, acc[k]))


def test_handheld(tmp, rng):
    """Real yaw while tilting and shaking is not an error: an accurate
    compass passes; poses not reached are n/a, not scored."""
    d = os.path.join(tmp, "handheld")
    os.mkdir(d)
    r = standard_run(ref=30.0, handheld=True)
    write_steps(d, r)
    raw(d, r, rng)
    live(d, r, rng)
    rc, out, res = run_tool(d)
    acc = res["sources"]["live"]["acceptance"]
    st = acc["tilt"]["steps"]
    check(acc["tilt"]["verdict"] == "PASS" and acc["tilt"]["scored"] == 1 and st["tilt.up"]["pose_ok"],
          "handheld: tilt %s" % acc["tilt"])
    check(not st["tilt.bar"]["pose_ok"] and not st["tilt.roll"]["pose_ok"], "handheld: poses %s" % st)
    check("tilt.roll n/a: pose not reached" in out and "aim median pitch >= 60, |roll| up to >= 20" in out,
          "handheld: report: %s" % [l for l in out.splitlines() if "2 tilt" in l])
    check(acc["shake"]["verdict"] == "PASS" and acc["shake"]["std"] < 1.5 and acc["shake"]["heading_std"] > 5,
          "handheld: shake %s" % acc["shake"])
    check(acc["quick"]["verdict"] == "PASS", "handheld: quick %s" % acc["quick"])
    # the same motion with a compass that drags 8 deg during the roll: still
    # n/a there (pose), but a tilt.up error is caught
    d = os.path.join(tmp, "handheld-bad")
    os.mkdir(d)
    write_steps(d, r)
    raw(d, r, rng)
    live(d, r, rng, err=lambda t, sid: 8.0 * math.sin(t) if sid == "tilt.up" else 0.0)
    rc, out, res = run_tool(d)
    check(res["sources"]["live"]["acceptance"]["tilt"]["verdict"] == "FAIL", "handheld-bad: tilt")
    # --calibrated-from: lines flagged uncalibrated before tilt.flat, then
    # the flag overridden from turn.0 on
    d = os.path.join(tmp, "calfrom")
    os.mkdir(d)
    write_steps(d, r)
    raw(d, r, rng)
    live(d, r, rng, cal=lambda t, sid: False)
    rc, out, res = run_tool(d, "--calibrated-from", "turn.0")
    acc = res["sources"]["live"]["acceptance"]
    check("NOT CALIBRATED" not in out and acc["turns"]["verdict"] == "PASS", "calfrom: %s" % acc["turns"])
    check("heading lines from turn.0 (" in out and "taken as calibrated (--calibrated-from)" in out, "calfrom: note")
    rc, out, res = run_tool(d, "--calibrated-from", "tilt.flat")
    acc = res["sources"]["live"]["acceptance"]
    check(acc["turns"]["verdict"] == "NOT CALIBRATED" and acc["shake"]["verdict"] == "PASS",
          "calfrom tilt.flat: %s %s" % (acc["turns"]["verdict"], acc["shake"]["verdict"]))
    p = subprocess.run([sys.executable, TOOL, d, "--calibrated-from", "nosuch"], capture_output=True, text=True)
    check(p.returncode == 2 and "no step nosuch" in p.stderr, "calfrom bad step")


def test_live_bad(tmp, rng):
    d = os.path.join(tmp, "bad")
    os.mkdir(d)
    r = standard_run(ref=200.0)
    write_steps(d, r)
    raw(d, r, rng)

    def err(t, sid):
        if sid == "turn.270":
            return -8.0
        if sid in ("tilt.up", "tilt.roll"):
            return 9.0 * math.sin(t)
        return 0.0

    def noisy(t, sid):
        return err(t, sid) + (rng.normal(0, 6) if sid == "shake" else 0)
    live(d, r, rng, err=noisy, lag=1.5)
    rc, out, res = run_tool(d, "--ref", "220")
    acc = res["sources"]["live"]["acceptance"]
    check(acc["turns"]["verdict"] == "FAIL", "bad: turns %s" % acc["turns"])
    check(acc["tilt"]["verdict"] == "FAIL" and abs(acc["tilt"]["max_err"]) > 5, "bad: tilt %s" % acc["tilt"])
    check(acc["shake"]["verdict"] == "FAIL" and acc["shake"]["std"] > 3, "bad: shake %s" % acc["shake"])
    check(acc["quick"]["verdict"] == "FAIL" and acc["quick"]["settle_s"] > 1.0, "bad: quick %s" % acc["quick"])
    # (the 1.5 s lag leaves the tail of the move to the reference in the hold)
    check(acc["absolute"]["verdict"] == "FAIL" and abs(acc["absolute"]["error"] + 20) < 3, "bad: absolute %s" % acc["absolute"])


def test_replay_run(tmp, rng):
    """Raw captures only: compass-check replays them (filter regression)."""
    d = os.path.join(tmp, "replay")
    os.mkdir(d)
    r = standard_run(ref=123.0)
    write_steps(d, r)
    raw(d, r, rng)
    rc, out, res = run_tool(d, "--ref", "123")
    check(rc == 0 and "heading from replay" in out, "replay: %s" % out[-400:])
    acc = res["sources"]["replay"]["acceptance"]
    for k in ("turns", "tilt", "shake", "quick", "absolute"):
        check(acc[k]["verdict"] == "PASS", "replay: %s %s" % (k, acc[k]))
    check(abs(acc["absolute"]["error"]) < 2, "replay: absolute error %s" % acc["absolute"])
    check(all(abs(x - 90) < 2 for x in acc["turns"]["diffs"]), "replay: turn diffs %s" % acc["turns"]["diffs"])


def test_magcal_dir(tmp, rng):
    """A magcal-style directory: a-/q- phases, *-magn-full, slow turns."""
    d = os.path.join(tmp, "magcal")
    os.mkdir(d)
    steps = []
    for ph in ("a", "q"):
        r = Run()
        r.t = 1000.0 if ph == "a" else 2000.0
        r.keys = [(r.t, 10.0, 0.0, 0.0)]
        r.step(ph + ".up", "FACE UP on the table, still", 10)
        r.step(ph + ".turn", "FACE UP on table, 1 slow full circle", 30,
               hold_to=[(28, {"psi": 10 + 360 + 45})])
        steps += r.steps
        raw(d, r, rng, prefix=ph + "-", magn_name="magn-full.jsonl")
        with open(os.path.join(d, ph + "-magn-factory.jsonl"), "w") as f:
            f.write("")
    write_steps(d, type("S", (), {"keys": [(1000.0,)], "steps": steps})())
    rc, out, res = run_tool(d)
    check(rc == 0 and "heading from replay" in out, "magcal: %s" % out[-400:])
    for ph in ("a", "q"):
        tr = res["sources"]["replay"]["turns"].get(ph + ".turn")
        check(tr is not None and abs(tr["gyro_yaw"] - tr["heading_change"]) < 3 and tr["max_err"] < 3,
              "magcal: %s.turn %s" % (ph, tr))
        st = res["sources"]["replay"]["steps"][ph + ".up"]["stats"]
        check(st is not None and abs((st["mean"] - 10 + 180) % 360 - 180) < 2 and st["std"] < 0.5,
              "magcal: %s.up %s" % (ph, st))


def test_bad_input(tmp):
    d = os.path.join(tmp, "empty")
    os.mkdir(d)
    p = subprocess.run([sys.executable, TOOL, d], capture_output=True, text=True)
    check(p.returncode == 2 and "no steps.txt" in p.stderr, "empty dir: %d %s" % (p.returncode, p.stderr))
    with open(os.path.join(d, "steps.txt"), "w") as f:
        f.write("1000.0 STEP turn.0: x (10 s)\n")
    p = subprocess.run([sys.executable, TOOL, d], capture_output=True, text=True)
    check(p.returncode == 2 and "nothing to replay" in p.stderr, "no captures: %d %s" % (p.returncode, p.stderr))


def test_evidence():
    ev = os.environ.get("COMPASS_EVIDENCE")
    if not ev:
        return
    with tempfile.NamedTemporaryFile(suffix=".json") as j:
        p = subprocess.run([sys.executable, TOOL, ev, "--json", j.name], capture_output=True, text=True)
        check(p.returncode == 0, "evidence: %s" % p.stderr[-300:])
        with open(j.name) as f:
            res = json.load(f)
    for sid, tr in res["sources"]["replay"]["turns"].items():
        check(tr and tr["max_err"] < 12, "evidence %s: %s" % (sid, tr))
    print("evidence replay: " + ", ".join("%s max err %.1f rms %.1f" % (k, v["max_err"], v["rms_err"])
                                          for k, v in res["sources"]["replay"]["turns"].items()))


def main():
    if not os.access(REPLAY, os.X_OK):
        print("test_compass_check.py: %s not built" % REPLAY, file=sys.stderr)
        return 1
    rng = np.random.default_rng(7)
    with tempfile.TemporaryDirectory() as tmp:
        test_live_good(tmp, rng)
        test_live_bad(tmp, rng)
        test_not_calibrated(tmp, rng)
        test_handheld(tmp, rng)
        test_replay_run(tmp, rng)
        test_magcal_dir(tmp, rng)
        test_bad_input(tmp)
    test_evidence()
    if FAILS:
        print("test_compass_check.py: %d failure(s)" % len(FAILS))
        return 1
    print("test_compass_check.py: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
