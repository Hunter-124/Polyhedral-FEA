# Handoff — variable-everything + advisor, wave 1 (2026-08-09)

Plan: [`variable-everything-and-advisor.md`](variable-everything-and-advisor.md) ·
ADRs [0026](../decisions/0026-anisotropic-metric-adaptivity.md),
[0027](../decisions/0027-learned-mesh-advisor.md) ·
Research: [`docs/research/ideabank/`](../research/ideabank/)

## State

Commits `ba06b23`, `18631c1`, `58c04d5` on `master`, **local only** — `git push`
returned 403: git is authenticated as `CharlesEdmonds`, the remote is
`Hunter-124/Polyhedral-FEA`. Fix the credential and push; nothing else blocks.

Suite (OCC ON): **347 cases / 344 passed / 3 skipped / 42,224 assertions / 0 failures.**
The 3 skips are two stub-path tests that skip *because* OCC is enabled, plus
CUDA SpMV.
Build: `cmd.exe /c "scripts\msvcbuild.bat cmake --build build -j 4"`.
A bare shell has no MSVC env and dies with `Cannot open include file: 'cmath'` —
that wrapper is now the way to build here.

Product smoke, `polymesh solve bench/geometries/public/unit_box.step -h 0.1`:
STEP import → hybrid mesh (10,648 elems, 12,167 nodes, snap max|d| 3.04e-16 m)
→ solve (max von Mises 6146 Pa, max |u| 3.686e-08 m, ZZ η 0.1146) → VTU.
Load conservation error 6.82e-13 N. At auto-`h` the same part trips the solver
memory guard (1011 MiB estimated vs a 716.8 MiB effective cap) — the guard
working, not a defect; pass `--max-mem` or free RAM.

## Blocked on you, not on code

1. **Push credential** — see above.
2. **SFEM corpus.** `scripts/fetch_advisor_corpus.py --source sfem` gets HTTP 429
   then `LocalEntryNotFoundError` — Hugging Face rate-limits anonymous IPs pulling
   ~16k small files. Set `HF_TOKEN` (any free account) and re-run; the script
   backs off, resumes, and is idempotent. Nothing was downloaded yet.
3. **Procedural part generator** — needs `pip install cadquery` (`scripts/gen_cad_parts.py`
   already imports OCP).

**OpenCASCADE is done.** vcpkg `opencascade:x64-windows` 8.0.0#1 installed and
`POLYMESH_WITH_OCC=ON` is now the configured state of `build/`. OCCT 8 deleted
`TopTools_ListIteratorOfListOfShape.hxx` (the iterator is a typedef in
`TopTools_ListOfShape.hxx` now); that stale include was the only breakage in the
whole tree.

## What is real now that was not

- `adapt::MetricField` — SPD metric infrastructure. Anisotropy is *enabled*, not
  shipped: the Mmg3d adapter and Hessian recovery are the next slice.
- Size fields reach `graded_tet_fill_surface` / `mixed_fill_surface`. Empty field
  is byte-identical to the old path, so the regression gate is the suite itself.
- Selective p-elevation is conforming. The headline number, printed by the suite:
  affine patch error **3.48e-19 constrained vs 3.74e-4 unconstrained**. That
  delta is the proof the old path was silently wrong.
- Campaign rows carry `advisor-row-v3` context features + action objects and
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
