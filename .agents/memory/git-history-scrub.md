---
name: Git history scrub state
description: Local repo was realigned to the scrubbed GitHub history; rules for keeping it clean.
---
On 2026-07-21 the local repo was rebased onto the filter-repo-rewritten `origin/master` (confidential Diablo txt + PDF purged from all history), all contaminated refs (subrepl-*, replit-agent, gitsafe-backup/main, agent-ledger) deleted, reflogs expired, and `git gc --prune=now` run.

**Rules:**
- `attached_assets/` must stay untracked and gitignored forever; never `git add -f` anything under it.
- **Why:** it contains confidential Diablo reference material; the old contaminated history was scrubbed from GitHub and must never be re-pushed or re-added.
- **How to apply:** any history operation (rebase, cherry-pick from old SHAs, restoring backups) must be checked with `git log --all -- 'attached_assets/*Diablo*' 'attached_assets/*Manual*'` (expect empty).

**Lesson:** rebasing onto a history where a file was never tracked deletes that file from disk during checkout even if it's gitignored now — snapshot the working tree first and restore untracked-but-wanted files afterward.

**Lesson (tag scrub):** `git fetch origin --prune` (no `--tags`) will NOT force-update an already-existing local tag, so a contaminated local tag (e.g. `V1.5.0` pointing at an old pre-scrub commit) silently survives and keeps the confidential blob reachable through `gc`. To guarantee clean tags, delete ALL local tags then `git fetch origin --tags --force`; origin's tags are already the scrubbed versions.

**Lesson (realign mechanics):** when local `master` and `origin/master` have diverged onto parallel histories after a scrub, the clean fix is `git reset --soft origin/master && git commit` (one consolidating commit on the clean base, drops the contaminated merge commit), delete leftover contaminated refs (`gitsafe-backup` remote + dangling `.git/refs/remotes/gitsafe-backup`, `replit-agent`, `subrepl-*`, `refs/replit/agent-ledger`), `reflog expire --expire=now --all`, `gc --prune=now`, verify `git log --all -- 'attached_assets/*Diablo*'` is empty, then `push --force-with-lease`.

**Constraint:** ALL destructive git (and even `rm` inside `.git/`) is blocked from the agent environment — these steps must be run by the user from the Replit Shell or via a protected background task. A stale `.git/**/*.lock` (e.g. from a removed remote) aborts `gc`; clear with `find .git -name '*.lock' -delete` at the start.
