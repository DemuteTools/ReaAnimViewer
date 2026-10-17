#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Story 10-8o: accepts the draft references that the detector agrees with, so that Antho only
# reviews the disagreements (decision 2026-10-17). A draft line (status draft) that the bench's
# found events of its row match within 60 ms becomes `confirmed`, source `agreed`: two methods
# that share no code (tools/ref_draft from the joint positions, Detect) put the event at the same
# place. Matching is per row (ref_draft.identity: kind, part for steps / slides / pivots, side
# but none for jump / land), in the configuration that emits the line (default: whole-foot step,
# slide, pivot; parts: heel / toe; both for the other kinds), one-to-one, smallest |err| first,
# against every non-deleted line (a confirmed line keeps its detection). Confirmed, deleted and
# antho lines are never touched, and nothing is ever demoted.
#
#   agree.py [--fixtures=<dir>] [--review=<review.json>] [--window=0.060] [--dry]
#
# Reads the found events from the bench's review.json (fixtures/_bench/review.json by default):
# run tools/bench/run.sh first.

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref_draft import identity, read_ref, write_ref  # noqa: E402


def emits(line, config):
    kind, part, _ = identity(line)
    if kind in ("step", "slide", "pivot"):
        return (part == "any") if config == "default" else part in ("heel", "toe")
    return True


def found_identity(f):
    return identity({"kind": f["k"], "side": f["s"], "part": "any"})


def agree_clip(lines, clip, window):
    """Promotes the agreed drafts of one clip in place; returns how many."""
    promoted = set()
    for config in ("default", "parts"):
        rows = {}
        for i, l in enumerate(lines):
            if l["status"] == "deleted" or not emits(l, config):
                continue
            rows.setdefault(identity(l), ([], []))[0].append(i)
        for f in clip.get("found_" + config, []):
            fid = found_identity(f)
            if fid in rows:
                rows[fid][1].append(f["t"])
        for refs, det in rows.values():
            pairs = sorted((abs(d - lines[i]["t"]), i, j) for i in refs for j, d in enumerate(det)
                           if abs(d - lines[i]["t"]) <= window + 1e-9)
            used_r, used_d = set(), set()
            for _, i, j in pairs:
                if i in used_r or j in used_d:
                    continue
                used_r.add(i)
                used_d.add(j)
                if lines[i]["status"] == "draft":
                    promoted.add(i)
    for i in promoted:
        lines[i]["status"], lines[i]["source"] = "confirmed", "agreed"
    return len(promoted)


def main(argv):
    fixtures, review, window, dry = "fixtures", None, 0.060, False
    for a in argv:
        if a.startswith("--fixtures="):
            fixtures = a[11:]
        elif a.startswith("--review="):
            review = a[9:]
        elif a.startswith("--window="):
            window = float(a[9:])
        elif a == "--dry":
            dry = True
        else:
            sys.exit("usage: agree.py [--fixtures=<dir>] [--review=<review.json>] [--window=0.060] [--dry]")
    review = review or os.path.join(fixtures, "_bench", "review.json")
    with open(review, encoding="utf-8") as f:
        data = json.load(f)
    total = drafts_left = 0
    for clip in data["clips"]:
        path = os.path.join(fixtures, clip["id"] + ".ref.tsv")
        if not os.path.exists(path):
            continue
        lines = read_ref(path)
        n = agree_clip(lines, clip, window)
        left = sum(1 for l in lines if l["status"] == "draft")
        total += n
        drafts_left += left
        if n:
            print("%s: %d agreed, %d drafts left" % (clip["id"], n, left))
            if not dry:
                write_ref(path, lines)
    print("agreed %d draft lines (window %.0f ms); %d drafts left to review" % (total, window * 1000, drafts_left))


if __name__ == "__main__":
    main(sys.argv[1:])
