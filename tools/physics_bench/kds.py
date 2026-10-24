# SPDX-License-Identifier: MIT
"""Story 10-13b: the Karate Dataset Stances corpus of the physics bench (air punches).

Pavani et al., "Karate Dataset Stances" (2026), figshare 10.6084/m9.figshare.32288943, CC BY 4.0.
12 karateka (ID003-ID014), three stances (ZEN zenkutsu-dachi, KOK kokutsu-dachi, KIB kiba-dachi),
5 gyaku-tsuki trials each = 180 trials, Theia3D markerless (20 Hz zero-lag filter), 180 Hz, 9 s.
Source: KDS_kinematic_timeseries.zip (file 64605942), per trial
`Kinematic Timeseries/<subject>_<base>_T<n>_kinematics.csv`: 19 segment origins in metres, lab
frame Z up (`__MACOSX/` and `static/` skipped). Cross-check: kinematic_analysis_wide.csv of
KDS_kinematic_analysis.zip (file 68281246): subject, base, trial, strike_hand, strike_peak_time_s
(the authors' wrist speed peak).

References (kind punch, part hand, side = the hand): per wrist speed peak >= 4 m/s with the reach
(wrist to upper arm origin) rising, the arrival at full extension = the first frame within 5 mm of
the max reach in the 200 ms after the peak (the karateka hold the punch: the reach plateaus at
+-1 mm, its argmax lands on noise up to 130 ms later), kept only when the reach gained >= 100 mm
in the 300 ms up to the arrival: the punching arm extends, the hikite (the other hand pulled back
to the hip at the same time, often faster) folds, and its brief reach rises are no blow. Then
draft (reason in doubtful.tsv): every reference of a trial holding more than one ("count N,
usual 1"); a reference outside [authors' peak - 50 ms, peak + 250 ms] ("authors' peak at X s").
The authors' strike_hand is their faster hand, the hikite on about a third of the trials, so a
side that differs is no draft, only counted in the run's log. A trial with none: "no blow"; a
trial missing from the authors' table keeps its references, listed "no authors' row". Antho's
lines are kept as for Kyokushin (kyokushin.antho_lines / without_antho).
"""
import csv
import os
import re
from collections import defaultdict

import numpy as np

import pig_rig
from kyokushin import Ref, antho_lines, speed_peaks, without_antho, write_if_changed

CORPUS = "kds"
PUBLIC_URL = "https://ndownloader.figshare.com/files/64605942"
ANALYSIS_URL = "https://ndownloader.figshare.com/files/68281246"
ANALYSIS_MEMBER = "kinematic_analysis_wide.csv"
TECHNIQUE = "gyaku-tsuki"
BASES = ("ZEN", "KOK", "KIB")
BASE_CANONICAL = {"KUK": "KOK"}               # the authors' loader fixes this typo silently
DEFAULT_SUBJECTS = "all"
KIND, PART = "punch", "hand"

NAME_RE = re.compile(r"(?:^|/)(ID\d+)_([A-Z]{3})_T(\d+)_kinematics\.csv$")

PEAK_MS = 4.0          # wrist speed peak (m/s)
REACH_S = 0.2          # the full extension: within this after the peak
ARRIVE_MM = 5.0        # the first frame within this of the max reach
GAIN_MM = 100.0        # a punch: the reach gained at least this...
GAIN_S = 0.3           # ...in this up to the arrival (the hikite folds)
AUTHORS_WIN_S = (-0.05, 0.25)   # a reference within [authors' peak + this[0], + this[1]]
EPS_S = 1e-6


# ---- Trials ------------------------------------------------------------------------------------

def parse_name(name):
    if name.startswith("__MACOSX/") or "/static/" in name:
        return None
    m = NAME_RE.search(name)
    if not m:
        return None
    base = BASE_CANONICAL.get(m.group(2), m.group(2))
    if base not in BASES:
        return None
    return dict(subject=m.group(1), base=base, trial=int(m.group(3)))


