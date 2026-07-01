#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# guard-story-closure.sh — PreToolUse(Bash) guard.
#
# Antho's rule (2026-07-01): CLOSING a story — committing it and/or flipping it
# to `done` — is the REVIEWER's job (the bmad-code-review workflow's on_complete
# step). A Reaper / in-game gate confirmation from Antho only means "the gate
# PASSED"; it is NOT authorization for a dev / correct-course / other agent to
# commit the story or set it to `done`. An agent overstepping this is exactly
# what this hook prevents.
#
# Mechanism: block any `git commit` whose message signals a story closure
# (`-> done` / `→ done`) OR whose staged diff flips a Status:/sprint entry to
# `done`, UNLESS the caller is the reviewer and opts in explicitly with
# BMAD_REVIEWER_CLOSURE=1 (env var, or inline `BMAD_REVIEWER_CLOSURE=1 git ...`).
#
# Exit 0 = allow. Exit 2 = block (stderr is fed back to the calling agent).
# The guard fails OPEN (exit 0) on any internal error so it can never wedge a
# normal commit.
# ---------------------------------------------------------------------------
set -uo pipefail

input="$(cat)"

# Extract the Bash command from the PreToolUse payload. Fail open if unparseable.
cmd="$(printf '%s' "$input" | python3 -c 'import sys,json
try:
    print(json.load(sys.stdin).get("tool_input",{}).get("command",""))
except Exception:
    print("")' 2>/dev/null)" || exit 0

# Only interested in git commits.
case "$cmd" in
  *"git commit"*) ;;
  *) exit 0 ;;
esac

# Reviewer opt-in present (inline env prefix or exported var) -> allow.
case "$cmd" in
  *BMAD_REVIEWER_CLOSURE=1*) exit 0 ;;
esac
[ "${BMAD_REVIEWER_CLOSURE:-}" = "1" ] && exit 0

closure=0

# Signal 1 — the commit message announces a closure: "-> done" / "→ done" / "=> done".
if printf '%s' "$cmd" | grep -Eq '(->|=>|→)[[:space:]]*done'; then
  closure=1
fi

# Signal 2 — the already-staged diff flips a story to done (best effort; catches
# the case where `git add` ran in an earlier tool call). A story file line
# "+Status: done" or a sprint-status line "+  <key>: done".
if [ "$closure" -eq 0 ]; then
  cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || true
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if git diff --cached -U0 2>/dev/null \
         | grep -Eq '^\+[[:space:]]*(Status:[[:space:]]*done|[A-Za-z0-9_.-]+:[[:space:]]*done)'; then
      closure=1
    fi
  fi
fi

[ "$closure" -eq 0 ] && exit 0

>&2 cat <<'MSG'
⛔ BLOCKED — this commit looks like a STORY CLOSURE (setting a story to `done`).

Closing a story is the REVIEWER's job — the bmad-code-review workflow's
on_complete step. A Reaper / in-game gate confirmation from Antho only confirms
the gate PASSED; it is NOT authorization for a dev / correct-course / other
agent to commit the story or flip it to `done`.

  • If you are NOT the code-review workflow:
        Stop. Leave the story at `review`. Do not commit the closure.
        Hand it to the reviewer (run /bmad-code-review), who closes it.

  • If you ARE the code-review workflow finalizing an Antho-confirmed gate:
        Re-run the commit with the reviewer opt-in, e.g.
            BMAD_REVIEWER_CLOSURE=1 git commit -m "chore(...): story X → done (gate passed)"
MSG
exit 2
