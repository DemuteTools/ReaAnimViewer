#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Story 10-8o: drafts each fixture clip's reference file (fixtures/<clip>.ref.tsv, the format
# of tests/ref_file.h) from the motion alone, for the all-trigger bench (tests/trigger_bench.cpp).
#
# Independent of the detector on purpose: it never calls, copies or reads the detector's code,
# constants or output (motion_physics.*, auto_detect.*, auto_types*), so the two cannot share a
# bug. It reads only the dump's joint positions (feet, toes, hands, hips) with its own simple
# rules, below. Claude reads each draft against the motion and fixes it before publishing; Antho
# confirms or corrects it on the review page (tools/ref_draft/apply_corrections.py).
#
#   ref_draft.py [--fixtures=<dir>] [--bench=<trigger_bench exe>] [--clip=<id>]... [--dry] [--debug]
#   --dry: write nothing; --debug: --dry, plus each clip's foot contacts, hips height and lines
#
# Clips: as the bench lists them: every '*.csv' of the folder (top level), and every
# '.fbx' / '.glb' without a '<name>.csv' beside it, read from <fixtures>/_bench/dumps/<name>.csv
# (the bench writes those; run it first). --clip limits the run to those ids.
#
# Merge (never overwrite): a line that is confirmed or deleted, or whose source is antho, is
# kept as is; old draft lines (source motion) are replaced by the new draft. Lines are compared by
# the bench's row identity (identity(): the kind as RefTarget reads it, a tip as the toe, no part
# for lift / grab / release / hand pivot / jump / land; story 10-9a-3: jump / land keep their
# side; the merge reads a jump / land of either side as the same event: one per flight). The old REF
# markers of a csv dump (`<bench> --old-refs <csv>`: ParseEventRefLabel and the step labels) are
# added as confirmed REF lines unless a kept line of their identity lies within 150 ms (the
# bench's match window: already there, or moved by Antho). A draft is dropped when a kept line or
# an old REF of its identity lies within 150 ms (Antho has already decided that one).
#
# Rules (body units: L = the leg length, thigh + shin; times in s):
#   world velocity   the clip's ground: 0 on a root-motion clip (the hips end 0.5 L or more from
#                    where they start, horizontally); else the mode of the end parts' velocities (heels, balls,
#                    toe tips, hands) within 0.12 L/s, each component under 0.15 L/s zeroed (an in-place clip's
#                    treadmill).
#   still part       |velocity - world| < 0.35 L/s, or the part on the clip's floor (within
#                    0.02 L of its own 3rd-percentile height) under 1.3 L/s (a landing that
#                    slides), and its
#                    vertical speed under 0.15 L/s (it has stopped coming down; positions
#                    smoothed over 25 ms). Gaps under 40 ms
#                    filled, holds under 60 ms dropped (20 ms for one cut by the clip's edge, within 15 ms of it). A hold starts where the part arrives:
#                    moved back while the part is within 0.005 L of its hold height (150 ms at
#                    most).
#   foot contact     any part of that foot still (heel, ball; the toe tip when the foot has no
#                    ball), gaps under 80 ms filled. step <side> any at its start, lift <side> any
#                    at its end (none at the clip's edges). step <side> heel / toe at each part's
#                    own hold start when the part came down to it (0.015 L higher in the 150 ms
#                    before).
#   flight           no foot in contact for 0.22 s or more, the hips on a ballistic arc (fitted
#                    parabola, curvature under -0.4 g), or falling freely over its first 0.3 s
#                    (curvature under -0.5 g: a jump that ends on the body). jump <last foot leaving> any at its start,
#                    land <first foot touching> any at its end (feet together: L) when the hips are still 0.2 L above
#                    that foot 0.2 s later (a fall onto the body gives no Land). Within 30 ms of a
#                    land no step line, within 30 ms of a jump no lift line (the product's
#                    replacement: Land replaces the steps, Jump the lift-offs).
#   pivot            during a foot contact, the foot's heading (heel to ball, horizontal) turns at
#                    40 deg/s or more (smoothed 60 ms; not while the foot is steep: its heel-to-ball
#                    length seen from above under 0.7 x its 95th percentile) for a total of 25 deg
#                    or more: pivot <side>
#                    any at the start of the turn, and pivot <side> heel / toe for each part still
#                    then.
#   hand hold        the hand still (|velocity - world| < 0.3 L/s) for 150 ms or more while the
#                    hips move 0.12 L or more relative to it: grab <side> hand at its start,
#                    release <side> hand at its end (none at the clip's edges).
#   body moves       story 10-12e, side B, part any, each at the move's start: the hips' height
#                    above the lowest foot part at that sample (never one floor: a climb's feet
#                    climb too), smoothed over 100 ms, swinging 0.2 L or more between two turning
#                    points (a zigzag with 0.2 L of hysteresis) within 1.5 s (its start: the last
#                    sample within 5 % of the swing of the first turning point), inside one foot
#                    contact stretch (a flight between breaks it), each foot's lowest part ending
#                    within 0.1 L of its height at the swing's start (a climb puts one foot on the
#                    next hold): crouch down, stand_up up. The trunk's forward lean (hips to
#                    neck, along the facing perpendicular to the up legs' line, 0 when upright or
#                    behind) swinging 20 deg or more the same way at 40 deg/s or more on average
#                    (walking's lean drifts 20-24 deg at about 30): bend forward, rise back, unless
#                    it overlaps a drafted crouch / stand_up by half the shorter of the two spans
#                    (one motion, one marker).
# No arrival (step, land, grab, pivot) within 30 ms of the clip's start, no departure (lift,
# jump, release) within 30 ms of its end. No slide and no hand pivot draft: Antho adds them on
# the page.
#
#   --kinds=<k1,k2...>  drafts only those kinds (story 10-12e: --kinds=crouch,stand_up,bend,rise)
#                       into the clips that have a reference file already (none is created: a new
#                       file would change which clips the bench sums). The file's other lines and
#                       its comments stay as they are; the old draft lines (source motion) of those
#                       kinds are dropped and the new drafts appended after a comment line (an
#                       earlier such line of those kinds dropped: a rerun changes nothing). An
#                       empty list is an error.

