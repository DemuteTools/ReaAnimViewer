#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Story 10-8o: decides the disagreements between the draft references and the detector, from the
# joint data alone, so that Antho judges by ear in REAPER instead of reviewing each one on the page
# (2026-10-17). A disagreement is what the review page (tools/bench/review_page.html,
# disagreements()) queues: per clip, per configuration (default: whole-foot step / slide / pivot
# and every other kind; parts: heel / toe step / slide / pivot), per row (ref_draft.identity), the
# non-deleted reference lines and the bench's found events (fixtures/_bench/review.json) are
# matched one-to-one, smallest |err| first, within 150 ms. Then:
#   draft   a draft line with no match       -> real: confirmed (source claude); not: deleted
#   rav     a found event with no line       -> real: a confirmed line added at the evidence's
#                                               time (source claude); not: nothing written
#   far     a draft matched > 60 ms away     -> the line moved to the evidence's time, confirmed
# Genuinely ambiguous evidence leaves the draft as it is and is listed as `unsure`. Confirmed,
# deleted and antho lines are never touched. Every decision is logged with its numbers in
# fixtures/_bench/decisions.tsv.
#
# Independent of the detector on purpose: it reads only the dump's joint positions, with the
# rules below and ref_draft's reading helpers. It never calls, ports or copies motion_physics.*,
# auto_* or their constants (the reference must not share the bugs of what it judges).
#
#   decide.py [--fixtures=<dir>] [--review=<review.json>] [--dry] [--clip=<id>]... [--verbose]
#
# Body units: L = leg length (thigh + shin, from the dump); speeds in L/s; heights reported in cm.
# The ground: on an in-place clip (treadmill, in-place loop) the floor moves at the world velocity
# w (ref_draft.world_velocity: the mode of the end parts' velocities; horizontally over a sliding
# 1.5 s window, so a belt that changes speed is followed); every speed below is taken
# relative to w (a belt-carried foot is still). On a root-motion clip w = 0.
#
# Rules (S = |v - w|, Vy = vertical speed relative to w; smoothing 25 ms):
#   part down       S < 0.40 L/s and |Vy| < 0.20 L/s, or (on the local floor: within 0.03 L of
#                   the part's own 5th-percentile height over +-0.6 s, and |Vy| < 0.25 L/s and
#                   S < 1.3 L/s: a landing that slides); gaps under 40 ms filled.
#   contact         a run of `part down` of 80 ms or more (a real contact holds; a brush does not).
#   arrival         the part came down to the contact: it was 1.0 cm or more higher in the 150 ms
#                   before, or fell at 0.25 L/s or more in the 100 ms before.
#   step            real when a part of the foot (heel, ball / toe tip; one part for heel / toe)
#                   starts a contact within 150 ms of the event, with an arrival. Its time: the
#                   start of that contact (whole foot: the first part down). A whole-foot step
#                   also needs the foot not already in contact 40 ms before (a heel dropping
#                   behind a planted toe is a heel step, not a new step).
#   lift            real when the foot's contact (its parts' contacts joined, gaps < 80 ms filled)
#                   ends within 150 ms, held 80 ms or more, and the foot then rises 1.5 cm or more
#                   or travels 0.08 L or more in the next 200 ms. Its time: the contact's end. A
#                   lift within 40 ms of a real jump is not added (Jump replaces the lift-off).
#   jump / land     a flight (no foot in contact) of 100 ms or more starting (jump) or ending
#                   (land) within 150 ms of the event, with the hips moving ballistically: the
#                   fitted parabola's curvature -0.35 g or less, or the hips falling 0.10 L or
#                   more. Time: the flight's start (the last foot leaving) / end (the first foot
#                   touching). A land is real only when the body lands on its feet (a foot
#                   contact starts within 100 ms of the flight's end).
#   grab / release  the hand comes to rest (S < 0.30 L/s) for 100 ms or more, starting (grab) /
#                   ending (release) within 150 ms, after (grab) / before (release) moving at
#                   0.6 L/s or more, and the hand is on something: the hips move 0.10 L or more
#                   relative to the resting hand (a support: rung, rope, wall, floor), or the hand
#                   rests within 0.15 L of the floor, or by the hips (within 0.45 L: a holster).
#                   A hand at rest in the air, away from the body, is a gesture end: not real.
#   pivot           a foot contact around the event in which the foot's heading (heel to ball,
#                   from above, ignoring a steep foot) turns 20 deg or more at 30 deg/s or more,
#                   the turn starting within 150 ms; heel / toe: that part down during the turn.
#                   Time: the turn's start.
#   slide           a part on the local floor (within 0.015 L, |Vy| < 0.15 L/s: it stays down) moving
#                   at 0.40 L/s or more relative to w for 60 ms or more and 0.04 L or more in all,
#                   within 150 ms. Time: its start.
#   hand_pivot      always unsure (no rule).
#   unsure          evidence on the threshold's edge (within 20% of a decisive threshold), or a
#                   contact cut by the clip's edge; the line stays a draft.

import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref_draft import (find_bones, identity, intervals, read_dump, read_ref, smooth,  # noqa: E402
                       velocity, world_velocity, write_ref)

