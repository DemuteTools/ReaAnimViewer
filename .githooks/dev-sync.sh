#!/bin/sh
# Syncs the PRIVATE dev files (see dev-sync.bat) automatically, from the git hooks
# in this folder: `git pull` also pulls them, `git push` also pushes them.
# Enabled per PC by `git config core.hooksPath .githooks`, which dev-sync.bat sets.
#
# Each run first commits the local dev changes, then merges the private repo
# (so a PC that forgot to sync is merged in, not overwritten), and `push` then
# uploads. It never fails the public git command: a problem only prints a
# warning, and dev-sync.bat pull / push fixes it by hand.
#
# Keep the path list in sync with :stage_dev_files in dev-sync.bat.

# WSL's git refuses a repo on a Windows drive ("dubious ownership") unless told to
# trust it; Windows git doesn't need it. GIT_TERMINAL_PROMPT=0: never wait for a
# password inside a hook.
export GIT_TERMINAL_PROMPT=0
sgit() { git -c 'safe.directory=*' "$@"; }
cd "$(sgit rev-parse --show-toplevel)" || exit 0
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE  # set by git for the PUBLIC repo's hook

DEVGIT=.devgit
dg() { sgit --git-dir="$DEVGIT" --work-tree=. "$@"; }
warn() { echo "[dev-sync] WARNING: $*" >&2; }

if [ ! -d "$DEVGIT" ]; then
    warn "private dev files are not set up on this PC: run dev-sync.bat setup"
    exit 0
fi

commit_local() {
    dg add -u || return 1
    for p in .claude _bmad _bmad-output CLAUDE.md sample \
             docs/PHASE*_VALIDATOR_GATE.md docs/SPIKE0_FINDINGS.md test-*.bat \
             release.bat reapack-check.bat tools/release.ps1 tools/ensure-gh.bat RELEASING.md; do
        [ -e "$p" ] && dg add -f -- "$p"
    done
    # Local only: the PC's own settings, and agent worktrees (git would record them as
    # submodule links).
    dg rm -r -q --cached --ignore-unmatch -- .claude/settings.local.json .claude/worktrees >/dev/null
    dg diff --cached --quiet && return 0
    dg commit -q -m "Sync dev files from ${COMPUTERNAME:-$(hostname)}"
}

merge_remote() {
    if ! dg fetch -q origin; then
        warn "could not reach the private repo (offline?): dev files not synced this time"
        return 1
    fi
    # A brand-new private repo has no main yet: nothing to merge.
    dg rev-parse -q --verify refs/remotes/origin/main >/dev/null || return 0
    # --no-edit: a merge commit must not open an editor inside a hook.
    if ! dg merge -q --no-edit origin/main; then
        warn "both PCs changed the same dev file: fix the conflicts, then run dev-sync.bat push"
        return 1
    fi
}

# Records the last successful sync: a dev file newer than this marker has not been
# sent yet (.claude/hooks/dev-sync-reminder.sh reminds the user).
mark_synced() { : > "$DEVGIT/last-sync"; }

case "$1" in
pull)
    echo "[dev-sync] Getting the private dev files..."
    commit_local || { warn "could not save the local dev changes: run dev-sync.bat push"; exit 0; }
    merge_remote && mark_synced
    ;;
push)
    echo "[dev-sync] Sending the private dev files..."
    commit_local || { warn "could not save the local dev changes: run dev-sync.bat push"; exit 0; }
    merge_remote || exit 0
    if dg push -q origin HEAD:main; then mark_synced; else warn "push failed: run dev-sync.bat push"; fi
    ;;
esac
exit 0
