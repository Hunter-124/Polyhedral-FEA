# Agent bootstrap — overnight / autonomous work on the DAG

Paste the block below into a fresh AI agent harness (Claude Code, Grok, Codex,
Cursor, aider, …) to have it work the program board autonomously. It is
identity-agnostic: the agent asks who to attribute the work to and verifies it
before committing. Keep this file in sync with `PROGRAM.yaml` and
`interfaces.md` when the process changes.

---

```
You are working autonomously on the Polyhedral-FEA project (aka PolyMesh): an
adaptive hybrid polyhedral FEA mesher+solver in C++20 (CMake + Ninja + Eigen +
Catch2), at its repo root on this machine.
GitHub: github.com/Hunter-124/Polyhedral-FEA, default branch master.

=== STEP 0: SYNC FROM REMOTE FIRST (always, before anything else) ===
- You do NOT know the true state until you sync. Before reading deeply,
  planning, or changing anything:
    git fetch origin
    git status                      # know your branch + working tree
    git pull --rebase origin master # get the correct, current state
- If the working tree is dirty or the rebase conflicts, STOP and resolve with
  the user before proceeding — never build on a stale or half-merged tree.
- Never force-push. Re-run `git pull --rebase` again right before you push if
  time has passed, so you never clobber work pushed in the meantime.

=== STEP 1: IDENTITY AND CONSENT (before your first commit) ===
- Ask the user who this work should be attributed to (git name + email).
  Record that identity in your own working context so you never lose it
  mid-session, then set and VERIFY it in THIS repo's active git config:
    git config user.name "<name from user>"
    git config user.email "<email from user>"
    git config user.name && git config user.email   # verify, must match
  Never inherit a stale, generic, or session-default identity, and never guess
  an email — get it from the user.
- NEVER add AI-attribution trailers (no "Co-Authored-By", no "Generated with
  …") to commits, PRs, or anywhere.
- Read CONTRIBUTING.md (§0 AI quick start, §4 anti-cheat) and CLAUDE.md in full.
- Double-check scope with the user before creating, editing, deleting, or
  pushing — with fresh confirmation for anything hard to reverse (force-push,
  history rewrite, deletes). Approval for one step is not approval for the next.

=== HOW THE WORK IS ORGANIZED ===
- docs/dag/PROGRAM.yaml is the program board (the DAG). Each node has id,
  status (todo/in_progress/done/blocked), deps, and a scope (the only dirs it
  may touch). docs/dag/README.md is the claim/parallelism protocol.
- docs/dag/interfaces.md is the NORMATIVE file-schema contract between the test
  lab, GUI, and analysis tooling. Change a schema only in the same commit as
  both sides of the code.
- Pick work: a node whose deps are all `done` and status is `todo` (or an
  abandoned `in_progress` — check `git log` on its scope to see if it's live).
  Flip it to `in_progress` in your first commit. Nodes with disjoint scopes can
  run in parallel.
- Vision context: docs/decisions/0019-mixed-fe-vem-adaptive-order-core.md,
  docs/solver-core.md, docs/decisions/0012-hybrid-graded-tet.md,
  docs/progress.md.

=== ANTI-CHEAT (sacred, non-negotiable — CONTRIBUTING §4) ===
- NEVER hardcode benchmark/reference answers in src/ or apps/. Truths live ONLY
  in bench/reference/*.json, loaded at runtime.
- Every mesh must pass validity before it is solved. If a benchmark fails, fix
  the code, not the expected value. The patch test and MMS tests are sacred.

=== BUILD / TEST / VERIFY (what "done" means) ===
- Configure once: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
  (add -DPOLYMESH_WITH_GUI=ON only for GUI work).
- Build: cmake --build build -j$(nproc)   (strict -Werror; warnings fail).
- Test from the REPO ROOT (tests load bench/reference/):
    ./build/tests/polymesh_tests
- Eigen traps: never call .cross()/.dot() on expression temporaries —
  materialize Eigen::Vector3d first; any file using .cross() needs
  #include <Eigen/Geometry>; don't chain .inverse().transpose().
- A node is `done` only when: clean -Werror build, full Catch2 suite green,
  curved scorecard not regressed, docs/ADR updated, and after structural code
  changes you ran `graphify update .` and committed the graphify-out/ changes.
  Do NOT commit machine-local graphify files (.graphify_python, .graphify_root,
  cache/, cost.json, graph.html) or dated snapshot dirs (graphify-out/<date>/).

=== COMMIT + PUSH PROTOCOL ===
- Commit each logical, verified unit and push to master immediately. Keep
  commits scoped to one DAG node's directories.
- Re-run `git pull --rebase origin master` right before pushing. No force-push.
- Before committing, re-verify `git config user.email` matches the identity the
  user gave you.

=== OPEN NODES (read PROGRAM.yaml for the live list; typical order) ===
As of 2026-07-12: **done** includes adaptive core (p-hierarchical*, fe-vem,
mesher-tendency, hp-driver, gui-sim-controls, part-library, testlab, …) and
**M0–M4, M6–M8**. **Active program = Lane M (measure-first) + G (CVT) on Lane V.**

**MUST READ FIRST (full plan — do not improvise a different strategy):**
  docs/plans/advisor-measure-first-program.md
ADRs: 0020 (BRep), 0021 (varyhedron), 0022 (warehouse+grok), **0023** (strategy),
**0024** (concrete Q&A: stress score, freeze→project→CVT, Geogram vendor,
chordal e, lfs balls, dual hard-block, tiers, traps).
Process: docs/process/grok-loop.md. Board: docs/dag/PROGRAM.yaml.
Every open M/G node note cites ADR-0024 Q# + that plan path — trust the notes.

Normative order (ADR-0023/0024 — do not skip or reorder):
1. **M0–M4 done:** probes, scorecard, sharp-only protect, N_pred
2. **M6 done:** stress score = face-mean centroid VM + energy; never raw nodal max
3. **M7 done:** chordal e = d/(ℓ²κ/8) with OCC κ + true mesh segments
4. **M8 done:** protecting balls r=min(αh, β·lfs); corner shrink
5. **M9 FREEZE baseline campaign** (done) on honest scorecard (before projection/CVT)
6. **M10 done** wall tangential smooth + OCC surface project
7. **G0 done** · **G1–G4 next** vendor → Lloyd CVT → constrained sites → clipped Voronoi
8. **M5** VEM headline only if beats hybrid_zoo on frozen plate_hole+cylinder
9. Dual-of-tet: HARD-BLOCK until G4. Frame fields: out of near-term horizon.
10. p>1 only with curved/isoparametric boundary in same work item (V6d deps M1)
11. **V11** packing iterate: no "win" loops until M9; dual hard-block until G4

Side nodes (parallel when deps ready; not ahead of critical path):
- **M11–M14 done** (h_min, wall-clock, expected_area, sphere freeze-ref)
- Icecream face-tags still design-only ([brep-face-tag-bc.md](../research/brep-face-tag-bc.md))
- Start next session: [SESSION_HANDOFF_g1-cvt.md](../plans/SESSION_HANDOFF_g1-cvt.md)

Substrate: sharp protect + graded tet + live BRep oracle.
Reward: scorecard + accuracy — never wire PNG, never residual alone.
Cylinder primary = strain_energy; tip = health. Plate SCF = face-mean VM/σ∞.

Owner product rules (non-negotiable):
- True BRep volume path; no product STL generators (compare-only).
- Mesher name **Varyhedron** with ADR-0021 tooltip in GUI.
- Short campaigns: varyhedron + hybrid_zoo only.
- Full warehouse committed (LFS for VTU/PNG).
- After campaign: analyze → HANDOFF → headless grok improve only if health_ok.
- CVT density = 1/h(x)³ from the **same** h field as N_pred.

=== WORKING STYLE ===
- Quality ahead of speed. Small, verified, pushed increments. Update
  docs/progress.md and each node's status/note as you go.
- Prefer graphify for codebase questions: `graphify query "<question>"`,
  `graphify path "<A>" "<B>"`, `graphify explain "<concept>"` before full greps.
- When a node is done, set it `done` in PROGRAM.yaml with a note, then pick the
  next available node. Keep going until the open nodes are done or you are
  genuinely blocked; leave the board and docs accurate for the next session.

Start now: sync from remote (Step 0), confirm identity with the user
(Step 1), read the board, check `git status` for in-flight files, and begin.
```
