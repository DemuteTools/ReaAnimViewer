# SPDX-License-Identifier: MIT
"""Story 10-13a: the Kyokushin karate corpus of the physics bench.

Szczesna, Blaszczyszyn, Pawlyta, "Optical motion capture dataset of selected techniques in
beginner and advanced Kyokushin karate athletes", Scientific Data 8, 13 (2021), CC0; figshare
file 24601187 (Karate.zip, 9.9 GB, 1,411 C3D trials). Vicon, 250 Hz, mm, Z up, Plug-In Gait.

File names <date>-<subject>-<Sxx>-<Exx>-<Txx>.c3d: S01 Gyaku-Zuki (reverse punch), S02 Mae-Geri,
S03 Mawashi-Geri gedan, S04 Mawashi-Geri jodan, S05 Ushiro-Mawashi-Geri (the paper's data records);
E01 air, E02 shield (4 markers Tarcza:Tarcza1-4), E03 attacker / E04 defender (both people's
markers; a subject's E04 is the same take as the partner's E03, so E04 is skipped); T01 / T02.

References (one per blow, the striking limb's side; punch part hand, kick part any):
  shield    the first frame the effector comes within 20 mm of the shield quad, after a >= 3 m/s
            (hand) / >= 4 m/s (foot) approach; contacts < 300 ms apart merge
  opponent  per effector speed peak (>= 3 / 4 m/s, the strongest within +-300 ms), its closest
            approach to any partner marker in [peak - 50 ms, peak + 100 ms]; under 80 mm: the
            first frame within closest + 30 mm (closest 50-80 mm: draft, "maybe no contact")
  air       per effector speed peak >= 4 m/s (hand) / 6 m/s (foot) with the reach rising, the max
            reach (effector to shoulder / hip) within 200 ms after the peak
Then draft (with its reason in doubtful.tsv): a side whose blow count differs from the mode of
the same technique x condition ("count N, usual M"), a reference with > 20 % of its effector's
(or the shield's) marker frames missing in +-100 ms ("marker gap"). A clip with no reference at
all is listed "no blow". A line of an existing reference file whose source is `antho` (Antho's
correction, any status) is kept on every rewrite, and a computed reference of the same kind and side
within 0.15 s of it is dropped.
"""
import os
import re
from collections import Counter, defaultdict

import numpy as np

import pig_rig

CORPUS = "kyokushin"
PUBLIC_URL = "https://ndownloader.figshare.com/files/24601187"
TECHNIQUES = {"S01": "gyaku-zuki", "S02": "mae-geri", "S03": "mawashi-geri-gedan", "S04": "mawashi-geri-jodan",
              "S05": "ushiro-mawashi-geri"}
CONDITIONS = {"E01": "air", "E02": "shield", "E03": "opponent"}
DEFAULT_SUBJECTS = 12

NAME_RE = re.compile(r"(\d{4}-\d\d-\d\d)-(B\d+)-(S\d\d)-(E\d\d)-(T\d\d)\.c3d$")

# Effectors (PiG names without side): the hand's front (HNO, the PiG hand origin, ahead of the
# knuckle marker) and the knuckle marker; the foot's markers and its PiG origins.
HAND = ("HNO", "FIN")
FOOT = ("TOE", "HEE", "ANK", "TOO", "FOO")
RAW_HAND = ("FIN",)                # raw markers (the gap check): the PiG origins are modelled
RAW_FOOT = ("TOE", "HEE", "ANK")
PIG_MARKERS = {"LFHD", "RFHD", "LBHD", "RBHD", "C7", "T10", "CLAV", "STRN", "RBAK", "LSHO", "LUPA", "LELB", "LFRM",
               "LWRA", "LWRB", "LFIN", "RSHO", "RUPA", "RELB", "RFRM", "RWRA", "RWRB", "RFIN", "LASI", "RASI", "LPSI",
               "RPSI", "LTHI", "LKNE", "LTIB", "LANK", "LHEE", "LTOE", "RTHI", "RKNE", "RTIB", "RANK", "RHEE", "RTOE"}

SHIELD_TOUCH_MM = 20.0
APPROACH_MS = {"hand": 3.0, "foot": 4.0}      # shield / opponent approach speed (m/s)
AIR_PEAK_MS = {"hand": 4.0, "foot": 6.0}      # air speed peak (m/s)
APPROACH_WIN_S = 0.06                         # the approach speed: max over the 60 ms before arrival
MERGE_S = 0.3
OPP_CONTACT_MM, OPP_SURE_MM, OPP_ARRIVE_MM = 80.0, 50.0, 30.0
OPP_WIN_S = (-0.05, 0.10)
AIR_REACH_S = 0.2
GAP_WIN_S, GAP_FRAC = 0.1, 0.2
ANTHO_S = 0.15                                # a computed reference this near an antho line goes