import math
import os
import subprocess
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("ref_draft.py needs numpy (pip install numpy)")

DEBUG = False
HEADER = "# kind\tside\tpart\ttime_s\tstatus\tsource"


# ---- Reading ------------------------------------------------------------------------------------

def read_dump(path):
    meta, hdr, rows = {}, None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            if line[0] == "#":
                k, _, v = line[1:].strip().partition("=")
                meta[k] = v
                continue
            if hdr is None:
                hdr = line.split(",")
                continue
            rows.append([float(x) for x in line.split(",")])
    stride = 7 if len(hdr) > 4 and hdr[4].endswith(".qw") else 3
    names = [hdr[i][:-2] for i in range(1, len(hdr), stride)]
    a = np.array(rows, dtype=float)
    pos = np.stack([a[:, 1 + stride * b: 4 + stride * b] for b in range(len(names))], axis=1)  # (n, bones, 3)
    rate = float(meta.get("rate_hz", "240"))
    lo, hi = 0.0, (len(rows) - 1) / rate
    if "visible" in meta:
        p = meta["visible"].split(",")
        if len(p) == 2:
            lo, hi = float(p[0]), float(p[1])
    return {"names": names, "pos": pos, "rate": rate, "lo": lo, "hi": hi}


def short(n):
    return n.split(":")[-1].lower()


BONES = {
    "hips": ["hips", "pelvis"],
    "heel_L": ["leftfoot", "foot_l"], "heel_R": ["rightfoot", "foot_r"],
    "ball_L": ["lefttoebase", "ball_l"], "ball_R": ["righttoebase", "ball_r"],
    "tip_L": ["lefttoe_end", "lefttoeend"], "tip_R": ["righttoe_end", "righttoeend"],
    "hand_L": ["lefthand", "hand_l"], "hand_R": ["righthand", "hand_r"],
    "upleg_L": ["leftupleg", "thigh_l"], "upleg_R": ["rightupleg", "thigh_r"],
    "knee_L": ["leftleg", "calf_l"], "knee_R": ["rightleg", "calf_r"],
    # Story 10-12e: the top of the trunk (the neck, else the highest spine bone).
    "neck": ["neck", "neck_01", "spine2", "spine_03", "spine1", "spine"],
}


