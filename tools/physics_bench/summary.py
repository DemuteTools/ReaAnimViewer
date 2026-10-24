# SPDX-License-Identifier: MIT
"""Story 10-13a: the physics bench's report, from each corpus' trigger_bench matches.

For every fixtures/_physics/<corpus>/ holding clips.tsv and _bench/matches.tsv (default matrix):
per corpus x condition x kind, then per technique, the expected references (confirmed / draft),
matched, recall, found, precision, the signed error (det - ref) mean / SD and the median |error|;
then the unreferenced punch / swing / kick events found on those clips (type confusions; story
10-9o removed Strike, folded into Punch). Story 10-13b: when two corpora have punches in the air,
one row per corpus compares them (Kyokushin air, KDS stances). Written to
fixtures/_physics/report.txt.

    python tools/physics_bench/summary.py [fixtures/_physics]
"""
import os
import statistics
import sys
from collections import defaultdict

ARM_LEG_KINDS = ("punch", "swing", "kick")
COND_ORDER = ("air", "shield", "opponent", "ZEN", "KOK", "KIB")   # Kyokushin, then KDS stances
AIR_CONDS = ("air", "ZEN", "KOK", "KIB")   # blows in the air (story 10-13b: KDS punches are all air)


def read_tsv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            rows.append(line.split("\t"))
    return rows


class Cell:
    def __init__(self):
        self.exp_c = self.exp_d = self.match_c = self.match_d = self.extra = 0
        self.err = []

    def add(self, ref_status, det, err):
        if ref_status:
            if ref_status == "confirmed":
                self.exp_c += 1
            else:
                self.exp_d += 1
            if det:
                if ref_status == "confirmed":
                    self.match_c += 1
                else:
                    self.match_d += 1
                self.err.append(float(err))
        elif det:
            self.extra += 1

    def text(self):
        exp, m = self.exp_c + self.exp_d, self.match_c + self.match_d
        found = m + self.extra
        rec = f"{100.0 * m / exp:5.1f}%" if exp else "    -"
        rec_c = f"{100.0 * self.match_c / self.exp_c:5.1f}%" if self.exp_c else "    -"
        prec = f"{100.0 * m / found:5.1f}%" if found else "    -"
        if self.err:
            mean = statistics.fmean(self.err)
            sd = statistics.stdev(self.err) if len(self.err) > 1 else 0.0
            med = statistics.median(abs(e) for e in self.err)
            t = f"{mean:+7.1f} {sd:6.1f} {med:6.1f}"
        else:
            t = f"{'-':>7} {'-':>6} {'-':>6}"
        return (f"{self.exp_c:5d}/{self.exp_d:<4d} {self.match_c:5d}/{self.match_d:<4d} {rec} {rec_c} {found:5d} "
                f"{prec}  {t}")


HEADER = (f"{'':34s} {'exp c/d':>10s} {'matched c/d':>10s} {'recall':>6s} {'rec c':>6s} {'found':>5s} {'prec':>6s}  "
          f"{'mean':>7s} {'SD':>6s} {'med|e|':>6s}")


def corpus_cells(folder):
    """(clips, seen, by_cond, by_tech, confusions) of one corpus folder."""
    clips = {r[0]: dict(tech=r[1], cond=r[2], kind=r[3]) for r in read_tsv(os.path.join(folder, "clips.tsv"))}
    matches = [r for r in read_tsv(os.path.join(folder, "_bench", "matches.tsv")) if r[0] == "default"]
    by_cond = defaultdict(Cell)        # (cond, kind)
    by_tech = defaultdict(Cell)        # (cond, kind, tech)
    confusions = defaultdict(lambda: defaultdict(int))   # (cond, kind) -> found kind -> n
    seen = set()
    for r in matches:
        r = r + [""] * (8 - len(r))
        _, clip, kind, side, ref_s, det_s, err, status = r[:8]
        c = clips.get(clip)
        if not c:
            continue
        seen.add(clip)
        if kind == c["kind"]:
            for cell in (by_cond[(c["cond"], kind)], by_tech[(c["cond"], kind, c["tech"])]):
                cell.add(status if ref_s else "", det_s, err)
        elif det_s and not ref_s and kind in ARM_LEG_KINDS:
            confusions[(c["cond"], c["kind"])][kind] += 1
    return clips, seen, by_cond, by_tech, confusions


def total(cells):
    tot = Cell()
    for cell in cells:
        for x in ("exp_c", "exp_d", "match_c", "match_d", "extra"):
            setattr(tot, x, getattr(tot, x) + getattr(cell, x))
        tot.err += cell.err
    return tot


def corpus_report(name, folder):
    clips, seen, by_cond, by_tech, confusions = corpus_cells(folder)
    lines = [f"=== {name}: {len(seen)} of {len(clips)} clips benched (default matrix, match +-150 ms; "
             f"error = detection - reference, ms) ===", HEADER]
    conds = sorted({k[0] for k in by_cond}, key=lambda x: COND_ORDER.index(x) if x in COND_ORDER else 9)
    for kind in sorted({k[1] for k in by_cond}):
        cells = [by_cond[(cond, kind)] for cond in conds if (cond, kind) in by_cond]
        for cond in conds:
            cell = by_cond.get((cond, kind))
            if cell:
                lines.append(f"{kind + ' / ' + cond:34s} {cell.text()}")
        tot = total(cells)
        lines.append(f"{kind + ' / all':34s} {tot.text()}")
    lines.append("Per technique:")
    for (cond, kind, tech) in sorted(by_tech, key=lambda k: (k[2], conds.index(k[0]) if k[0] in conds else 9)):
        lines.append(f"  {tech + ' / ' + cond:32s} {by_tech[(cond, kind, tech)].text()}")
    lines.append("Unreferenced arm / leg events found (type confusions), per condition x clip kind:")
    for cond in conds:
        for kind in sorted({k[1] for k in by_cond}):
            n = confusions.get((cond, kind))
            if n is None and (cond, kind) not in by_cond:
                continue
            txt = ", ".join(f"{k} {n.get(k, 0)}" for k in ARM_LEG_KINDS if k != kind) if n is not None else \
                ", ".join(f"{k} 0" for k in ARM_LEG_KINDS if k != kind)
            lines.append(f"  {kind + ' clips / ' + cond:32s} {txt}")
    return "\n".join(lines)


def air_punch_rows(folders):
    """Story 10-13b: each corpus' punches in the air on one row (shown when two corpora have some)."""
    rows = []
    for name, folder in folders:
        by_cond = corpus_cells(folder)[2]
        cells = [c for (cond, kind), c in by_cond.items() if kind == "punch" and cond in AIR_CONDS]
        if cells:
            rows.append(f"{name + ' / punch / air':34s} {total(cells).text()}")
    if len(rows) < 2:
        return None
    return "\n".join(["=== punches in the air, per corpus ===", HEADER] + rows)


def write_report(phys):
    parts, folders = [], []
    for name in sorted(os.listdir(phys)) if os.path.isdir(phys) else []:
        folder = os.path.join(phys, name)
        if os.path.exists(os.path.join(folder, "clips.tsv")) and os.path.exists(os.path.join(folder, "_bench", "matches.tsv")):
            parts.append(corpus_report(name, folder))
            folders.append((name, folder))
    air = air_punch_rows(folders)
    if air:
        parts.append(air)
    text = "\n\n".join(parts) if parts else "(no corpus benched)"
    with open(os.path.join(phys, "report.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")
    return text


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    print(write_report(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "fixtures", "_physics")))
