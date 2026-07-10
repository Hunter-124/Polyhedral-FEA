# BENCHMARKS — Verification Suite & Anti-Cheat Design

All reference values live in `bench/reference/*.json` with citations/derivations.
Solver, mesher, and adapt crates must never read this directory.

## Tier 0 — Correctness gates (must pass exactly, every commit)
| Test | What it proves |
|---|---|
| Patch test (constant strain) per element type/order on distorted meshes | Element formulation is consistent |
| Rigid-body modes produce zero strain energy | No spurious stiffness |
| Single-element eigenvalue check (no spurious zero-energy modes) | VEM stabilization is correct |
| Mesh validity: watertight, positive Jacobians, conforming interfaces | Mesher output is legal input |

## Tier 1 — Analytical solutions (energy-norm + peak-stress error targets)
| Case | Closed form | What it stresses |
|---|---|---|
| Thick-walled cylinder under internal pressure | Lamé | Curved geometry, radial gradients |
| Plate with circular hole, uniaxial tension | Kirsch (SCF = 3 at hole) | Stress concentration capture |
| Slender cantilever, end load | Timoshenko beam | Bending, shear-locking detection on hexes |
| Sphere w/ spherical cavity under remote tension | Goodier | Fully 3D concentration |
| Pressurized sphere | Lamé (spherical) | Symmetry preservation |
| L-shaped domain (re-entrant edge) | Known singularity exponent | Edge singularity: does hp-grading recover optimal rate? |

Pass criteria per case: (a) error ≤ target at the adaptive stopping point, AND
(b) observed convergence order under uniform refinement matches theory ±0.2.

## Tier 2 — Method of Manufactured Solutions (MMS)
Pick smooth displacement fields u(x,y,z), derive body forces symbolically,
verify O(h^p) convergence for every element type at p = 1..3. MMS fields are
generated with a randomized seed at test time so their coefficients cannot be
memorized or hardcoded.

## Tier 3 — Performance benchmarks (the "win" metric)
On each geometry in `bench/geometries/public/`: adaptive hybrid run vs. uniform
2nd-order tet baseline at equal energy-norm error. Record DOFs, mesh time,
solve time, peak memory. Targets: ≥5× DOF reduction, ≥3× wall time.

## Anti-cheat / adversarial audit design
1. **Holdout geometries**: `bench/geometries/holdout/` is git-ignored and never
   shown to the implementation loop. The audit runs the full suite on holdouts;
   performance/accuracy must be within 25% of public-suite results, else the
   improvement is overfit and gets reverted or generalized.
2. **Perturbation invariance**: rotate/translate/scale every benchmark geometry
   by random rigid transforms; results must be invariant (stress is objective).
   Catches axis-aligned or coordinate-dependent hacks.
3. **Parameter sweep**: re-run with perturbed material constants (E, ν) and
   loads; error behavior must be stable. Catches constants tuned to defaults.
4. **Grep audit**: automated scan of `mesh/`, `adapt/`, `fea/` for numeric
   literals matching any reference value (e.g. 3.0 near SCF logic) — flagged
   for human review with justification required.
5. **Estimator honesty**: effectivity index of the ZZ estimator (estimated/true
   error on analytical cases) must stay in [0.5, 2]. Catches "improving" scores
   by making the estimator lie so the loop stops early.
6. **Trade-off matrix**: any change that improves one Tier-1 case must report
   deltas on *all* Tier-1/2 cases + mesh/solve times. Net-negative changes
   don't merge even if they fix the targeted case.