def clip_id(p):
    return f"kds-{p['subject']}-{p['base']}-T{p['trial']}"


def select(entries, subjects):
    """The trials to bench, in order: every subject's (`subjects` "all") or the first N's."""
    by = defaultdict(list)
    for e in entries:
        if e.get("usize") is None or e["name"].endswith("/"):
            continue
        p = parse_name(e["name"])
        if p:
            by[p["subject"]].append((e, p))
    chosen = sorted(by) if subjects == "all" else sorted(by)[:int(subjects)]
    out = []
    for s in chosen:
        out += sorted(by[s], key=lambda ep: (BASES.index(ep[1]["base"]), ep[1]["trial"]))
    return out


# ---- Reading -----------------------------------------------------------------------------------

def load(path):
    """(rate_hz, {segment: F x 3 track in mm}) from a KDS kinematics csv (segment origins, m)."""
    with open(path, encoding="utf-8") as f:
        header = f.readline().strip().split(",")
    data = np.loadtxt(path, delimiter=",", skiprows=1, ndmin=2)
    col = {n: i for i, n in enumerate(header)}
    t = data[:, col["time_s"]]
    # time_s has 6 decimals (0.005556): the rate from the whole span, not one rounded step.
    rate = float(round((len(t) - 1) / (t[-1] - t[0]), 3)) if len(t) > 1 and t[-1] > t[0] else 180.0
    segs = {}
    for n in header:
        if n.endswith("_x") and n[:-2] + "_y" in col and n[:-2] + "_z" in col:
            s = n[:-2]
            segs[s] = data[:, [col[s + "_x"], col[s + "_y"], col[s + "_z"]]] * 1000.0
    return rate, segs


def _lerp(a, b, t):
    return a + (b - a) * t


def bones(S):
    """Theia segment origins (mm, lab Z up) -> mixamorig bones (pig_rig.PARENT's set)."""
    pel, tor = S["pelvis"], S["torso"]
    B = {"Hips": pel, "Spine": _lerp(pel, tor, 0.25), "Spine1": _lerp(pel, tor, 0.5),
         "Spine2": _lerp(pel, tor, 0.75), "Neck": tor, "Head": S["head"]}
    for s, side in (("l", "Left"), ("r", "Right")):
        hand, larm = S[s + "_hand"], S[s + "_larm"]
        B[side + "Shoulder"] = _lerp(tor, S[s + "_uarm"], 0.5)
        B[side + "Arm"] = S[s + "_uarm"]
        B[side + "ForeArm"] = larm
        B[side + "Hand"] = hand
        hm1 = hand + 0.4 * (hand - larm)
        B[side + "HandMiddle1"] = hm1
        B[side + "HandMiddle4"] = hm1 + 0.5 * (hm1 - hand)
        B[side + "UpLeg"] = S[s + "_thigh"]
        B[side + "Leg"] = S[s + "_shank"]
        B[side + "Foot"] = S[s + "_foot"]
        B[side + "ToeBase"] = S[s + "_toes"]
        B[side + "Toe_End"] = S[s + "_toes"] + 0.15 * (S[s + "_toes"] - S[s + "_foot"])
    return B


def read_authors(path):
    """{(subject, base, trial): (side "L"/"R", peak_s)} from the authors' wide table."""
    out = {}
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            try:
                base = BASE_CANONICAL.get(row["base"].strip(), row["base"].strip())
                key = (row["subject"].strip(), base, int(float(row["trial"])))
                hand = row["strike_hand"].strip()
                side = {"l_hand": "L", "r_hand": "R"}.get(hand)
                if side is None:
                    continue
                out[key] = (side, float(row["strike_peak_time_s"]))
            except (KeyError, ValueError, AttributeError):
                continue
    return out


# ---- References --------------------------------------------------------------------------------