# ---- Trials ------------------------------------------------------------------------------------

def parse_name(name):
    m = NAME_RE.search(name)
    if not m:
        return None
    return dict(date=m.group(1), subject=m.group(2), tech=m.group(3), cond=m.group(4), trial=m.group(5))


def clip_id(p):
    return f"kyo-{p['subject']}-{p['tech']}-{p['cond']}-{p['trial']}"


def select(entries, subjects):
    """The T01 trials (E01-E03 of S01-S05) to bench, in order. `subjects`: an int = the first N
    subjects with a complete set (15 trials), "all" = every subject (what it has)."""
    by = defaultdict(dict)
    for e in entries:
        p = parse_name(e["name"])
        if not p or p["trial"] != "T01" or p["cond"] not in CONDITIONS or p["tech"] not in TECHNIQUES:
            continue
        by[p["subject"]][(p["tech"], p["cond"])] = (e, p)
    full = len(TECHNIQUES) * len(CONDITIONS)
    if subjects == "all":
        chosen = sorted(by)
    else:
        chosen = [s for s in sorted(by) if len(by[s]) == full][:int(subjects)]
    out = []
    for s in chosen:
        for key in sorted(by[s]):
            out.append(by[s][key])
    return out


# ---- Geometry ----------------------------------------------------------------------------------

def quad_dist(p, Q):
    """Distance (mm) from points p (F x 3) to the quad Q (4 x F x 3, any marker order): to the
    plane when the point projects inside the quad's convex outline, else to its nearest edge.
    NaN where a point or a corner is missing."""
    Q = np.transpose(np.asarray(Q, float), (1, 0, 2))
    p = np.asarray(p, float)
    F = p.shape[0]
    out = np.full(F, np.nan)
    ok = ~(np.isnan(Q).any(axis=(1, 2)) | np.isnan(p).any(axis=1))
    if not ok.any():
        return out
    q, pp = Q[ok], p[ok]
    c = q.mean(1)
    _, _, vt = np.linalg.svd(q - c[:, None])
    e1, e2, n = vt[:, 0], vt[:, 1], vt[:, 2]
    d = pp - c
    h = np.abs((d * n).sum(1))
    x, y = (d * e1).sum(1), (d * e2).sum(1)
    qc = q - c[:, None]
    qx, qy = (qc * e1[:, None]).sum(2), (qc * e2[:, None]).sum(2)
    o = np.argsort(np.arctan2(qy, qx), axis=1)
    qx, qy = np.take_along_axis(qx, o, 1), np.take_along_axis(qy, o, 1)
    q3 = np.take_along_axis(q, o[:, :, None], 1)
    cr = np.stack([(qx[:, (k + 1) % 4] - qx[:, k]) * (y - qy[:, k]) - (qy[:, (k + 1) % 4] - qy[:, k]) * (x - qx[:, k])
                   for k in range(4)], 1)
    inside = (cr >= 0).all(1) | (cr <= 0).all(1)
    best = np.full(len(pp), np.inf)
    for k in range(4):
        a, b = q3[:, k], q3[:, (k + 1) % 4]
        ab = b - a
        t = np.clip(((pp - a) * ab).sum(1) / np.maximum((ab * ab).sum(1), 1e-12), 0, 1)
        best = np.minimum(best, np.linalg.norm(pp - a - t[:, None] * ab, axis=1))
    out[ok] = np.where(inside, h, best)
    return out


def min_dist(points, targets):
    """Per frame, the smallest distance between any of `points` and any of `targets` (lists of
    F x 3 tracks); NaN where every pair is missing."""
    d = None
    for e in points:
        for m in targets:
            x = np.linalg.norm(e - m, axis=1)
            d = x if d is None else np.fmin(d, x)
    return d


def nanmin_stack(arrs):
    a = np.stack(arrs)
    out = np.full(a.shape[1], np.nan)
    ok = ~np.isnan(a).all(0)
    out[ok] = np.nanmin(a[:, ok], axis=0)
    return out


def nanmean_rows(arrs):
    """Per frame, the mean of the values present (NaN where none is)."""
    a = np.stack(arrs)
    out = np.full(a.shape[1], np.nan)
    ok = ~np.isnan(a).all(0)
    out[ok] = np.nanmean(a[:, ok], axis=0)
    return out


