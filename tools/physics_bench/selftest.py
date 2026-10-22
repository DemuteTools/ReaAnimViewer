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
                    "default\tc1\tstrike\tR\t\t4.0100\t\t\n"
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
        check("summary: a type confusion counted", "strike 1" in conf and "swing 0" in conf, conf)


for f in (test_quad_distance, test_shield_contact, test_air_max_reach, test_opponent_band, test_count_mode,
          test_selection, test_marker_gap, test_fetch_failure, test_draft_reasons, test_antho_lines,
          test_marker_dropout_speed, test_summary):
    f()
print(f"\n{'all checks pass' if not failures else str(len(failures)) + ' FAILED'}")
sys.exit(1 if failures else 0)
