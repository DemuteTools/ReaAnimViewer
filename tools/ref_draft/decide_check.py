#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# A self-check of decide.py (2026-10-19, deferred from the 10-9a-3 review), as merge_check.py is
# for the merge: the jump / land side rule (jump_side: the foot that leaves last / touches first,
# feet within TIE together, the first foot L naming it), the antho guard of settle_jumps (an antho
# or deleted line is never touched, on the side pass nor the part steps under a land) and the
# decisions.tsv append (append_decisions). Runs on a made-up motion (no dump, no review.json).
#   python3 tools/ref_draft/decide_check.py      exit 1 on a failure

import copy
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decide as dc  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + what)


R = 100        # frames per second
A, B = 100, 160  # the flight: both feet off over frames [A, B) (0.6 s)
N = 300


class FakeMotion(dc.Motion):
    """The attributes decide's jump pieces read, without a dump: a 0.9 m leg, heel and ball per
    foot, the hips ballistic over the flight. take_off[s] / land[s]: the frame the foot's last
    contact before the flight ends / its first touchdown after it."""

    def __init__(self, take_off, land):
        self.rate, self.n, self.L = R, N, 0.9
        self.B = {k: i for i, k in enumerate(("hips", "heel_L", "ball_L", "heel_R", "ball_R"))}
        hy = np.full(N, 1.0)
        t = (np.arange(A, B) - A) / R
        dur = (B - A) / R
        hy[A:B] += 9.81 * dur / 2 * t - 0.5 * 9.81 * t * t
        self.P = {"hips": np.stack([np.zeros(N), hy, np.zeros(N)], axis=1)}
        self.contact, self.hloc, self.imp = {}, {}, {}
        for s in "LR":
            for p in ("heel_", "ball_"):
                k = p + s
                # the heel leaves 2 frames before the ball (the ball gives the take-off)
                self.contact[k] = [[0, take_off[s] - (2 if p == "heel_" else 0)], [land[s], N]]
                self.hloc[k] = np.ones(N)  # never at the floor in the flight: no sliding landing
                self.imp[k] = [(land[s] + (1 if p == "ball_" else 0), 2.0, 1.0, 0.2, 0.0, 0.0, False, "descent")]
            self.contact["foot_" + s] = [[0, take_off[s]], [land[s], N]]


def side(take_off, land, kind):
    M = FakeMotion(take_off, land)
    s, tt, why = dc.jump_side(M, kind, (A - 1 if kind == "jump" else B) / R, wide=True)
    return s


tie = int(dc.TIE * R)  # 1 frame
# Take-off: the foot whose part leaves last.
check(side({"L": A, "R": A - 5}, {"L": B, "R": B}, "jump") == "L", "jump: L leaves last -> L")
check(side({"L": A - 5, "R": A}, {"L": B, "R": B}, "jump") == "R", "jump: R leaves last -> R")
# Within TIE: together, the first foot (L) names it; just past it: the later foot.
check(side({"L": A - tie, "R": A}, {"L": B, "R": B}, "jump") == "L", "jump: R within the tie -> L")
check(side({"L": A - tie - 1, "R": A}, {"L": B, "R": B}, "jump") == "R", "jump: R past the tie -> R")
# Landing: the foot that touches first.
check(side({"L": A, "R": A}, {"L": B + 5, "R": B}, "land") == "R", "land: R touches first -> R")
check(side({"L": A, "R": A}, {"L": B, "R": B + 5}, "land") == "L", "land: L touches first -> L")
check(side({"L": A, "R": A}, {"L": B + tie, "R": B}, "land") == "L", "land: L within the tie -> L")
check(side({"L": A, "R": A}, {"L": B + tie + 1, "R": B}, "land") == "R", "land: L past the tie -> R")


def L(kind, side_, part, t, status="draft", source="motion"):
    return {"kind": kind, "side": side_, "part": part, "t": t, "status": status, "source": source}


def norm(lines):
    return sorted((l["kind"], l["side"], l["part"], round(l["t"], 4), l["status"], l["source"]) for l in lines)


# settle_jumps: R leaves last, L touches first.
M = FakeMotion({"L": A - 5, "R": A}, {"L": B, "R": B + 5})
lines = [
    L("jump", "L", "any", (A - 1) / R),                        # wrong side: -> R, source claude
    L("land", "R", "any", B / R, "confirmed", "agreed"),       # wrong side: -> L
    L("jump", "L", "any", (A - 1) / R, "confirmed", "antho"),  # Antho's: never touched
    L("land", "R", "any", B / R, "deleted", "claude"),         # deleted: never touched
    L("step", "L", "heel", B / R + 0.02),                       # a heel step 20 ms from the land, missed: deleted
    L("step", "L", "toe", B / R + 0.02, "confirmed", "antho"),  # Antho's: kept
    L("step", "L", "heel", B / R + 0.05),                       # 50 ms away: kept
]
before = copy.deepcopy(lines)
log = dc.settle_jumps("c", lines, {"found_parts": []}, M)
n = norm(lines)
check(("jump", "R", "any", round((A - 1) / R, 4), "draft", "claude") in n, "side pass: draft jump L -> R, source claude")
check(("land", "L", "any", round(B / R, 4), "confirmed", "claude") in n, "side pass: land R -> L, source claude")
check(lines[2] == before[2], "antho jump line untouched")
check(lines[3] == before[3], "deleted land line untouched")
check(lines[4]["status"] == "deleted" and lines[4]["source"] == "claude", "missed heel step under the land deleted")
check(lines[5] == before[5], "antho toe step under the land untouched")
check(lines[6] == before[6], "heel step 50 ms from the land kept")
check(sum(1 for r in log if r[5] == "side") == 2 and sum(1 for r in log if r[5] == "under land") == 1,
      "log: two side rows, one under-land row")
# A heel step the detector finds (a found 'heel' event of that foot, 'parts') is no disagreement: kept.
lines = [L("land", "L", "any", B / R), L("step", "L", "heel", B / R + 0.02)]
dc.settle_jumps("c", lines, {"found_parts": [{"k": "heel", "s": "L", "t": B / R + 0.025}]}, M)
check(lines[1]["status"] == "draft", "a heel step the detector finds under the land is kept")
# Run twice: nothing more changes.
lines2 = copy.deepcopy(before)
dc.settle_jumps("c", lines2, {"found_parts": []}, M)
once = norm(lines2)
log2 = dc.settle_jumps("c", lines2, {"found_parts": []}, M)
check(norm(lines2) == once and not log2, "settle_jumps run twice changes nothing")

# decisions.tsv: kept, appended, no repeat, one header.
with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, "decisions.tsv")
    r1 = ("c", "jump", "L", "any", 0.99, "side", "side L->R", "why 1")
    r2 = ("c", "land", "R", "any", 1.6, "side", "side R->L", "why 2")
    dc.append_decisions(out, [r1])
    dc.append_decisions(out, [r1, r2])
    dc.append_decisions(out, [])
    with open(out, encoding="utf-8") as f:
        rows = f.read().split("\n")
    check(rows[0].startswith("# clip\t") and sum(1 for x in rows if x.startswith("#")) == 1, "one header")
    body = [x for x in rows if x and not x.startswith("#")]
    check(body == ["c\tjump\tL\tany\t0.9900\tside\tside L->R\twhy 1", "c\tland\tR\tany\t1.6000\tside\tside R->L\twhy 2"],
          "earlier rows kept, new appended, none repeated")

if fails:
    print("decide_check: %d FAILED" % fails)
    sys.exit(1)
print("decide_check: all passed")