WIN = 0.150        # evidence search window around the event (s)
FAR = 0.060        # the page's "far" threshold (s)
MATCH = 0.150      # the bench's match window (s)


def emits(idn, cfg):
    if idn[0] in ("step", "slide", "pivot"):
        return idn[1] == "any" if cfg == "default" else idn[1] in ("heel", "toe")
    return cfg == "default"


# ---- Disagreements (the page's disagreements()) --------------------------------------------------

def disagreements(lines, clip):
    ev = [l for l in lines if l["status"] != "deleted"]
    out = []
    for cfg in ("default", "parts"):
        rows = {}
        for l in ev:
            i = identity(l)
            if emits(i, cfg):
                rows.setdefault(i, ([], []))[0].append(l)
        for f in clip.get("found_" + cfg, []):
            i = identity({"kind": f["k"], "side": f["s"], "part": "any"})
            if emits(i, cfg):
                rows.setdefault(i, ([], []))[1].append(f)
        for i, (R, D) in rows.items():
            pr = sorted((abs(f["t"] - e["t"]), a, b) for a, e in enumerate(R) for b, f in enumerate(D)
                        if abs(f["t"] - e["t"]) <= MATCH + 1e-9)
            ur, ud = {}, set()
            for _, a, b in pr:
                if a in ur or b in ud:
                    continue
                ur[a] = b
                ud.add(b)
            for a, e in enumerate(R):
                if e["status"] != "draft":
                    continue
                if a not in ur:
                    out.append({"type": "draft", "id": i, "t": e["t"], "line": e})
                elif abs(D[ur[a]]["t"] - e["t"]) > FAR:
                    out.append({"type": "far", "id": i, "t": e["t"], "line": e, "rav_t": D[ur[a]]["t"]})
            for b, f in enumerate(D):
                if b not in ud:
                    out.append({"type": "rav", "id": i, "t": f["t"], "found": f})
    out.sort(key=lambda x: (x["t"], x["id"]))
    return out


# ---- The motion ------------------------------------------------------------------------------------

class Motion:
    z_up = False

    def __init__(self, path):
        d = read_dump(path)
        self.rate = r = d["rate"]
        self.lo, self.hi = d["lo"], d["hi"]
        B = find_bones(d["names"])
        self.B = B
        pos = d["pos"]
        # Up axis: the dumps are Y up, but a Z-up export (UE Manny) is read as such: when the hips
        # stand above the heels along +Z at the first frame (and in the median), Z becomes Y.
        if "heel_L" in B and "heel_R" in B:
            u0 = pos[0, B["hips"]] - (pos[0, B["heel_L"]] + pos[0, B["heel_R"]]) / 2
            um = np.median(pos[:, B["hips"]] - (pos[:, B["heel_L"]] + pos[:, B["heel_R"]]) / 2, axis=0)
            if u0[2] > 2 * abs(u0[1]) and um[2] > 2 * abs(um[1]):
                pos = np.stack([pos[..., 0], pos[..., 2], -pos[..., 1]], axis=-1)
                self.z_up = True
        self.n = n = pos.shape[0]
        sm = max(1, int(round(0.025 * r)))
        self.P = {k: smooth(pos[:, i, :], sm) for k, i in B.items()}
        self.V = {k: velocity(p, r) for k, p in self.P.items()}
        legs = []
        for s in "LR":
            if all(k in B for k in ("upleg_" + s, "knee_" + s, "heel_" + s)):
                legs.append(float(np.median(np.linalg.norm(self.P["upleg_" + s] - self.P["knee_" + s], axis=1)
                                            + np.linalg.norm(self.P["knee_" + s] - self.P["heel_" + s], axis=1))))
        self.L = L = float(np.mean(legs)) if legs else 0.9
        ends = [k for k in B if k.split("_")[0] in ("heel", "ball", "tip", "hand")]
        # Root motion: the hips travel half a leg or more, horizontally or vertically (a climb that
        # really rises has no moving ground; ref_draft looks only horizontally).
        rm = float(np.linalg.norm(self.P["hips"][-1] - self.P["hips"][0])) >= 0.5 * L
        self.w = np.zeros(3) if rm else world_velocity([self.V[k] for k in ends], L)
        # A belt that changes speed (Treadmill Running: 3.5 -> 7 L/s): the horizontal world
        # velocity per sample, the same mode over a sliding 1.5 s window (several strides), every
        # 0.25 s, interpolated; the vertical one stays the clip's (heights use it).
        self.wt = np.tile(self.w, (self.n, 1))
        if not rm:
            hw, step = int(0.75 * r), max(1, int(0.25 * r))
            cs = list(range(0, self.n, step))
            ws = [world_velocity([self.V[k][max(0, c - hw):c + hw + 1] for k in ends], L) for c in cs]
            for a in (0, 2):
                self.wt[:, a] = np.interp(np.arange(self.n), cs, [w[a] for w in ws])
        self.down, self.contact, self.hloc, self.still, self.imp = {}, {}, {}, {}, {}
        for s in "LR":
            for part in ("heel", "toe"):
                k = self.key(part, s)
                if k:
                    self._part(k)
            self.foot_contact_mask(s)

    def key(self, part, s):
        if part == "heel":
            return "heel_" + s if "heel_" + s in self.B else None
        if part == "toe":
            for p in ("ball_", "tip_"):
                if p + s in self.B:
                    return p + s
            return None
        if part == "hand":
            return "hand_" + s if "hand_" + s in self.B else None
        return None

    def i(self, t):
        return int(min(self.n - 1, max(0, round(t * self.rate))))

    def t(self, i):
        return i / self.rate

    def rel(self, k):
        return self.V[k] - self.wt

    def yrel(self, k):
        """The part's height relative to the moving ground (a vertical belt: an in-place climb)."""
        return self.P[k][:, 1] - self.w[1] * np.arange(self.n) / self.rate

    def local_floor(self, k):
        y = self.yrel(k)
        h = int(0.6 * self.rate)
        f = np.array([np.percentile(y[max(0, i - h):i + h + 1], 5) for i in range(0, self.n, 6)])
        return np.interp(np.arange(self.n), np.arange(0, self.n, 6), f)

    def _part(self, k):
        L, r = self.L, self.rate
        v = self.rel(k)
        S = np.linalg.norm(v, axis=1)
        vy = np.abs(v[:, 1])
        h = self.yrel(k) - self.local_floor(k)
        self.hloc[k] = h
        still = (S < 0.35 * L) & (vy < 0.15 * L)
        m = still | ((h < 0.025 * L) & (vy < 0.30 * L) & (S < 1.3 * L))
        self.still[k] = still
        runs = fill(intervals(m), int(0.030 * r))
        self.down[k] = runs
        out = []
        for a, b in runs:
            edge = a <= int(0.03 * r) or b >= self.n - int(0.03 * r)
            if b - a >= int(0.080 * r) or (edge and b - a >= int(0.020 * r)):
                # the touchdown: the first frame really on the ground (not still settling)
                i = a
                while i < min(b - 1, a + int(0.06 * r)) and not (h[i] < 0.012 * L or still[i]):
                    i += 1
                out.append([i, b])
        self.contact[k] = out

    def foot_contact_mask(self, s):
        m = np.zeros(self.n, dtype=bool)
        for part in ("heel", "toe"):
            k = self.key(part, s)
            if k:
                for a, b in self.contact[k]:
                    m[a:b] = True
        runs = fill(intervals(m), int(0.080 * self.rate))
        self.contact["foot_" + s] = runs
        return runs

    def any_contact(self):
        m = np.zeros(self.n, dtype=bool)
        for s in "LR":
            for a, b in self.contact.get("foot_" + s, []):
                m[a:b] = True
        return m

    def cm(self, x):
        """A body-unit length in cm of a 0.9 m leg (comparable across scaled rigs)."""
        return 90.0 * x / self.L


