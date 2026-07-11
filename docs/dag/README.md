# Program DAG — how to pick up work

This directory is the coordination point for the adaptive-polyhedral-core
program. Any agent or human, in any session, resumes work like this:

1. Read [PROGRAM.yaml](PROGRAM.yaml). Find a node whose `deps` are all
   `done` and whose `status` is `todo` (or an abandoned `in_progress` —
   check `git log` on its `scope` paths to see if someone is actually on it).
2. Flip it to `in_progress` and commit that flip together with your first
   real change, so the claim is visible.
3. Work only inside the node's `scope` paths; nodes with disjoint scopes run
   in parallel. Cross-node communication happens ONLY through the file
   formats in [interfaces.md](interfaces.md) — change those schemas only in
   the same commit as the code on both sides.
4. `done` means: built clean (`-Werror`), full Catch2 suite green, scorecard
   not regressed, docs updated (ADR amendment or docs/ page), committed and
   pushed. Nothing less.
5. `feedback-loop` is repeatable: it never stays `done` — after each pass
   set it back to `todo` with a note pointing at the campaign data consumed.

House rules that always apply: CONTRIBUTING.md (anti-cheat, Eigen traps,
layout), commits authored as Hunter-124 with no AI attribution, push to
master when verified, `graphify update .` after structural changes.
