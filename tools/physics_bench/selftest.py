# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
# SPDX-License-Identifier: MIT
"""Story 10-13a: synthetic checks of the physics bench's reference rules (no data needed).

    uv run tools/physics_bench/selftest.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kyokushin as K  # noqa: E402

RATE = 250.0
failures = []


def check(name, ok, detail=""):
    print(f"{'ok  ' if ok else 'FAIL'} {name}{'  ' + detail if detail and not ok else ''}")
    if not ok:
        failures.append(name)


def quad(F, corners):
    return np.stack([np.tile(np.asarray(c, float), (F, 1)) for c in corners])


# A 400 x 600 mm shield in the plane x = 0, corners given out of order (the outline is rebuilt).
SHIELD = [(0, 0, 0), (0, 400, 600), (0, 400, 0), (0, 0, 600)]


def test_quad_distance():
    pts = np.array([[50, 200, 300],      # in front of the middle: the plane distance
                    [-30, 200, 300],     # behind (sunk into the pad): unsigned
                    [0, 500, 300],       # beside, in the plane: to the edge y = 400
                    [30, 440, 640],      # off a corner: to the corner
                    [np.nan, 0, 0]], float)
    d = K.quad_dist(pts, quad(len(pts), SHIELD))
    want = [50, 30, 100, np.sqrt(30 ** 2 + 40 ** 2 + 40 ** 2)]
    check("quad distance: plane, behind, edge, corner", np.allclose(d[:4], want, atol=1e-6), f"{d} vs {want}")
    check("quad distance: missing point is NaN", np.isnan(d[4]))
    Q = quad(len(pts), SHIELD)
    Q[2, 0] = np.nan
    check("quad distance: missing corner is NaN", np.isnan(K.quad_dist(pts, Q)[0]))


def blow(F, t0, v_ms, x_start=600.0, rest=-40.0):
    """A fist moving along -x at v_ms from x_start (mm) from frame t0 down to `rest` (sunk into the
    pad), then back out slowly."""
    x = np.full(F, x_start)
    step = v_ms * 1000.0 / RATE
    k = t0
    while k < F and x[k - 1] - step > rest:
        x[k] = x[k - 1] - step
        k += 1
    k0 = k
    while k < F and x[k - 1] < x_start:
        x[k] = min(x_start, x[k - 1] + 2.0)
        k += 1
    if k < F:
        x[k:] = x_start
    p = np.stack([x, np.full(F, 200.0), np.full(F, 300.0)], 1)
    return p, k0


def test_shield_contact():
    F = 500
    p, _ = blow(F, 100, 6.0)
    d = K.quad_dist(p, quad(F, SHIELD))
    v = np.linalg.norm(np.gradient(p, axis=0), axis=1) * RATE / 1000.0
    refs = K.shield_refs(d, v, "hand", RATE)
    first = int(np.argmax(d < 20))
    check("shield contact: one blow at the first frame within 20 mm", refs == [first], f"{refs} vs {first}")
    slow, _ = blow(F, 100, 1.0)
    d2 = K.quad_dist(slow, quad(F, SHIELD))
    v2 = np.linalg.norm(np.gradient(slow, axis=0), axis=1) * RATE / 1000.0
    check("shield contact: a slow approach (1 m/s) is no blow", K.shield_refs(d2, v2, "hand", RATE) == [])
    check("shield contact: a foot needs 4 m/s", K.shield_refs(d2 * 0 + d, v * 0 + 3.5, "foot", RATE) == [])
    # Two touches 200 ms apart merge; 600 ms apart stay two.
    dd = np.full(F, 300.0)
    dd[100:110] = 5.0
    dd[160:170] = 5.0
    dd[310:320] = 5.0
    vv = np.full(F, 6.0)
    check("shield contact: touches < 300 ms apart merge", K.shield_refs(dd, vv, "hand", RATE) == [100, 310],
          str(K.shield_refs(dd, vv, "hand", RATE)))


def test_air_max_reach():
    F = 400
    t = np.arange(F) / RATE
    # Reach 300 -> 700 mm, a smooth extension centred at 0.6 s (speed peak there), max at ~0.7 s.
    reach = 300 + 400 / (1 + np.exp(-(t - 0.6) / 0.02))   # peak speed 5 m/s
    reach[int(0.7 * RATE):] = reach[int(0.7 * RATE)] - (t[int(0.7 * RATE):] - 0.7) * 400   # pulled back after
    eff = np.stack([reach, np.zeros(F), np.zeros(F)], 1)
    root = np.zeros((F, 3))
    v = np.linalg.norm(np.gradient(eff, axis=0), axis=1) * RATE / 1000.0
    reach_mm = K.effector_reach([eff], root)
    refs = K.air_refs(reach_mm, v, "hand", RATE)
    want = int(0.7 * RATE)
    check("air: one blow, at the max reach after the speed peak", refs == [want], f"{refs} vs {want}")
    check("air: a foot needs a 6 m/s peak", K.air_refs(reach_mm, np.minimum(v, 5.5), "foot", RATE) == [])


def held_punch(F, rate, t_peak=0.6, top=478.0):
    """A reach (mm) rising to `top` around t_peak (speed peak there), then held on a +-1 mm noisy
    plateau whose argmax is late (the KDS held gyaku-tsuki)."""
    t = np.arange(F) / rate
    reach = 300 + (top - 300) / (1 + np.exp(-(t - t_peak) / 0.008))   # 5.6 m/s peak
    plateau = t > t_peak + 0.03
    reach[plateau] = np.minimum(reach[plateau], top - 1.0 + np.sin(t[plateau] * 40.0) * 0.8)
    late = int(round((t_peak + 0.16) * rate))
    reach[late] = top + 0.5                       # the plateau's argmax, on noise, 160 ms after the peak
    return reach


def test_kds_arrival():
    import kds as D

    R = 180.0
    F = 400
    reach = held_punch(F, R)
    eff = np.stack([reach, np.zeros(F), np.zeros(F)], 1)
    v = np.linalg.norm(np.gradient(eff, axis=0), axis=1) * R / 1000.0
    refs = D.arrival_refs(reach, v, R)
    p = int(np.argmax(v))
    seg = reach[p:p + int(round(0.2 * R)) + 1]
    want = p + int(np.argmax(seg >= seg.max() - 5.0))
    check("kds: held punch, the arrival is the first frame within 5 mm of the max reach",
          refs == [want] and want < p + int(round(0.16 * R)), f"{refs} vs {want} (peak {p})")
    check("kds: the arrival is not the plateau's argmax", refs != [p + int(np.nanargmax(seg))])
    # Hikite: the hand pulled back to the hip, as fast, with the reach falling: no reference.
    back = reach[::-1].copy()
    eb = np.stack([back, np.zeros(F), np.zeros(F)], 1)
    vb = np.linalg.norm(np.gradient(eb, axis=0), axis=1) * R / 1000.0
    check("kds: hikite (reach falling, >= 4 m/s) is no blow", D.arrival_refs(back, vb, R) == [],
          str(D.arrival_refs(back, vb, R)))
    check("kds: under 4 m/s is no blow", D.arrival_refs(reach, np.minimum(v, 3.9), R) == [])
    # Hikite blip: the reach falls 478 -> 270 mm at >= 4 m/s, with a 30 mm rise in the middle
    # (the masked speed peak passes): the reach gained < 100 mm, no blow.
    t = np.arange(F) / R
    fold = 478 - 208 / (1 + np.exp(-(t - 0.6) / 0.01))
    bump = (t > 0.58) & (t < 0.62)
    fold[bump] += 30 * np.sin((t[bump] - 0.58) / 0.04 * np.pi)
    ef = np.stack([fold, np.zeros(F), np.zeros(F)], 1)
    vf = np.maximum(np.linalg.norm(np.gradient(ef, axis=0), axis=1) * R / 1000.0, 4.5 * bump)
    check("kds: hikite with a brief reach rise (gain < 100 mm) is no blow", D.arrival_refs(fold, vf, R) == [],
          str(D.arrival_refs(fold, vf, R)))
    small = 300 + 90 / (1 + np.exp(-(t - 0.6) / 0.004))   # a 90 mm jab at > 4 m/s
    es = np.stack([small, np.zeros(F), np.zeros(F)], 1)
    vs = np.linalg.norm(np.gradient(es, axis=0), axis=1) * R / 1000.0
    check("kds: a 90 mm extension is no punch, 178 mm is", D.arrival_refs(small, vs, R) == [] and refs != [],
          f"{D.arrival_refs(small, vs, R)} (peak {vs.max():.1f} m/s)")


def test_kds_drafts():
    import kds as D

    R = 180.0

    def refs(*st):
        return [K.Ref(s, int(round(t * R)), R) for s, t in st]

    a = refs(("R", 4.5833))
    check("kds: in [peak - 50 ms, peak + 250 ms] is confirmed",
          D.cross_check(a, ("R", 4.55)) == [] and a[0].status == "confirmed", str(a[0].reasons))
    b = refs(("L", 4.5833))
    D.cross_check(b, ("R", 4.55))
    check("kds: the authors' other hand is no draft, only counted",
          b[0].status == "confirmed" and D.side_differs(b, ("R", 4.55)) and not D.side_differs(a, ("R", 4.55)),
          str(b[0].reasons))
    c = refs(("R", 2.0), ("L", 4.5833))
    D.cross_check(c, ("R", 4.55))
    check("kds: two in a trial (any hands), both drafts, count 2", all("count 2, usual 1" in r.reasons for r in c)
          and not D.side_differs(c, ("R", 4.55)), str([r.reasons for r in c]))
    check("kds: the one off the authors' window says so",
          c[0].reasons == ["count 2, usual 1", "authors' peak at 4.550 s"], str(c[0].reasons))
    d = refs(("R", 4.85))
    D.cross_check(d, ("R", 4.55))
    check("kds: 300 ms after the authors' peak is a draft", d[0].reasons == ["authors' peak at 4.550 s"], str(d[0].reasons))
    e = refs(("R", 4.5333))
    D.cross_check(e, ("R", 4.55))
    check("kds: 17 ms before the authors' peak is confirmed", e[0].status == "confirmed", str(e[0].reasons))
    g = refs(("R", 4.4833))
    D.cross_check(g, ("R", 4.55))
    check("kds: 67 ms before the authors' peak is a draft", g[0].status == "draft")
    f = refs(("R", 4.5833))
    check("kds: no authors' row keeps the references", D.cross_check(f, None) == ["no authors' row"]
          and f[0].status == "confirmed")
    h = refs(("R", 2.0), ("R", 4.5833))
    D.cross_check(h, None)
    check("kds: no authors' row, two in a trial are still drafts", all(r.reasons == ["count 2, usual 1"] for r in h),
          str([r.reasons for r in h]))


def test_kds_trial_refs():
    import kds as D
    import pig_rig

    R = 180.0
    F = 400
    t = np.arange(F)
    ext = held_punch(F, R)                                   # the right wrist's reach (mm), extending
    fold = ext[::-1].copy()                                   # the left one folds (hikite)
    zero = np.zeros(F)
    S = {n: np.zeros((F, 3)) for n in ("pelvis", "torso", "head", "l_thigh", "l_shank", "l_foot", "l_toes",
                                       "r_thigh", "r_shank", "r_foot", "r_toes")}
    S["torso"][:, 2] = 450.0
    S["l_toes"][:, 1] = 100.0
    S["r_toes"][:, 1] = 100.0
    for s, reach in (("l", fold), ("r", ext)):
        S[s + "_uarm"] = np.stack([zero, zero, zero + 400.0], 1)
        S[s + "_hand"] = np.stack([zero, reach, zero + 400.0], 1)
        S[s + "_larm"] = np.stack([zero, reach * 0.5, zero + 400.0], 1)
    refs = D.trial_refs(S, R)
    want = D.arrival_refs(ext, pig_rig.speed(S["r_hand"], R), R)
    check("kds: trial_refs, one reference on the extending hand (R), none on the hikite",
          [(r.side, r.frame) for r in refs] == [("R", f) for f in want] and len(want) == 1,
          str([(r.side, r.frame) for r in refs]))
    B = D.bones(S)
    k = int(t[-1])
    check("kds: bones fill pig_rig's set", set(B) == set(pig_rig.PARENT), str(set(pig_rig.PARENT) ^ set(B)))
    check("kds: HandMiddle1 / 4 beyond the wrist along the forearm",
          B["RightHandMiddle1"][k, 1] > B["RightHand"][k, 1] and B["RightHandMiddle4"][k, 1] > B["RightHandMiddle1"][k, 1],
          f"{B['RightHand'][k]} {B['RightHandMiddle1'][k]} {B['RightHandMiddle4'][k]}")
    check("kds: Neck at the torso origin, Spine1 halfway pelvis -> torso, Toe_End past the toes",
          B["Neck"][0, 2] == 450.0 and B["Spine1"][0, 2] == 225.0 and B["LeftToe_End"][0, 1] == 115.0)


def test_summary_air_rows():
    import tempfile

    import summary

    def corpus(root, name, clips, matches):
        d = os.path.join(root, name)
        os.makedirs(os.path.join(d, "_bench"))
        with open(os.path.join(d, "clips.tsv"), "w") as f:
            f.write("# clip\ttechnique\tcondition\tkind\tsubject\n" + "".join(c + "\n" for c in clips))
        with open(os.path.join(d, "_bench", "matches.tsv"), "w") as f:
            f.write("# config\tclip\tkind\tside\tref_s\tdet_s\terr_ms\tref_status\n" + "".join(m + "\n" for m in matches))

    with tempfile.TemporaryDirectory() as root:
        corpus(root, "kyokushin", ["a1\tS01 gyaku-zuki\tair\tpunch\tB1", "a2\tS01 gyaku-zuki\tshield\tpunch\tB1",
                                    "k1\tS02 mae-geri\tair\tkick\tB1"],
               ["default\ta1\tpunch\tR\t1.0\t0.98\t-20.0\tconfirmed",
                "default\ta2\tpunch\tR\t1.0\t1.05\t+50.0\tconfirmed",
                "default\tk1\tkick\tR\t1.0\t0.9\t-100.0\tconfirmed"])
        check("summary: one corpus with air punches, no per-corpus section",
              "per corpus" not in summary.write_report(root))
        corpus(root, "kds", ["z1\tgyaku-tsuki\tZEN\tpunch\tID1", "b1\tgyaku-tsuki\tKIB\tpunch\tID1"],
               ["default\tz1\tpunch\tR\t4.0\t4.01\t+10.0\tconfirmed",
                "default\tb1\tpunch\tL\t4.0\t4.03\t+30.0\tconfirmed"])
        text = summary.write_report(root)
        rows = {l.split(" / ")[0]: l[34:].split() for l in text.split("=== punches in the air")[-1].splitlines()
                if " / punch / air" in l}
        check("summary: per-corpus air row, KDS stances summed", rows.get("kds", [])[:2] == ["2/0", "2/0"]
              and rows["kds"][6] == "+20.0", str(rows))
        check("summary: Kyokushin air row leaves shield punches and kicks out", rows.get("kyokushin", [])[:2] == ["1/0", "1/0"]
              and rows["kyokushin"][6] == "-20.0", str(rows))


def test_kds_no_authors_table():
    import tempfile
    import types

    import kds as D

    with tempfile.TemporaryDirectory() as d:
        z = types.SimpleNamespace(cache_dir=d, offline=True)   # no cached analysis listing
        logs = []
        tab, why = D.authors_table(z, logs.append)
        check("kds: analysis zip unreachable, the run goes on without the cross-check",
              tab is None and why is not None and why.startswith("authors' table (") and logs, f"{tab} {why}")


def test_kds_names():
    import tempfile

    import kds as D

    p = D.parse_name("Kinematic Timeseries/ID003_ZEN_T1_kinematics.csv")
    check("kds: name parse", p == dict(subject="ID003", base="ZEN", trial=1), str(p))
    check("kds: clip id", D.clip_id(p) == "kds-ID003-ZEN-T1", D.clip_id(p))
    check("kds: __MACOSX and static skipped",
          D.parse_name("__MACOSX/Kinematic Timeseries/._ID003_ZEN_T1_kinematics.csv") is None
          and D.parse_name("Kinematic Timeseries/static/ID003_STATIC_T1_kinematics.csv") is None)
    check("kds: KUK is KOK", D.parse_name("Kinematic Timeseries/ID004_KUK_T2_kinematics.csv")["base"] == "KOK")
    ents = [dict(name="Kinematic Timeseries/", usize=None)]
    for s in ("ID004", "ID003", "ID005"):
        for b in ("KIB", "ZEN", "KOK"):
            for t in range(1, 6):
                ents.append(dict(name=f"Kinematic Timeseries/{s}_{b}_T{t}_kinematics.csv", usize=10))
    sel = D.select(ents, "2")
    check("kds: first N subjects, 15 trials each, ZEN KOK KIB order",
          len(sel) == 30 and sel[0][1] == dict(subject="ID003", base="ZEN", trial=1)
          and sel[5][1]["base"] == "KOK" and {p["subject"] for _, p in sel} == {"ID003", "ID004"}, str(len(sel)))
    check("kds: 'all' keeps every subject", len(D.select(ents, "all")) == 45)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "w.csv")
        with open(path, "w") as f:
            f.write("subject,base,trial,strike_hand,strike_peak_time_s\n"
                    "ID003,ZEN,1,r_hand,4.55\nID003,KUK,2,l_hand,3.861111111111111\nID003,KIB,3,,\n")
        kin = os.path.join(d, "k.csv")
        with open(kin, "w") as f:
            f.write("frame,time_s,r_hand_x,r_hand_y,r_hand_z\n")
            for i in range(1620):
                f.write(f"{i},{i / 180.0:.6f},{i * 0.001:.6f},0.5,1.0\n")
        rate, segs = D.load(kin)
        check("kds: load, 180 Hz from 6-decimal times, metres to mm",
              rate == 180.0 and abs(segs["r_hand"][10, 0] - 10.0) < 1e-9 and abs(segs["r_hand"][0, 2] - 1000.0) < 1e-9,
              f"{rate} {segs['r_hand'][10]}")
        tab = D.read_authors(path)
        check("kds: the authors' table", tab == {("ID003", "ZEN", 1): ("R", 4.55),
                                                 ("ID003", "KOK", 2): ("L", 3.861111111111111)}, str(tab))


def test_opponent_band():
    F = 300
    v = np.zeros(F)
    v[100] = 6.0
    v[95:106] = np.maximum(v[95:106], 4.0)
    v[100] = 6.0

    def dist(closest):
        d = np.full(F, 400.0)
        for k in range(90, 130):
            d[k] = closest + abs(k - 110) * 10.0
        return d

    sure = K.opponent_refs(dist(30), v, "hand", RATE)
    check("opponent: closest 30 mm is a contact, at the first frame within closest + 30 mm",
          len(sure) == 1 and sure[0][0] == 107 and abs(sure[0][1] - 30) < 1e-9, str(sure))
    band = K.opponent_refs(dist(65), v, "hand", RATE)
    check("opponent: closest 65 mm is kept (the doubtful band)", len(band) == 1 and abs(band[0][1] - 65) < 1e-9, str(band))
    check("opponent: closest 90 mm is no contact", K.opponent_refs(dist(90), v, "hand", RATE) == [])
    # The band's status: analyse() drafts 50-80 mm; the constants hold the band.
    check("opponent: draft band is 50-80 mm", (K.OPP_SURE_MM, K.OPP_CONTACT_MM) == (50.0, 80.0))
    # Story 10-9o: a blow that misses the partner gets its air reference; one near a contact none.
    got = K.opponent_air_refs([107], [130, 400], RATE)
    check("opponent: an air blow with no contact within 0.3 s is a reference", got == [400], str(got))


def test_count_mode():
    def clip(tech, cond, sides):
        return dict(tech=tech, cond=cond, refs=[K.Ref(s, 10 * i, RATE) for i, s in enumerate(sides)])

    clips = [clip("S01", "E02", "LLLRRR"), clip("S01", "E02", "LLLRRR"), clip("S01", "E02", "LLRRR"),
             clip("S02", "E02", "RR"), clip("S02", "E02", "RR"), clip("S02", "E02", "LLL")]
    modes = K.count_mode(clips)
    check("count mode: per technique x condition", modes == {("S01", "E02"): 3, ("S02", "E02"): 2}, str(modes))
    odd = clips[2]
    l_draft = [r.status for r in odd["refs"] if r.side == "L"]
    r_conf = [r.status for r in odd["refs"] if r.side == "R"]
    check("count mode: the odd side is drafted", l_draft == ["draft", "draft"] and r_conf == ["confirmed"] * 3,
          f"{l_draft} {r_conf}")
    check("count mode: its reason", odd["refs"][0].reasons == ["count 2, usual 3"], str(odd["refs"][0].reasons))
    check("count mode: a usual clip stays confirmed", all(r.status == "confirmed" for r in clips[0]["refs"]))
    check("count mode: the other technique's mode", [r.status for r in clips[5]["refs"]] == ["draft"] * 3)


def test_selection():
    names = []
    for s in ("B0001", "B0002", "B0003"):
        for tech in ("S01", "S02", "S03", "S04", "S05"):
            for cond in ("E01", "E02", "E03", "E04"):
                if s == "B0002" and tech == "S05" and cond == "E03":
                    continue
                for tr in ("T01", "T02"):
                    names.append(dict(name=f"{s}/x/2017-01-01-{s}-{tech}-{cond}-{tr}.c3d"))
    sel = K.select(names, 2)
    subj = sorted({p["subject"] for _, p in sel})
    check("selection: first N complete subjects, T01, E01-E03", subj == ["B0001", "B0003"] and len(sel) == 30
          and all(p["trial"] == "T01" and p["cond"] != "E04" for _, p in sel), f"{subj} {len(sel)}")
    check("selection: 'all' keeps incomplete subjects", len(K.select(names, "all")) == 44)


def test_marker_gap():
    F = 200
    x = np.zeros((F, 3))
    holed = x.copy()
    holed[95:105] = np.nan                      # 10 of the 51 frames of +-100 ms: 20 %, not over
    check("marker gap: 20 % is not a gap", K.gap_frac([x, holed], 100, RATE) <= K.GAP_FRAC)
    holed[90:110] = np.nan                      # 20 of 51: 39 %
    g = K.gap_frac([x, holed], 100, RATE)
    check("marker gap: 39 % is a gap", g > K.GAP_FRAC, f"{g}")


def test_fetch_failure():
    import tempfile
    import urllib.error

    import remotezip as R

    calls = {"resolve": 0, "rng": 0}
    real_resolve, real_rng, real_sleep = R.resolve, R._rng, R.time.sleep

    def resolve(url):
        calls["resolve"] += 1
        return url + "?signed", 0

    def rng_fail(url, a, b):
        calls["rng"] += 1
        raise urllib.error.HTTPError(url, 403, "expired", None, None)

    R.resolve, R._rng, R.time.sleep = resolve, rng_fail, (lambda s: None)
    try:
        with tempfile.TemporaryDirectory() as d:
            z = R.RemoteZip("http://example.invalid/zip", d)
            entry = dict(name="a/b.c3d", meth=0, csize=4, usize=4, off=0)
            try:
                z.extract(entry)
                raised = False
            except R.FetchError:
                raised = True
            check("fetch failure: three tries then FetchError", raised and calls["rng"] == 3, str(calls))
            check("fetch failure: the URL is re-resolved before each retry", calls["resolve"] == 3, str(calls))
            # prepare() skips the trial and goes on.
            class Z:
                def entries(self):
                    return [dict(name=f"B0001/x/2017-01-01-B0001-{t}-{c}-T01.c3d") for t in K.TECHNIQUES
                            for c in K.CONDITIONS]

                def extract(self, e):
                    raise R.FetchError("expired")

            res = K.prepare(Z(), os.path.join(d, "out"), 1, log=lambda s: None)
            check("fetch failure: prepare() lists the trial as skipped and continues",
                  len(res["skipped"]) == 15 and res["clips"] == [], str(res["skipped"][:1]))
    finally:
        R.resolve, R._rng, R.time.sleep = real_resolve, real_rng, real_sleep


def test_draft_reasons():
    F = 200
    x = np.zeros((F, 3))
    check("draft reasons: 30 mm from the opponent is confirmed", K.draft_reasons(30.0, [x], 100, RATE) == [])
    r = K.draft_reasons(65.0, [x], 100, RATE)
    check("draft reasons: 65 mm is a draft, maybe no contact",
          len(r) == 1 and r[0].startswith("maybe no contact"), str(r))
    holed = x.copy()
    holed[90:110] = np.nan
    r = K.draft_reasons(None, [x, holed], 100, RATE)
    check("draft reasons: a > 20 % gap is a draft, marker gap", len(r) == 1 and r[0].startswith("marker gap"), str(r))
    check("draft reasons: off the opponent, no gap, confirmed", K.draft_reasons(None, [x], 100, RATE) == [])


def test_antho_lines():
    import tempfile

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "kyo-B0001-S01-E02-T01.ref.tsv")
        with open(path, "w") as f:
            f.write("# kind\tside\tpart\ttime_s\tstatus\tsource\n"
                    "punch\tL\thand\t1.0000\tconfirmed\tphysics\n"
                    "punch\tL\thand\t1.1000\tconfirmed\tantho\n"
                    "punch\tR\thand\t2.0000\tdeleted\tantho\n")
        antho = K.antho_lines(path)
        check("antho lines: both read, any status", [(k, s, t) for _, k, s, t in antho] ==
              [("punch", "L", 1.1), ("punch", "R", 2.0)], str(antho))
        refs = [K.Ref("L", 250, RATE), K.Ref("R", 250, RATE), K.Ref("L", 1000, RATE), K.Ref("R", 470, RATE)]
        kept = K.without_antho("punch", refs, antho)
        check("antho lines: computed refs within 0.15 s of one (same side) are dropped",
              [(r.side, r.t) for r in kept] == [("R", 1.0), ("L", 4.0)], str([(r.side, r.t) for r in kept]))
        K.write_if_changed(path, K.ref_text("S01", "E02", kept, antho))
        text = open(path).read()
        check("antho lines: they survive a rewrite", "punch\tL\thand\t1.1000\tconfirmed\tantho" in text
              and "punch\tR\thand\t2.0000\tdeleted\tantho" in text and "punch\tL\thand\t1.0000" not in text, text)
        again = K.without_antho("punch", refs, K.antho_lines(path))
        check("antho lines: a second rewrite keeps the same text",
              K.ref_text("S01", "E02", again, K.antho_lines(path)) == text)


def test_marker_dropout_speed():
    F = 200
    t = np.arange(F) / RATE
    a = np.stack([t * 1000.0, np.zeros(F), np.zeros(F)], 1)      # 1 m/s along x
    b = a + np.array([0.0, 60.0, 0.0])                           # 60 mm beside it
    b[100:120] = np.nan                                           # drops out for 80 ms
    v = K.effector_speed([a, b], RATE)
    check("marker dropout: the per-marker speed shows no spike", np.nanmax(np.abs(v - 1.0)) < 1e-6,
          f"max {np.nanmax(v):.2f} m/s")
    reach = K.effector_reach([a, b], np.zeros((F, 3)))
    check("marker dropout: the reach steps by under the markers' spread",
          np.nanmax(np.abs(np.diff(reach))) < 60.0, f"{np.nanmax(np.abs(np.diff(reach))):.1f} mm")


def test_summary():
    import tempfile

    import summary

    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "_bench"))
        with open(os.path.join(d, "clips.tsv"), "w") as f:
            f.write("# clip\ttechnique\tcondition\tkind\tsubject\n"
                    "c1\tS01 gyaku-zuki\tshield\tpunch\tB0001\n")
        with open(os.path.join(d, "_bench", "matches.tsv"), "w") as f:
            f.write("# config\tclip\tkind\tside\tref_s\tdet_s\terr_ms\tref_status\n"
                    "default\tc1\tpunch\tL\t1.0000\t0.9800\t-20.0\tconfirmed\n"
                    "default\tc1\tpunch\tR\t2.0000\t1.9600\t-40.0\tconfirmed\n"
                    "default\tc1\tpunch\tL\t3.0000\t3.0100\t+10.0\tdraft\n"
                    "default\tc1\tpunch\tR\t4.0000\t\t\tconfirmed\n"
                    "default\tc1\tpunch\tL\t5.0000\t\t\tdraft\n"
                    "default\tc1\tpunch\tR\t\t6.0000\t\t\n"
                    "default\tc1\tswing\tR\t\t4.0100\t\t\n"
                    "parts\tc1\tpunch\tL\t1.0000\t\t\tconfirmed\n")
        text = summary.corpus_report("kyokushin", d)
        row = next(l for l in text.splitlines() if l.startswith("punch / shield"))
        w = row[34:].split()
        # exp c/d, matched c/d, recall, recall c, found, precision, mean, SD, med|e|
        check("summary: exp c/d and matched c/d", w[0] == "3/2" and w[1] == "2/1", row)
        check("summary: recall and confirmed recall", w[2] == "60.0%" and w[3] == "66.7%", row)
        check("summary: found and precision", w[4] == "4" and w[5] == "75.0%", row)
        check("summary: mean / SD / median |err|", w[6] == "-16.7" and w[7] == "25.2" and w[8] == "20.0", row)
        conf = next(l for l in text.splitlines() if "punch clips / shield" in l)
        check("summary: a type confusion counted", "swing 1" in conf and "kick 0" in conf, conf)


for f in (test_quad_distance, test_shield_contact, test_air_max_reach, test_opponent_band, test_count_mode,
          test_selection, test_marker_gap, test_fetch_failure, test_draft_reasons, test_antho_lines,
          test_marker_dropout_speed, test_summary, test_kds_arrival, test_kds_drafts, test_kds_no_authors_table, test_kds_names,
          test_kds_trial_refs, test_summary_air_rows):
    f()
print(f"\n{'all checks pass' if not failures else str(len(failures)) + ' FAILED'}")
sys.exit(1 if failures else 0)