def fill(runs, gap):
    out = []
    for a, b in runs:
        if out and a - out[-1][1] < gap:
            out[-1][1] = b
        else:
            out.append([a, b])
    return out


def near_runs(runs, i, win, end=False):
    """Runs whose start (end) lies within `win` frames of i, nearest first."""
    c = [(abs((b - 1 if end else a) - i), a, b) for a, b in runs if abs((b - 1 if end else a) - i) <= win]
    return [(a, b) for _, a, b in sorted(c)]


# ---- Evidence per kind ---------------------------------------------------------------------------
# Each returns (verdict, time, reason): verdict True (real), False (not), None (unsure).

def impacts(M, k):
    """The part's touchdowns: the frames where a descent ends (vertical speed rising through
    -0.05 L/s) after coming down 1.0 cm or more in 150 ms or at 0.25 L/s or more in 100 ms, the
    part then staying within 1.5 cm of that height, on the local floor (0.03 L) or still (a rung).
    Each: (frame, descent cm, fall L/s, hold s, height cm, horizontal speed L/s, cut by the end)."""
    if k in M.imp:
        return M.imp[k]
    r, L = M.rate, M.L
    y = M.yrel(k)
    v = M.rel(k)
    vy = v[:, 1]
    tol = 1.5 * L / 90.0
    out = []
    h = M.hloc[k]
    for a, b in intervals(vy < -0.05 * L):
        # the descent ends where the part stops falling, or reaches the floor slowing down
        i = b
        if b >= M.n - int(0.06 * r):  # near the clip's end: reaching the floor slowing down is the touchdown
            for j in range(a, b):
                if h[j] <= 0.012 * L and vy[j] >= -0.15 * L:
                    i = j
                    break
        if i >= M.n:
            continue
        # the impact: where the fall is mostly braked (a quarter of its peak speed, 0.12 L/s at
        # least) within 0.012 L of the rest height; a slow settle after it is not the sound
        pk = a + int(np.argmin(vy[a:i + 1]))
        peak = float(-vy[pk])
        for j in range(pk, i):
            if -vy[j] <= max(0.25 * peak, 0.12 * L) and y[j] - y[i] <= 0.012 * L:
                i = j
                break
        pre = y[max(0, i - int(0.15 * r)):i + 1]
        drop = M.cm(float(np.max(pre) - y[i]))
        fall = float(-np.min(vy[max(a, i - int(0.10 * r)):i + 1])) / L
        j = i
        while j < M.n and y[j] <= y[i] + tol:
            j += 1
        hold = (j - i) / r
        still = M.still[k][i:min(M.n, i + int(0.10 * r))]
        grounded = h[i] <= 0.03 * L or int(np.sum(still)) >= int(0.04 * r)
        cut = j >= M.n
        # planted: the part slows under 1.0 L/s along the ground within 150 ms, or it hit hard
        # (5 cm+ at 1 L/s+: a landing that slides); a part brushing past at speed is a swing
        Sx = np.linalg.norm(v[i:min(M.n, i + int(0.15 * r)) + 1], axis=1)
        planted = (len(Sx) > 0 and float(np.min(Sx)) < 1.0 * L) or (drop >= 5.0 and fall >= 1.0)
        if (drop >= 1.0 or fall >= 0.25) and grounded and planted and (hold >= 0.080 or (cut and hold >= 0.020)):
            if not any(abs(o[0] - i) <= int(0.06 * r) for o in out):
                out.append((i, drop, fall, hold, M.cm(float(h[i])), float(np.hypot(v[i, 0], v[i, 2])) / L, cut, "descent"))
    # A stop on a support off the floor (a rung, a wall, a rope: the part arrives sideways or from
    # below): moving at 0.8 L/s or more in the 150 ms before (0.5-0.8: a soft stop, unsure), then
    # still for 80 ms or more, 0.08 L or more above the local floor (a heel hovering behind a
    # planted toe is not on a support). Not within 100 ms of a touchdown above.
    S = np.linalg.norm(v, axis=1)
    for a, b in fill(intervals(M.still[k]), int(0.030 * r)):
        if a == 0 or (b - a < int(0.080 * r) and b < M.n) or b - a < int(0.020 * r):
            continue
        pre = float(np.max(S[max(0, a - int(0.15 * r)):a + 1])) / L
        if pre < 0.5 or M.hloc[k][a] <= 0.08 * L or any(abs(o[0] - a) <= int(0.10 * r) for o in out):
            continue
        out.append((a, M.cm(float(np.max(y[max(0, a - int(0.15 * r)):a + 1]) - y[a])), pre, (b - a) / r,
                    M.cm(float(M.hloc[k][a])), float(np.hypot(v[a, 0], v[a, 2])) / L, b >= M.n,
                    "stop" if pre >= 0.8 else "soft stop"))
    out.sort()
    M.imp[k] = out
    return out