def arrival_refs(reach, v, rate):
    """Air punches: per speed peak >= PEAK_MS with the reach (mm) rising, the first frame within
    ARRIVE_MM of the max reach in the REACH_S after the peak, when the reach gained >= GAIN_MM
    in the GAIN_S up to it."""
    rising = np.gradient(reach) > 0
    out = []
    H = int(round(REACH_S * rate))
    for p in speed_peaks(v, PEAK_MS, rate, allowed=rising):
        seg = reach[p:p + H + 1]
        if not np.isfinite(seg).any():
            continue
        top = float(np.nanmax(seg))
        a = p + int(np.argmax(np.nan_to_num(seg, nan=-np.inf) >= top - ARRIVE_MM))
        back = reach[max(0, a - int(round(GAIN_S * rate))):a + 1]
        if reach[a] - np.nanmin(back) >= GAIN_MM:
            out.append(a)
    return out


def trial_refs(S, rate):
    """Both hands' references (Ref list, by time)."""
    refs = []
    for s, side in (("l", "L"), ("r", "R")):
        hand = S[s + "_hand"]
        reach = np.linalg.norm(hand - S[s + "_uarm"], axis=1)
        refs += [Ref(side, f, rate) for f in arrival_refs(reach, pig_rig.speed(hand, rate), rate)]
    refs.sort(key=lambda r: r.t)
    return refs


def cross_check(refs, authors):
    """Drafts against the authors' row `authors` = (strike side, peak_s) (None: no row). Returns
    the clip-level doubtful reasons (the references' own go to their `reasons`)."""
    if len(refs) > 1:
        for r in refs:
            r.reasons.append(f"count {len(refs)}, usual 1")
    if authors is None:
        return ["no authors' row"]
    _, peak = authors
    for r in refs:
        if not (peak + AUTHORS_WIN_S[0] - EPS_S <= r.t <= peak + AUTHORS_WIN_S[1] + EPS_S):
            r.reasons.append(f"authors' peak at {peak:.3f} s")
    return []


def side_differs(refs, authors):
    """True when the trial's one reference is on the other hand than the authors' strike_hand."""
    return authors is not None and len(refs) == 1 and refs[0].side != authors[0]


def ref_text(base, refs, antho=()):
    lines = ["# kind\tside\tpart\ttime_s\tstatus\tsource",
             f"# KDS {TECHNIQUE}, {base} stance, air: written by tools/physics_bench"]
    rows = [(r.t, f"{KIND}\t{r.side}\t{PART}\t{r.t:.4f}\t{r.status}\tphysics") for r in refs]
    rows += [(t, line) for line, _, _, t in antho]
    lines += [x for _, x in sorted(rows, key=lambda x: x[0])]
    return "\n".join(lines) + "\n"


# ---- The corpus --------------------------------------------------------------------------------

CONVERTER_VERSION = "2"   # bump to reconvert every csv


def authors_table(zipper, log):
    """The authors' table from the analysis zip (its own RemoteZip in <cache>/analysis):
    (table or None, skipped text or None)."""
    from remotezip import FetchError, RemoteZip

    try:
        a = RemoteZip(ANALYSIS_URL, os.path.join(zipper.cache_dir, "analysis"), offline=zipper.offline)
        e = next((x for x in a.entries() if x["name"].rsplit("/", 1)[-1] == ANALYSIS_MEMBER), None)
        if e is None:
            raise FetchError(f"no {ANALYSIS_MEMBER} in the analysis zip")
        path, _ = a.extract(e)
        return read_authors(path), None
    except (FetchError, OSError, ValueError, csv.Error) as ex:   # ValueError: a bad encoding too
        log(f"  no authors' table, no cross-check: {ex}")
        return None, f"authors' table ({ex})"


