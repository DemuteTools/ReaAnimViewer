#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Story 10-8o: writes the review page's corrections into the reference files
# (fixtures/<clip>.ref.tsv, tests/ref_file.h).
#
#   apply_corrections.py [--fixtures=<dir>] [--dry] <export>...
#
# <export>: what ArtifactData read from the page's `corrections` collection: a directory written
# with `out_dir` (one JSON file per document, searched recursively) or a JSON file (a document,
# a list of them, or any wrapper holding them). A correction is any JSON object with "clip" and
# "action" fields (a wrapper's "data" is looked into):
#   clip    the clip id (the reference file's name without .ref.tsv)
#   ref     the line it corrects, "kind|side|part|time" as the page got it from review.json
#           ("" for an added event)
#   action  "confirm" | "move" | "delete" | "add"
#   kind, side, part, t   the event after the gesture (move: the new time; add: the new event)
# The page keeps one document per event (its latest gesture). Each is written as:
#   confirm / move / add   the line confirmed, source antho, at the given kind / side / part / t
#   delete                 the line kept with status deleted, source antho (never expected)
# A line is found by its `ref` (same kind, side, part, time within 0.5 ms), else by the nearest
# line of that kind, side and part within 60 ms (a redraft moved it). An add (or a confirm /
# move whose line is gone) is appended unless an equal line is there. Applying twice changes
# nothing.

import json
import os
import sys

HEADER = "# kind\tside\tpart\ttime_s\tstatus\tsource"


def read_ref(path):
    out = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                p = line.split()
                if len(p) == 6 and not p[0].startswith("#"):
                    out.append({"kind": p[0], "side": p[1], "part": p[2], "t": float(p[3]), "status": p[4], "source": p[5]})
    return out


def write_ref(path, lines):
    lines = sorted(lines, key=lambda l: (l["t"], l["kind"], l["side"], l["part"]))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n")
        for l in lines:
            f.write("%s\t%s\t%s\t%.4f\t%s\t%s\n" % (l["kind"], l["side"], l["part"], l["t"], l["status"], l["source"]))


def find_corrections(obj, out):
    if isinstance(obj, dict):
        if "clip" in obj and "action" in obj:
            out.append(obj)
            return
        for v in obj.values():
            find_corrections(v, out)
    elif isinstance(obj, list):
        for v in obj:
            find_corrections(v, out)


def load(paths):
    out = []
    for p in paths:
        files = []
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.endswith(".json")]
        else:
            files.append(p)
        for f in files:
            with open(f, encoding="utf-8") as fh:
                find_corrections(json.load(fh), out)
    return out


def parse_ref(ref):
    p = (ref or "").split("|")
    if len(p) != 4:
        return None
    try:
        return {"kind": p[0], "side": p[1], "part": p[2], "t": float(p[3])}
    except ValueError:
        return None


def apply(lines, c):
    """Applies one correction; returns a short text of what it did."""
    action = c.get("action")
    new = {"kind": c.get("kind"), "side": c.get("side"), "part": c.get("part"), "t": c.get("t")}
    old = parse_ref(c.get("ref"))
    if old:
        for k in ("kind", "side", "part", "t"):
            if new[k] is None:
                new[k] = old[k]
    if None in new.values():
        return "skipped (incomplete: %s)" % json.dumps(c)
    new["t"] = round(float(new["t"]), 4)
    target = None
    if old:
        same = [l for l in lines if l["kind"] == old["kind"] and l["side"] == old["side"] and l["part"] == old["part"]]
        exact = [l for l in same if abs(l["t"] - old["t"]) <= 0.0005]
        near = sorted((abs(l["t"] - old["t"]), i) for i, l in enumerate(same) if abs(l["t"] - old["t"]) <= 0.060)
        target = exact[0] if exact else (same[near[0][1]] if near else None)
    status = "deleted" if action == "delete" else "confirmed"
    if target is None:
        if action == "delete":
            return "skipped (no line %s)" % c.get("ref")
        if any(l["kind"] == new["kind"] and l["side"] == new["side"] and l["part"] == new["part"] and
               abs(l["t"] - new["t"]) <= 0.0005 and l["status"] == status for l in lines):
            return "already there"
        lines.append(dict(new, status=status, source="antho"))
        return "added %s %s %s %.4f %s" % (new["kind"], new["side"], new["part"], new["t"], status)
    before = dict(target)
    target.update(new if action != "delete" else {})
    target["status"] = status
    target["source"] = "antho"
    if before == target:
        return "already there"
    return "%s %s %s %s %.4f -> %.4f %s" % (action, target["kind"], target["side"], target["part"], before["t"], target["t"], status)


def main(argv):
    fixtures, dry, paths = "fixtures", False, []
    for a in argv:
        if a.startswith("--fixtures="):
            fixtures = a[11:]
        elif a == "--dry":
            dry = True
        else:
            paths.append(a)
    if not paths:
        sys.exit("usage: apply_corrections.py [--fixtures=<dir>] [--dry] <export dir or json>...")
    by_clip = {}
    for c in load(paths):
        by_clip.setdefault(c["clip"], []).append(c)
    for clip, cs in sorted(by_clip.items()):
        path = os.path.join(fixtures, clip + ".ref.tsv")
        if not os.path.exists(path):
            print("%s: no reference file, %d corrections skipped" % (clip, len(cs)))
            continue
        lines = read_ref(path)
        cs.sort(key=lambda c: str(c.get("at", "")))
        for c in cs:
            print("%s: %s" % (clip, apply(lines, c)))
        if not dry:
            write_ref(path, lines)


if __name__ == "__main__":
    main(sys.argv[1:])