def ev_step(M, s, part, t, wide=False):
    r, i0, win = M.rate, M.i(t), int((0.300 if wide else WIN) * M.rate)
    parts = ["heel", "toe"] if part == "any" else [part]
    cands = []
    for p in parts:
        k = M.key(p, s)
        if not k:
            continue
        for im in impacts(M, k):
            if abs(im[0] - i0) <= win and im[0] > 0:
                cands.append((abs(im[0] - i0), p, k) + im)
    what = "foot" if part == "any" else part
    if not cands:
        return False, None, "no touchdown of the %s within 150 ms (no descent of 1 cm+ ending on the floor and holding 80 ms, no stop on a support)" % what
    if part == "any":
        # the foot's first touchdown of this contact: the earliest of the parts close together
        near_ = min(c[0] for c in cands)
        cands = [c for c in cands if c[0] <= near_ + int(0.06 * r)]
        c = min(cands, key=lambda c: c[3])
    else:
        c = min(cands)
    _, p, k, i, drop, fall, hold, h, hs, cut, how = c
    why = "%s touches down at %.3f (%s): came down %.1f cm / %.2f L/s (speed in), holds %.0f ms%s, %.1f cm above local floor, %.2f L/s along" % (
        p, M.t(i), how, drop, fall, hold * 1000, " (to the clip's end)" if cut else "", h, hs)
    if part == "any":
        # not a new step when the other part was already down (a heel dropping behind a planted
        # toe is a heel step, not a step of the foot)
        for q in ("heel", "toe"):
            kq = M.key(q, s)
            if not kq or kq == k:
                continue
            # the other part has been down for the 90 ms before, planted (still half that time) or
            # having touched down at the start of that contact (a landing that slides)
            a0 = max(0, i - int(0.09 * r))
            run = [(a, b) for a, b in M.down[kq] if a <= a0 and b >= i]
            if run and i - a0 >= int(0.08 * r):
                ra = run[0][0]
                still = float(np.mean(M.still[kq][a0:i]))
                prior = [o[0] for o in impacts(M, kq) if ra - int(0.03 * r) <= o[0] < i]
                if still >= 0.5 or prior:
                    return False, M.t(i), why + "; the %s was already down since %.3f (not a new step)" % (q, M.t(ra))
    if how == "descent" and drop < 1.3 and fall < 0.20:
        return None, M.t(i), why + "; a small descent (1.0-1.3 cm, slow)"
    if how == "soft stop":
        return None, M.t(i), why + "; a soft arrival on a support (0.5-0.8 L/s)"
    return True, M.t(i), why