def prepare(zipper, out_dir, subjects, log=print):
    """Fetch, convert, reference. Returns dict(clips, skipped, downloaded, converted)."""
    os.makedirs(out_dir, exist_ok=True)
    from remotezip import FetchError

    trials = select(zipper.entries(), subjects)
    n_subj = len({p["subject"] for _, p in trials})
    if subjects != "all" and n_subj < int(subjects):
        log(f"  only {n_subj} subjects (asked for {subjects})")
    authors, why = authors_table(zipper, log)
    skipped = [why] if why else []
    stamp = os.path.join(out_dir, ".converter")
    try:
        old_version = open(stamp).read().strip()
    except OSError:
        old_version = ""
    skipped_ids, downloaded, converted, clips = [], 0, 0, []
    for k, (e, p) in enumerate(trials):
        cid = clip_id(p)
        try:
            path, fresh = zipper.extract(e)
        except FetchError as ex:
            skipped.append(f"{cid} ({ex})")
            skipped_ids.append(cid)
            log(f"  skip {cid}: {ex}")
            continue
        downloaded += fresh
        try:
            rate, S = load(path)
            refs = trial_refs(S, rate)
            csv_path = os.path.join(out_dir, cid + ".csv")
            stale = (fresh or old_version != CONVERTER_VERSION or not os.path.exists(csv_path)
                     or os.path.getmtime(csv_path) < os.path.getmtime(path))
            if stale:
                pig_rig.write_csv(csv_path, os.path.basename(e["name"]), bones(S), rate)
                converted += 1
        except Exception as ex:  # noqa: BLE001 - any unreadable trial is skipped, the run goes on
            skipped.append(f"{cid} ({type(ex).__name__}: {ex})")
            skipped_ids.append(cid)
            log(f"  skip {cid}: {ex}")
            continue
        key = (p["subject"], p["base"], p["trial"])
        clips.append(dict(id=cid, base=p["base"], subject=p["subject"], refs=refs, key=key))
        if (k + 1) % 15 == 0:
            log(f"  {k + 1} / {len(trials)} trials")
    with open(stamp, "w") as f:
        f.write(CONVERTER_VERSION + "\n")
    keep = {i + ext for i in skipped_ids for ext in (".csv", ".ref.tsv")}
    doubt = ["# clip\tkind\tside\ttime_s\treason"]
    n_one = n_side = 0
    for c in clips:
        ref_path = os.path.join(out_dir, c["id"] + ".ref.tsv")
        antho = antho_lines(ref_path)
        c["refs"] = without_antho(KIND, c["refs"], antho)
        # The checks run on the references left after Antho's lines (a table missing: count only).
        row = authors.get(c["key"]) if authors is not None else None
        c["notes"] = cross_check(c["refs"], row)
        if authors is None:
            c["notes"] = []
        n_one += row is not None and len(c["refs"]) == 1
        n_side += side_differs(c["refs"], row)
        write_if_changed(ref_path, ref_text(c["base"], c["refs"], antho))
        keep |= {c["id"] + ".csv", c["id"] + ".ref.tsv"}
        if not c["refs"] and not antho:
            doubt.append(f"{c['id']}\t{KIND}\t-\t-\tno blow")
        for n in c["notes"]:
            doubt.append(f"{c['id']}\t{KIND}\t-\t-\t{n}")
        for r in c["refs"]:
            if r.reasons:
                doubt.append(f"{c['id']}\t{KIND}\t{r.side}\t{r.t:.4f}\t{'; '.join(r.reasons)}")
    write_if_changed(os.path.join(out_dir, "doubtful.tsv"), "\n".join(doubt) + "\n")
    if authors is not None:
        log(f"  the authors' strike_hand is the other hand on {n_side} of the {n_one} one-punch trials "
            f"(their faster hand, the hikite; no draft)")
    rows = ["# clip\ttechnique\tcondition\tkind\tsubject"]
    rows += [f"{c['id']}\t{TECHNIQUE}\t{c['base']}\t{KIND}\t{c['subject']}" for c in clips]
    write_if_changed(os.path.join(out_dir, "clips.tsv"), "\n".join(rows) + "\n")
    for f in os.listdir(out_dir):
        if f.startswith("kds-") and (f.endswith(".csv") or f.endswith(".ref.tsv")) and f not in keep:
            os.remove(os.path.join(out_dir, f))
    return dict(clips=clips, skipped=skipped, downloaded=downloaded, converted=converted)