def effector_speed(tracks, rate):
    """The mean of the markers' own speeds (m/s): a marker dropping out does not move the value
    the way it moves a centroid."""
    return nanmean_rows([pig_rig.speed(x, rate) for x in tracks])


def effector_reach(tracks, root):
    """The mean of the markers' own distances to `root` (mm)."""
    return nanmean_rows([np.linalg.norm(x - root, axis=1) for x in tracks])


def speed_peaks(v, thr, rate, allowed=None):
    """Frames where v >= thr is the maximum within +-MERGE_S (`allowed`: a mask, other frames
    count as 0)."""
    w = np.where(np.isnan(v), 0.0, v)
    if allowed is not None:
        w = np.where(allowed, w, 0.0)
    W = int(round(MERGE_S * rate))
    out = []
    for i in np.nonzero(w >= thr)[0]:
        lo, hi = max(0, i - W), min(len(w), i + W + 1)
        seg = w[lo:hi]
        if w[i] == seg.max() and i - lo == int(np.argmax(seg)):  # the first of a tie
            out.append(int(i))
    return out


# ---- References --------------------------------------------------------------------------------

class Ref:
    def __init__(self, side, frame, rate, note=""):
        self.side = side
        self.frame = frame
        self.t = frame / rate
        self.reasons = []
        self.note = note

    @property
    def status(self):
        return "draft" if self.reasons else "confirmed"


def shield_refs(dist, v, limb, rate):
    """Shield contacts: `dist` = effector to the quad per frame (mm), `v` = effector speed (m/s)."""
    near = np.nan_to_num(dist, nan=np.inf) < SHIELD_TOUCH_MM
    out, last_end = [], -10 ** 9
    i, n = 0, len(near)
    W = max(1, int(round(APPROACH_WIN_S * rate)))
    M = int(round(MERGE_S * rate))
    while i < n:
        if not near[i]:
            i += 1
            continue
        j = i
        while j + 1 < n and near[j + 1]:
            j += 1
        seg = v[max(0, i - W):i + 1]
        fast = seg.size and np.nanmax(np.where(np.isnan(seg), 0, seg)) >= APPROACH_MS[limb]
        if i - last_end < M and out:
            last_end = j                                    # merged into the previous contact
        elif fast:
            out.append(i)
            last_end = j
        i = j + 1
    return out


def opponent_refs(dist, v, limb, rate):
    """Opponent contacts: (frame, closest mm) per effector speed peak whose closest approach to
    the partner is under OPP_CONTACT_MM."""
    out = []
    for p in speed_peaks(v, APPROACH_MS[limb], rate):
        lo = max(0, p + int(round(OPP_WIN_S[0] * rate)))
        hi = min(len(dist), p + int(round(OPP_WIN_S[1] * rate)) + 1)
        seg = dist[lo:hi]
        if not np.isfinite(seg).any():
            continue
        closest = float(np.nanmin(seg))
        if closest >= OPP_CONTACT_MM:
            continue
        k = int(np.argmax(np.nan_to_num(seg, nan=np.inf) <= closest + OPP_ARRIVE_MM))
        out.append((lo + k, closest))
    return out


def air_refs(reach, v, limb, rate):
    """Air blows: the max reach (effector to root, mm) within AIR_REACH_S after each speed peak
    with the reach rising."""
    rising = np.gradient(reach) > 0
    out = []
    H = int(round(AIR_REACH_S * rate))
    for p in speed_peaks(v, AIR_PEAK_MS[limb], rate, allowed=rising):
        seg = reach[p:p + H + 1]
        if not np.isfinite(seg).any():
            continue
        out.append(p + int(np.nanargmax(seg)))
    return out


def gap_frac(raw_tracks, frame, rate):
    W = int(round(GAP_WIN_S * rate))
    lo, hi = max(0, frame - W), frame + W + 1
    miss = np.zeros(max(0, min(hi, len(raw_tracks[0])) - lo), bool)
    for x in raw_tracks:
        miss |= np.isnan(x[lo:hi]).any(axis=1)
    return float(miss.mean()) if miss.size else 1.0


def draft_reasons(closest, raw_tracks, frame, rate):
    """Why a reference is a draft ([] = confirmed): `closest` = its closest approach to the
    opponent (mm; None off the opponent), `raw_tracks` = the raw markers checked for gaps."""
    out = []
    if closest is not None and closest >= OPP_SURE_MM:
        out.append(f"maybe no contact (closest {closest:.0f} mm)")
    g = gap_frac(raw_tracks, frame, rate)
    if g > GAP_FRAC:
        out.append(f"marker gap ({g * 100:.0f} % of +-100 ms)")
    return out