def ev_lift(M, s, t):
    r, i0, win = M.rate, M.i(t), int(WIN * M.rate)
    runs = M.contact["foot_" + s]
    c = near_runs(runs, i0, win, end=True)
    if not c:
        return False, None, "no foot contact ending within 150 ms"
    a, b = c[0]
    if b >= M.n:
        return False, None, "contact runs to the clip's end"
    dur = (b - a) / r
    rise, travel = 0.0, 0.0
    for p in ("heel", "toe"):
        k = M.key(p, s)
        if not k:
            continue
        y = M.yrel(k)
        seg = slice(b - 1, min(M.n, b + int(0.2 * r)))
        rise = max(rise, float(np.max(y[seg]) - y[b - 1]))
        rel = M.P[k][seg] - M.P[k][b - 1] - np.outer(np.arange(len(M.P[k][seg])) / r, M.w)
        travel = max(travel, float(np.max(np.linalg.norm(rel, axis=1))))
    why = "foot down %.0f ms from %.3f to %.3f, then rises %.1f cm / travels %.2f L in 200 ms" % (
        dur * 1000, M.t(a), M.t(b - 1), M.cm(rise), travel / M.L)
    if dur < 0.060 and a > int(0.03 * r):
        return False, M.t(b - 1), why + "; contact too short"
    if M.cm(rise) >= 2.0 or travel >= 0.15 * M.L:
        return True, M.t(b - 1), why
    if M.cm(rise) >= 1.2 or travel >= 0.10 * M.L:
        return None, M.t(b - 1), why + "; a small departure"
    return False, M.t(b - 1), why + "; the foot does not leave"


def flights(M):
    """Both feet off for 80 ms or more: (start, end, hips curvature g, hips fall L). A flight from
    the clip's start has no take-off but can end in a landing."""
    r = M.rate
    out = []
    for a, b in intervals(~M.any_contact()):
        if (b - a) / r < 0.080:
            continue
        hy = M.P["hips"][a:b, 1]
        tt = np.arange(b - a) / r
        curv = np.polyfit(tt, hy, 2)[0] * 2.0 / 9.81 if len(hy) >= 5 else 0.0
        fall = float(np.max(hy) - hy[-1]) / M.L
        out.append((a, b, curv, fall))
    return out


def ev_jump(M, t, land=False, wide=False):
    r, i0, win = M.rate, M.i(t), int((0.400 if wide else WIN) * M.rate)
    best = None
    for a, b, curv, fall in flights(M):
        if not land and a == 0:
            continue
        dur = (b - a) / r
        ballistic = (dur >= 0.200 and (curv <= -0.35 or fall >= 0.10)) or (curv <= -0.35 and fall >= 0.10)
        d = abs((b if land else a - 1) - i0)
        # nearest first; a wide search takes only a real flight (not a run's stride)
        if d <= win and (not wide or (ballistic and dur >= 0.100)) and (best is None or d < best[0]):
            best = (d, a, b, curv, fall)
    if best is None:
        allf = [(a, b) for a, b in intervals(~M.any_contact()) if abs((b if land else a) - i0) <= win]
        msg = "no flight of 80 ms+ %s within %d ms" % ("ending" if land else "starting", 400 if wide else 150)
        if allf:
            a, b = allf[0]
            msg += " (both feet off %.0f ms from %.3f)" % ((b - a) / r * 1000, M.t(a))
        return False, None, msg
    _, a, b, curv, fall = best
    dur = (b - a) / r
    why = "both feet off %.0f ms (%.3f-%.3f), hips curvature %.2f g, hips fall %.2f L" % (
        dur * 1000, M.t(a), M.t(b), curv, fall)
    # A jump: 200 ms+ off with the hips ballistic (curvature -0.35 g or less) or falling 0.10 L+;
    # a shorter flight needs both (a run's stride: ~100-150 ms off, the hips hardly falling).
    ballistic = (dur >= 0.200 and (curv <= -0.35 or fall >= 0.10)) or (dur >= 0.100 and curv <= -0.35 and fall >= 0.10)
    edge = (dur >= 0.080 and curv <= -0.35 and fall >= 0.08) or (dur >= 0.200 and (curv <= -0.28 or fall >= 0.08))
    if land:
        if b >= M.n:
            return False, None, why + "; flight runs to the clip's end"
        starts = [c for s in "LR" for c, d in M.contact["foot_" + s] if abs(c - b) <= int(0.1 * r)]
        if not starts:
            return False, M.t(b), why + "; no foot lands (a fall onto the body)"
        tt = M.t(b)
    else:
        tt = M.t(a - 1)
    if not ballistic:
        if edge:
            return None, tt, why + "; on the edge of a jump"
        return False, tt, why + "; not a jump (too short, or the hips not ballistic: a stride)"
    return True, tt, why


