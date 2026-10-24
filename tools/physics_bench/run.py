# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy", "c3d", "cmake", "ninja", "ziglang"]
# ///
# SPDX-License-Identifier: MIT
"""Stories 10-13a / 10-13b: the physics bench in one command.

    uv run tools/physics_bench/run.py [--corpus=kyokushin|kds] [--subjects=N|all] [--no-fetch]

Corpora: kyokushin (default; --subjects default 12, the first N with a complete set) and kds
(Karate Dataset Stances, air gyaku-tsuki in three stances; --subjects default all, 12 karateka).

Fetches the corpus' trials on demand into a cache outside the repo ($RAV_PHYSICS_CACHE, default
~/.cache/rav-physics), converts them to RAV detection csvs with references written from the
markers in fixtures/_physics/<corpus>/, builds tests/trigger_bench (the system C++ compiler when
one is on PATH, else zig + cmake + ninja from uv) in <cache>/build, runs it on that folder and
prints recall, precision and signed timing error per kind, condition and technique
(fixtures/_physics/report.txt). The whole bench output: fixtures/_physics/<corpus>/_bench/bench.txt.
"""
import argparse
import os
import shutil
import subprocess
import sys
import sysconfig

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import kds  # noqa: E402
import kyokushin  # noqa: E402
import summary  # noqa: E402
from remotezip import FetchError, RemoteZip  # noqa: E402

CORPORA = {"kyokushin": kyokushin, "kds": kds}


def cache_dir():
    return os.environ.get("RAV_PHYSICS_CACHE") or os.path.join(os.path.expanduser("~"), ".cache", "rav-physics")


def tool(name):
    """A tool of this script's environment (cmake / ninja from uv), else from PATH."""
    exe = name + (".exe" if os.name == "nt" else "")
    p = os.path.join(sysconfig.get_path("scripts"), exe)
    return p if os.path.exists(p) else shutil.which(name)


def zig_wrappers(build):
    """Compiler / archiver wrappers calling `python -m ziglang` (no system compiler needed)."""
    os.makedirs(build, exist_ok=True)
    out = {}
    for name, sub in (("c++", "c++"), ("ar", "ar"), ("ranlib", "ranlib")):
        if os.name == "nt":
            p = os.path.join(build, f"zig-{name}.cmd")
            body = f'@"{sys.executable}" -m ziglang {sub} %*\r\n'
        else:
            p = os.path.join(build, f"zig-{name}")
            body = f'#!/bin/sh\nexec "{sys.executable}" -m ziglang {sub} "$@"\n'
        with open(p, "w", newline="") as f:
            f.write(body)
        if os.name != "nt":
            os.chmod(p, 0o755)
        out[name] = p
    return out


def build_bench(cache):
    """Builds trigger_bench; returns its path."""
    cmake, ninja = tool("cmake"), tool("ninja")
    if not cmake or not ninja:
        sys.exit("cmake / ninja not found (run through uv: uv run tools/physics_bench/run.py)")
    system = next((c for c in (os.environ.get("CXX"), "c++", "g++", "clang++", "cl") if c and shutil.which(c)), None)
    build = os.path.join(cache, "build", "system" if system else "zig")
    os.makedirs(build, exist_ok=True)
    args = [cmake, "-S", os.path.join(ROOT, "tests"), "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    if system:
        args.append(f"-DCMAKE_CXX_COMPILER={shutil.which(system)}")
        how = system
    else:
        w = zig_wrappers(os.path.join(cache, "build", "zig-bin"))
        args += [f"-DCMAKE_CXX_COMPILER={w['c++']}", f"-DCMAKE_AR={w['ar']}", f"-DCMAKE_RANLIB={w['ranlib']}"]
        how = "zig (uv)"
    log = os.path.join(build, "build.log")
    print(f"== building trigger_bench with {how} in {build}", flush=True)
    with open(log, "w") as f:
        for cmd in (args, [cmake, "--build", build, "--target", "trigger_bench"]):
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
            if r.returncode:
                f.close()
                sys.stderr.write("".join(open(log).readlines()[-40:]))
                sys.exit(f"build failed, see {log}")
    exe = os.path.join(build, "trigger_bench" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(exe):
        sys.exit(f"no {exe} after the build, see {log}")
    return exe


def bench_section(text, title):
    """The bench output's section `title` ("--- <title> ---" up to the next section or "Saved")."""
    out, on = [], False
    for line in text.splitlines():
        if line.startswith("--- " + title):
            on = True
        elif on and (line.startswith("Saved ") or line.startswith("---")):
            break
        if on:
            out.append(line)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default="kyokushin", choices=sorted(CORPORA))
    ap.add_argument("--subjects", default=None,
                    help="the first N subjects (kyokushin: with a complete set), or 'all' "
                         "(default: kyokushin 12, kds all)")
    ap.add_argument("--no-fetch", action="store_true", help="use the cached trials only")
    a = ap.parse_args()
    if a.subjects is None:
        a.subjects = str(CORPORA[a.corpus].DEFAULT_SUBJECTS)
    if a.subjects != "all" and not (a.subjects.isdigit() and int(a.subjects) > 0):
        ap.error("--subjects: a positive number or 'all'")
    mod = CORPORA[a.corpus]
    cache = cache_dir()
    phys = os.path.join(ROOT, "fixtures", "_physics")
    out_dir = os.path.join(phys, mod.CORPUS)

    print(f"== {mod.CORPUS}: fetch (cache {os.path.join(cache, mod.CORPUS)}), convert, reference", flush=True)
    z = RemoteZip(mod.PUBLIC_URL, os.path.join(cache, mod.CORPUS), offline=a.no_fetch)
    try:
        res = mod.prepare(z, out_dir, a.subjects, log=lambda s: print(s, flush=True))
    except FetchError as e:
        sys.exit(f"cannot list the corpus: {e}")
    n_ref = sum(len(c["refs"]) for c in res["clips"])
    n_draft = sum(r.status == "draft" for c in res["clips"] for r in c["refs"])
    print(f"   {len(res['clips'])} clips ({res['downloaded']} trials downloaded, {res['converted']} converted), "
          f"{n_ref} references ({n_draft} draft, see {os.path.relpath(os.path.join(out_dir, 'doubtful.tsv'), ROOT)})")
    print("   skipped: " + (", ".join(res["skipped"]) if res["skipped"] else "(none)"), flush=True)
    if not res["clips"]:
        sys.exit("no clip to bench")

    exe = build_bench(cache)
    print(f"== trigger_bench on {os.path.relpath(out_dir, ROOT)}", flush=True)
    bench_out = os.path.join(out_dir, "_bench")
    os.makedirs(bench_out, exist_ok=True)
    r = subprocess.run([exe, f"--fixtures={out_dir}", f"--out={bench_out}"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    with open(os.path.join(bench_out, "bench.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(r.stdout)
    if r.returncode:
        sys.stderr.write(r.stderr)
        sys.exit(f"trigger_bench failed ({r.returncode})")
    print(bench_section(r.stdout, "Not benched"))
    print(bench_section(r.stdout, "Changes against the previous run"))
    print()
    text = summary.write_report(phys)
    print(text)
    print(f"Saved {os.path.relpath(os.path.join(phys, 'report.txt'), ROOT)} "
          f"(whole bench output: {os.path.relpath(os.path.join(bench_out, 'bench.txt'), ROOT)})")


if __name__ == "__main__":
    main()