def find_bones(names):
    s = [short(n) for n in names]
    out = {}
    for key, cands in BONES.items():
        for c in cands:
            if c in s:
                out[key] = s.index(c)
                break
    return out


# ---- Signals ------------------------------------------------------------------------------------

def smooth(x, n):
    if n <= 1:
        return x
    k = np.ones(n) / n
    pad = n // 2
    xp = np.pad(x, [(pad, n - 1 - pad)] + [(0, 0)] * (x.ndim - 1), mode="edge")
    return np.apply_along_axis(lambda c: np.convolve(c, k, mode="valid"), 0, xp)


def velocity(p, rate):
    v = np.gradient(p, axis=0) * rate
    return v


def intervals(mask):
    """[a, b) runs of True."""
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            out.append([i, j])
            i = j
        else:
            i += 1
    return out


def clean(runs, fill, keep, n=None):
    """Gaps under `fill` filled, runs under `keep` dropped (a run cut by the clip's edge, when `n`
    is given: a third of `keep`)."""
    merged = []
    for a, b in runs:
        if merged and a - merged[-1][1] < fill:
            merged[-1][1] = b
        else:
            merged.append([a, b])
    m = 0 if n is None else max(1, keep // 4)
    edge = lambda r: n is not None and (r[0] <= m or r[1] >= n - m)
    return [r for r in merged if r[1] - r[0] >= (keep / 3.0 if edge(r) else keep)]


def to_mask(runs, n):
    m = np.zeros(n, dtype=bool)
    for a, b in runs:
        m[a:b] = True
    return m


def world_velocity(vels, L):
    """Mode of the end parts' velocities (3D) within 0.12 L/s."""
    pts = np.concatenate([v[::6] for v in vels], axis=0)
    if len(pts) == 0:
        return np.zeros(3)
    r = 0.12 * L
    best, best_n = np.zeros(3), -1
    cand = pts[:: max(1, len(pts) // 600)]
    for c in np.concatenate([cand, np.zeros((1, 3))]):
        n = int(np.sum(np.linalg.norm(pts - c, axis=1) < r))
        if n > best_n:
            best, best_n = c, n
    inl = pts[np.linalg.norm(pts - best, axis=1) < r]
    w = inl.mean(axis=0) if len(inl) else best
    w[np.abs(w) < 0.15 * L] = 0.0  # a small component is noise (a treadmill runs along one axis)
    return w


# ---- Draft --------------------------------------------------------------------------------------

def draft(clip):
    names, pos, rate = clip["names"], clip["pos"], clip["rate"]
    n = pos.shape[0]
    B = find_bones(names)
    if "hips" not in B or not any(k in B for k in ("heel_L", "heel_R")):
        return None, "no hips / feet found by name"
    sm = max(1, int(round(0.025 * rate)))
    P = {k: smooth(pos[:, i, :], sm) for k, i in B.items()}
    V = {k: velocity(p, rate) for k, p in P.items()}
    legs = []
    for s in "LR":
        if all(k in B for k in ("upleg_" + s, "knee_" + s, "heel_" + s)):
            d = np.linalg.norm(P["upleg_" + s] - P["knee_" + s], axis=1) + np.linalg.norm(P["knee_" + s] - P["heel_" + s], axis=1)
            legs.append(float(np.median(d)))
    L = float(np.mean(legs)) if legs else 0.9
    ends = [k for k in B if k.split("_")[0] in ("heel", "ball", "tip", "hand")]
    # A root-motion clip (the hips travel half a leg or more): the ground is the world.
    root_motion = float(np.linalg.norm((P["hips"][-1] - P["hips"][0])[[0, 2]])) >= 0.5 * L
    w = np.zeros(3) if root_motion else world_velocity([V[k] for k in ends], L)
    hips_y = P["hips"][:, 1]
    foot_parts = [k for k in B if k.split("_")[0] in ("heel", "ball", "tip")]
    floor = float(np.percentile(np.concatenate([P[k][:, 1] for k in foot_parts]), 2))
    fill, keep = int(0.040 * rate), int(0.060 * rate)
    t = lambda i: i / rate
    lines = []

    edge = int(0.030 * rate)

    def add(kind, side, part, i):
        # A contact cut by the clip's edge gives no event: no arrival (step, land, grab, pivot)
        # within 30 ms of the start, no departure (lift, jump, release) within 30 ms of the end.
        arrival = kind in ("step", "land", "grab", "pivot")
        if (i >= edge) if arrival else (i < n - edge):
            lines.append({"kind": kind, "side": side, "part": part, "t": round(t(i), 4)})

    def still_part(k, thr):
        """The part's holds [a, b, came_down]: the start moved back while the part is already within
        0.005 L of its hold height (an impact is read where the part arrives, not where it settles)."""
        sp = np.linalg.norm(V[k] - w, axis=1)
        vy = np.abs(V[k][:, 1] - w[1])
        y = P[k][:, 1]
        # A landing that slides is still a contact: on the floor, slower than a swing.
        on_floor = (y <= float(np.percentile(y, 3)) + 0.02 * L) & (sp < 1.3 * L)
        out = []
        for a, b in clean(intervals(((sp < thr * L) | on_floor) & (vy < 0.15 * L)), fill, keep, n):
            h = float(np.median(y[a:min(b, a + int(0.1 * rate))]))
            i = a
            while i > 0 and y[i - 1] <= h + 0.005 * L and a - i < int(0.15 * rate):
                i -= 1
            before = y[max(0, i - int(0.15 * rate)):i]
            out.append([i, b, len(before) > 0 and float(np.max(before)) >= h + 0.015 * L])
        return out

    foot_runs = {}
    for s in "LR":
        parts = [p for p in ("heel", "ball") if p + "_" + s in B]
        if "ball" not in parts and "tip_" + s in B:
            parts.append("tip")
        if not parts:
            continue
        pr = {p: still_part(p + "_" + s, 0.35) for p in parts}
        any_mask = np.zeros(n, dtype=bool)
        for p in parts:
            any_mask |= to_mask([r[:2] for r in pr[p]], n)
        fr = clean(intervals(any_mask), int(0.080 * rate), keep, n)
        foot_runs[s] = fr
        for a, b in fr:
            if a > 0:
                add("step", s, "any", a)
            if b < n:
                add("lift", s, "any", b - 1)
        for p in parts:
            for a, b, came_down in pr[p]:
                if a > 0 and came_down:
                    add("step", s, "heel" if p == "heel" else "toe", a)
        # Pivot: the foot's heading during each contact.
        if "heel_" + s in B and ("ball_" + s in B or "tip_" + s in B):
            fwd = P[("ball_" if "ball_" + s in B else "tip_") + s] - P["heel_" + s]
            yaw = np.unwrap(np.arctan2(fwd[:, 0], fwd[:, 2]))
            rate_dps = np.degrees(np.abs(smooth(np.gradient(yaw) * rate, max(1, int(0.06 * rate)))))
            # A steep foot (heel up, toe down) has no reliable heading: its turn is not read.
            flat = np.hypot(fwd[:, 0], fwd[:, 2])
            rate_dps[flat < 0.7 * float(np.percentile(flat, 95))] = 0.0
            for a, b in fr:
                turning = clean(intervals(rate_dps[a:b] >= 40.0), int(0.03 * rate), int(0.05 * rate))
                for ta, tb in turning:
                    total = abs(math.degrees(yaw[a + tb - 1] - yaw[a + ta]))
                    if total < 25.0:
                        continue
                    i = a + ta
                    add("pivot", s, "any", i)
                    for p in parts:
                        if to_mask([r[:2] for r in pr[p]], n)[i]:
                            add("pivot", s, "heel" if p == "heel" else "toe", i)

    # Flights.
    contact = np.zeros(n, dtype=bool)
    for s, fr in foot_runs.items():
        contact |= to_mask(fr, n)
    for a, b in intervals(~contact):
        if (b - a) / rate < 0.22 or a == 0:
            continue
        seg = hips_y[a:b]
        tt = np.arange(b - a) / rate
        head = int(min(b - a, 0.3 * rate))
        arc = len(seg) >= 5 and np.polyfit(tt, seg, 2)[0] * 2.0 < -0.4 * 9.81
        fall = head >= 5 and np.polyfit(tt[:head], seg[:head], 2)[0] * 2.0 < -0.5 * 9.81
        if not (arc or fall):
            continue
        # Story 10-9a-3: the last foot leaving (its run ends where the flight starts; L first).
        lifts = sorted((s, fr_b) for s, fr in foot_runs.items() for fr_a, fr_b in fr if fr_b == a)
        side = lifts[0][0] if lifts else "L"
        add("jump", side, "any", a - 1)
        if b < n:
            lands = sorted(s for s, fr in foot_runs.items() for fr_a, fr_b in fr if fr_a == b)
            ls = lands[0] if lands else "L"
            j = min(n - 1, b + int(0.2 * rate))
            fk = "heel_" + ls if "heel_" + ls in P else foot_parts[0]
            if hips_y[j] - P[fk][j, 1] > 0.2 * L:
                add("land", ls, "any", b)

    # Land replaces the steps, Jump the lift-offs (within 30 ms).
    lands = [l["t"] for l in lines if l["kind"] == "land"]
    jumps = [l["t"] for l in lines if l["kind"] == "jump"]
    lines = [l for l in lines
             if not (l["kind"] == "step" and any(abs(l["t"] - x) <= 0.030 for x in lands))
             and not (l["kind"] == "lift" and any(abs(l["t"] - x) <= 0.030 for x in jumps))]

    # Hand holds.
    for s in "LR":
        k = "hand_" + s
        if k not in B:
            continue
        sp = np.linalg.norm(V[k] - w, axis=1)
        for a, b in clean(intervals(sp < 0.3 * L), fill, int(0.150 * rate)):
            rel = P["hips"][a:b] - P[k][a:b]
            moved = float(np.max(np.linalg.norm(rel - rel[0], axis=1)))
            if moved < 0.12 * L:
                continue
            if a > 0:
                add("grab", s, "hand", a)
            if b < n:
                add("release", s, "hand", b - 1)

    lines += body_moves(P, B, contact, L, rate, n)

    lines.sort(key=lambda l: (l["t"], l["kind"], l["side"], l["part"]))
    if DEBUG:
        step = max(1, int(round(rate / 30)))
        def row(m):
            return "".join("#" if m[i:i + step].any() else "." for i in range(0, n, step))
        print("    t(s)    " + "".join(str(int(i / rate)) if (i // step) % 30 == 0 else " " for i in range(0, n, step)))
        for s_ in "LR":
            if s_ in foot_runs:
                print("    foot %s  " % s_ + row(to_mask(foot_runs[s_], n)))
        hy = hips_y - floor
        print("    hips/L  " + "".join(str(max(0, min(9, int(5 * hy[i] / L)))) for i in range(0, n, step)))
        for l in lines:
            print("      %-8s %s %-5s %.3f" % (l["kind"], l["side"], l["part"], l["t"]))
    info = "L %.2f m, world velocity (%.2f, %.2f, %.2f) m/s, floor %.3f m" % (L, w[0], w[1], w[2], floor)
    return lines, info


def zigzag(x, h):
    """The turning points of `x` with `h` of hysteresis, alternating highs and lows, the first
    sample and the last one included as ends: [(arrive, leave, value)], `arrive` the first sample
    within 5 % of h of the extreme (where a move ends), `leave` the last one (where the next move
    starts: a plateau is no move)."""
    n = len(x)
    if n == 0:
        return []
    tol = 0.05 * h
    pts = []
    ext_v, d = x[0], 0
    arrive = leave = 0
    for i in range(1, n):
        v = x[i]
        if d == 0:
            if abs(v - x[0]) <= tol:
                leave = i
                continue
            if abs(v - x[0]) >= h:
                pts.append((0, leave, x[0]))
                d = 1 if v > x[0] else -1
                ext_v, arrive, leave = v, i, i
            continue
        if (d > 0 and v > ext_v) or (d < 0 and v < ext_v):
            if abs(v - ext_v) > tol:
                arrive = i
            ext_v, leave = v, i
        elif abs(v - ext_v) <= tol:
            leave = i
        elif (d > 0 and ext_v - v >= h) or (d < 0 and v - ext_v >= h):
            pts.append((arrive, leave, ext_v))
            d = -d
            ext_v, arrive, leave = v, i, i
    if d == 0:
        return [(0, n - 1, x[0])]
    pts.append((arrive, leave, ext_v))
    return pts


def move_start(seg, i0, i1):
    """A swing's start: the last sample from i0 still within 5 % of the swing's amount of where it
    starts (a slow drift before a big move is no part of it)."""
    v0, amount = seg[i0], abs(seg[i1] - seg[i0])
    i = i0
    while i + 1 < i1 and abs(seg[i + 1] - v0) <= 0.05 * amount:
        i += 1
    return i


def body_moves(P, B, contact, L, rate, n):
    """Story 10-12e: crouch / stand_up / bend / rise lines (see the rules at the top)."""
    out = []
    feet = {s: [k for k in ("heel_" + s, "ball_" + s, "tip_" + s) if k in P] for s in "LR"}
    if "hips" not in P or not (feet["L"] or feet["R"]):
        return out
    low_s = {s: np.min(np.stack([P[k][:, 1] for k in feet[s]], axis=1), axis=1) for s in "LR" if feet[s]}
    low = np.min(np.stack(list(low_s.values()), axis=1), axis=1)
    above = (P["hips"][:, 1] - low) / L
    w = max(1, int(round(0.1 * rate)))
    hips_moves = []
    for a, b in intervals(contact):
        seg = smooth(above[a:b], w)
        pts = zigzag(seg, 0.2)
        for (_, i0, v0), (i1, _, v1) in zip(pts, pts[1:]):
            if abs(v1 - v0) < 0.2:
                continue
            i0 = move_start(seg, i0, i1)
            if (i1 - i0) / rate > 1.5:
                continue
            if any(abs(low_s[s][a + i1] - low_s[s][a + i0]) > 0.1 * L for s in low_s):
                continue
            hips_moves.append((a + i0, a + i1))
            out.append({"kind": "crouch" if v1 < v0 else "stand_up", "side": "B", "part": "any", "t": round((a + i0) / rate, 4)})
    if "neck" in P and "upleg_L" in P and "upleg_R" in P:
        d = P["neck"] - P["hips"]
        line = P["upleg_R"] - P["upleg_L"]
        fx, fz = line[:, 2], -line[:, 0]
        fl = np.hypot(fx, fz)
        fl[fl < 1e-9] = 1e-9
        lean = np.degrees(np.arctan2((d[:, 0] * fx + d[:, 2] * fz) / fl, d[:, 1]))
        lean = np.maximum(lean, 0.0)
        for a, b in intervals(contact):
            seg = smooth(lean[a:b], w)
            pts = zigzag(seg, 20.0)
            for (_, i0, v0), (i1, _, v1) in zip(pts, pts[1:]):
                i0 = move_start(seg, i0, i1)
                dur = (i1 - i0) / rate
                if abs(v1 - v0) < 20.0 or dur > 1.5 or dur <= 0 or abs(v1 - v0) / dur < 40.0:
                    continue
                s0, s1 = a + i0, a + i1
                if any(min(s1, h1) - max(s0, h0) >= 0.5 * min(s1 - s0, h1 - h0) for h0, h1 in hips_moves):
                    continue
                out.append({"kind": "bend" if v1 > v0 else "rise", "side": "B", "part": "any", "t": round(s0 / rate, 4)})
    return out


# ---- Reference files ----------------------------------------------------------------------------

def read_ref(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if not p or p[0].startswith("#") or len(p) != 6:
                continue
            out.append({"kind": p[0], "side": p[1], "part": p[2], "t": float(p[3]), "status": p[4], "source": p[5]})
    return out


KINDS_NOTE_HEAD, KINDS_NOTE_TAIL = "# 10-12e: ", " drafted from the motion (ref_draft.py --kinds)"


def kinds_note(kinds):
    return KINDS_NOTE_HEAD + ", ".join(sorted(kinds)) + KINDS_NOTE_TAIL


def write_ref_kinds(path, merged, kinds, note=None):
    """Story 10-12e: --kinds' writer: the file's text kept line for line, except the draft lines
    (source motion) of `kinds` and the earlier notes of a --kinds draft of those kinds only, then
    `note` (kinds_note by default) and the merged lines of `kinds` not kept from it. Running it
    again on its own result changes nothing."""
    note = note or kinds_note(kinds)
    with open(path, encoding="utf-8") as f:
        raw = f.read().splitlines()
    out, kept_lines = [], []
    for line in raw:
        if line.startswith(KINDS_NOTE_HEAD) and line.endswith(KINDS_NOTE_TAIL):
            noted = line[len(KINDS_NOTE_HEAD):-len(KINDS_NOTE_TAIL)].split(", ")
            if set(noted) <= set(kinds):
                continue
        p = line.split()
        if len(p) == 6 and not p[0].startswith("#") and p[0] in kinds and p[4] == "draft" and p[5] == "motion":
            continue
        if len(p) == 6 and not p[0].startswith("#") and p[0] in kinds:
            kept_lines.append((p[0], p[1], float(p[3])))
        out.append(line)
    new = [l for l in sorted(merged, key=lambda l: (l["t"], l["kind"]))
           if l["kind"] in kinds and (l["kind"], l["side"], l["t"]) not in kept_lines]
    if new:
        out.append(note)
        for l in new:
            out.append("%s\t%s\t%s\t%.4f\t%s\t%s" % (l["kind"], l["side"], l["part"], l["t"], l["status"], l["source"]))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def write_ref(path, lines):
    lines = sorted(lines, key=lambda l: (l["t"], l["kind"], l["side"], l["part"]))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n")
        for l in lines:
            f.write("%s\t%s\t%s\t%.4f\t%s\t%s\n" % (l["kind"], l["side"], l["part"], l["t"], l["status"], l["source"]))


def kept(l):
    return l["status"] in ("confirmed", "deleted") or l["source"] == "antho"


def identity(l):
    """The bench's row identity of a line (RefTarget, tests/ref_file.h): the base kind; for steps,
    slides and pivots the part, a tip reading as the toe; no part for the other kinds; the side
    (story 10-9a-3: jump / land too; the merge's same_id matches them across sides)."""
    kind, part = l["kind"], l["part"]
    if kind in ("heel", "toe"):
        kind, part = "step", kind
    for b in ("slide", "pivot"):
        if kind.startswith(b + "_"):
            kind, part = b, ("heel" if "heel" in kind else "toe")
    if kind == "hand_pivot_any":
        kind = "hand_pivot"
    if kind in ("step", "slide", "pivot"):
        part = "toe" if part == "tip" else part
    else:
        part = ""
    return (kind, part, l["side"])


def same_id(x, y):
    """Two identities of one event for the merge: the same row, or a jump / land of either side (one
    take-off / landing per flight, whichever foot a line names)."""
    if x[:2] != y[:2]:
        return False
    return x[2] == y[2] or x[0] in ("jump", "land")


def near(a, lines, window=0.150):
    ia = identity(a)
    return any(same_id(identity(k), ia) and abs(k["t"] - a["t"]) <= window for k in lines)


def merge(old, drafts, old_refs, kinds=None):
    """The kept lines of `old`, then each old REF line unless a kept line of its identity lies
    within 150 ms (already there, or moved by Antho), then each draft unless one of those lies
    within 150 ms of it. Running it again on its own result gives the same lines. `kinds`
    (story 10-12e): only those kinds are drafted; every line of another kind is kept as it is."""
    if kinds:
        drafts = [d for d in drafts if d["kind"] in kinds]
        old_refs = [r for r in old_refs if r["kind"] in kinds]
    keep = [l for l in old if kept(l) or (kinds and l["kind"] not in kinds)]
    out = list(keep)
    for r in old_refs:
        if not near(r, keep):
            out.append(r)
    fixed = list(out)
    for d in drafts:
        if not near(d, fixed):
            out.append(dict(d, status="draft", source="motion"))
    return out


def old_refs_of(bench, csv):
    if not bench:
        return [], []
    r = subprocess.run([bench, "--old-refs", csv], capture_output=True, text=True)
    lines, skipped = [], []
    for line in r.stdout.splitlines():
        if line.startswith("# skipped "):
            skipped.append(line[10:])
            continue
        p = line.split("\t")
        if len(p) == 6:
            lines.append({"kind": p[0], "side": p[1], "part": p[2], "t": float(p[3]), "status": p[4], "source": p[5]})
    return lines, skipped


def list_clips(fixtures):
    names = sorted(f for f in os.listdir(fixtures) if os.path.isfile(os.path.join(fixtures, f)))
    have = set(names)
    out = []
    for f in names:
        l = f.lower()
        if l.endswith(".csv"):
            out.append((f[:-4], os.path.join(fixtures, f), True))
        elif (l.endswith(".fbx") or l.endswith(".glb")) and f + ".csv" not in have:
            out.append((f, os.path.join(fixtures, "_bench", "dumps", f + ".csv"), False))
    return out


def main(argv):
    fixtures, bench, only, dry, kinds = "fixtures", "", [], False, None
    for a in argv:
        if a.startswith("--fixtures="):
            fixtures = a[11:]
        elif a.startswith("--bench="):
            bench = a[8:]
        elif a.startswith("--clip="):
            only.append(a[7:])
        elif a == "--dry":
            dry = True
        elif a.startswith("--kinds="):
            kinds = set(k for k in a[8:].split(",") if k)
            if not kinds:
                sys.exit("ref_draft.py: --kinds= needs at least one kind (e.g. --kinds=crouch,stand_up,bend,rise)")
        elif a == "--debug":
            global DEBUG
            DEBUG = dry = True
        else:
            sys.exit(__doc__ or "usage: ref_draft.py [--fixtures=<dir>] [--bench=<exe>] [--clip=<id>]... [--dry]")
    for cid, csv, is_reaper in list_clips(fixtures):
        if only and cid not in only:
            continue
        if not os.path.exists(csv):
            print("%s: no dump (run the bench first)" % cid)
            continue
        try:
            clip = read_dump(csv)
        except Exception as e:  # noqa: BLE001
            print("%s: read failed: %s" % (cid, e))
            continue
        drafts, info = draft(clip)
        if drafts is None:
            print("%s: no draft (%s)" % (cid, info))
            continue
        refs, skipped = old_refs_of(bench, csv) if is_reaper else ([], [])
        path = os.path.join(fixtures, cid + ".ref.tsv")
        if kinds and not os.path.exists(path):
            print("%s: no reference file (--kinds creates none)" % cid)
            continue
        merged = merge(read_ref(path), drafts, refs, kinds)
        counts = {}
        for l in merged:
            if l["status"] != "deleted":
                counts[l["kind"]] = counts.get(l["kind"], 0) + 1
        print("%s: %d lines (%s)  [%s]%s" % (cid, len(merged), ", ".join("%s %d" % kv for kv in sorted(counts.items())), info,
                                             ("  skipped REF: " + "; ".join(skipped)) if skipped else ""))
        if not dry and kinds:
            write_ref_kinds(path, merged, kinds)
        elif not dry:
            write_ref(path, merged)


if __name__ == "__main__":
    main(sys.argv[1:])