def ev_hand(M, s, t, release=False):
    r, L, i0, win = M.rate, M.L, M.i(t), int(WIN * M.rate)
    k = M.key("hand", s)
    if not k:
        return False, None, "no hand bone"
    S = np.linalg.norm(M.rel(k), axis=1)
    holds = [ab for ab in fill(intervals(S < 0.30 * L), int(0.040 * r)) if ab[1] - ab[0] >= int(0.100 * r)]
    c = near_runs(holds, i0, win, end=release)
    if not c:
        return False, None, "the hand does not rest 100 ms+ %s within 150 ms" % ("ending" if release else "starting")
    a, b = c[0]
    if (release and b >= M.n) or (not release and a == 0):
        return False, None, "the rest is cut by the clip's edge"
    seg = S[b:min(M.n, b + int(0.25 * r))] if release else S[max(0, a - int(0.25 * r)):a]
    move = float(np.max(seg)) / L if len(seg) else 0.0
    # how far the hand goes (comes from) in the 300 ms after (before) the rest, relative to the ground
    j0 = b - 1 if release else a
    idx = np.arange(j0, min(M.n, j0 + int(0.3 * r))) if release else np.arange(max(0, j0 - int(0.3 * r)), j0 + 1)
    off = M.P[k][idx] - M.P[k][j0] - np.outer((idx - j0) / r, M.w)
    travel = float(np.max(np.linalg.norm(off, axis=1))) / L if len(idx) else 0.0
    relh = M.P["hips"][a:b] - M.P[k][a:b]
    support = float(np.max(np.linalg.norm(relh - relh[0], axis=1))) / L
    yk = M.P[k][a:b, 1]
    floor = min(float(np.percentile(M.P[f][:, 1], 3)) for f in ("heel_L", "heel_R", "ball_L", "ball_R") if f in M.P)
    above = (float(np.median(yk)) - floor) / L
    byhip = float(np.median(np.linalg.norm(M.P[k][a:b] - M.P["hips"][a:b], axis=1))) / L
    ko = M.key("hand", "R" if s == "L" else "L")
    other = float(np.median(np.linalg.norm(M.P[k][a:b] - M.P[ko][a:b], axis=1))) / L if ko else 9.0
    tt = M.t(b - 1) if release else M.t(a)
    why = ("hand rests %.0f ms (%.3f-%.3f), moves %.2f L/s / %.2f L in 300 ms %s, hips move %.2f L on it, %.2f L above floor, "
           "%.2f L from hips, %.2f L from the other hand") % (
        (b - a) / r * 1000, M.t(a), M.t(b - 1), move, travel, "after" if release else "before", support, above, byhip, other)
    on = support >= 0.12 or above <= 0.15 or other <= 0.25
    if move < 0.5 or travel < 0.15:
        edge = move >= 0.4 and travel >= 0.10
        return (None if edge else False), tt, why + "; no clear %s (the hand shuffles)" % ("departure" if release else "approach")
    if not on:
        edge = support >= 0.08 or above <= 0.18 or other <= 0.30
        return (None if edge else False), tt, why + "; the hand rests in the air (a gesture end)"
    return True, tt, why


def heading(M, s):
    kh, kt = M.key("heel", s), M.key("toe", s)
    if not kh or not kt:
        return None, None
    fwd = M.P[kt] - M.P[kh]
    yaw = np.unwrap(np.arctan2(fwd[:, 0], fwd[:, 2]))
    flat = np.hypot(fwd[:, 0], fwd[:, 2])
    ok = flat >= 0.7 * float(np.percentile(flat, 95))
    return yaw, ok


def ev_pivot(M, s, part, t):
    r, i0, win = M.rate, M.i(t), int(WIN * M.rate)
    yaw, ok = heading(M, s)
    if yaw is None:
        return False, None, "no heel / toe bones"
    rate_dps = np.degrees(np.abs(smooth(np.gradient(yaw) * r, max(1, int(0.06 * r)))))
    rate_dps[~ok] = 0.0
    parts = ["heel", "toe"] if part == "any" else [part]
    downm = np.zeros(M.n, dtype=bool)
    for p in parts:
        k = M.key(p, s)
        if k:
            for a, b in M.down[k]:
                downm[a:b] = True
    best = None
    for a, b in intervals((rate_dps >= 30.0) & downm):
        if abs(a - i0) > win and not (a <= i0 < b):
            continue
        tot = abs(math.degrees(yaw[b - 1] - yaw[a]))
        if best is None or tot > best[2]:
            best = (a, b, tot)
    if best is None:
        return False, None, "no turn of the foot at 30 deg/s+ while the %s is down within 150 ms" % (
            "foot" if part == "any" else part)
    a, b, tot = best
    why = "foot turns %.0f deg in %.0f ms from %.3f while the %s is down" % (
        tot, (b - a) / r * 1000, M.t(a), "foot" if part == "any" else part)
    if a <= int(0.03 * r):
        return False, None, why + "; at the clip's start (cut)"
    if tot >= 20.0:
        return True, M.t(a), why
    if tot >= 16.0:
        return None, M.t(a), why
    return False, M.t(a), why + "; too small a turn"


