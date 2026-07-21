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
