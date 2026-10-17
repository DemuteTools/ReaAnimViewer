#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Story 10-8o: the all-trigger bench in one command (WSL / Linux). Builds tools/fbx_dump and
# tests/trigger_bench in a scratch folder outside the repo (the Windows mount cannot chmod), then
# runs the bench on fixtures/: every auto type ticked, through Detect, on every clip, scored
# against fixtures/<clip>.ref.tsv. Output: the two matrices, the raise, the clips not benched,
# the changes against the previous run; fixtures/_bench/last.tsv and review.json.
#
#   tools/bench/run.sh [--draft] [trigger_bench options]
#     --draft   then check the merge (tools/ref_draft/merge_check.py), redraft the references (tools/ref_draft/ref_draft.py, never overwrites a
#               confirmed / deleted / antho line) and run the bench again
#   RAV_BENCH_BUILD=<dir>   the scratch folder (default /tmp/rav-bench-build)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${RAV_BENCH_BUILD:-/tmp/rav-bench-build}
DRAFT=0
if [ "$1" = "--draft" ]; then DRAFT=1; shift; fi

mkdir -p "$BUILD"
echo "== building fbx_dump and trigger_bench in $BUILD" >&2
LOG="$BUILD/build.log"
build() { "$@" >>"$LOG" 2>&1 || { tail -40 "$LOG" >&2; echo "build failed, see $LOG" >&2; exit 1; }; }
: >"$LOG"
build cmake -S "$ROOT/tools/fbx_dump" -B "$BUILD/fbx_dump" -DCMAKE_BUILD_TYPE=Release -DRAV_DEPS="$ROOT/build/_deps"
build cmake --build "$BUILD/fbx_dump" -j8
build cmake -S "$ROOT/tests" -B "$BUILD/tests" -DCMAKE_BUILD_TYPE=Release
build cmake --build "$BUILD/tests" -j8 --target trigger_bench
cd "$ROOT"
"$BUILD/tests/trigger_bench" --fixtures=fixtures --fbx-dump="$BUILD/fbx_dump/fbx_dump" "$@"
if [ "$DRAFT" = 1 ]; then
    python3 tools/ref_draft/merge_check.py >&2
    echo "== drafting references" >&2
    python3 tools/ref_draft/ref_draft.py --fixtures=fixtures --bench="$BUILD/tests/trigger_bench" >&2
    "$BUILD/tests/trigger_bench" --fixtures=fixtures --fbx-dump="$BUILD/fbx_dump/fbx_dump" "$@" >/dev/null
    echo "== accepting the drafts the detector agrees with (60 ms)" >&2
    python3 tools/ref_draft/agree.py --fixtures=fixtures >&2
    "$BUILD/tests/trigger_bench" --fixtures=fixtures --fbx-dump="$BUILD/fbx_dump/fbx_dump" "$@"
fi