def ev_slide(M, s, part, t):
    r, L, i0, win = M.rate, M.L, M.i(t), int(WIN * M.rate)
    parts = ["heel", "toe"] if part == "any" else [part]
    best = None
    for p in parts:
        k = M.key(p, s)
        if not k:
            continue
        v = M.rel(k)
        hs = np.hypot(v[:, 0], v[:, 2])
        # on the floor and staying there (a heel peeling off as it moves is not a slide)
        m = (M.hloc[k] < 0.015 * L) & (np.abs(v[:, 1]) < 0.15 * L) & (hs >= 0.40 * L)
        for a, b in fill(intervals(m), int(0.02 * r)):
            if abs(a - i0) > win and not (a <= i0 < b):
                continue
            dist = float(np.sum(hs[a:b]) / r) / L
            cand = ((b - a) / r, dist, a, b, p, M.cm(float(np.max(M.hloc[k][a:b]))))
            if best is None or cand[1] > best[1]:
                best = cand
    if best is None:
        return False, None, "no part on the floor moving 0.4 L/s+ within 150 ms"
    dur, dist, a, b, p, h = best
    why = "%s on the floor (<= %.1f cm) slides %.2f L in %.0f ms from %.3f" % (p, h, dist, dur * 1000, M.t(a))
    if a == 0:
        return False, None, why + "; at the clip's start"
    if dur >= 0.060 and dist >= 0.04:
        return True, M.t(a), why
    if dur >= 0.048 and dist >= 0.032:
        return None, M.t(a), why
    return False, M.t(a), why + "; too short"


def evidence(M, kind, part, side, t, wide=False):
    if kind == "step":
        v = ev_step(M, side, part, t)
        if wide and v[0] is False and v[1] is None:
            # a draft whose touchdown lies 150-300 ms away (a heel set down after the toe):
            # the event exists, at the touchdown
            w = ev_step(M, side, part, t, wide=True)
            if w[0]:
                return True, w[1], w[2] + " (found 150-300 ms away)"
        return v
    if kind == "lift":
        return ev_lift(M, side, t)
    if kind in ("jump", "land"):
        v = ev_jump(M, t, land=(kind == "land"))
        if wide and v[0] is False:
            w = ev_jump(M, t, land=(kind == "land"), wide=True)
            if w[0]:
                return True, w[1], w[2] + " (the real flight, found within 400 ms; nearer: %s)" % v[2]
        return v
    if kind in ("grab", "release"):
        return ev_hand(M, side, t, release=(kind == "release"))
    if kind == "pivot":
        return ev_pivot(M, side, part, t)
    if kind == "slide":
        return ev_slide(M, side, part, t)
    return None, None, "no rule for %s" % kind


# ---- Decisions -----------------------------------------------------------------------------------

def dump_path(fixtures, cid):
    p = os.path.join(fixtures, cid + ".csv")
    if cid.lower().endswith(".csv") or not os.path.exists(p):
        p = os.path.join(fixtures, "_bench", "dumps", cid + ".csv")
    top = os.path.join(fixtures, cid + ".csv")
    return top if os.path.exists(top) else p


def decide_clip(cid, lines, clip, M):
    log = []
    adds = []
    items = disagreements(lines, clip)
    # Jumps and lands decided first: a lift-off within 40 ms of a real jump is replaced by it, a
    # whole-foot step from 60 ms before to 100 ms after a real land is that land (the product's
    # replacement; heel / toe touchdowns at a landing stay, as Antho tagged them in Jumping Down).
    jump_times, land_times = [], []
    for x in items:
        kind, part, side = x["id"]
        if kind in ("jump", "land"):
            v, tt, _ = evidence(M, kind, "", side, x["t"], wide=(x["type"] == "draft"))
            if v and tt is not None:
                (jump_times if kind == "jump" else land_times).append(tt)
    for l in lines:
        if l["kind"] in ("jump", "land") and l["status"] == "confirmed":
            (jump_times if l["kind"] == "jump" else land_times).append(l["t"])
    for x in items:
        kind, part, side = x["id"]
        part_w = part or ("hand" if kind in ("grab", "release", "hand_pivot") else "any")
        v, tt, why = evidence(M, kind, part_w, side, x["t"], wide=(x["type"] == "draft"))
        if x["type"] == "draft" and v and tt is not None and abs(tt - x["t"]) > MATCH:
            # moved this far, it must not land on another line of its row
            twin = [l for l in lines if l is not x["line"] and l["status"] != "deleted"
                    and identity(l) == x["id"] and abs(l["t"] - tt) <= MATCH]
            if twin:
                v, why = False, why + "; already a line at %.3f" % twin[0]["t"]
        if v and kind == "lift" and tt is not None and any(abs(tt - j) <= 0.040 for j in jump_times):
            v, why = False, why + "; replaced by the jump"
        if v and kind == "step" and part_w == "any" and tt is not None and any(-0.060 <= tt - j <= 0.100 for j in land_times):
            v, why = False, why + "; replaced by the land"
        if v is False and kind == "lift" and x["type"] == "draft":
            nxt = [l["t"] for l in lines if l["kind"] == "step" and l["side"] == side and l["part"] == "any"
                   and l["status"] == "confirmed" and 0.0 < l["t"] - x["t"] <= 0.5]
            if nxt:
                v, why = None, why + "; but a confirmed step of this foot follows at %.3f (the foot must leave)" % nxt[0]
        if x["type"] == "far":
            why = "draft %.3f, RAV %.3f; " % (x["t"], x["rav_t"]) + why
        line = x.get("line")
        if v is None:
            dec = "unsure"
        elif x["type"] == "draft":
            if v:
                dec = "confirmed"
                line["status"], line["source"] = "confirmed", "claude"
                if tt is not None and abs(tt - line["t"]) > FAR:
                    line["t"] = round(tt, 4)
                    dec = "confirmed+moved"
            else:
                dec = "deleted"
                line["status"], line["source"] = "deleted", "claude"
        elif x["type"] == "far":
            if v and tt is not None:
                line["t"] = round(tt, 4)
                line["status"], line["source"] = "confirmed", "claude"
                dec = "moved"
            elif v is False:
                # the matched detection is then wrong too, but the draft's own event is judged
                dec = "deleted"
                line["status"], line["source"] = "deleted", "claude"
            else:
                dec = "unsure"
        else:
            if v:
                f = x["found"]
                at = tt if tt is not None else f["t"]
                twin = [l for l in lines + adds if l["status"] != "deleted" and identity(l) == x["id"]
                        and abs(l["t"] - at) <= MATCH]
                if twin:
                    log.append((cid, kind, side, part_w, x["t"], x["type"], "not added",
                                why + "; already a line at %.3f" % twin[0]["t"]))
                    continue
                adds.append({"kind": kind, "side": f["s"] if kind not in ("jump", "land") else f["s"],
                             "part": part_w, "t": round(at, 4), "status": "confirmed", "source": "claude"})
                dec = "added"
            else:
                dec = "not added"
        log.append((cid, kind, side, part_w, x["t"], x["type"], dec, why))
    lines.extend(adds)
    return log


