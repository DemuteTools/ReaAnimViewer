#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Story 10-8o: a self-check of the reference merge (ref_draft.merge) and of the corrections'
# apply (apply_corrections.apply). Run by tools/bench/run.sh before it drafts. Exit 1 on a failure.

import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apply_corrections as ac  # noqa: E402
import ref_draft as rd  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + what)


def L(kind, side, part, t, status="draft", source="motion"):
    return {"kind": kind, "side": side, "part": part, "t": t, "status": status, "source": source}


def norm(lines):
    return sorted((l["kind"], l["side"], l["part"], round(l["t"], 4), l["status"], l["source"]) for l in lines)


old = [
    L("step", "L", "heel", 1.0, "confirmed", "REF"),
    L("lift", "R", "any", 2.0, "deleted", "motion"),
    L("grab", "L", "hand", 3.0, "confirmed", "antho"),
    L("step", "R", "any", 4.0),                              # an old draft: replaced
    L("land", "R", "any", 5.3, "confirmed", "antho"),        # Antho moved the REF land (5.2) here
]
old_refs = [
    L("step", "L", "heel", 1.0, "confirmed", "REF"),         # equal to a kept line: not duplicated
    L("land", "B", "any", 5.2, "confirmed", "REF"),          # moved by Antho: not re-added
    L("step", "R", "tip", 6.0, "confirmed", "REF"),          # new
    L("grab", "R", "any", 7.0, "confirmed", "REF"),          # new
]
drafts = [
    L("step", "L", "heel", 1.1),     # 0.1 s from a kept line: dropped
    L("lift", "R", "any", 2.05),     # near a deleted line: dropped
    L("grab", "L", "hand", 3.1),     # dropped
    L("step", "R", "any", 4.2),      # kept (the old draft at 4.0 is gone)
    L("step", "R", "toe", 6.1),      # tip REF = toe: dropped
    L("grab", "R", "hand", 7.1),     # grab REF any = hand: dropped
    L("jump", "R", "any", 8.0),
    L("land", "L", "any", 5.25),     # land B vs L: dropped
]
old.append(L("jump", "B", "any", 8.1, "confirmed", "REF"))  # jump B vs the draft's R: dropped

out = rd.merge(copy.deepcopy(old), copy.deepcopy(drafts), copy.deepcopy(old_refs))
n = norm(out)
for k in old:
    if rd.kept(k):
        check((k["kind"], k["side"], k["part"], k["t"], k["status"], k["source"]) in n, "kept line survives: %s" % k)
check(("step", "R", "any", 4.0, "draft", "motion") not in n, "old motion draft replaced")
check(("step", "R", "any", 4.2, "draft", "motion") in n, "new draft added")
check(sum(1 for x in n if x[:4] == ("step", "L", "heel", 1.0)) == 1, "old REF equal to a line not duplicated")
check(not any(x[0] == "land" and abs(x[3] - 5.2) < 1e-9 for x in n), "REF moved by Antho not re-added")
check(("step", "R", "tip", 6.0, "confirmed", "REF") in n and ("grab", "R", "any", 7.0, "confirmed", "REF") in n,
      "new old REFs added")
for d in ("step L heel 1.1", "lift R any 2.05", "grab L hand 3.1", "step R toe 6.1", "grab R hand 7.1", "jump R any 8.0",
          "land L any 5.25"):
    k, s, p, t = d.split()
    check((k, s, p, float(t), "draft", "motion") not in n, "draft dropped: " + d)
again = rd.merge(copy.deepcopy(out), copy.deepcopy(drafts), copy.deepcopy(old_refs))
check(norm(again) == n, "merge run twice gives the same lines")

# apply_corrections.apply, twice.
lines = copy.deepcopy(out)
cs = [
    {"clip": "c", "ref": "step|R|any|4.2000", "action": "move", "kind": "step", "side": "R", "part": "any", "t": 4.25},
    {"clip": "c", "ref": "step|R|tip|6.0000", "action": "delete", "kind": "step", "side": "R", "part": "tip", "t": 6.0},
    {"clip": "c", "ref": "", "action": "add", "kind": "pivot", "side": "L", "part": "heel", "t": 9.0},
    {"clip": "c", "ref": "grab|R|any|7.0000", "action": "confirm", "kind": "grab", "side": "R", "part": "any", "t": 7.0},
]
for c in cs:
    ac.apply(lines, c)
a1 = norm(lines)
check(("step", "R", "any", 4.25, "confirmed", "antho") in a1, "move written as confirmed antho")
check(("step", "R", "tip", 6.0, "deleted", "antho") in a1, "delete kept as deleted")
check(("pivot", "L", "heel", 9.0, "confirmed", "antho") in a1, "add appended")
for c in cs:
    ac.apply(lines, c)
check(norm(lines) == a1, "apply run twice changes nothing")
# A redraft after the corrections keeps them.
re = rd.merge(copy.deepcopy(lines), copy.deepcopy(drafts), copy.deepcopy(old_refs))
check(("step", "R", "any", 4.25, "confirmed", "antho") in norm(re) and
      ("step", "R", "any", 4.2, "draft", "motion") not in norm(re), "redraft keeps a moved line, no draft back")

if fails:
    print("merge_check: %d FAILED" % fails)
    sys.exit(1)
print("merge_check: all passed")
