# Handoff — variable-everything + advisor, wave 1 (2026-08-09)

Plan: [`variable-everything-and-advisor.md`](variable-everything-and-advisor.md) ·
ADRs [0026](../decisions/0026-anisotropic-metric-adaptivity.md),
[0027](../decisions/0027-learned-mesh-advisor.md) ·
Research: [`docs/research/ideabank/`](../research/ideabank/)

## State

Commit `ba06b23` on `master`, **local only** — `git push` returned 403:
git is authenticated as `CharlesEdmonds`, the remote is `Hunter-124/Polyhedral-FEA`.
Fix the credential and push; nothing else is blocking.

Suite: **347 cases / 318 passed / 29 skipped (all OCC-gated) / 41,881 assertions / 0 failures.**
Build: `cmd.exe /c "scripts\msvcbuild.bat cmake --build build -j 4"`.
A bare shell has no MSVC env and dies with `Cannot open include file: 'cmath'` —
that wrapper is now the way to build here.

## Blocked on the machine, not on code

1. **OpenCASCADE.** `vcpkg install opencascade:x64-windows` is running detached
   (`occt-install2`, `VCPKG_MAX_CONCURRENCY=5`). It was killed once mid-debug-build
   and restarted; vcpkg resumes. Until it finishes, `POLYMESH_WITH_OCC` stays OFF
   and **the product CAD path cannot run at all** — the CLI is CAD-only and 29
   tests skip. Reconfigure with `-DPOLYMESH_WITH_OCC=ON` afterwards.
2. **SFEM corpus.** `scripts/fetch_advisor_corpus.py --source sfem` gets HTTP 429
   then `LocalEntryNotFoundError` — Hugging Face rate-limits anonymous IPs pulling
   ~16k small files. Set `HF_TOKEN` (any free account) and re-run; the script
   backs off, resumes, and is idempotent. Nothing was downloaded yet.

## What is real now that was not

- `adapt::MetricField` — SPD metric infrastructure. Anisotropy is *enabled*, not
  shipped: the Mmg3d adapter and Hessian recovery are the next slice.
- Size fields reach `graded_tet_fill_surface` / `mixed_fill_surface`. Empty field
  is byte-identical to the old path, so the regression gate is the suite itself.
- Selective p-elevation is conforming. The headline number, printed by the suite:
  affine patch error **3.48e-19 constrained vs 3.74e-4 unconstrained**. That
  delta is the proof the old path was silently wrong.
- Campaign rows carry `advisor-row-v2` context features + action objects and
  per-pass adapt traces. `bench/campaigns/advisor-pilot-1/` is loadable but
  **unrun** (needs OCC).

## Next, in order

1. Push. Then OCC on, reconfigure, re-run the suite with the 29 skipped tests live.
2. Run `advisor-pilot-1` and `python scripts/build_advisor_dataset.py` — first
   real advisor rows.
3. Quantitative sizing: replace the fixed `h_next = 0.75·h` with
   `h·(η_target/η_e)^{1/p}` (Adaptivity phase, todo list).
4. Then either the Mmg3d metric adapter (biggest accuracy-per-DOF lever) or
   symmetry detection (cheapest true zero-added-error win, Phase 4).

## Loose ends worth knowing

- `element_tendency` is still a **global** mesher selector; per-element shape
  marks from `drive_hp` still change zero elements. Untouched this wave.
- The `HpModel` conforming hierarchical assembler (hex p≤6, tet p≤4) still has
  no production caller. The MPC fix makes p=2 correct; p>2 needs that cutover.
- Auto-`h` is still clamped to `[diag/80, diag/6]` and the product ceiling is
  589,824 elements. Raising those is a todo, deliberately not done blind.
- `tet_face_orientation` in `hp_assembly.cpp` uses a "dominant component"
  approximation that is likely wrong for tet p=4 shared faces. Pre-existing;
  flagged by the audit; no test covers it.