def analyse(path, p):
    """Loads one trial: (rate, labels, P, prefix, refs). refs: Ref list (both sides)."""
    rate, labels, P = pig_rig.load(path)
    pre = p["subject"] + ":" if any(l.startswith(p["subject"] + ":") for l in labels) else pig_rig.subject_prefix(labels)
    L = lambda n: pig_rig.track(P, labels, pre + n)
    limb = "hand" if p["tech"] == "S01" else "foot"
    effn, rawn = (HAND, RAW_HAND) if limb == "hand" else (FOOT, RAW_FOOT)
    cond = CONDITIONS[p["cond"]]
    refs = []
    shield = partner = None
    if cond == "shield":
        shield = [pig_rig.track(P, labels, f"Tarcza:Tarcza{k}") for k in range(1, 5)]
    if cond == "opponent":
        partner = [pig_rig.track(P, labels, l) for l in labels
                   if ":" in l and not l.startswith(pre) and l.split(":", 1)[1] in PIG_MARKERS]
        if not partner:
            raise ValueError("no partner markers")
    for side in "LR":
        eff = [L(side + n) for n in effn]
        v = effector_speed(eff, rate)
        raw = [L(side + n) for n in rawn]
        closest = {}
        if cond == "shield":
            d = nanmin_stack([quad_dist(e, np.stack(shield)) for e in eff])
            new = [Ref(side, f, rate) for f in shield_refs(d, v, limb, rate)]
            raw = raw + shield
        elif cond == "opponent":
            new = []
            for f, cl in opponent_refs(min_dist(eff, partner), v, limb, rate):
                r = Ref(side, f, rate, note=f"closest {cl:.0f} mm")
                closest[id(r)] = cl
                new.append(r)
        else:
            if limb == "hand":
                root = L(side + "CLO")
            else:
                root = pig_rig._lerp(pig_rig._lerp(L(side + "ASI"), L(side + "PSI"), 0.5), L(side + "FEO"), 0.15)
            new = [Ref(side, f, rate) for f in air_refs(effector_reach(eff, root), v, limb, rate)]
        for r in new:
            r.reasons += draft_reasons(closest.get(id(r)), raw, r.frame, rate)
        refs += new
    refs.sort(key=lambda r: r.t)
    return rate, labels, P, pre, refs


def count_mode(clips):
    """Odd counts: `clips` = list of dicts (tech, cond, refs). Per technique x condition, the mode
    of the per-side blow counts (sides with at least one blow); a side off the mode is drafted.
    Returns {(tech, cond): mode}."""
    counts = defaultdict(list)
    for c in clips:
        for side in "LR":
            n = sum(r.side == side for r in c["refs"])
            if n:
                counts[(c["tech"], c["cond"])].append(n)
    modes = {}
    for k, v in counts.items():
        cnt = Counter(v)
        top = max(cnt.values())
        modes[k] = min(x for x, y in cnt.items() if y == top)   # a tie: the smaller count
    for c in clips:
        m = modes.get((c["tech"], c["cond"]))
        for side in "LR":
            mine = [r for r in c["refs"] if r.side == side]
            if mine and m is not None and len(mine) != m:
                for r in mine:
                    r.reasons.append(f"count {len(mine)}, usual {m}")
    return modes


def kind_of(tech):
    return ("punch", "hand") if tech == "S01" else ("kick", "any")


def antho_lines(path):
    """The lines of an existing reference file whose source is `antho`: (line, kind, side, t)."""
    out = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\r\n")
                w = line.split()
                if len(w) == 6 and not line.lstrip().startswith("#") and w[5] == "antho":
                    try:
                        out.append(("\t".join(w), w[0], w[1], float(w[3])))
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def without_antho(kind, refs, antho):
    """`refs` less those of the same kind and side within ANTHO_S of an antho line."""
    return [r for r in refs
            if not any(k == kind and s == r.side and abs(t - r.t) <= ANTHO_S + 1e-9 for _, k, s, t in antho)]


