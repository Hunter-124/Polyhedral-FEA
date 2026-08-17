# ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening

Status: accepted
Date: 2026-08-16

## Context

Four gaps were open at once, all visible from the same measurements:

1. **No derefinement anywhere.** `HpAction` had refine / p-raise / shape /
   none; LEB is refine-only; the adapt loop's global `h_use` ratcheted down
   but never back up. Over-resolved regions stayed fine forever.
2. **Curvature noise → sizing noise.** OCC BRepLProp κ along CAD edges and
   the discrete per-vertex κ proxy (`indicators.hpp:9-19`) both carry noise
   that the min-plus envelope faithfully turns into spurious fine seeds.
3. **Budgets were ceilings, not trims.** `resolve_mesh_size` + auto-retry
   enforce a cap by coarsening the *whole* lattice after the fact; nothing
   removed the *insignificant* fine structure first.
4. **The advisor could not answer "within this budget".** The gated
   enumeration ranked by predicted error only; a user with a hard DOF cap
   got the best-accuracy action even when its predicted DOF blew the cap.

The measure-first program (ADR-0023/0024) constrains every one of these:
no retuned frozen gates, no hardcoded answers, campaign baselines unchanged.

## Decision

### 1. Spectral sizing (`adapt::spectral`, new module)

Self-contained radix-2 FFT (double, deterministic, power-of-two grids) plus
three uses, each matched to what Fourier analysis is actually good at:

- **Edge curvature denoise.** κ(s) along each CAD edge (uniform arc-length
  resample → even-reflect → FFT → keep dominant modes to 99.5% energy →
  inverse) recovers the smooth curvature before chordal sources are emitted
  at the constant-relative-sag rule h = 0.25/κ (sagitta = 0.78% of the local
  radius — the same relative sag the surface-vertex rule already uses).
  Seams are skipped; flat runs after denoise emit nothing.
- **Field trimming.** The fused size field is sampled on a power-of-two
  Cartesian grid (≤64³), and the spectrum is truncated at 99.5% energy.
  Spatially extended demands survive verbatim; isolated seed artifacts merge
  into the surrounding coarse field. Values are clamped to the entry
  [min, max], then the **geometry-only field is re-imposed as an elementwise
  min**, so the trim can never blur a real curvature / thin-wall demand.