def clip_type(cid):
    c = cid.lower()
    for k, ws in (("treadmill", ("treadmill",)), ("jump", ("jump",)), ("climb", ("climb", "ladder", "rope")),
                  ("crawl", ("crawl", "kneel")), ("dance", ("danc",)), ("run", ("run", "sprint")),
                  ("walk", ("walk", "strafe", "turn", "stairs", "topdown"))):
        if any(w in c for w in ws):
            return k
    return "other"


def main(argv):
    fixtures, review, dry, only, verbose = "fixtures", None, False, [], False
    for a in argv:
        if a.startswith("--fixtures="):
            fixtures = a[11:]
        elif a.startswith("--review="):
            review = a[9:]
        elif a == "--dry":
            dry = True
        elif a == "--verbose":
            verbose = True
        elif a.startswith("--clip="):
            only.append(a[7:])
        else:
            sys.exit("usage: decide.py [--fixtures=<dir>] [--review=<review.json>] [--dry] [--clip=<id>]... [--verbose]")
    review = review or os.path.join(fixtures, "_bench", "review.json")
    with open(review, encoding="utf-8") as f:
        data = json.load(f)
    logs = []
    for clip in data["clips"]:
        cid = clip["id"]
        if only and cid not in only:
            continue
        path = os.path.join(fixtures, cid + ".ref.tsv")
        if not os.path.exists(path):
            continue
        lines = read_ref(path)
        if not disagreements(lines, clip):
            continue
        M = Motion(dump_path(fixtures, cid))
        log = decide_clip(cid, lines, clip, M)
        logs.extend(log)
        if not dry:
            write_ref(path, lines)
    out = os.path.join(fixtures, "_bench", "decisions.tsv")
    if not dry and not only:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write("# clip\tkind\tside\tpart\ttime_s\tdisagreement\tdecision\treason\n")
            for r in logs:
                f.write("%s\t%s\t%s\t%s\t%.4f\t%s\t%s\t%s\n" % r)
    per = {}
    for r in logs:
        c = per.setdefault(r[0], {})
        c[r[6]] = c.get(r[6], 0) + 1
        if verbose:
            print("  %-34.34s %-8s %s %-4s %.3f %-5s -> %-15s %s" % r)
    tot = {}
    for cid, c in per.items():
        print("%-52s %s" % (cid, ", ".join("%s %d" % kv for kv in sorted(c.items()))))
        for k, v in c.items():
            tot[k] = tot.get(k, 0) + v
    bytype = {}
    for r in logs:
        c = bytype.setdefault(clip_type(r[0]), {})
        c[r[6]] = c.get(r[6], 0) + 1
    for k, c in sorted(bytype.items()):
        print("type %-10s %s" % (k, ", ".join("%s %d" % kv for kv in sorted(c.items()))))
    print("total: %s (%d disagreements)" % (", ".join("%s %d" % kv for kv in sorted(tot.items())), len(logs)))
    unsure = [r for r in logs if r[6] == "unsure"]
    if unsure:
        print("unsure:")
        for r in unsure:
            print("  %s %s %s %s %.3f (%s): %s" % (r[0], r[1], r[2], r[3], r[4], r[5], r[7]))


if __name__ == "__main__":
    main(sys.argv[1:])