def ref_text(tech, cond, refs, antho=()):
    """The reference file: the computed references (already less those Antho replaced), then
    Antho's lines, by time."""
    kind, part = kind_of(tech)
    lines = ["# kind\tside\tpart\ttime_s\tstatus\tsource",
             f"# Kyokushin {TECHNIQUES[tech]} ({tech}), {CONDITIONS[cond]} ({cond}): written by tools/physics_bench"]
    rows = [(r.t, f"{kind}\t{r.side}\t{part}\t{r.t:.4f}\t{r.status}\tphysics") for r in refs]
    rows += [(t, line) for line, _, _, t in antho]
    lines += [x for _, x in sorted(rows, key=lambda x: x[0])]
    return "\n".join(lines) + "\n"


def write_if_changed(path, text):
    """Writes `text` unless the file already holds it (its time stays: the bench sees no change)."""
    try:
        with open(path, newline="") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)
    return True


# ---- The corpus --------------------------------------------------------------------------------

CONVERTER_VERSION = "1"   # bump to reconvert every csv


def prepare(zipper, out_dir, subjects, log=print):
    """Fetch, convert, reference. Returns dict(clips, skipped, downloaded, converted)."""
    os.makedirs(out_dir, exist_ok=True)
    from remotezip import FetchError

    trials = select(zipper.entries(), subjects)
    n_subj = len({p["subject"] for _, p in trials})
    if subjects != "all" and n_subj < int(subjects):
        log(f"  only {n_subj} subjects have a complete set (asked for {subjects})")
    stamp = os.path.join(out_dir, ".converter")
    try:
        old_version = open(stamp).read().strip()
    except OSError:
        old_version = ""
    skipped, skipped_ids, downloaded, converted, clips = [], [], 0, 0, []
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
            rate, labels, P, pre, refs = analyse(path, p)
            csv = os.path.join(out_dir, cid + ".csv")
            stale = (fresh or old_version != CONVERTER_VERSION or not os.path.exists(csv)
                     or os.path.getmtime(csv) < os.path.getmtime(path))
            if stale:
                L = lambda n: pig_rig.track(P, labels, pre + n)
                pig_rig.write_csv(csv, os.path.basename(e["name"]), pig_rig.bones(L), rate)
                converted += 1
        except Exception as ex:  # noqa: BLE001 - any unreadable trial is skipped, the run goes on
            skipped.append(f"{cid} ({type(ex).__name__}: {ex})")
            skipped_ids.append(cid)
            log(f"  skip {cid}: {ex}")
            continue
        clips.append(dict(id=cid, tech=p["tech"], cond=p["cond"], subject=p["subject"], refs=refs))
        if (k + 1) % 15 == 0:
            log(f"  {k + 1} / {len(trials)} trials")
    with open(stamp, "w") as f:
        f.write(CONVERTER_VERSION + "\n")
    count_mode(clips)
    # A trial skipped this run keeps its clip from an earlier run.
    keep = {i + ext for i in skipped_ids for ext in (".csv", ".ref.tsv")}
    doubt = ["# clip\tkind\tside\ttime_s\treason"]
    for c in clips:
        kind, _ = kind_of(c["tech"])
        ref_path = os.path.join(out_dir, c["id"] + ".ref.tsv")
        antho = antho_lines(ref_path)
        c["refs"] = without_antho(kind, c["refs"], antho)
        write_if_changed(ref_path, ref_text(c["tech"], c["cond"], c["refs"], antho))
        keep |= {c["id"] + ".csv", c["id"] + ".ref.tsv"}
        if not c["refs"] and not antho:
            doubt.append(f"{c['id']}\t{kind}\t-\t-\tno blow")
        for r in c["refs"]:
            if r.reasons:
                doubt.append(f"{c['id']}\t{kind}\t{r.side}\t{r.t:.4f}\t{'; '.join(r.reasons)}")
    write_if_changed(os.path.join(out_dir, "doubtful.tsv"), "\n".join(doubt) + "\n")
    rows = ["# clip\ttechnique\tcondition\tkind\tsubject"]
    for c in clips:
        rows.append(f"{c['id']}\t{c['tech']} {TECHNIQUES[c['tech']]}\t{CONDITIONS[c['cond']]}\t{kind_of(c['tech'])[0]}"
                    f"\t{c['subject']}")
    write_if_changed(os.path.join(out_dir, "clips.tsv"), "\n".join(rows) + "\n")
    # Clips of an earlier, wider selection leave the bench folder (the cache keeps their trials).
    for f in os.listdir(out_dir):
        if f.startswith("kyo-") and (f.endswith(".csv") or f.endswith(".ref.tsv")) and f not in keep:
            os.remove(os.path.join(out_dir, f))
    return dict(clips=clips, skipped=skipped, downloaded=downloaded, converted=converted)