- **Budget targeting (library API).** `enforce_element_budget` truncates,
  then applies one uniform h scale landing the Σvol/h³ prediction on a
  budget. This is only well-calibrated for meshers honoring the CVT density
  contract (ADR-0024 Q10 #4).

**Deliberately not wired:** the budget scale is *not* driven from the
lattice-meshers' element ceilings. Measured on sphere (auto-h, cap 8000):
the grid density model predicted 432 where the real mesh had 9,456 elements
(fine bands, skin cells, lattice quantization) — a ~20× part-dependent
mismatch no constant repairs. `resolve_mesh_size` + measured auto-retry
remain the element-cap authority (the same run enforced 8000 exactly); the
spectral budget serves density-contract callers.

### 2. Coarsening (`HpAction::kCoarsen` + loop executor)

- `dorfler_coarsen_mark` (anti-Dörfler): the largest ascending-η prefix whose
  cumulative η² ≤ θ·total (default θ = 0.02) — the *insignificant tail*.
- The driver may mark `kCoarsen` only when the element is otherwise `kNone`
  (h / p / shape always win), is in the tail, and is measurably over-resolved:
  exact element size (cube-root volume, not the global-h proxy the signals
  used before) finer than the a-priori fused-field demand by ≥ 1.5×. The
  coarsen target is the full demand — the remesh executor rebuilds from the
  size field, so the prediction must name the field's own answer.
- Loop: a pure-coarsen pass suppresses the Dörfler→LEB fallback (LEB cannot
  coarsen), suggests a bounded global h rise (×1.25/pass, capped at the
  resolved a-priori h), disables early-stop, and remeshes with no seeds so
  over-refined regions revert to the a-priori field. A-priori geometry and
  BC demands live *in* that field, so they are structurally untouchable.

**Measured firing domain:** on plate_hole / cantilever at adapt 2–3 no
coarsen marks fired — smooth elements are legitimately claimed by p-raise
first, and the singularities stay refine-marked. The mechanism targets LEB /
seed-ball overshoot and long quiet runs; it is conservative by design, not a
default agitator. (One free global-h ratchet: 0.75 > 1/1.5, two ratchets
0.75² open the gate.)

### 3. Budget-feasible advisor chooser

`Advisor::recommend(..., max_dof)` drops candidate actions whose dof head
exceeds the budget after the failure gate and before the accuracy ranking;
an emptied candidate set is a refusal (`budget_refusal`, clamp-box defaults,
NaN-suppressed predictions) — never an unaffordable answer. CLI:
`--advisor-max-dof N`. Measured on corpus part `box_hole_s0`: unfiltered →
graded_tet order 2 (pred DOF 66,179, rel_err_rel −1.485); budget 3000 → hex
order 1 (pred DOF 2,841 ≤ 3000, rel_err_rel +0.127). The dof head's held-out
MAE (~0.5 log10) makes this a feasibility filter, not a guarantee.

### 4. CG equilibration

`symmetric_diagonal_scaling` (S = diag(1/√diag K); throws on non-SPD input →
falls back to the previous behavior) equilibrates K_ff before the existing
IC → shifted-IC → Jacobi cascade. MPC transforms spread the K_ff diagonal
over orders of magnitude; on S·K·S incomplete Cholesky is measurably more
robust. Acceptance still measures the *physical* system: every attempt is
unscaled and the true relative residual ‖b−Kx‖/‖b‖ is recomputed in the
original space before selection, acceptance, or the throw path. Direct LDLT
and every GATE-1 frozen path are untouched. Measured: 12×2×2 cantilever
with a 10³-weight MPC (diagonal spread ~10⁶) — equilibrated IC converges in
58 iterations to 1.6e-9 original-space residual, parity with LDLT < 1e-5.

### 5. Advisor hygiene (measured, no retrain)

- Four stale comments claimed a 43-column contract and geo_*-not-in-network;
  the shipped contract is 62 columns and the geo_* descriptors are network
  inputs *and* the OOD space. Comments corrected (also scene.hpp).
- testlab `geom_class_of` curved_frac used (ntri−12)/ntri, saturating to 1.0
  on any real triangulation; now the serving-side definition (fraction of
  vertices with κ·diag > 1e-8). The dataset's curved_frac was verified to
  come from `case_features_json` (already honest), so no retrain is implied.
- testlab campaign knob `"spectral_smooth"` (default false) mirrors
  `bc_grading`: frozen baselines unchanged, campaigns opt in.

## Defaults and baselines

- Library: `SimSetup.spectral_smooth = false` default. CLI product path
  (mesh / solve / diag): spectral ON, `--no-spectral` opt-out — the same
  split `bc_grading` already uses (progress 2026-07-14). Campaign grids are
  untouched; `spectral_smooth` is a new opt-in grid key.
- Suite: 432/432 green (OCC on), including new `[spectral]` (14 cases),
  hp-driver coarsen, equilibration, and advisor-budget tests.

## Measured A/B (this machine, 2026-08-16)

| Case | Base | Spectral | Note |
|---|---|---|---|
| sphere mesh h=0.008 | 51 seeds | 41 seeds (+h_min 0.005117→0.00507) | identical 9,194-elem mesh |
| sphere solve auto-h | — | 209,873/262,143 modes kept; 14→2 seeds | CG ran equilibrated IC → Jacobi cascade |
| icecream_cone h=0.008 | 95 seeds | 37 + 43 denoised edge seeds | identical 27,254 elems, quality_min 0.02098, VM 2.35535e7 Pa, BRep p99/h 0.03878 both |
| box_hole_s0 advisor | graded_tet p2, DOF 66k | hex p1, DOF 2.8k at cap 3000 | budget filter live |

The trim is a no-op in element terms on the clean public fixtures — their
fields are already smooth. Its value is on noisy real-world curvature, and
the seed sets it emits are leaner and denoised everywhere.

## Consequences

- `build_refinement_plan` grows `spectral` / `spectral_budget` trailing
  params (defaults preserve every existing caller); `RefinementPlan.spectral`
  reports modes / energy / predictions for CLI text and `diag --json`.
- `HpDriverPlan` grows `coarsen_mark` / `n_coarsen`; `make_hp_signals` and
  `drive_hp` grow trailing optional params; pre-change plans are reproduced
  exactly when `h_geometry` is unset (regression-tested).
- `AdvisorDecision` grows `budget_refusal`; `to_json` carries it.
- New module `adapt::spectral` is deterministic and double-only; tests pin
  FFT known-answers, Parseval, truncation recovery, budget scaling law, and
  clamp semantics.
